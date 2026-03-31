#include "flexio_pp_dev_utils.h"

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
	struct flexio_dpa_dev_queue* this_tenant;
	flexio_dev_get_thread_ctx(&dtctx);
	com_step_cq(&(this_thd_ctx->rq_cq_ctx));

	if(!data_from_host->not_first_run){
		spin_on_status(i, EU_HANG);
		data_from_host->not_first_run = 1;
	}
	this_tenant = __atomic_load_n(&offload_info[i].tenant, __ATOMIC_RELAXED);

	register size_t pkt_count = 0;
	register size_t cycle_delta;
	register size_t t0_pkt_count = 0, t0_cycle_sum = 0, t0_result_sum = 0;
	register size_t t1_pkt_count = 0, t1_cycle_sum = 0, t1_result_sum = 0;
	uint32_t result;
	while (dtctx != NULL) {
		while (flexio_dev_cqe_get_owner(this_tenant->rq_cq_ctx.cqe) != this_tenant->rq_cq_ctx.cq_hw_owner_bit) {
			cycle_delta = __dpa_thread_cycles();
			int t_id = pp_queue(dtctx, this_thd_ctx, this_tenant, i, &result);
			// flexio_dev_print("pp_queue return tid = %d\n", t_id);
			// com_step_cq(&(this_tenant->rq_cq_ctx));
			cycle_delta = __dpa_thread_cycles() - cycle_delta;
			if (t_id >= 0) {
				__atomic_fetch_add(&offload_info[i].busy_cycle[t_id], cycle_delta, __ATOMIC_RELAXED);
			}
			if (t_id == 1) {
				t1_cycle_sum += cycle_delta;
				t1_pkt_count++;
				t1_result_sum += result;
			} else {
				t0_cycle_sum += cycle_delta;
				t0_pkt_count++;
				t0_result_sum += result;
			}

			pkt_count++;
			if (pkt_count >= 1000000) {
				pkt_count = 0;
				if (t0_pkt_count != 0 && t1_pkt_count != 0) {
					flexio_dev_print("tnt 0 pkt num %7zu, avg cycle per pkt %6zu, avg result %zu\n", t0_pkt_count, t0_cycle_sum/t0_pkt_count, t0_result_sum/t0_pkt_count);
					flexio_dev_print("tnt 1 pkt num %7zu, avg cycle per pkt %6zu, avg result %zu\n", t1_pkt_count, t1_cycle_sum/t1_pkt_count, t1_result_sum/t1_pkt_count);
				}
				else{
					flexio_dev_print("--- tnt running ---, t0_pkt_count: %zu, t1_pkt_count: %zu\n", t0_pkt_count, t1_pkt_count);
				}
				__atomic_store_n(&offload_info[i].status, EU_OFF, __ATOMIC_RELEASE);
				__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
				flexio_dev_cq_arm(dtctx, this_thd_ctx->rq_cq_ctx.cq_idx, this_thd_ctx->rq_cq_ctx.cq_number);
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