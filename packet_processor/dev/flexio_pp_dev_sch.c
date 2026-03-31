#include "flexio_pp_dev_utils.h"

#define report_cycle_usage 1

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
	size_t threads_num_per_scheduler = data_from_host->threads_num_per_scheduler;
	size_t tenants_num = data_from_host->tenants_num;
	scheduler_num = data_from_host->scheduler_num;
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
	/* Initialize per-tenant cycle budgeting for the scheduler. */
	uint32_t sum_weight = 0;
	if (tenants_num > 0) {
		for (uint32_t t = 0; t < tenants_num; t++) {
			sum_weight += cycle_weights[t];
			dpa_schs_ctx[i].tenant_cycle_used[t] = 0;
		}
	}

	/* 1ms period = 1,800,000 cycles at 1.8GHz. Base budget = 85% = 1,530,000
	* cycles per core. */
	size_t base_cycle_budget = SCHED_PERIOD_CYCLES_BUDGET * threads_num_per_scheduler;
	// size_t base_cycle_budget = 1530000;
	if (tenants_num > 0 && sum_weight > 0) {
		for (uint32_t t = 0; t < tenants_num; t++) {
			dpa_schs_ctx[i].tenant_cycle_target[t] = base_cycle_budget * cycle_weights[t] / sum_weight;
			flexio_dev_print("scheduler %d, tenant %d cycle target: %ld\n", i, t, dpa_schs_ctx[i].tenant_cycle_target[t]);
		}
	} else {
		for (uint32_t t = 0; t < tenants_num; t++) {
			dpa_schs_ctx[i].tenant_cycle_target[t] = base_cycle_budget / tenants_num;
			flexio_dev_print("scheduler %d, tenant %d cycle target: %ld\n", i, t, dpa_schs_ctx[i].tenant_cycle_target[t]);
		}
	}
	for (uint32_t j = 0; j < data_from_host->num_queues; j++) {
		uint32_t thd_id = i * data_from_host->num_queues + j;
		for (uint32_t t = 0; t < tenants_num; t++) {
			__atomic_store_n(&offload_info[thd_id].busy_cycle[t], 0, __ATOMIC_RELAXED);
		}
	}
	// dpa_schs_ctx[i].rq_ctx.rqd_dpa_addr = data_from_host->queues[j].rq_transf.wqd_daddr;
	// dpa_schs_ctx[i].sq_ctx.sqd_dpa_addr = data_from_host->queues[j].sq_transf.wqd_daddr;
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
// 只swap mac 需要 1400 cycle 左右+-10
// static void forward_packet(struct flexio_dev_thread_ctx *dtctx,
//                            struct flexio_dpa_dev_queue *tenant,
//                            uint32_t *data_sz, size_t *cycles_inside) {

//   /* RX packet handling variables */
//   struct flexio_dev_wqe_rcv_data_seg *rwqe;
//   /* RQ WQE index */
//   uint32_t rq_wqe_idx;
//   /* Pointer to RQ data */
//   char *rq_data;
//   /* TX packet handling variables */
//   union flexio_dev_sqe_seg *swqe;
//   /* Extract relevant data from the CQE */

//   rq_wqe_idx = be16_to_cpu((volatile __be16)tenant->rq_cq_ctx.cqe->wqe_counter);
//   *data_sz = be32_to_cpu((volatile __be32)tenant->rq_cq_ctx.cqe->byte_cnt);

//   /* Get the RQ WQE pointed to by the CQE */
//   rwqe = &(tenant->rq_ctx.rq_ring[rq_wqe_idx & RQ_IDX_MASK]);

//   /* Extract data (whole packet) pointed to by the RQ WQE */
//   rq_data = (void *)be64_to_cpu((volatile __be64)rwqe->addr);

//   swap_mac(rq_data);

//   swqe = &(tenant->sq_ctx
//                .sq_ring[(tenant->sq_ctx.sq_wqe_seg_idx + 2) & SQ_IDX_MASK]);
//   tenant->sq_ctx.sq_wqe_seg_idx += 4;
//   flexio_dev_swqe_seg_mem_ptr_data_set(swqe, *data_sz, tenant->rq_lkey,
//                                        (uint64_t)rq_data);
//   /* Ring DB */
//   __dpa_thread_memory_writeback();
//   flexio_dev_qp_sq_ring_db(dtctx, ++tenant->sq_ctx.sq_pi,
//                            tenant->sq_ctx.sq_number);
//   // Free the RQ WQE after processing the packet.
//   *cycles_inside = __dpa_thread_cycles();
//   flexio_dev_dbr_rq_inc_pi(tenant->rq_ctx.rq_dbr);
//   com_step_cq(&(tenant->rq_cq_ctx));
//   *cycles_inside = __dpa_thread_cycles() - *cycles_inside;
// }

