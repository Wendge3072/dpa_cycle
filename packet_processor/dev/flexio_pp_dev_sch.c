#include "flexio_pp_dev_utils.h"

static void
sch_init_cycle_accounting(struct dpa_sche_context *sch_ctx,
			  struct host2dev_packet_processor_data_sch *data_from_host)
{
	int sch_id = data_from_host->sch_id;
	size_t tenants_num = data_from_host->tenants_num;
	size_t threads_num_per_scheduler = data_from_host->threads_num_per_scheduler;
	// size_t base_cycle_budget = SCHED_PERIOD_CYCLES * threads_num_per_scheduler * MAX_CYCLE_PERCENTAGE / MAX_CYCLE_TOTAL;
	size_t base_cycle_budget = SCHED_PERIOD_CYCLES * threads_num_per_scheduler * 7815 / 10000;
	uint32_t sum_weight = 0;

	if (tenants_num > MAX_TENANT_NUM) {
		tenants_num = MAX_TENANT_NUM;
	}

	for (uint32_t t = 0; t < MAX_TENANT_NUM; t++) {
		sch_ctx->tenant_cycle_target[t] = 0;
#if SCH_ROLLOVER_WORK_CONSERVING
		sch_ctx->tenant_cycle_budget[t] = 0;
		sch_ctx->tenant_cycle_budget_cap[t] = 0;
#endif
		__atomic_store_n(&sch_ctx->tenant_cycle_consumed[t], 0, __ATOMIC_RELAXED);
		__atomic_store_n(&sch_ctx->restrict_tenant[t], 0, __ATOMIC_RELAXED);
#if SCH_CYCLE_USAGE_REPORT
		sch_ctx->tenant_cycle_report_used[t] = 0;
#endif
	}

	for (uint32_t t = 0; t < tenants_num; t++) {
		sum_weight += cycle_weights[t];
	}

	if (tenants_num == 0) {
		return;
	}

	if (sum_weight > 0) {
		for (uint32_t t = 0; t < tenants_num; t++) {
			size_t tenant_quota = base_cycle_budget * cycle_weights[t] / sum_weight;
			sch_ctx->tenant_cycle_target[t] = tenant_quota;
#if SCH_ROLLOVER_WORK_CONSERVING
			sch_ctx->tenant_cycle_budget[t] = tenant_quota;
			sch_ctx->tenant_cycle_budget_cap[t] = sch_budget_cap(tenant_quota);
			flexio_dev_print("sch %d tenant %u cycle budget: quota=%zu budget=%zu cap=%zu period=%zu weight=%u\n",
					 sch_id, t, tenant_quota,
					 sch_ctx->tenant_cycle_budget[t],
					 sch_ctx->tenant_cycle_budget_cap[t],
					 (size_t)SCHED_PERIOD_CYCLES, cycle_weights[t]);
#else
			flexio_dev_print("sch %d tenant %u cycle budget: quota=%zu period=%zu weight=%u\n",
					 sch_id, t, tenant_quota,
					 (size_t)SCHED_PERIOD_CYCLES, cycle_weights[t]);
#endif
		}
		return;
	}

	for (uint32_t t = 0; t < tenants_num; t++) {
		size_t tenant_quota = base_cycle_budget / tenants_num;

		sch_ctx->tenant_cycle_target[t] = tenant_quota;
#if SCH_ROLLOVER_WORK_CONSERVING
			sch_ctx->tenant_cycle_budget[t] = tenant_quota;
			sch_ctx->tenant_cycle_budget_cap[t] = sch_budget_cap(tenant_quota);
			flexio_dev_print("sch %d tenant %u cycle budget: quota=%zu budget=%zu cap=%zu period=%zu weight=%u\n",
					 sch_id, t, tenant_quota,
					 sch_ctx->tenant_cycle_budget[t],
					 sch_ctx->tenant_cycle_budget_cap[t],
					 (size_t)SCHED_PERIOD_CYCLES, cycle_weights[t]);
#else
			flexio_dev_print("sch %d tenant %u cycle budget: quota=%zu period=%zu weight=%u\n",
					 sch_id, t, tenant_quota,
					 (size_t)SCHED_PERIOD_CYCLES, cycle_weights[t]);
#endif
	}
}

