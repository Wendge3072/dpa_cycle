#include "flexio_pp_dev_utils.h"

// threads and scheduler context
struct dpa_thread_context dpa_thds_ctx[190];
struct dpa_sche_context dpa_schs_ctx[32];

// Offload dispatch info for each thread, used for scheduler to dispatch packets to threads.
struct offload_dispatch_info offload_info[190];
size_t check[2];

struct memory_pool dpa_mempools[32];

void mempool_init(struct memory_pool *pool, void *base_addr, uint32_t lkey)
{
	pool->base_addr = base_addr;
	pool->lkey = lkey;
	for (int i = 0; i < MEM_POOL_BITMAP_SIZE; i++) {
		pool->bitmap[i] = 0; // 0 = free, 1 = used
	}
}

void *mempool_alloc(struct memory_pool *pool)
{
	for (int i = 0; i < MEM_POOL_BITMAP_SIZE; i++) {
		uint32_t map = __atomic_load_n(&pool->bitmap[i], __ATOMIC_RELAXED);
		if (map != 0xFFFFFFFF) { // ~0U
			// Find the first free bit (0)
			int bit_idx = __builtin_ctz(~map);
			uint32_t mask = 1U << bit_idx;
			
			// Try to atomically set the bit
			uint32_t prev = __atomic_fetch_or(&pool->bitmap[i], mask, __ATOMIC_RELEASE);
			if ((prev & mask) == 0) {
				// Successfully claimed
				uint32_t slot_idx = i * 32 + bit_idx;
				return (void *)((char *)pool->base_addr + slot_idx * Q_DATA_ENTRY_BSIZE);
			}
			// Another thread claimed it concurrently, retry (though in SPSC with 1 worker dropping, rare collision)
			i--; // Retry this element
		}
	}
	return NULL;
}

void mempool_free(struct memory_pool *pool, void *ptr)
{
	uint64_t offset = (char *)ptr - (char *)pool->base_addr;
	uint32_t slot_idx = offset / Q_DATA_ENTRY_BSIZE;
	uint32_t i = slot_idx / 32;
	uint32_t bit_idx = slot_idx % 32;
	uint32_t mask = ~(1U << bit_idx);
	
	__atomic_fetch_and(&pool->bitmap[i], mask, __ATOMIC_RELEASE);
}

void fifo_init(struct sw_fifo *fifo)
{
	fifo->head = 0;
	fifo->tail = 0;
}

int fifo_push(struct sw_fifo *fifo, const struct fwd_pkt *pkt)
{
	uint32_t t = __atomic_load_n(&fifo->tail, __ATOMIC_RELAXED);
	uint32_t h = __atomic_load_n(&fifo->head, __ATOMIC_ACQUIRE);
	
	if (t - h == FIFO_QUEUE_SIZE) {
		return -1; // Full
	}
	
	// Copy data
	fifo->pkts[t % FIFO_QUEUE_SIZE] = *pkt;
	__atomic_store_n(&fifo->tail, t + 1, __ATOMIC_RELEASE);
	return 0;
}

int fifo_pop(struct sw_fifo *fifo, struct fwd_pkt *pkt)
{
	uint32_t h = __atomic_load_n(&fifo->head, __ATOMIC_RELAXED);
	uint32_t t = __atomic_load_n(&fifo->tail, __ATOMIC_ACQUIRE);
	
	if (h == t) {
		return -1; // Empty
	}
	
	*pkt = fifo->pkts[h % FIFO_QUEUE_SIZE];
	__atomic_store_n(&fifo->head, h + 1, __ATOMIC_RELEASE);
	return 0;
}

uint32_t fifo_count(struct sw_fifo *fifo)
{
	uint32_t h = __atomic_load_n(&fifo->head, __ATOMIC_ACQUIRE);
	uint32_t t = __atomic_load_n(&fifo->tail, __ATOMIC_ACQUIRE);
	uint32_t n = t - h;

	if (n > FIFO_QUEUE_SIZE) {
		n = FIFO_QUEUE_SIZE;
	}
	return n;
}

uint32_t mempool_count_free_slots(struct memory_pool *pool)
{
	uint32_t used = 0;

	for (int i = 0; i < MEM_POOL_BITMAP_SIZE; i++) {
		uint32_t map = __atomic_load_n(&pool->bitmap[i], __ATOMIC_ACQUIRE);
		used += (uint32_t)__builtin_popcount(map);
	}
	if (used > MEM_POOL_SIZE) {
		used = MEM_POOL_SIZE;
	}
	return MEM_POOL_SIZE - used;
}

