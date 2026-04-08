#include "flexio_pp_dev_utils.h"

#define WORKER_BATCH_SIZE 1048576UL
#define WORKER_QUEUE_BURST_SIZE (Q_DEPTH * 2UL)

flexio_dev_event_handler_t flexio_pp_dev_32;
__dpa_global__ void flexio_pp_dev_32(uint64_t thread_arg)
{	
	struct host2dev_packet_processor_data_thd *data_from_host = (void *)thread_arg;
	int i = data_from_host->thd_id;
	struct offload_dispatch_info *thd_info = &offload_info[i];
	struct flexio_dev_thread_ctx *dtctx;	
	struct flexio_dpa_dev_queue *thd_queue = &(dpa_thds_ctx[i].queue);
	cq_ctx_t *wakeup_cq_ctx = &(thd_queue->rq_cq_ctx);
	struct flexio_dpa_dev_queue *rq_queues[WORKER_QUEUES_PER_THREAD];
	register struct dpa_sche_context *sch_ctx;
	register struct flexio_dpa_dev_queue *rq_queue = NULL;
	register size_t pkt_count = 0;
	register size_t cycle_delta = 0;
	register size_t queue_burst = 0;
	register sq_ctx_t *tx_sq_ctx;
	register uint32_t tx_sq_number;

	flexio_dev_get_thread_ctx(&dtctx);
	com_step_cq(wakeup_cq_ctx);

	if(!data_from_host->not_first_run){
		if (__atomic_load_n(&thd_info->status, __ATOMIC_ACQUIRE) == EU_OFF) {
			__atomic_store_n(&thd_info->status, EU_FREE, __ATOMIC_RELEASE);
		}
		spin_on_status(i, EU_HANG);
		data_from_host->not_first_run = 1;
	}

	rq_queues[0] = __atomic_load_n(&thd_info->assigned_queues[0], __ATOMIC_RELAXED);
	rq_queues[1] = __atomic_load_n(&thd_info->assigned_queues[1], __ATOMIC_RELAXED);
	sch_ctx = __atomic_load_n(&thd_info->sch_ctx, __ATOMIC_RELAXED);

	for (;;) {
		for (register uint32_t q = 0; q < WORKER_QUEUES_PER_THREAD; q++) {
			rq_queue = rq_queues[q];
			queue_burst = 0;

#if WORKER_TX_USE_PRIVATE_SQ
			tx_sq_ctx = &(thd_queue->sq_ctx);
			tx_sq_number = tx_sq_ctx->sq_number;
#else
			tx_sq_ctx = &(rq_queue->sq_ctx);
			tx_sq_number = tx_sq_ctx->sq_number;
#endif

			while (flexio_dev_cqe_get_owner(rq_queue->rq_cq_ctx.cqe) != rq_queue->rq_cq_ctx.cq_hw_owner_bit &&
			       queue_burst < WORKER_QUEUE_BURST_SIZE) {
				cycle_delta = __dpa_thread_cycles();
				pp_queue(dtctx, rq_queue, tx_sq_ctx, tx_sq_number);
				cycle_delta = __dpa_thread_cycles() - cycle_delta; 
				/* Each worker queue slot corresponds to one tenant in the current 2-queue layout. */
				// if (sch_ctx != NULL) {
					__atomic_fetch_add(&sch_ctx->tenant_cycle_used[q], cycle_delta, __ATOMIC_RELAXED);
				// }
				pkt_count++;
				queue_burst++;
				if (pkt_count >= WORKER_BATCH_SIZE) {
					goto worker_sleep;
				}
			}
		}
	}

worker_sleep:
	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	flexio_dev_cq_arm(dtctx, wakeup_cq_ctx->cq_idx, wakeup_cq_ctx->cq_number);
	__atomic_store_n(&thd_info->status, EU_OFF, __ATOMIC_RELEASE);
	flexio_dev_thread_reschedule();
}