static void
sch_init_bandwidth_accounting(struct dpa_sche_context *sch_ctx,
			      struct host2dev_packet_processor_data_sch *data_from_host)
{
	int sch_id = data_from_host->sch_id;
	size_t tenants_num = data_from_host->tenants_num;
	size_t scheduler_num = data_from_host->scheduler_num ? data_from_host->scheduler_num : 1;
	size_t per_period_total_budget =
		(DEFAULT_LINK_BANDWIDTH_BPS / 8 / 1000) / scheduler_num;
	uint32_t sum_weight = 0;

	if (tenants_num > MAX_TENANT_NUM) {
		tenants_num = MAX_TENANT_NUM;
	}

	for (uint32_t t = 0; t < MAX_TENANT_NUM; t++) {
		sch_ctx->tenant_bw_target[t] = 0;
#if SCH_ROLLOVER_WORK_CONSERVING
		sch_ctx->tenant_bw_budget[t] = 0;
		sch_ctx->tenant_bw_budget_cap[t] = 0;
#endif
		__atomic_store_n(&sch_ctx->tenant_bw_consumed[t], 0, __ATOMIC_RELAXED);
	}

	for (uint32_t t = 0; t < tenants_num; t++) {
		sum_weight += bandwidth_weights[t];
	}

	if (tenants_num == 0) {
		return;
	}

	if (sum_weight > 0) {
		for (uint32_t t = 0; t < tenants_num; t++) {
			size_t tenant_budget = per_period_total_budget * bandwidth_weights[t] / sum_weight;

			sch_ctx->tenant_bw_target[t] = tenant_budget;
#if SCH_ROLLOVER_WORK_CONSERVING
			sch_ctx->tenant_bw_budget[t] = tenant_budget;
			sch_ctx->tenant_bw_budget_cap[t] = sch_budget_cap(tenant_budget);
			__atomic_store_n(&sch_ctx->tenant_bw_consumed[t], 0, __ATOMIC_RELAXED);
			flexio_dev_print("sch %d tenant %u bandwidth budget: quota=%zuB budget=%zuB cap=%zuB period=1ms weight=%u\n",
					 sch_id, t, tenant_budget,
					 sch_ctx->tenant_bw_budget[t],
					 sch_ctx->tenant_bw_budget_cap[t],
					 bandwidth_weights[t]);
#else
			flexio_dev_print("sch %d tenant %u bandwidth budget: quota=%zuB period=1ms weight=%u\n",
					 sch_id, t, tenant_budget,
					 bandwidth_weights[t]);
#endif
		}
		return;
	}

	for (uint32_t t = 0; t < tenants_num; t++) {
		size_t tenant_budget = per_period_total_budget / tenants_num;

		sch_ctx->tenant_bw_target[t] = tenant_budget;
#if SCH_ROLLOVER_WORK_CONSERVING
			sch_ctx->tenant_bw_budget[t] = tenant_budget;
			sch_ctx->tenant_bw_budget_cap[t] = sch_budget_cap(tenant_budget);
			__atomic_store_n(&sch_ctx->tenant_bw_consumed[t], 0, __ATOMIC_RELAXED);
			flexio_dev_print("sch %d tenant %u bandwidth budget: quota=%zuB budget=%zuB cap=%zuB period=1ms weight=%u\n",
					 sch_id, t, tenant_budget,
					 sch_ctx->tenant_bw_budget[t],
					 sch_ctx->tenant_bw_budget_cap[t],
					 bandwidth_weights[t]);
#else
			flexio_dev_print("sch %d tenant %u bandwidth budget: quota=%zuB period=1ms weight=%u\n",
					 sch_id, t, tenant_budget,
					 bandwidth_weights[t]);
#endif
	}
}

/* Initialize the app_ctx structure from the host data.
 *  data_from_host - pointer host2dev_packet_processor_data from host.
 */
