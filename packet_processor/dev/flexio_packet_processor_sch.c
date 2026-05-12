#include "flexio_packet_processor_sch.h"

/* Initialize the app_ctx structure from the host data.
 *  data_from_host - pointer host2dev_packet_processor_data from host.
 */
static void sch_ctx_init(struct flexio_dev_thread_ctx *dtctx, struct host2dev_packet_processor_data2 *data_from_host)
{
	int i = data_from_host->sch_id;
	sch_ctx[i].packets_count = 0;
	sch_ctx[i].idx = i;
	sch_ctx[i].window_id = data_from_host->window_id;
	tenant_num_per_scheduler = data_from_host->tenant_num_per_scheduler;
	scheduler_num = data_from_host->scheduler_num;
	worker_threads_num = data_from_host->threads_num;
	for (uint32_t j = 0; j < data_from_host->num_queues; j++){
		sch_ctx[i].queues[j].sq_lkey = data_from_host->queues[j].sq_transf.wqd_mkey_id;
		sch_ctx[i].queues[j].rq_lkey = data_from_host->queues[j].rq_transf.wqd_mkey_id;
	
		/* Set context for RQ's CQ */
		com_cq_ctx_init(&(sch_ctx[i].queues[j].rq_cq_ctx),
				data_from_host->queues[j].rq_cq_transf.cq_num,
				data_from_host->queues[j].rq_cq_transf.log_cq_depth,
				data_from_host->queues[j].rq_cq_transf.cq_ring_daddr,
				data_from_host->queues[j].rq_cq_transf.cq_dbr_daddr);

		/* Set context for RQ */
		com_rq_ctx_init(&(sch_ctx[i].queues[j].rq_ctx),
				data_from_host->queues[j].rq_transf.wq_num,
				data_from_host->queues[j].rq_transf.wq_ring_daddr,
				data_from_host->queues[j].rq_transf.wq_dbr_daddr);

		/* Set context for SQ */
		com_sq_ctx_init(&(sch_ctx[i].queues[j].sq_ctx),
				data_from_host->queues[j].sq_transf.wq_num,
				data_from_host->queues[j].sq_transf.wq_ring_daddr);

		/* Set context for SQ's CQ */
		com_cq_ctx_init(&(sch_ctx[i].queues[j].sq_cq_ctx),
				data_from_host->queues[j].sq_cq_transf.cq_num,
				data_from_host->queues[j].sq_cq_transf.log_cq_depth,
				data_from_host->queues[j].sq_cq_transf.cq_ring_daddr,
				data_from_host->queues[j].sq_cq_transf.cq_dbr_daddr);

		/* Set context for data */
		com_dt_ctx_init(&(sch_ctx[i].queues[j].dt_ctx), data_from_host->queues[j].sq_transf.wqd_daddr);
	}


	for (uint32_t j = 0; j < data_from_host->num_queues; j++){
		for (uint64_t a = 0; a < (1UL << LOG_SQ_DEPTH); a++) {

			union flexio_dev_sqe_seg *swqe;
			swqe = get_next_sqe(&(sch_ctx[i].queues[j].sq_ctx), SQ_IDX_MASK);
			flexio_dev_swqe_seg_ctrl_set(swqe, a, sch_ctx[i].queues[j].sq_ctx.sq_number,
						     MLX5_CTRL_SEG_CE_CQE_ON_CQE_ERROR, FLEXIO_CTRL_SEG_SEND_EN);

			swqe = get_next_sqe(&(sch_ctx[i].queues[j].sq_ctx), SQ_IDX_MASK);
			flexio_dev_swqe_seg_eth_set(swqe, 0, 0, 0, NULL);

			swqe = get_next_sqe(&(sch_ctx[i].queues[j].sq_ctx), SQ_IDX_MASK);
			flexio_dev_swqe_seg_mem_ptr_data_set(swqe, 0, sch_ctx[i].queues[j].sq_lkey, 0);

			swqe = get_next_sqe(&(sch_ctx[i].queues[j].sq_ctx), SQ_IDX_MASK);
		}
		sch_ctx[i].queues[j].sq_ctx.sq_wqe_seg_idx = 0;
	}
	// sch_ctx[i].rq_ctx.rqd_dpa_addr = data_from_host->queues[j].rq_transf.wqd_daddr;
	// sch_ctx[i].sq_ctx.sqd_dpa_addr = data_from_host->queues[j].sq_transf.wqd_daddr;
	flexio_dev_status_t ret;
	ret = flexio_dev_window_config(dtctx, (uint16_t)sch_ctx[i].window_id, data_from_host->result_buffer_mkey_id);
	if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
		flexio_dev_print("failed to config rq window, thread %d\n", i);
	}
	// ret = flexio_dev_window_ptr_acquire(dtctx, (uint64_t)(data_from_host->result_buffer), &(result));
	// if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
	// 	flexio_dev_print("failed to acquire result ptr, thread %d\n", i);
	// }
}

