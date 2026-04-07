#include "flexio_pp_dev_utils.h"

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

flexio_dev_event_handler_t flexio_scheduler_handle;
__dpa_global__ void flexio_scheduler_handle(uint64_t thread_arg) {
	struct host2dev_packet_processor_data_sch *data_from_host = (void *)thread_arg;
	struct flexio_dev_thread_ctx *dtctx;
	int i = data_from_host->sch_id;
	struct dpa_sche_context *this_sch_ctx = &(dpa_schs_ctx[i]);
	int first_run = !data_from_host->not_first_run;

	flexio_dev_get_thread_ctx(&dtctx);

	if (first_run) {
		sch_ctx_init(dtctx, data_from_host);
		data_from_host->not_first_run = 1;
	}

	for (uint32_t j = 0; j < data_from_host->num_queues; j++) {
		uint32_t thd_id = i * data_from_host->num_queues + j;
		offload_info[thd_id].tenant = &(this_sch_ctx->queues[j]);
		if (first_run) {
			__atomic_store_n(&offload_info[thd_id].status, EU_HANG, __ATOMIC_RELEASE);
		} else if (__atomic_load_n(&offload_info[thd_id].status, __ATOMIC_ACQUIRE) == EU_OFF &&
			dpa_thds_ctx[thd_id].rq_cq_ctx.cq_number) {
			flexio_dev_msix_send(dtctx, dpa_thds_ctx[thd_id].rq_cq_ctx.cq_number);
			__atomic_store_n(&offload_info[thd_id].status, EU_HANG, __ATOMIC_RELEASE);
		} else if (__atomic_load_n(&offload_info[thd_id].status, __ATOMIC_ACQUIRE) == EU_FREE) {
			__atomic_store_n(&offload_info[thd_id].status, EU_HANG, __ATOMIC_RELEASE);
		}
	}

	size_t time_interval = 15;
	register size_t reschedule_cycle = __dpa_thread_cycles() + time_interval * DPA_FREQ_HZ;

	size_t now_cycle = __dpa_thread_cycles();
	while (now_cycle < reschedule_cycle) {

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
		now_cycle = __dpa_thread_cycles();
	}

	__dpa_thread_memory_writeback();
	for (uint32_t j = 0; j < data_from_host->num_queues; j++) {
		struct flexio_dpa_dev_queue *this_tenant = &(this_sch_ctx->queues[j]);
		flexio_dev_cq_arm(dtctx, this_tenant->rq_cq_ctx.cq_idx, this_tenant->rq_cq_ctx.cq_number);
	}
	flexio_dev_thread_reschedule();
}
