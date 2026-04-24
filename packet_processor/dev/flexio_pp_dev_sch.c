#include "flexio_pp_dev_utils.h"

static inline void
sch_assign_workers(struct host2dev_packet_processor_data_sch *data_from_host,
		   struct dpa_sche_context *this_sch_ctx)
{
	int i = data_from_host->sch_id;

	for (uint32_t worker_idx = 0; worker_idx < data_from_host->threads_num_per_scheduler; worker_idx++) {
		uint32_t thd_id = i * data_from_host->threads_num_per_scheduler + worker_idx;
		uint32_t base_queue_idx = worker_idx * WORKER_QUEUES_PER_THREAD;
		struct offload_dispatch_info *thd_info = &offload_info[thd_id];

		__atomic_store_n(&thd_info->assigned_queues[0],
				 &(this_sch_ctx->queues[base_queue_idx]),
				 __ATOMIC_RELAXED);
		__atomic_store_n(&thd_info->assigned_queues[1],
				 &(this_sch_ctx->queues[base_queue_idx + 1]),
				 __ATOMIC_RELAXED);
		__atomic_store_n(&thd_info->sch_ctx, this_sch_ctx, __ATOMIC_RELAXED);
		thd_info->wakeup_cq_num = dpa_thds_ctx[thd_id].queue.rq_cq_ctx.cq_number;
	}
}

static inline void
sch_check_workers(struct flexio_dev_thread_ctx *dtctx,
		  int sch_id,
		  uint32_t threads_num_per_scheduler)
{
	for (uint32_t worker_idx = 0; worker_idx < threads_num_per_scheduler; worker_idx++) {
		uint32_t thd_id = sch_id * threads_num_per_scheduler + worker_idx;
		struct offload_dispatch_info *thd_info = &offload_info[thd_id];
		eu_status current_status = __atomic_load_n(&thd_info->status, __ATOMIC_ACQUIRE);

		if (!thd_info->wakeup_cq_num) {
			thd_info->wakeup_cq_num = dpa_thds_ctx[thd_id].queue.rq_cq_ctx.cq_number;
		}

		if (current_status == EU_OFF && thd_info->wakeup_cq_num) {
			flexio_dev_msix_send(dtctx, thd_info->wakeup_cq_num);
			__atomic_store_n(&thd_info->status, EU_HANG, __ATOMIC_RELEASE);
		} else if (current_status == EU_FREE) {
			__atomic_store_n(&thd_info->status, EU_HANG, __ATOMIC_RELEASE);
		}
	}
}

static inline void
sch_check_budget(struct dpa_sche_context *sch_ctx, uint32_t tenants_num)
{
	for (uint32_t t = 0; t < tenants_num; t++) {
		size_t current_cycle_used = 0;
		size_t current_bw_used = 0;

		if (__atomic_load_n(&sch_ctx->restrict_tenant[t], __ATOMIC_RELAXED)) {
			continue;
		}

		current_cycle_used = __atomic_load_n(&sch_ctx->tenant_cycle_consumed[t], __ATOMIC_RELAXED);
		current_bw_used = __atomic_load_n(&sch_ctx->tenant_bw_consumed[t], __ATOMIC_RELAXED);
		if (current_cycle_used >= sch_ctx->tenant_cycle_budget[t] ||
		    current_bw_used >= sch_ctx->tenant_bw_budget[t]) {
			__atomic_store_n(&sch_ctx->restrict_tenant[t], 1, __ATOMIC_RELAXED);
		}
	}
}

#if SCH_ROLLOVER_WORK_CONSERVING

static inline __attribute__((always_inline)) size_t
sch_budget_settle(size_t quota, size_t cap, size_t used, size_t *budget)
{
	size_t carried = *budget > used ? *budget - used : 0;
	size_t headroom = cap > quota ? cap - quota : 0;

	if (carried > headroom) {
		*budget = cap;
		return carried - headroom;
	}

	*budget = quota + carried;
	return 0;
}

static inline __attribute__((always_inline)) void
sch_budget_receive(size_t *budget, size_t cap, size_t shared_budget)
{
	size_t borrow = 0;

	if (!shared_budget || *budget >= cap) {
		return;
	}

	borrow = cap - *budget;
	if (borrow > shared_budget) {
		borrow = shared_budget;
	}
	*budget += borrow;
}