/*
	dstmac: 0xa088c2320440
	bigend: 0xXX 0x40 0x04 0x32 0xc2 0x88 0xa0
*/ 
static void save_set_dstmac(char* packet, uint32_t mac_index)
{
	uint64_t dst_mac = *((uint64_t *)packet);
	*((uint64_t *)(packet + PACKET_METADATA_ORIG_DMAC_OFFSET)) = dst_mac;
	dst_mac = (dst_mac & mac_prefix_mask) | (zero_mac + ((uint64_t)mac_index << 40));
	*((uint64_t *)packet) = dst_mac;
}

// 只swap mac 需要 1400 cycle 左右+-10 
static void forward_packet(struct flexio_dev_thread_ctx *dtctx, struct flexio_dpa_dev_queue* tenant, 
	uint32_t worker_idx, uint32_t *data_sz)
{
	/* RX packet handling variables */
	struct flexio_dev_wqe_rcv_data_seg *rwqe;
	/* RQ WQE index */
	uint32_t rq_wqe_idx;
	/* Pointer to RQ data */
	char *rq_data;

	/* TX packet handling variables */
	union flexio_dev_sqe_seg *swqe;

	/* Extract relevant data from the CQE */
	rq_wqe_idx = be16_to_cpu((volatile __be16)tenant->rq_cq_ctx.cqe->wqe_counter);
	*data_sz = be32_to_cpu((volatile __be32)tenant->rq_cq_ctx.cqe->byte_cnt);

	/* Get the RQ WQE pointed to by the CQE */
	rwqe = &(tenant->rq_ctx.rq_ring[rq_wqe_idx & RQ_IDX_MASK]);

	/* Extract data (whole packet) pointed to by the RQ WQE */
	rq_data = (void *)be64_to_cpu((volatile __be64)rwqe->addr);

	uint32_t mac_index = tenant_num_per_scheduler * scheduler_num;
	mac_index += worker_idx;

	save_set_dstmac(rq_data, mac_index);
	// swap_mac(rq_data);

	swqe = &(tenant->sq_ctx.sq_ring[(tenant->sq_ctx.sq_wqe_seg_idx + 2) & SQ_IDX_MASK]);
	tenant->sq_ctx.sq_wqe_seg_idx += 4;
	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, *data_sz, tenant->sq_lkey, (uint64_t)rq_data);

	/* Ring DB */
	__dpa_thread_memory_writeback();
	flexio_dev_qp_sq_ring_db(dtctx, ++tenant->sq_ctx.sq_pi, tenant->sq_ctx.sq_number);
	flexio_dev_dbr_rq_inc_pi(tenant->rq_ctx.rq_dbr);
}

#define test_size_in_bytes 102400000
#define test_packet_count 100000