flexio_dev_event_handler_t flexio_scheduler_handle;
__dpa_global__ void flexio_scheduler_handle(uint64_t thread_arg) {
	struct host2dev_packet_processor_data_sch *data_from_host = (void *)thread_arg;
	struct flexio_dev_thread_ctx *dtctx;
	int i = data_from_host->sch_id;
	size_t tenants_num = data_from_host->tenants_num;
	struct dpa_sche_context *this_sch_ctx = &(dpa_schs_ctx[i]);

	flexio_dev_get_thread_ctx(&dtctx);

	if (!data_from_host->not_first_run) {
		sch_ctx_init(dtctx, data_from_host);
		// for (uint32_t j = 0; j < data_from_host->num_queues; j++){
		// 	this_sch_ctx->deficit[j] += weights[j] * quantum;
		// 	this_sch_ctx->n_packet[j] = q_packet;
		// }
		data_from_host->not_first_run = 1;
		flexio_dev_print("sch running ... \n");
	}

	for (uint32_t j = 0; j < data_from_host->num_queues; j++) {
		uint32_t thd_id = i * data_from_host->num_queues + j;
		if (__atomic_load_n(&offload_info[thd_id].status, __ATOMIC_ACQUIRE) ==EU_OFF &&
			dpa_thds_ctx[thd_id].rq_cq_ctx.cq_number) {
			flexio_dev_msix_send(dtctx, dpa_thds_ctx[thd_id].rq_cq_ctx.cq_number);
			flexio_dev_print("sch %d sent msix for thd %d, cq_num %u\n", i, thd_id,
							dpa_thds_ctx[thd_id].rq_cq_ctx.cq_number);
		}
	}

	size_t time_interval = 15;
	register size_t reschedule_cycle = __dpa_thread_cycles() + time_interval * DPA_FREQ_HZ;

	/* 1ms scheduling period */
	register size_t sched_period_cycles = SCHED_PERIOD_CYCLES;
	register size_t next_sched_cycle = __dpa_thread_cycles() + sched_period_cycles;

#if report_cycle_usage
	register size_t overload_budget = 0;
	register size_t report_interval_cycles = DPA_FREQ_HZ; /* 1s */
	register size_t next_report_cycle = __dpa_thread_cycles() + report_interval_cycles;
	register uint16_t reschedule = 0;
#endif

	struct flexio_dpa_dev_queue *this_tenant = NULL;
	for (uint32_t j = 0; j < data_from_host->num_queues; j++) {
		uint32_t thd_id = i * data_from_host->num_queues + j;
		this_tenant = &(this_sch_ctx->queues[j]);
		offload_info[thd_id].tenant = this_tenant;
		for (uint32_t t = 0; t < tenants_num; t++) {
			__atomic_store_n(&offload_info[thd_id].busy_cycle[t], 0, __ATOMIC_RELEASE);
			__atomic_store_n(&offload_info[thd_id].restrict_tenant[t], 0, __ATOMIC_RELEASE);
		}
		__atomic_store_n(&offload_info[thd_id].status, EU_HANG, __ATOMIC_RELEASE);
	}

	size_t now_cycle = __dpa_thread_cycles();
	while (now_cycle < reschedule_cycle) {
		now_cycle = __dpa_thread_cycles();

		for (uint32_t j = 0; j < data_from_host->num_queues; j++) {
			uint32_t thd_id = i * data_from_host->num_queues + j;
			eu_status current_status = __atomic_load_n(&offload_info[thd_id].status, __ATOMIC_ACQUIRE);

			if (current_status == EU_OFF) {
				flexio_dev_msix_send(dtctx, dpa_thds_ctx[thd_id].rq_cq_ctx.cq_number);
				__atomic_store_n(&offload_info[thd_id].status, EU_HANG, __ATOMIC_RELEASE);
			} else if (current_status == EU_FREE) {
				__atomic_store_n(&offload_info[thd_id].status, EU_HANG, __ATOMIC_RELEASE);
			}
		}

		/* Check cycle budgets aggregate per tenant */
		for (uint32_t t = 0; t < tenants_num; t++) {
			if (__atomic_load_n(&offload_info[i * data_from_host->num_queues].restrict_tenant[t], __ATOMIC_ACQUIRE)) continue;

			size_t current_used = 0;
			for (uint32_t j = 0; j < data_from_host->num_queues; j++) {
				uint32_t thd_id = i * data_from_host->num_queues + j;
				current_used += __atomic_load_n(&offload_info[thd_id].busy_cycle[t], __ATOMIC_ACQUIRE);
			}
			if (current_used >= this_sch_ctx->tenant_cycle_target[t]) {
#if report_cycle_usage
				if (t){
					overload_budget += (current_used - this_sch_ctx->tenant_cycle_target[t]);
					reschedule++;
				}
#endif
				for (uint32_t j = 0; j < data_from_host->num_queues; j++) {
					uint32_t thd_id = i * data_from_host->num_queues + j;
					__atomic_store_n(&offload_info[thd_id].restrict_tenant[t], 1, __ATOMIC_RELEASE);
				}
			}
		}

		if (now_cycle >= next_sched_cycle) {
// #if report_cycle_usage
			
// #endif
			for (uint32_t t = 0; t < tenants_num; t++) {
				size_t total_thd_cycles = 0;
				for (uint32_t j = 0; j < data_from_host->num_queues; j++) {
					uint32_t thd_id = i * data_from_host->num_queues + j;
					size_t thd_cycles = __atomic_exchange_n(&offload_info[thd_id].busy_cycle[t], 0, __ATOMIC_ACQ_REL);
#if report_cycle_usage
					total_thd_cycles += thd_cycles;
#endif
					__atomic_store_n(&offload_info[thd_id].restrict_tenant[t], 0, __ATOMIC_RELEASE);
				}
#if report_cycle_usage
				this_sch_ctx->tenant_cycle_used[t] += total_thd_cycles;
#endif
			}
			next_sched_cycle = now_cycle + sched_period_cycles;
		}

#if report_cycle_usage
		if (now_cycle >= next_report_cycle && tenants_num > 0) {
			for (uint32_t t = 0; t < tenants_num; t++) {
				// flexio_dev_print("sch %d 1s cycle report: tenant %u total_used %10zu\n", i, t, this_sch_ctx->tenant_cycle_used[t]/1000);
				this_sch_ctx->tenant_cycle_used[t] = 0;
			}
			flexio_dev_print("sch %d 1s cycle report: tenant1 overload_budget %10zu\n", i, overload_budget/reschedule);
			flexio_dev_print("sch %d 1s cycle report: reschedule %d\n", i, reschedule);
			overload_budget = 0,reschedule = 0;
			next_report_cycle = now_cycle + report_interval_cycles;
		}
#endif
	}

	__dpa_thread_memory_writeback();
	for (uint32_t j = 0; j < data_from_host->num_queues; j++) {
		struct flexio_dpa_dev_queue *this_tenant = &(this_sch_ctx->queues[j]);
		flexio_dev_cq_arm(dtctx, this_tenant->rq_cq_ctx.cq_idx, this_tenant->rq_cq_ctx.cq_number);
	}
	flexio_dev_thread_reschedule();
}