static inline void
sch_rollover_budget(struct dpa_sche_context *sch_ctx,
		    uint32_t tenants_num)
{
	size_t cycle_extra[2] = {0};
	size_t bw_extra[2] = {0};
	size_t cycle_used = 0;
	size_t bw_used = 0;

	if (tenants_num == 0) {
		return;
	}
	for(int t = 0; t < tenants_num; t++){
		cycle_used = __atomic_exchange_n(&sch_ctx->tenant_cycle_consumed[t], 0,
						__ATOMIC_RELAXED);
		bw_used = __atomic_exchange_n(&sch_ctx->tenant_bw_consumed[t], 0,
						__ATOMIC_RELAXED);
		cycle_extra[t] = sch_budget_settle(sch_ctx->tenant_cycle_target[t],
						sch_ctx->tenant_cycle_budget_cap[t],
						cycle_used,
						&sch_ctx->tenant_cycle_budget[t]);
		bw_extra[t] = sch_budget_settle(sch_ctx->tenant_bw_target[t],
						sch_ctx->tenant_bw_budget_cap[t],
						bw_used,
						&sch_ctx->tenant_bw_budget[t]);
	#if SCH_CYCLE_USAGE_REPORT
		sch_ctx->tenant_cycle_report_used[t] += cycle_used;
	#endif
	}

	for(int t = 0; t < tenants_num; t++){
		sch_budget_receive(&sch_ctx->tenant_cycle_budget[t],
				   sch_ctx->tenant_cycle_budget_cap[t],
				   cycle_extra[t]);
	}
	for(int t = 0; t < tenants_num; t++){
		sch_budget_receive(&sch_ctx->tenant_bw_budget[t],
				   sch_ctx->tenant_bw_budget_cap[t],
				   bw_extra[t]);

		__atomic_store_n(&sch_ctx->restrict_tenant[t], 0, __ATOMIC_RELAXED);
	}

}
#else
static inline void
sch_rollover_budget(struct dpa_sche_context *sch_ctx,
			  uint32_t tenants_num)
{
	for (uint32_t t = 0; t < tenants_num; t++) {
#if SCH_CYCLE_USAGE_REPORT
		size_t period_used =
#endif
		__atomic_exchange_n(&sch_ctx->tenant_cycle_consumed[t], 0, __ATOMIC_RELAXED);
		__atomic_exchange_n(&sch_ctx->tenant_bw_consumed[t], 0, __ATOMIC_RELAXED);
		sch_ctx->tenant_cycle_budget[t] = sch_ctx->tenant_cycle_target[t];
		sch_ctx->tenant_bw_budget[t] = sch_ctx->tenant_bw_target[t];
		__atomic_store_n(&sch_ctx->restrict_tenant[t], 0, __ATOMIC_RELAXED);
#if SCH_CYCLE_USAGE_REPORT
		sch_ctx->tenant_cycle_report_used[t] += period_used;
#endif
	}
}
#endif

// report functions 
#if SCH_CYCLE_USAGE_REPORT
static inline void
sch_report_cycle_usage(struct dpa_sche_context *sch_ctx,
		       int sch_id,
		       uint32_t tenants_num)
{
	for (uint32_t t = 0; t < tenants_num; t++) {
		flexio_dev_print("sch %d cycle report: tenant %u total_used %8zu\n",
				 sch_id, t, sch_ctx->tenant_cycle_report_used[t] / 1000);
		sch_ctx->tenant_cycle_report_used[t] = 0;
	}
}
#endif

#if SCH_LOOP_ITER_REPORT
static inline void
sch_report_loop_iters(struct dpa_sche_context *sch_ctx, int sch_id)
{
	size_t loop_periods = sch_ctx->sched_loop_report_periods;
	size_t loop_avg = loop_periods ?
				sch_ctx->sched_loop_report_total / loop_periods : 0;

	flexio_dev_print("sch %d loop report: periods=%zu avg=%zu\n",
				sch_id, loop_periods, loop_avg);
	sch_ctx->sched_loop_report_periods = 0;
	sch_ctx->sched_loop_report_total = 0;
}
#endif

#if SCH_ROLLOVER_COST_REPORT
static inline void
sch_report_rollover_cost(struct dpa_sche_context *sch_ctx, int sch_id)
{
	size_t rollover_periods = sch_ctx->rollover_cost_report_periods;
	size_t rollover_avg_cycles = rollover_periods ?
						sch_ctx->rollover_cost_report_total_cycles /
						rollover_periods : 0;

	flexio_dev_print("sch %d rollover report: wc=%u periods=%zu avg_cycles=%zu\n",
				sch_id, (unsigned)SCH_ROLLOVER_WORK_CONSERVING,
				rollover_periods, rollover_avg_cycles);
	sch_ctx->rollover_cost_report_periods = 0;
	sch_ctx->rollover_cost_report_total_cycles = 0;
}
#endif