flexio_dev_event_handler_t flexio_scheduler_handle;
__dpa_global__ void flexio_scheduler_handle(uint64_t thread_arg)
{
	// uint64_t thread_start = __dpa_thread_cycles();
	struct host2dev_packet_processor_data2 *data_from_host = (void *)thread_arg;
	struct flexio_dev_thread_ctx *dtctx;
	int i = data_from_host->sch_id;
	// flexio_uintptr_t result = 0;
	struct dpa_sche_context* this_sch_ctx = &(sch_ctx[i]);

	flexio_dev_get_thread_ctx(&dtctx);

	if (!data_from_host->not_first_run) {
		flexio_dev_print("start schedular %d\n", i);
		sch_ctx_init(dtctx, data_from_host);
		data_from_host->not_first_run = 1;
	}

	size_t time_interval = 30;
	register size_t interrupt_cycle = time_interval * DPA_FREQ_HZ;
	interrupt_cycle += __dpa_thread_cycles();
	register size_t cycle_interval_defict = 1800000; // 1ms
	register size_t defict_update_cycle = __dpa_thread_cycles() + cycle_interval_defict;
	if (!data_from_host->not_first_run){
		for (uint32_t j = 0; j < data_from_host->num_queues; j++){
			this_sch_ctx->deficit[j] += weights[j] * quantum;
		}
	}
	uint32_t worker_count = worker_threads_num ? worker_threads_num : DEFAULT_WORKER_FORWARD_STRIPE;
	uint32_t target_worker = worker_count ? (i % worker_count) : 0;
	while (dtctx != NULL) {
		// int deficit_all_zero = 1;
		struct flexio_dpa_dev_queue* this_tenant = NULL;
		for (uint32_t j = 0; j < data_from_host->num_queues; j++){
			this_tenant = &(this_sch_ctx->queues[j]);
			// while (flexio_dev_cqe_get_owner(this_tenant->rq_cq_ctx.cqe) != this_tenant->rq_cq_ctx.cq_hw_owner_bit) {
			while (this_sch_ctx->deficit[j] > 0 && flexio_dev_cqe_get_owner(this_tenant->rq_cq_ctx.cqe) != this_tenant->rq_cq_ctx.cq_hw_owner_bit) {
			// while (this_sch_ctx->n_packet[j] > 0 && this_sch_ctx->deficit[j] > 0 && flexio_dev_cqe_get_owner(this_tenant->rq_cq_ctx.cqe) != this_tenant->rq_cq_ctx.cq_hw_owner_bit) {
				uint32_t data_sz = 0;
				uint32_t worker_idx = target_worker;
				forward_packet(dtctx, this_tenant, worker_idx, &data_sz);
				target_worker = (target_worker + 1) % worker_count;
				// flexio_dev_print("pkt size %d\n", data_sz);
				com_step_cq(&(this_tenant->rq_cq_ctx));
				this_sch_ctx->deficit[j] -= data_sz;
				// if(data_sz < 532) this_sch_ctx->n_packet[j] --;
				this_sch_ctx->n_packet[j] --;
			}
			// if(this_sch_ctx->deficit[j] > 0)
			// 	deficit_all_zero = 0;
		}
		size_t cycle_now = __dpa_thread_cycles();
		// if (deficit_all_zero){
		if (cycle_now >= defict_update_cycle){
			defict_update_cycle = cycle_now + cycle_interval_defict;
			for (uint32_t j = 0; j < data_from_host->num_queues; j++){
				uint32_t quantum_increase = weights[j] * quantum;
				this_sch_ctx->deficit[j] = quantum_increase;
				this_sch_ctx->n_packet[j] = q_packet;
				// this_sch_ctx->deficit[j] += quantum_increase;
				// if (this_sch_ctx->deficit[j] > quantum_increase * 2){
				// 	global_free_deficit += (this_sch_ctx->deficit[j] - quantum_increase * 2);
					// this_sch_ctx->deficit[j] = quantum_increase * 2;
				// }
				// // // else if(this_sch_ctx->deficit[j] < quantum_increase * 3 / 2){
				// else{
				// 	uint64_t borrow = min(quantum_increase * 2 - this_sch_ctx->deficit[j], global_free_deficit);
				// 	global_free_deficit -= borrow;
				// 	this_sch_ctx->deficit[j] += borrow;
				// }
			}
		}
		if (cycle_now >= interrupt_cycle) {
			__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
			flexio_dev_print("Scheduler Thread %d\n", i);
			for (uint32_t j = 0; j < data_from_host->num_queues; j++){
				struct flexio_dpa_dev_queue* this_tenant = &(this_sch_ctx->queues[j]);
				flexio_dev_cq_arm(dtctx, this_tenant->rq_cq_ctx.cq_idx, this_tenant->rq_cq_ctx.cq_number);
			}
			flexio_dev_thread_reschedule();
			return;
		}
	}

	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	for (uint32_t j = 0; j < data_from_host->num_queues; j++){
		struct flexio_dpa_dev_queue* this_tenant = &(this_sch_ctx->queues[j]);
		flexio_dev_cq_arm(dtctx, this_tenant->rq_cq_ctx.cq_idx, this_tenant->rq_cq_ctx.cq_number);
	}
	flexio_dev_thread_reschedule();
}
