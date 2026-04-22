#include "flexio_pp_dev_utils.h"

#define WORKER_BATCH_SIZE 1048576UL
#define WORKER_QUEUE_BURST_SIZE 128*2

#if WORKER_QUEUE_BURST_REPORT
static inline void
worker_queue_burst_report_reset(struct dpa_thread_context *thd_ctx)
{
	for (uint32_t q = 0; q < WORKER_QUEUES_PER_THREAD; q++) {
		thd_ctx->queue_burst_sum[q] = 0;
		thd_ctx->queue_poll_count[q] = 0;
	}
}

static inline void
worker_queue_burst_report_accumulate(struct dpa_thread_context *thd_ctx,
				     uint32_t queue_idx,
				     size_t queue_burst)
{
	thd_ctx->queue_burst_sum[queue_idx] += queue_burst;
	thd_ctx->queue_poll_count[queue_idx]++;
}

static inline void
worker_queue_burst_report_print(int thd_id,
				struct dpa_thread_context *thd_ctx)
{
	for (uint32_t q = 0; q < WORKER_QUEUES_PER_THREAD; q++) {
		size_t poll_count = thd_ctx->queue_poll_count[q];
		size_t avg_burst = poll_count ? thd_ctx->queue_burst_sum[q] / poll_count : 0;

		flexio_dev_print("worker %d queue %u avg queue_burst per poll: %zu polls: %zu\n",
				 thd_id, q, avg_burst, poll_count);
	}
}
#endif

flexio_dev_event_handler_t flexio_pp_dev_32;
__dpa_global__ void flexio_pp_dev_32(uint64_t thread_arg)
{	
	struct host2dev_packet_processor_data_thd *data_from_host = (void *)thread_arg;
	register int i = data_from_host->thd_id;
	register struct offload_dispatch_info *thd_info = &offload_info[i];
	struct flexio_dev_thread_ctx *dtctx;	
	register struct dpa_thread_context *thd_ctx = &(dpa_thds_ctx[i]);
	register struct flexio_dpa_dev_queue *thd_queue = &(thd_ctx->queue);
	register cq_ctx_t *wakeup_cq_ctx = &(thd_queue->rq_cq_ctx);
	register size_t pkt_count = 0;
	struct flexio_dpa_dev_queue *rq_queues[WORKER_QUEUES_PER_THREAD];

	flexio_dev_get_thread_ctx(&dtctx);
	com_step_cq(wakeup_cq_ctx);

	if(!data_from_host->not_first_run){
		if (__atomic_load_n(&thd_info->status, __ATOMIC_ACQUIRE) == EU_OFF) {
			__atomic_store_n(&thd_info->status, EU_FREE, __ATOMIC_RELEASE);
			// flexio_dev_print(dtctx, "Worker thread %d is set to FREE status.\n", i);
		}
		spin_on_status(i, EU_HANG);
		data_from_host->not_first_run = 1;
	}

	rq_queues[0] = __atomic_load_n(&thd_info->assigned_queues[0], __ATOMIC_RELAXED);
	rq_queues[1] = __atomic_load_n(&thd_info->assigned_queues[1], __ATOMIC_RELAXED);

#if WORKER_QUEUE_BURST_REPORT
	worker_queue_burst_report_reset(thd_ctx);
#endif

	for (;;) {
		for (register uint32_t q = 0; q < WORKER_QUEUES_PER_THREAD; q++) {
			register struct flexio_dpa_dev_queue *rq_queue = rq_queues[q];
			register sq_ctx_t *tx_sq_ctx;
			register uint32_t tx_sq_number;
			register size_t queue_burst = 0;

#if WORKER_TX_USE_PRIVATE_SQ
			tx_sq_ctx = &(thd_queue->sq_ctx);
			tx_sq_number = tx_sq_ctx->sq_number;
#else
			tx_sq_ctx = &(rq_queue->sq_ctx);
			tx_sq_number = tx_sq_ctx->sq_number;
#endif

			while (flexio_dev_cqe_get_owner(rq_queue->rq_cq_ctx.cqe) != rq_queue->rq_cq_ctx.cq_hw_owner_bit &&
			       queue_burst < WORKER_QUEUE_BURST_SIZE) {
				pp_queue(dtctx, rq_queue, tx_sq_ctx, tx_sq_number);
				pkt_count++;
				queue_burst++;
				if (pkt_count >= WORKER_BATCH_SIZE) {
#if WORKER_QUEUE_BURST_REPORT
					worker_queue_burst_report_accumulate(thd_ctx, q, queue_burst);
#endif
					goto worker_sleep;
				}
			}
#if WORKER_QUEUE_BURST_REPORT
			worker_queue_burst_report_accumulate(thd_ctx, q, queue_burst);
#endif
		}
	}

worker_sleep:
#if WORKER_QUEUE_BURST_REPORT
	worker_queue_burst_report_print(i, thd_ctx);
#endif
	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	flexio_dev_cq_arm(dtctx, wakeup_cq_ctx->cq_idx, wakeup_cq_ctx->cq_number);
	__atomic_store_n(&thd_info->status, EU_OFF, __ATOMIC_RELEASE);
	flexio_dev_thread_reschedule();
}
