#include "flexio_pp_dev_utils.h"

#define WORKER_BATCH_SIZE 1048576UL
#define WORKER_QUEUE_BURST_SIZE (Q_DEPTH * 2UL)

flexio_dev_event_handler_t flexio_pp_dev_32;
__dpa_global__ void flexio_pp_dev_32(uint64_t thread_arg)
{	
	struct host2dev_packet_processor_data_thd *data_from_host = (void *)thread_arg;
	int i = data_from_host->thd_id;

	if (__atomic_load_n(&offload_info[i].status, __ATOMIC_ACQUIRE) == EU_OFF){
		__atomic_store_n(&offload_info[i].status, EU_FREE, __ATOMIC_RELEASE);
	}

	struct flexio_dev_thread_ctx *dtctx;	
	struct dpa_thread_context* this_thd_ctx = &(dpa_thds_ctx[i]);
	struct flexio_dpa_dev_queue *rq_queues[WORKER_QUEUES_PER_THREAD] = {0};
	uint32_t active_queues;
	flexio_dev_get_thread_ctx(&dtctx);
	com_step_cq(&(this_thd_ctx->queue.rq_cq_ctx));

	if(!data_from_host->not_first_run){
		spin_on_status(i, EU_HANG);
		data_from_host->not_first_run = 1;
	}
	active_queues = __atomic_load_n(&offload_info[i].num_queues, __ATOMIC_ACQUIRE);
	// if (active_queues > WORKER_QUEUES_PER_THREAD) {
	// 	active_queues = WORKER_QUEUES_PER_THREAD;
	// }
	for (uint32_t q = 0; q < active_queues; q++) {
		rq_queues[q] = __atomic_load_n(&offload_info[i].assigned_queues[q], __ATOMIC_RELAXED);
	}

	register size_t pkt_count = 0, queue_burst = 0;
	while (dtctx != NULL) {
		for (uint32_t q = 0; q < active_queues; q++) {
			struct flexio_dpa_dev_queue *rq_queue = rq_queues[q];
			queue_burst = 0;

			while (flexio_dev_cqe_get_owner(rq_queue->rq_cq_ctx.cqe) != rq_queue->rq_cq_ctx.cq_hw_owner_bit &&
			       queue_burst < WORKER_QUEUE_BURST_SIZE) {
				pp_queue(dtctx, rq_queue, rq_queue, &(this_thd_ctx->queue));
				pkt_count++;
				queue_burst++;
				if (pkt_count >= WORKER_BATCH_SIZE) {
					__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
					flexio_dev_cq_arm(dtctx,
							  this_thd_ctx->queue.rq_cq_ctx.cq_idx,
							  this_thd_ctx->queue.rq_cq_ctx.cq_number);
					__atomic_store_n(&offload_info[i].status, EU_OFF, __ATOMIC_RELEASE);
					flexio_dev_thread_reschedule();
					return;
				}
			}
		}
	}

	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	flexio_dev_cq_arm(dtctx, dpa_thds_ctx[i].queue.rq_cq_ctx.cq_idx,
			  dpa_thds_ctx[i].queue.rq_cq_ctx.cq_number);
	__atomic_store_n(&offload_info[i].status, EU_OFF, __ATOMIC_RELEASE);
	flexio_dev_thread_reschedule();
}