flexio_dev_event_handler_t flexio_scheduler_handle;
__dpa_global__ void flexio_scheduler_handle(uint64_t thread_arg) {
	struct host2dev_packet_processor_data_sch *data_from_host = (void *)thread_arg;
	struct flexio_dev_thread_ctx *dtctx;
	register int i = data_from_host->sch_id;
	register uint32_t threads_num_per_scheduler = data_from_host->threads_num_per_scheduler;
	register uint32_t tenants_num = data_from_host->tenants_num > MAX_TENANT_NUM ?
				       MAX_TENANT_NUM : data_from_host->tenants_num;
	register uint32_t num_queues = data_from_host->num_queues;
	struct dpa_sche_context *this_sch_ctx = &(dpa_schs_ctx[i]);
	int first_run = !data_from_host->not_first_run;
	size_t time_interval = 15;
	register size_t reschedule_cycle = __dpa_thread_cycles() + time_interval * DPA_FREQ_HZ;
	register size_t next_sched_cycle = __dpa_thread_cycles() + SCHED_PERIOD_CYCLES;
#if SCH_CYCLE_USAGE_REPORT || SCH_LOOP_ITER_REPORT || SCH_ROLLOVER_COST_REPORT
	register size_t next_report_cycle = __dpa_thread_cycles() + DPA_FREQ_HZ;
#endif
	size_t now_cycle = 0;

	flexio_dev_get_thread_ctx(&dtctx);

	if (first_run) {
		sch_ctx_init(dtctx, data_from_host);
		sch_assign_workers(data_from_host, this_sch_ctx);
		data_from_host->not_first_run = 1;
	}

	sch_check_workers(dtctx, i, threads_num_per_scheduler);

	while (__dpa_thread_cycles() < reschedule_cycle) {
#if SCH_LOOP_ITER_REPORT
		this_sch_ctx->sched_loop_current++;
#endif
		sch_check_workers(dtctx, i, threads_num_per_scheduler);
		sch_check_budget(this_sch_ctx, tenants_num);

		now_cycle = __dpa_thread_cycles();
		if (now_cycle >= next_sched_cycle) {
#if SCH_LOOP_ITER_REPORT
			this_sch_ctx->sched_loop_report_total += this_sch_ctx->sched_loop_current;
			this_sch_ctx->sched_loop_current = 0;
			this_sch_ctx->sched_loop_report_periods++;
#endif
#if SCH_ROLLOVER_COST_REPORT
			size_t rollover_begin = __dpa_thread_cycles();
#endif
			sch_rollover_budget(this_sch_ctx, tenants_num);
#if SCH_ROLLOVER_COST_REPORT
			this_sch_ctx->rollover_cost_report_total_cycles +=
				__dpa_thread_cycles() - rollover_begin;
			this_sch_ctx->rollover_cost_report_periods++;
#endif
			next_sched_cycle = now_cycle + SCHED_PERIOD_CYCLES;
		}

#if SCH_CYCLE_USAGE_REPORT || SCH_LOOP_ITER_REPORT || SCH_ROLLOVER_COST_REPORT
		if (now_cycle >= next_report_cycle) {
#if SCH_CYCLE_USAGE_REPORT
			sch_report_cycle_usage(this_sch_ctx, i, tenants_num);
#endif
#if SCH_LOOP_ITER_REPORT
			sch_report_loop_iters(this_sch_ctx, i);
#endif
#if SCH_ROLLOVER_COST_REPORT
			sch_report_rollover_cost(this_sch_ctx, i);
#endif
			next_report_cycle = now_cycle + DPA_FREQ_HZ;
		}
#endif
	}

	__dpa_thread_memory_writeback();
	for (uint32_t j = 0; j < num_queues; j++) {
		struct flexio_dpa_dev_queue *this_tenant = &(this_sch_ctx->queues[j]);
		flexio_dev_cq_arm(dtctx, this_tenant->rq_cq_ctx.cq_idx, this_tenant->rq_cq_ctx.cq_number);
	}
	flexio_dev_thread_reschedule();
}