int worker_pp_queue(struct flexio_dev_thread_ctx *dtctx, struct dpa_thread_context *this_thd_ctx,
		    int thd_id, const struct fwd_pkt *pkt, void **tx_inflight,
		    uint32_t *tx_t_id_inflight, uint32_t *result)
{
	uint32_t t_id = pkt->tnt_id;
	uint8_t restricted = __atomic_load_n(&offload_info[thd_id].sch_ctx->restrict_tenant[t_id], __ATOMIC_ACQUIRE);

	if (restricted) {
		mempool_free(&offload_info[thd_id].sch_ctx->queues[t_id].mempool, pkt->rq_data);
		__atomic_fetch_add(&offload_info[thd_id].sch_ctx->worker_drop_restricted[t_id], 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&offload_info[thd_id].sch_ctx->worker_free_slots[t_id], 1, __ATOMIC_RELAXED);
		return 1;
	}

	swap_mac(pkt->rq_data);

	union flexio_dev_sqe_seg *swqe;
	swqe = &(this_thd_ctx->sq_ctx.sq_ring[(this_thd_ctx->sq_ctx.sq_wqe_seg_idx + 2) & SQ_IDX_MASK]);
	this_thd_ctx->sq_ctx.sq_wqe_seg_idx += 4;
	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, pkt->data_sz, pkt->rq_lkey, (uint64_t)pkt->rq_data);

	__dpa_thread_memory_writeback();
	this_thd_ctx->sq_ctx.sq_pi++;
	flexio_dev_qp_sq_ring_db(dtctx, this_thd_ctx->sq_ctx.sq_pi, this_thd_ctx->sq_ctx.sq_number);

	/* Defer freeing previous packet to avoid freeing inflight memory. */
	uint32_t ring_idx = this_thd_ctx->sq_ctx.sq_pi & ((1UL << LOG_Q_DEPTH) - 1);
	if (tx_inflight[ring_idx] != NULL) {
		uint32_t prev_t_id = tx_t_id_inflight[ring_idx];
		mempool_free(&offload_info[thd_id].sch_ctx->queues[prev_t_id].mempool, tx_inflight[ring_idx]);
		__atomic_fetch_add(&offload_info[thd_id].sch_ctx->worker_free_slots[prev_t_id], 1, __ATOMIC_RELAXED);
	}
	tx_inflight[ring_idx] = pkt->rq_data;
	tx_t_id_inflight[ring_idx] = t_id;
	__atomic_fetch_add(&offload_info[thd_id].sch_ctx->worker_tx_submit[t_id], 1, __ATOMIC_RELAXED);
	*result = 0;

	return 0;
}



int pp_queue(struct flexio_dev_thread_ctx *dtctx, struct dpa_thread_context* this_thd_ctx, int thd_id, uint32_t *result)
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
	rq_wqe_idx = be16_to_cpu((volatile __be16)this_thd_ctx->rq_cq_ctx.cqe->wqe_counter);
	data_sz = be32_to_cpu((volatile __be32)this_thd_ctx->rq_cq_ctx.cqe->byte_cnt);

	/* Get the RQ WQE pointed to by the CQE */
	rwqe = &(this_thd_ctx->rq_ctx.rq_ring[rq_wqe_idx & RQ_IDX_MASK]);

	/* Extract data (whole packet) pointed to by the RQ WQE */
	rq_data = (void *)be64_to_cpu((volatile __be64)rwqe->addr);

	uint32_t tenant_indicator = *(uint32_t *)(rq_data + 46);
	int tenant_id = (tenant_indicator == 0) ? 0 : 1;

	get_swap_mac(rq_data);

	// if (tenant_id == 1) {
	// 	*result = calculate_checksum_nrnd((uint_test*)rq_data, data_sz / 4, 5);
	// } else {
	// 	*result = calculate_checksum_nrnd((uint_test*)rq_data, data_sz / 4, 5);
	// }

	swqe = &(this_thd_ctx->sq_ctx.sq_ring[(this_thd_ctx->sq_ctx.sq_wqe_seg_idx + 2) & SQ_IDX_MASK]);
	this_thd_ctx->sq_ctx.sq_wqe_seg_idx += 4;
	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, this_thd_ctx->rq_lkey, (uint64_t)rq_data);
	
	/* Ring DB */
	__dpa_thread_memory_writeback();
	flexio_dev_qp_sq_ring_db(dtctx, ++this_thd_ctx->sq_ctx.sq_pi, this_thd_ctx->sq_ctx.sq_number);
	flexio_dev_dbr_rq_inc_pi(this_thd_ctx->rq_ctx.rq_dbr);
	com_step_cq(&(this_thd_ctx->rq_cq_ctx));

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
	for (uint32_t k = 0; k < Q_DEPTH; k++) {
		dpa_thds_ctx[i].tx_inflight[k] = NULL;
		dpa_thds_ctx[i].tx_t_id_inflight[k] = 0;
	}
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