static void
sch_ctx_init(struct flexio_dev_thread_ctx *dtctx,
             struct host2dev_packet_processor_data_sch *data_from_host) {
	int i = data_from_host->sch_id;
	dpa_schs_ctx[i].packets_count = 0;
	dpa_schs_ctx[i].idx = i;
	dpa_schs_ctx[i].window_id = data_from_host->window_id;
	sch_init_cycle_accounting(&(dpa_schs_ctx[i]), data_from_host);
	sch_init_bandwidth_accounting(&(dpa_schs_ctx[i]), data_from_host);
#if SCH_LOOP_ITER_REPORT
	dpa_schs_ctx[i].sched_loop_current = 0;
	dpa_schs_ctx[i].sched_loop_report_periods = 0;
	dpa_schs_ctx[i].sched_loop_report_total = 0;
#endif
#if SCH_ROLLOVER_COST_REPORT
	dpa_schs_ctx[i].rollover_cost_report_periods = 0;
	dpa_schs_ctx[i].rollover_cost_report_total_cycles = 0;
#endif
	for (uint32_t j = 0; j < data_from_host->num_queues; j++) {
		dpa_schs_ctx[i].queues[j].sq_lkey = data_from_host->queues[j].sq_transf.wqd_mkey_id;
		dpa_schs_ctx[i].queues[j].rq_lkey = data_from_host->queues[j].rq_transf.wqd_mkey_id;

		/* Set context for RQ's CQ */
		com_cq_ctx_init(&(dpa_schs_ctx[i].queues[j].rq_cq_ctx),
						data_from_host->queues[j].rq_cq_transf.cq_num,
						data_from_host->queues[j].rq_cq_transf.log_cq_depth,
						data_from_host->queues[j].rq_cq_transf.cq_ring_daddr,
						data_from_host->queues[j].rq_cq_transf.cq_dbr_daddr);

		/* Set context for RQ */
		com_rq_ctx_init(&(dpa_schs_ctx[i].queues[j].rq_ctx),
						data_from_host->queues[j].rq_transf.wq_num,
						data_from_host->queues[j].rq_transf.wq_ring_daddr,
						data_from_host->queues[j].rq_transf.wq_dbr_daddr);

		/* Set context for SQ */
		com_sq_ctx_init(&(dpa_schs_ctx[i].queues[j].sq_ctx),
						data_from_host->queues[j].sq_transf.wq_num,
						data_from_host->queues[j].sq_transf.wq_ring_daddr);

		/* Set context for SQ's CQ */
		com_cq_ctx_init(&(dpa_schs_ctx[i].queues[j].sq_cq_ctx),
						data_from_host->queues[j].sq_cq_transf.cq_num,
						data_from_host->queues[j].sq_cq_transf.log_cq_depth,
						data_from_host->queues[j].sq_cq_transf.cq_ring_daddr,
						data_from_host->queues[j].sq_cq_transf.cq_dbr_daddr);

		/* Set context for data */
		com_dt_ctx_init(&(dpa_schs_ctx[i].queues[j].dt_ctx),
						data_from_host->queues[j].sq_transf.wqd_daddr);
	}

	for (uint32_t j = 0; j < data_from_host->num_queues; j++) {
		for (uint64_t sq_pi = 0; sq_pi < (1UL << LOG_Q_DEPTH); sq_pi++) {

			union flexio_dev_sqe_seg *swqe;
			swqe = get_next_sqe(&(dpa_schs_ctx[i].queues[j].sq_ctx), SQ_IDX_MASK);
			flexio_dev_swqe_seg_ctrl_set(swqe, sq_pi, dpa_schs_ctx[i].queues[j].sq_ctx.sq_number, 
				MLX5_CTRL_SEG_CE_CQE_ON_CQE_ERROR, FLEXIO_CTRL_SEG_SEND_EN);

			swqe = get_next_sqe(&(dpa_schs_ctx[i].queues[j].sq_ctx), SQ_IDX_MASK);
			flexio_dev_swqe_seg_eth_set(swqe, 0, 0, 0, NULL);

			swqe = get_next_sqe(&(dpa_schs_ctx[i].queues[j].sq_ctx), SQ_IDX_MASK);
			flexio_dev_swqe_seg_mem_ptr_data_set(swqe, 0, dpa_schs_ctx[i].queues[j].sq_lkey, 0);

			swqe = get_next_sqe(&(dpa_schs_ctx[i].queues[j].sq_ctx), SQ_IDX_MASK);
		}
		dpa_schs_ctx[i].queues[j].sq_ctx.sq_wqe_seg_idx = 0;
	}
	flexio_dev_status_t ret;
	ret = flexio_dev_window_config(dtctx, (uint16_t)dpa_schs_ctx[i].window_id, data_from_host->result_buffer_mkey_id);
	if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
		flexio_dev_print("failed to config rq window, thread %d\n", i);
	}
	// ret = flexio_dev_window_ptr_acquire(dtctx,
	// (uint64_t)(data_from_host->result_buffer), &(result)); if (ret !=
	// FLEXIO_DEV_STATUS_SUCCESS) { 	flexio_dev_print("failed to acquire result
	// ptr, thread %d\n", i);
	// }
}

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

