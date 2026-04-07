#include "flexio_pp_dev_utils.h"

// threads and scheduler context
struct dpa_thread_context dpa_thds_ctx[190];
struct dpa_sche_context dpa_schs_ctx[32];

// Offload dispatch info for each thread, used for scheduler to dispatch packets to threads.
struct offload_dispatch_info offload_info[190];


void pp_queue(struct flexio_dev_thread_ctx *dtctx,
	      struct flexio_dpa_dev_queue *rq_queue,
	      struct flexio_dpa_dev_queue *sch_queue,
	      struct dpa_thread_context *thd_queue)
{
	/* RX packet handling variables */
	struct flexio_dev_wqe_rcv_data_seg *rwqe;
	/* RQ WQE index */
	uint32_t rq_wqe_idx;
	/* Pointer to RQ data */
	char *rq_data;

	/* TX packet handling variables */
	union flexio_dev_sqe_seg *swqe;
	sq_ctx_t *sch_sq_ctx = &sch_queue->sq_ctx;
	sq_ctx_t *thd_sq_ctx = &thd_queue->sq_ctx;
	sq_ctx_t *tx_sq_ctx = sch_sq_ctx;
	uint32_t sch_sq_number = sch_queue->sq_ctx.sq_number;
	uint32_t thd_sq_number = thd_queue->sq_ctx.sq_number;
	uint32_t tx_sq_number = sch_sq_number;

	/* Size of the data */
	uint32_t data_sz;

	/* Extract relevant data from the CQE */
	rq_wqe_idx = be16_to_cpu((volatile __be16)rq_queue->rq_cq_ctx.cqe->wqe_counter);
	data_sz = be32_to_cpu((volatile __be32)rq_queue->rq_cq_ctx.cqe->byte_cnt);

	/* Get the RQ WQE pointed to by the CQE */
	rwqe = &(rq_queue->rq_ctx.rq_ring[rq_wqe_idx & RQ_IDX_MASK]);

	/* Extract data (whole packet) pointed to by the RQ WQE */
	rq_data = (void *)be64_to_cpu((volatile __be64)rwqe->addr);

	swap_mac(rq_data);

	/* Toggle tx_sq_ctx/tx_sq_number here when comparing sch SQ vs worker SQ. */
	(void)sch_sq_ctx;
	(void)sch_sq_number;
	swqe = &(tx_sq_ctx->sq_ring[(tx_sq_ctx->sq_wqe_seg_idx + 2) & SQ_IDX_MASK]);
	tx_sq_ctx->sq_wqe_seg_idx += 4;
	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, rq_queue->rq_lkey, (uint64_t)rq_data);
	
	/* Ring DB */
	__dpa_thread_memory_writeback();
	flexio_dev_qp_sq_ring_db(dtctx, ++tx_sq_ctx->sq_pi, tx_sq_number);
	flexio_dev_dbr_rq_inc_pi(rq_queue->rq_ctx.rq_dbr);
	com_step_cq(&(rq_queue->rq_cq_ctx));
}

flexio_dev_rpc_handler_t thd_ctx_init;
__dpa_rpc__ uint64_t thd_ctx_init(uint64_t data)
{
	struct host2dev_packet_processor_data_thd *data_from_host = (struct host2dev_packet_processor_data_thd *)data;
	struct flexio_dev_thread_ctx *dtctx;
	flexio_dev_get_thread_ctx(&dtctx);
	int i = data_from_host->thd_id;
	dpa_thds_ctx[i].packets_count = 0;
	dpa_thds_ctx[i].sq_lkey = data_from_host->sq_transf.wqd_mkey_id;
	dpa_thds_ctx[i].rq_lkey = data_from_host->rq_transf.wqd_mkey_id;
	dpa_thds_ctx[i].window_id = data_from_host->window_id;
	dpa_thds_ctx[i].idx = i;
	/* Set context for RQ's CQ */
	com_cq_ctx_init(&(dpa_thds_ctx[i].rq_cq_ctx),
			data_from_host->rq_cq_transf.cq_num,
			data_from_host->rq_cq_transf.log_cq_depth,
			data_from_host->rq_cq_transf.cq_ring_daddr,
			data_from_host->rq_cq_transf.cq_dbr_daddr);

	/* Set context for RQ */
	com_rq_ctx_init(&(dpa_thds_ctx[i].rq_ctx),
			data_from_host->rq_transf.wq_num,
			data_from_host->rq_transf.wq_ring_daddr,
			data_from_host->rq_transf.wq_dbr_daddr);

	/* Set context for SQ */
	com_sq_ctx_init(&(dpa_thds_ctx[i].sq_ctx),
			data_from_host->sq_transf.wq_num,
			data_from_host->sq_transf.wq_ring_daddr);

	/* Set context for SQ's CQ */
	com_cq_ctx_init(&(dpa_thds_ctx[i].sq_cq_ctx),
			data_from_host->sq_cq_transf.cq_num,
			data_from_host->sq_cq_transf.log_cq_depth,
			data_from_host->sq_cq_transf.cq_ring_daddr,
			data_from_host->sq_cq_transf.cq_dbr_daddr);

	/* Set context for data */
	com_dt_ctx_init(&(dpa_thds_ctx[i].dt_ctx), data_from_host->sq_transf.wqd_daddr);


	for (uint64_t a = 0; a < (1UL << LOG_Q_DEPTH); a++) {

		union flexio_dev_sqe_seg *swqe;
        swqe = get_next_sqe(&(dpa_thds_ctx[i].sq_ctx), SQ_IDX_MASK);
		flexio_dev_swqe_seg_ctrl_set(swqe, a, dpa_thds_ctx[i].sq_ctx.sq_number,
				     MLX5_CTRL_SEG_CE_CQE_ON_CQE_ERROR, FLEXIO_CTRL_SEG_SEND_EN);

		swqe = get_next_sqe(&(dpa_thds_ctx[i].sq_ctx), SQ_IDX_MASK);
		flexio_dev_swqe_seg_eth_set(swqe, 0, 0, 0, NULL);

        swqe = get_next_sqe(&(dpa_thds_ctx[i].sq_ctx), SQ_IDX_MASK);
		flexio_dev_swqe_seg_mem_ptr_data_set(swqe, 0, dpa_thds_ctx[i].sq_lkey, 0);

        swqe = get_next_sqe(&(dpa_thds_ctx[i].sq_ctx), SQ_IDX_MASK);
	}
    dpa_thds_ctx[i].sq_ctx.sq_wqe_seg_idx = 0;
	dpa_thds_ctx[i].rq_ctx.rqd_dpa_addr = data_from_host->rq_transf.wqd_daddr;
	dpa_thds_ctx[i].sq_ctx.sqd_dpa_addr = data_from_host->sq_transf.wqd_daddr;
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

inline void spin_on_status(uint16_t thd_id, eu_status expected_status){
	eu_status status;
	do{
		status = __atomic_load_n(&offload_info[thd_id].status, __ATOMIC_ACQUIRE);
	}while (status != expected_status);
}
