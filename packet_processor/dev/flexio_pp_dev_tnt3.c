#include "flexio_pp_dev_utils.h"

#define WORKER_BATCH_SIZE 1048576UL
#define WORKER_QUEUE_BURST_SIZE (Q_DEPTH * 2UL)

flexio_dev_event_handler_t flexio_pp_dev_32;
__dpa_global__ void flexio_pp_dev_32(uint64_t thread_arg)
{	
	struct host2dev_packet_processor_data_thd *data_from_host = (void *)thread_arg;
	int i = data_from_host->thd_id;
	struct flexio_dev_thread_ctx *dtctx;	
	struct dpa_thread_context *this_thd_ctx = &(dpa_thds_ctx[i]);
	struct flexio_dpa_dev_queue *thd_queue = &(this_thd_ctx->queue);
	struct flexio_dpa_dev_queue *rq_queue0;
	struct flexio_dpa_dev_queue *rq_queue1;
	cq_ctx_t *wakeup_cq_ctx = &(thd_queue->rq_cq_ctx);
	sq_ctx_t *tx_sq_ctx = &(thd_queue->sq_ctx);
	uint32_t tx_sq_number = tx_sq_ctx->sq_number;
	size_t pkt_count = 0;
	size_t queue_burst;

	flexio_dev_get_thread_ctx(&dtctx);
	com_step_cq(wakeup_cq_ctx);

	if(!data_from_host->not_first_run){
		if (__atomic_load_n(&offload_info[i].status, __ATOMIC_ACQUIRE) == EU_OFF) {
			__atomic_store_n(&offload_info[i].status, EU_FREE, __ATOMIC_RELEASE);
		}
		spin_on_status(i, EU_HANG);
		data_from_host->not_first_run = 1;
	}

	rq_queue0 = __atomic_load_n(&offload_info[i].assigned_queues[0], __ATOMIC_RELAXED);
	rq_queue1 = __atomic_load_n(&offload_info[i].assigned_queues[1], __ATOMIC_RELAXED);

	for (;;) {
		queue_burst = 0;
		while (flexio_dev_cqe_get_owner(rq_queue0->rq_cq_ctx.cqe) != rq_queue0->rq_cq_ctx.cq_hw_owner_bit &&
		       queue_burst < WORKER_QUEUE_BURST_SIZE) {
			pp_queue(dtctx, rq_queue0, tx_sq_ctx, tx_sq_number);
			pkt_count++;
			queue_burst++;
			if (pkt_count >= WORKER_BATCH_SIZE) {
				goto worker_sleep;
			}
		}

		queue_burst = 0;
		while (flexio_dev_cqe_get_owner(rq_queue1->rq_cq_ctx.cqe) != rq_queue1->rq_cq_ctx.cq_hw_owner_bit &&
		       queue_burst < WORKER_QUEUE_BURST_SIZE) {
			pp_queue(dtctx, rq_queue1, tx_sq_ctx, tx_sq_number);
			pkt_count++;
			queue_burst++;
			if (pkt_count >= WORKER_BATCH_SIZE) {
				goto worker_sleep;
			}
		}
	}

worker_sleep:
	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	flexio_dev_cq_arm(dtctx, wakeup_cq_ctx->cq_idx, wakeup_cq_ctx->cq_number);
	__atomic_store_n(&offload_info[i].status, EU_OFF, __ATOMIC_RELEASE);
	flexio_dev_thread_reschedule();
}