/*
        size_t time_interval = 15;
        register size_t reschedule_cycle = __dpa_thread_cycles() + time_interval
   * DPA_FREQ_HZ; register size_t cycle_interval_deficit = 1800000; // 1ms
        register size_t defict_update_cycle = __dpa_thread_cycles() +
   cycle_interval_deficit; register size_t pkt_lmt = 0; uint32_t data_sz = 0;
        size_t cycles_inside = 0;

        if (!data_from_host->not_first_run){
                for (uint32_t j = 0; j < data_from_host->num_queues; j++){
                        this_sch_ctx->thrput_deficit[j] += thrput_weights[j] *
   thrput_quantum;
                }
        }

        while (dtctx != NULL) {
                struct flexio_dpa_dev_queue* this_tenant = NULL;
                for (uint32_t j = 0; j < data_from_host->num_queues; j++){
                        this_tenant = &(this_sch_ctx->queues[j]);
                        pkt_lmt = 1 << 9; // queue size * 4, 512 packets once,
   512000 cycles approximately. while (
                                // this_sch_ctx->n_packet[j] > 0 &&
                                this_sch_ctx->thrput_deficit[j] > 0 &&
                                flexio_dev_cqe_get_owner(this_tenant->rq_cq_ctx.cqe)
   != this_tenant->rq_cq_ctx.cq_hw_owner_bit && pkt_lmt > 0) {
                                forward_packet(dtctx, this_tenant, &data_sz,
   &cycles_inside);

                                pkt_lmt--;
                                this_sch_ctx->thrput_deficit[j] -= data_sz;
                                // this_sch_ctx->n_packet[j] --;
                        }
                }
                size_t cycle_now = __dpa_thread_cycles();
                if (cycle_now >= defict_update_cycle){
                        defict_update_cycle = cycle_now +
   cycle_interval_deficit; for (uint32_t j = 0; j < data_from_host->num_queues;
   j++){ this_sch_ctx->thrput_deficit[j] = thrput_weights[j] * thrput_quantum;
                                this_sch_ctx->n_packet[j] = q_packet;
                        }
                }
                if (cycle_now >= reschedule_cycle) {
                        __dpa_thread_memory_writeback();
                        for (uint32_t j = 0; j < data_from_host->num_queues;
   j++){ struct flexio_dpa_dev_queue* this_tenant = &(this_sch_ctx->queues[j]);
                                flexio_dev_cq_arm(dtctx,
   this_tenant->rq_cq_ctx.cq_idx, this_tenant->rq_cq_ctx.cq_number);
                        }
                        flexio_dev_print("sch %d rescheduled, cycles: %zu\n", i,
   cycles_inside); flexio_dev_thread_reschedule();
                }
        }
*/
