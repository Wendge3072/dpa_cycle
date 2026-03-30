#include "flexio_pp_dev_utils.h"

// threads and scheduler context
struct dpa_thread_context dpa_thds_ctx[190];
struct dpa_sche_context dpa_schs_ctx[32];

// Offload dispatch info for each thread, used for scheduler to dispatch packets to threads.
struct offload_dispatch_info offload_info[190];
size_t check[2];


int pp_queue(struct flexio_dev_thread_ctx *dtctx, struct dpa_thread_context* this_thd_ctx __unused, struct flexio_dpa_dev_queue* tenant, int thd_id)
{
	/* RX packet handling variables */
	struct flexio_dev_wqe_rcv_data_seg *rwqe;
	/* RQ WQE index */
	uint32_t rq_wqe_idx;
	/* Pointer to RQ data */
	char *rq_data;

	/* TX packet handling variables */
	union flexio_dev_sqe_seg *swqe;

	/* Size of the data */
	uint32_t data_sz;

	/* Extract relevant data from the CQE */
	rq_wqe_idx = be16_to_cpu((volatile __be16)tenant->rq_cq_ctx.cqe->wqe_counter);
	data_sz = be32_to_cpu((volatile __be32)tenant->rq_cq_ctx.cqe->byte_cnt);

	/* Get the RQ WQE pointed to by the CQE */
	rwqe = &(tenant->rq_ctx.rq_ring[rq_wqe_idx & RQ_IDX_MASK]);

	/* Extract data (whole packet) pointed to by the RQ WQE */
	rq_data = (void *)be64_to_cpu((volatile __be64)rwqe->addr);

	uint32_t tenant_indicator = *(uint32_t *)(rq_data + 46);
	int tenant_id = (tenant_indicator == 0) ? 0 : 1;

	if (__atomic_load_n(&offload_info[thd_id].restrict_tenant[tenant_id], __ATOMIC_RELAXED) == 1) {
		__dpa_thread_memory_writeback();
		flexio_dev_dbr_rq_inc_pi(tenant->rq_ctx.rq_dbr);
		// return -1;
		return tenant_id;
	}

	swap_mac(rq_data);
	volatile uint_test checksum = calculate_checksum_nrnd(rq_data, data_sz / 4, 8);

	swqe = &(tenant->sq_ctx.sq_ring[(tenant->sq_ctx.sq_wqe_seg_idx + 2) & SQ_IDX_MASK]);
	tenant->sq_ctx.sq_wqe_seg_idx += 4;
	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, tenant->rq_lkey, (uint64_t)rq_data);
	
	/* Ring DB */
	__dpa_thread_memory_writeback();
	flexio_dev_qp_sq_ring_db(dtctx, ++tenant->sq_ctx.sq_pi, tenant->sq_ctx.sq_number);
	flexio_dev_dbr_rq_inc_pi(tenant->rq_ctx.rq_dbr);

	return tenant_id;
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
	flexio_dev_print("thd %d rq_cq_num %u, dpa_thds_ctx: %p\n", i, data_from_host->rq_cq_transf.cq_num, (void*)&(dpa_thds_ctx[0]));
	// flexio_dev_print("thd %d rq_cq_num %u\n", i, dpa_thds_ctx[i].rq_cq_ctx.cq_number);

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

inline flexio_dev_status_t change_status(uint16_t thd_id, eu_status old_status, eu_status new_status){
	eu_status status = __atomic_load_n(&offload_info[thd_id].status, __ATOMIC_ACQUIRE);
	flexio_dev_print("thd: %d, status: %d, old_status: %d, new_status: %d\n", thd_id, status, old_status, new_status);
	if (status == old_status){
		__atomic_store_n(&offload_info[thd_id].status, new_status, __ATOMIC_RELEASE);
		return FLEXIO_DEV_STATUS_SUCCESS;
	}else{
		return FLEXIO_DEV_STATUS_FAILED;
	}
}

inline void spin_on_status(uint16_t thd_id, eu_status expected_status){
	eu_status status;
	do{
		status = __atomic_load_n(&offload_info[thd_id].status, __ATOMIC_ACQUIRE);
	}while (status != expected_status);
}