static inline size_t
sch_budget_cap(size_t target)
{
	size_t cap = target * WC_BUDGET_CAP_NUM / WC_BUDGET_CAP_DEN;

	return cap > target ? cap : target;
}

static inline void
sch_rollover_resource_budget(size_t resource_target[MAX_TENANT_NUM],
			     size_t resource_budget[MAX_TENANT_NUM],
			     size_t resource_budget_cap[MAX_TENANT_NUM],
			     size_t resource_consumed[MAX_TENANT_NUM],
			     size_t resource_report_used[MAX_TENANT_NUM],
			     uint32_t tenants_num)
{
	size_t shared_pool = 0;

	/* First pass: settle each tenant's last-period usage.
	 * resource_target: the guaranteed quota added every period.
	 * resource_budget: the currently usable budget for the new period.
	 * resource_budget_cap: the maximum budget one tenant may hold.
	 * shared_pool: extra idle budget above a tenant's cap, available to lend.
	 */
	for (uint32_t t = 0; t < tenants_num; t++) {
		/* Atomically take the previous period's usage and reset it for workers. */
		size_t period_used = __atomic_exchange_n(&resource_consumed[t], 0, __ATOMIC_RELAXED);
		/* quota is this tenant's guaranteed per-period share. */
		size_t quota = resource_target[t];
		/* cap bounds how much carried/borrowed budget this tenant can use. */
		size_t cap = resource_budget_cap[t];
		/* carried is the unused part of the old budget. Overspending carries 0. */
		size_t carried = resource_budget[t] > period_used ?
				 resource_budget[t] - period_used : 0;
		/* tenant_headroom is the part above quota that may be kept privately. */
		size_t tenant_headroom = cap > quota ? cap - quota : 0;

		if (carried > tenant_headroom) {
			/* Keep only up to cap; lend the extra idle budget to the pool. */
			shared_pool += carried - tenant_headroom;
			resource_budget[t] = cap;
		} else {
			/* Start the new period with quota plus the kept unused budget. */
			resource_budget[t] = quota + carried;
		}

		if (resource_report_used) {
			/* Optional accounting path used by the cycle usage report. */
			resource_report_used[t] += period_used;
		}
	}

	/* Second pass: lend pooled idle budget to tenants below their cap. */
	for (uint32_t t = 0; t < tenants_num && shared_pool; t++) {
		size_t cap = resource_budget_cap[t];
		size_t borrow = 0;

		if (resource_budget[t] >= cap) {
			continue;
		}

		/* Borrow only enough to reach cap, and never more than the pool has. */
		borrow = cap - resource_budget[t];
		if (borrow > shared_pool) {
			borrow = shared_pool;
		}
		resource_budget[t] += borrow;
		shared_pool -= borrow;
	}
}
#endif

#if SCH_ROLLOVER_WORK_CONSERVING
static inline void
sch_rollover_budget(struct dpa_sche_context *sch_ctx,
				    uint32_t tenants_num)
{
#if SCH_CYCLE_USAGE_REPORT
	sch_rollover_resource_budget(sch_ctx->tenant_cycle_target,
				     sch_ctx->tenant_cycle_budget,
				     sch_ctx->tenant_cycle_budget_cap,
				     sch_ctx->tenant_cycle_consumed,
				     sch_ctx->tenant_cycle_report_used,
				     tenants_num);
#else
	sch_rollover_resource_budget(sch_ctx->tenant_cycle_target,
				     sch_ctx->tenant_cycle_budget,
				     sch_ctx->tenant_cycle_budget_cap,
				     sch_ctx->tenant_cycle_consumed,
				     NULL,
				     tenants_num);
#endif
	sch_rollover_resource_budget(sch_ctx->tenant_bw_target,
				     sch_ctx->tenant_bw_budget,
				     sch_ctx->tenant_bw_budget_cap,
				     sch_ctx->tenant_bw_consumed,
				     NULL,
				     tenants_num);

	for (uint32_t t = 0; t < tenants_num; t++) {
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
