#include "flexio_pp_dev_utils.h"

#define WORKER_BATCH_SIZE 1000000UL

flexio_dev_event_handler_t flexio_pp_dev_1;
__dpa_global__ void flexio_pp_dev_1(uint64_t thread_arg)
{
	struct host2dev_packet_processor_data_thd *data_from_host = (void *)thread_arg;
	int i = data_from_host->thd_id;

	if (__atomic_load_n(&offload_info[i].status, __ATOMIC_ACQUIRE) == EU_OFF){
		__atomic_store_n(&offload_info[i].status, EU_FREE, __ATOMIC_RELEASE);
	}

	struct flexio_dev_thread_ctx *dtctx;	
	struct dpa_thread_context* this_thd_ctx = &(dpa_thds_ctx[i]);
	struct flexio_dpa_dev_queue* this_tenant;
	flexio_dev_get_thread_ctx(&dtctx);
	com_step_cq(&(this_thd_ctx->rq_cq_ctx));

	spin_on_status(i, EU_HANG);
	this_tenant = __atomic_load_n(&offload_info[i].tenant, __ATOMIC_RELAXED);

	register size_t pkt_count = 0;
	while (dtctx != NULL) {
		while (flexio_dev_cqe_get_owner(this_tenant->rq_cq_ctx.cqe) != this_tenant->rq_cq_ctx.cq_hw_owner_bit) {
			pp_queue(dtctx, this_tenant);
			pkt_count++;
			if (pkt_count >= WORKER_BATCH_SIZE) {
				pkt_count = 0;
				__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
				flexio_dev_cq_arm(dtctx, this_thd_ctx->rq_cq_ctx.cq_idx, this_thd_ctx->rq_cq_ctx.cq_number);
				__atomic_store_n(&offload_info[i].status, EU_OFF, __ATOMIC_RELEASE);
				flexio_dev_thread_reschedule();
				return;
			}
		}
	}

	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	flexio_dev_cq_arm(dtctx, dpa_thds_ctx[i].rq_cq_ctx.cq_idx, dpa_thds_ctx[i].rq_cq_ctx.cq_number);
	__atomic_store_n(&offload_info[i].status, EU_OFF, __ATOMIC_RELEASE);
	flexio_dev_thread_reschedule();
}
