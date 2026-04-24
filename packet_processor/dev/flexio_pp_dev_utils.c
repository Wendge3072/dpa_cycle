#include "flexio_pp_dev_utils.h"

// threads and scheduler context
struct dpa_thread_context dpa_thds_ctx[190];
struct dpa_sche_context dpa_schs_ctx[32];

// Offload dispatch info for each thread, used for scheduler to dispatch packets to threads.
struct offload_dispatch_info offload_info[190];

flexio_dev_rpc_handler_t thd_ctx_init;
__dpa_rpc__ uint64_t thd_ctx_init(uint64_t data)
{
	struct host2dev_packet_processor_data_thd *data_from_host = (struct host2dev_packet_processor_data_thd *)data;
	struct flexio_dev_thread_ctx *dtctx;
	flexio_dev_get_thread_ctx(&dtctx);
	int i = data_from_host->thd_id;
	struct flexio_dpa_dev_queue *queue_ctx = &(dpa_thds_ctx[i].queue);

	dpa_thds_ctx[i].packets_count = 0;
	dpa_thds_ctx[i].buffer_location = data_from_host->buffer_location;
	dpa_thds_ctx[i].window_id = data_from_host->window_id;
	dpa_thds_ctx[i].idx = i;
	queue_ctx->sq_lkey = data_from_host->sq_transf.wqd_mkey_id;
	queue_ctx->rq_lkey = data_from_host->rq_transf.wqd_mkey_id;

	/* Set context for RQ's CQ */
	com_cq_ctx_init(&(queue_ctx->rq_cq_ctx),
			data_from_host->rq_cq_transf.cq_num,
			data_from_host->rq_cq_transf.log_cq_depth,
			data_from_host->rq_cq_transf.cq_ring_daddr,
			data_from_host->rq_cq_transf.cq_dbr_daddr);

	/* Set context for RQ */
	com_rq_ctx_init(&(queue_ctx->rq_ctx),
			data_from_host->rq_transf.wq_num,
			data_from_host->rq_transf.wq_ring_daddr,
			data_from_host->rq_transf.wq_dbr_daddr);

	/* Set context for SQ */
	com_sq_ctx_init(&(queue_ctx->sq_ctx),
			data_from_host->sq_transf.wq_num,
			data_from_host->sq_transf.wq_ring_daddr);

	/* Set context for SQ's CQ */
	com_cq_ctx_init(&(queue_ctx->sq_cq_ctx),
			data_from_host->sq_cq_transf.cq_num,
			data_from_host->sq_cq_transf.log_cq_depth,
			data_from_host->sq_cq_transf.cq_ring_daddr,
			data_from_host->sq_cq_transf.cq_dbr_daddr);

	/* Set context for data */
	com_dt_ctx_init(&(queue_ctx->dt_ctx), data_from_host->sq_transf.wqd_daddr);

	for (uint64_t a = 0; a < (1UL << LOG_Q_DEPTH); a++) {
		union flexio_dev_sqe_seg *swqe;

		swqe = get_next_sqe(&(queue_ctx->sq_ctx), SQ_IDX_MASK);
		flexio_dev_swqe_seg_ctrl_set(swqe, a, queue_ctx->sq_ctx.sq_number,
				     MLX5_CTRL_SEG_CE_CQE_ON_CQE_ERROR,
				     FLEXIO_CTRL_SEG_SEND_EN);

		swqe = get_next_sqe(&(queue_ctx->sq_ctx), SQ_IDX_MASK);
		flexio_dev_swqe_seg_eth_set(swqe, 0, 0, 0, NULL);

		swqe = get_next_sqe(&(queue_ctx->sq_ctx), SQ_IDX_MASK);
		flexio_dev_swqe_seg_mem_ptr_data_set(swqe, 0, queue_ctx->sq_lkey, 0);

		swqe = get_next_sqe(&(queue_ctx->sq_ctx), SQ_IDX_MASK);
	}
	queue_ctx->sq_ctx.sq_wqe_seg_idx = 0;
	queue_ctx->rq_ctx.rqd_dpa_addr = data_from_host->rq_transf.wqd_daddr;
	queue_ctx->sq_ctx.sqd_dpa_addr = data_from_host->sq_transf.wqd_daddr;

	flexio_dev_status_t ret;
	ret = flexio_dev_window_config(dtctx, (uint16_t)dpa_thds_ctx[i].window_id, data_from_host->result_buffer_mkey_id);
	if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
		flexio_dev_print("failed to config rq window, thread %d\n", i);
	}
	// ret = flexio_dev_window_ptr_acquire(dtctx, (uint64_t)(data_from_host->result_buffer), &(result));
	// if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
	// 	flexio_dev_print("failed to acquire result ptr, thread %d\n", i);
	// }
	ret = flexio_dev_window_ptr_acquire(dtctx, (uint64_t)(data_from_host->host_buffer),  &(dpa_thds_ctx[i].host_buffer));
	if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
		flexio_dev_print("failed to acquire result ptr, thread %d\n", i);
	}
	return 0;
}

/* Initialize the app_ctx structure from the host data.
 *  data_from_host - pointer host2dev_packet_processor_data from host.
 */
void sch_ctx_init(struct flexio_dev_thread_ctx *dtctx,
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

inline void spin_on_status(uint16_t thd_id, eu_status expected_status){
	eu_status status;
	do{
		status = __atomic_load_n(&offload_info[thd_id].status, __ATOMIC_ACQUIRE);
	}while (status != expected_status);
}
