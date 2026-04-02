#include "flexio_pp_dev_utils.h"

flexio_dev_event_handler_t flexio_pp_dev_32;
__dpa_global__ void flexio_pp_dev_32(uint64_t thread_arg)
{	
	struct host2dev_packet_processor_data_thd *data_from_host = (void *)thread_arg;
	int i = data_from_host->thd_id;

	/* Initialize status: EU_OFF -> EU_FREE to signal scheduler */
	if (__atomic_load_n(&offload_info[i].status, __ATOMIC_ACQUIRE) == EU_OFF){
		__atomic_store_n(&offload_info[i].status, EU_FREE, __ATOMIC_RELEASE);
	}

	struct flexio_dev_thread_ctx *dtctx;	
	struct dpa_thread_context* this_thd_ctx = &(dpa_thds_ctx[i]);
	flexio_dev_get_thread_ctx(&dtctx);
	com_step_cq(&(this_thd_ctx->rq_cq_ctx));

	if(!data_from_host->not_first_run){
		/* Wait for scheduler to start this thread (status == EU_HANG) */
		spin_on_status(i, EU_HANG);
		data_from_host->not_first_run = 1;
	}

	register size_t pkt_count = 0;
	register size_t cycle_delta;
	uint64_t stat_start_cycle = 0;
	uint64_t stat_busy_cycles = 0;
#if report_thread_pkt_usage
	register size_t t0_pkt_count = 0, t0_cycle_sum = 0, t0_result_sum = 0;
	register size_t t1_pkt_count = 0, t1_cycle_sum = 0, t1_result_sum = 0;
	register size_t tb_pkt_count = 0, tb_cycle_sum = 0, tb_result_sum = 0;
#endif
	uint32_t result;

	while (dtctx != NULL) {
		struct fwd_pkt pkt;
		int pkt_lmt = 64;
		while (fifo_pop(&this_thd_ctx->fifo, &pkt) == 0 && pkt_lmt > 0) {
			cycle_delta = __dpa_thread_cycles();
			if (pkt_count == 0) {
				stat_start_cycle = cycle_delta;
				stat_busy_cycles = 0;
			}
			uint32_t t_id = pkt.tnt_id;
			
			worker_pp_queue(dtctx, this_thd_ctx, i, &pkt, &result);
			
			cycle_delta = __dpa_thread_cycles() - cycle_delta;
			stat_busy_cycles += cycle_delta;
#if CHECK_BUDGET_AT_WORKER
			size_t current_used = __atomic_add_fetch(&offload_info[i].sch_ctx->busy_cycle[t_id], cycle_delta, __ATOMIC_RELAXED);
			if (current_used >= offload_info[i].sch_ctx->tenant_cycle_target[t_id]) {
				__atomic_store_n(&offload_info[i].sch_ctx->restrict_tenant[t_id], 1, __ATOMIC_RELEASE);
			}
#else
			__atomic_fetch_add(&offload_info[i].sch_ctx->busy_cycle[t_id], cycle_delta, __ATOMIC_RELAXED);
#endif

#if report_thread_pkt_usage
			if (t_id == 1) {
				t1_cycle_sum += cycle_delta;
				t1_pkt_count++;
			} else if (t_id == 0){
				t0_cycle_sum += cycle_delta;
				t0_pkt_count++;
			}
#endif
			pkt_count++;
			pkt_lmt--;
		}
		
		/* Periodic reschedule after 1000000 packets */
		if (pkt_count >= 1000000) {
			uint64_t stat_end_cycle = __dpa_thread_cycles();
			uint64_t total_cycles = stat_end_cycle - stat_start_cycle;
			uint64_t busy_cycles = stat_busy_cycles;
			uint64_t wait_cycles = (total_cycles > busy_cycles) ? (total_cycles - busy_cycles) : 0;
			uint64_t avg_total_cycles = total_cycles / pkt_count;
			uint64_t avg_busy_cycles = busy_cycles / pkt_count;
			uint64_t avg_wait_cycles = wait_cycles / pkt_count;
			flexio_dev_print("thd %d 1M pkt cycle report: total %llu busy %llu wait %llu avg(total/busy/wait) %llu/%llu/%llu\n",
				i,
				(unsigned long long)total_cycles,
				(unsigned long long)busy_cycles,
				(unsigned long long)wait_cycles,
				(unsigned long long)avg_total_cycles,
				(unsigned long long)avg_busy_cycles,
				(unsigned long long)avg_wait_cycles);

			pkt_count = 0;
#if report_thread_pkt_usage
			if (t0_pkt_count != 0 && t1_pkt_count != 0) {
				flexio_dev_print("tnt 0 pkt num %7zu, avg cycle per pkt %6zu\n", t0_pkt_count, t0_cycle_sum/t0_pkt_count);
				flexio_dev_print("tnt 1 pkt num %7zu, avg cycle per pkt %6zu\n", t1_pkt_count, t1_cycle_sum/t1_pkt_count);
			} else {
				flexio_dev_print("--- tnt running ---, t0_pkt_count: %zu, t1_pkt_count: %zu\n", t0_pkt_count, t1_pkt_count);
			}
#endif
			/* Set status to EU_OFF and reschedule, scheduler will restart us */
			__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
			flexio_dev_cq_arm(dtctx, this_thd_ctx->rq_cq_ctx.cq_idx, this_thd_ctx->rq_cq_ctx.cq_number);
			__atomic_store_n(&offload_info[i].status, EU_OFF, __ATOMIC_RELEASE);
			flexio_dev_thread_reschedule();
			return;
		}
	}

	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	flexio_dev_cq_arm(dtctx, dpa_thds_ctx[i].rq_cq_ctx.cq_idx, dpa_thds_ctx[i].rq_cq_ctx.cq_number);
	__atomic_store_n(&offload_info[i].status, EU_OFF, __ATOMIC_RELEASE);
	flexio_dev_thread_reschedule();
}
