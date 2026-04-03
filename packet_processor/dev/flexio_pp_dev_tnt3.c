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
	struct flexio_dpa_dev_queue* this_tenant_base;
	flexio_dev_get_thread_ctx(&dtctx);

	if(!data_from_host->not_first_run){
		spin_on_status(i, EU_HANG);
		data_from_host->not_first_run = 1;
	}
	this_tenant_base = __atomic_load_n(&offload_info[i].tenant, __ATOMIC_RELAXED);
	size_t tenants_num = offload_info[i].sch_ctx->tenants_num;

	register size_t pkt_count = 0;
	register size_t cycle_delta;
#if report_pkt_usage
	register size_t t0_pkt_count = 0, t0_cycle_sum = 0, t0_result_sum = 0;
	register size_t t1_pkt_count = 0, t1_cycle_sum = 0, t1_result_sum = 0;
	register size_t tb_pkt_count = 0, tb_cycle_sum = 0, tb_result_sum = 0;
#endif
	uint32_t result;
	while (dtctx != NULL) {
		for (uint32_t t = 0; t < tenants_num; t++) {
			struct flexio_dpa_dev_queue *this_tenant = &this_tenant_base[t];
			int pkt_lmt = 1 << 9;

			if (__atomic_load_n(&offload_info[i].sch_ctx->restrict_tenant[t], __ATOMIC_ACQUIRE)) {
				continue;
			}

			while (flexio_dev_cqe_get_owner(this_tenant->rq_cq_ctx.cqe) != this_tenant->rq_cq_ctx.cq_hw_owner_bit &&
			       pkt_lmt > 0) {
				cycle_delta = __dpa_thread_cycles();
				int t_id = pp_queue(dtctx, this_thd_ctx, this_tenant, i, (int)t, &result);
				cycle_delta = __dpa_thread_cycles() - cycle_delta;
				if (t_id >= 0) {
					__atomic_fetch_add(&offload_info[i].sch_ctx->busy_cycle[t_id], cycle_delta, __ATOMIC_RELAXED);
				}
#if report_pkt_usage
				if (t_id == 1) {
					t1_cycle_sum += cycle_delta;
					t1_pkt_count++;
					t1_result_sum += result;
				} else if (t_id == 0) {
					t0_cycle_sum += cycle_delta;
					t0_pkt_count++;
					t0_result_sum += result;
				} else {
					tb_cycle_sum += cycle_delta;
					tb_pkt_count++;
					tb_result_sum += result;
				}
#endif
				pkt_count++;
				pkt_lmt--;

				if (pkt_count >= 1000000) {
					pkt_count = 0;
#if report_pkt_usage
					if (t0_pkt_count != 0 && t1_pkt_count != 0) {
						flexio_dev_print("tnt 0 pkt num %7zu, avg cycle per pkt %6zu, avg result %zu\n",
								 t0_pkt_count, t0_cycle_sum / t0_pkt_count, t0_result_sum / t0_pkt_count);
						flexio_dev_print("tnt 1 pkt num %7zu, avg cycle per pkt %6zu, avg result %zu\n",
								 t1_pkt_count, t1_cycle_sum / t1_pkt_count, t1_result_sum / t1_pkt_count);
						flexio_dev_print("tnt b pkt num %7zu, avg cycle per pkt %6zu, avg result %zu\n",
								 tb_pkt_count, tb_cycle_sum / tb_pkt_count, tb_result_sum / tb_pkt_count);
					} else {
						flexio_dev_print("--- tnt running ---, t0_pkt_count: %zu, t1_pkt_count: %zu\n",
								 t0_pkt_count, t1_pkt_count);
					}
#endif
					__atomic_store_n(&offload_info[i].status, EU_OFF, __ATOMIC_RELEASE);
					__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
					for (uint32_t arm_t = 0; arm_t < tenants_num; arm_t++) {
						struct flexio_dpa_dev_queue *arm_q = &this_tenant_base[arm_t];
						flexio_dev_cq_arm(dtctx, arm_q->rq_cq_ctx.cq_idx, arm_q->rq_cq_ctx.cq_number);
					}
					flexio_dev_thread_reschedule();
					return;
				}
			}
		}
	}

	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	for (uint32_t arm_t = 0; arm_t < tenants_num; arm_t++) {
		struct flexio_dpa_dev_queue *arm_q = &this_tenant_base[arm_t];
		flexio_dev_cq_arm(dtctx, arm_q->rq_cq_ctx.cq_idx, arm_q->rq_cq_ctx.cq_number);
	}
	__atomic_store_n(&offload_info[i].status, EU_OFF, __ATOMIC_RELEASE);
	flexio_dev_thread_reschedule();
}
