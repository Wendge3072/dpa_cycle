#include "flexio_pp_dev_utils.h"

flexio_dev_event_handler_t flexio_pp_dev_2;
__dpa_global__ void flexio_pp_dev_2(uint64_t thread_arg)
{	
	struct host2dev_packet_processor_data_thd *data_from_host = (void *)thread_arg;
	int i = data_from_host->thd_id;
	// flexio_dev_print("thread %2d start to run\n", i);

	// if(change_status(i, EU_OFF, EU_FREE)){
	// 	flexio_dev_print("ERROR: thd %d status change failed\n", i);
	// 	return;
	// }

	if (__atomic_load_n(&offload_info[i].status, __ATOMIC_ACQUIRE) == EU_OFF){
		__atomic_store_n(&offload_info[i].status, EU_FREE, __ATOMIC_RELEASE);
		// __dpa_thread_memory_writeback();
	}

	struct flexio_dev_thread_ctx *dtctx;	
	struct dpa_thread_context* this_thd_ctx = &(dpa_thds_ctx[i]);
	struct flexio_dpa_dev_queue* this_tenant;
	flexio_dev_get_thread_ctx(&dtctx);
	com_step_cq(&(this_thd_ctx->rq_cq_ctx));

	if(!data_from_host->not_first_run){
		spin_on_status(i, EU_HANG);
		data_from_host->not_first_run = 1;
	}
	this_tenant = __atomic_load_n(&offload_info[i].tenant, __ATOMIC_RELAXED);

	// size_t time_interval = 10;
	// register size_t reschedule_cycle = __dpa_thread_cycles() + time_interval * DPA_FREQ_HZ;
	register size_t pkt_count = 0;
	register size_t cycle_Mp = __dpa_thread_cycles();
	register size_t busy_cycle = 0;
	while (dtctx != NULL) {
		while (flexio_dev_cqe_get_owner(this_tenant->rq_cq_ctx.cqe) != this_tenant->rq_cq_ctx.cq_hw_owner_bit) {
			size_t cycle_start = __dpa_thread_cycles();
			pp_queue(dtctx, this_thd_ctx, this_tenant);
			size_t cycle_delta = __dpa_thread_cycles() - cycle_start;
			com_step_cq(&(this_tenant->rq_cq_ctx));
			__atomic_fetch_add(&offload_info[i].busy_cycle, cycle_delta, __ATOMIC_RELAXED);
			busy_cycle += cycle_delta;

			// size_t now_cycle = __dpa_thread_cycles();
			pkt_count++;
			// if (now_cycle >= reschedule_cycle) {
			if (pkt_count >= 1000000) {
				pkt_count = 0;
				__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
				flexio_dev_cq_arm(dtctx, this_thd_ctx->rq_cq_ctx.cq_idx, this_thd_ctx->rq_cq_ctx.cq_number);
				__atomic_store_n(&offload_info[i].status, EU_OFF, __ATOMIC_RELEASE);
				cycle_Mp = __dpa_thread_cycles() - cycle_Mp;
				flexio_dev_print("thread %2d: cycles per packet: %ld， cycles processing packet: %ld\n", i, cycle_Mp/1000000, busy_cycle/1000000);
				busy_cycle = 0;
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