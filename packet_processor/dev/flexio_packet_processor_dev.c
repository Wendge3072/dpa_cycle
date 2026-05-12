#include "flexio_packet_processor_sch.h"

/* Initialize the app_ctx structure from the host data.
 *  data_from_host - pointer host2dev_packet_processor_data from host.
 */
static void thd_ctx_init(struct flexio_dev_thread_ctx *dtctx, struct host2dev_packet_processor_data1 *data_from_host)
{
	int i = data_from_host->thd_id;
	thd_ctx[i].packets_count = 0;
	thd_ctx[i].sq_lkey = data_from_host->sq_transf.wqd_mkey_id;
	thd_ctx[i].rq_lkey = data_from_host->rq_transf.wqd_mkey_id;
	thd_ctx[i].window_id = data_from_host->window_id;
	thd_ctx[i].idx = i;

	/* Set context for RQ's CQ */
	com_cq_ctx_init(&(thd_ctx[i].rq_cq_ctx),
			data_from_host->rq_cq_transf.cq_num,
			data_from_host->rq_cq_transf.log_cq_depth,
			data_from_host->rq_cq_transf.cq_ring_daddr,
			data_from_host->rq_cq_transf.cq_dbr_daddr);

	/* Set context for RQ */
	com_rq_ctx_init(&(thd_ctx[i].rq_ctx),
			data_from_host->rq_transf.wq_num,
			data_from_host->rq_transf.wq_ring_daddr,
			data_from_host->rq_transf.wq_dbr_daddr);

	/* Set context for SQ */
	com_sq_ctx_init(&(thd_ctx[i].sq_ctx),
			data_from_host->sq_transf.wq_num,
			data_from_host->sq_transf.wq_ring_daddr);

	/* Set context for SQ's CQ */
	com_cq_ctx_init(&(thd_ctx[i].sq_cq_ctx),
			data_from_host->sq_cq_transf.cq_num,
			data_from_host->sq_cq_transf.log_cq_depth,
			data_from_host->sq_cq_transf.cq_ring_daddr,
			data_from_host->sq_cq_transf.cq_dbr_daddr);

	/* Set context for data */
	com_dt_ctx_init(&(thd_ctx[i].dt_ctx), data_from_host->sq_transf.wqd_daddr);


	for (uint64_t a = 0; a < (1UL << LOG_SQ_DEPTH); a++) {

		union flexio_dev_sqe_seg *swqe;
        swqe = get_next_sqe(&(thd_ctx[i].sq_ctx), SQ_IDX_MASK);
		flexio_dev_swqe_seg_ctrl_set(swqe, a, thd_ctx[i].sq_ctx.sq_number,
				     MLX5_CTRL_SEG_CE_CQE_ON_CQE_ERROR, FLEXIO_CTRL_SEG_SEND_EN);

		swqe = get_next_sqe(&(thd_ctx[i].sq_ctx), SQ_IDX_MASK);
		flexio_dev_swqe_seg_eth_set(swqe, 0, 0, 0, NULL);

        swqe = get_next_sqe(&(thd_ctx[i].sq_ctx), SQ_IDX_MASK);
		flexio_dev_swqe_seg_mem_ptr_data_set(swqe, 0, thd_ctx->sq_lkey, 0);

        swqe = get_next_sqe(&(thd_ctx[i].sq_ctx), SQ_IDX_MASK);
	}
    thd_ctx[i].sq_ctx.sq_wqe_seg_idx = 0;
	thd_ctx[i].rq_ctx.rqd_dpa_addr = data_from_host->rq_transf.wqd_daddr;
	thd_ctx[i].sq_ctx.sqd_dpa_addr = data_from_host->sq_transf.wqd_daddr;
	flexio_dev_status_t ret;
	ret = flexio_dev_window_config(dtctx, (uint16_t)thd_ctx[i].window_id, data_from_host->result_buffer_mkey_id);
	if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
		flexio_dev_print("failed to config rq window, thread %d\n", i);
	}
	// ret = flexio_dev_window_ptr_acquire(dtctx, (uint64_t)(data_from_host->result_buffer), &(result));
	// if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
	// 	flexio_dev_print("failed to acquire result ptr, thread %d\n", i);
	// }
	ret = flexio_dev_window_ptr_acquire(dtctx, (uint64_t)(data_from_host->host_buffer),  &(thd_ctx[i].host_buffer));
	if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
		flexio_dev_print("failed to acquire result ptr, thread %d\n", i);
	}
}

__attribute__((optimize("O0")))
static uint16_t calculate_checksum(uint16_t *data, int length) {
    uint32_t sum = 0;
    int i;

    // 逐块相加
    for (i = 0; i < length; i++) {
        sum += data[i];
        // 处理溢出
        if (sum & 0xFFFF0000) {
            sum &= 0xFFFF;
            sum++;
        }
    }
    // 取一的补码
    return ~sum;
}

// typedef uint32_t uint_test;
// __attribute__((optimize("O0")))
// inline uint_test calculate_checksum(uint_test *data, int length, int round) {
// 	uint_test sum = 0;
//     int i;
// 	while(round--){
// 		for (i = 0; i < length; i++) {
// 			sum += data[i];
// 			// sum &= 0xFFFFFFFF;
// 		}
// 	}

//     return ~sum;
// }

// static void swap_mac(char* packet)
// {
// 	uint64_t src_mac = *((uint64_t *)packet);
// 	uint64_t dst_mac = *((uint64_t *)(packet + 6));
// 	*((uint64_t *)packet) = dst_mac;
// 	*((uint64_t *)(packet + 6)) = (src_mac & 0x0000FFFFFFFFFFFF) | (dst_mac & 0xFFFF000000000000);
// }

static void get_swap_mac(char* packet)
{
	uint64_t dstmac_ori = get_packet_orig_dmac(packet);
	uint64_t srcmac_ori = *((uint64_t *)(packet + 6));
	// uint64_t dstmac_now = *((uint64_t *)(packet));
	*((uint64_t *)packet) = srcmac_ori;
	*((uint64_t *)(packet + 6)) = (dstmac_ori & 0x0000FFFFFFFFFFFF) | (srcmac_ori & 0xFFFF000000000000);
}

static void workload_direct_forward(char *rq_data, uint32_t *data_sz,
	struct dpa_thread_context *thd_ctx)
{
	(void)data_sz;
	(void)thd_ctx;
	get_swap_mac(rq_data);
}

static void workload_memcpy(char *rq_data, uint32_t *data_sz,
	struct dpa_thread_context *thd_ctx)
{
	uint32_t copy_sz;

	workload_direct_forward(rq_data, data_sz, thd_ctx);
	if (*data_sz <= ETH_HEADER_SIZE)
		return;

	copy_sz = *data_sz - ETH_HEADER_SIZE;
	memcpy((void *)(thd_ctx->host_buffer +
		       (thd_ctx->idx % NVME_QUEUE_ENTRY_NUM) * NVME_QUEUE_ENTRY_SIZE),
	       rq_data + ETH_HEADER_SIZE,
	       copy_sz > NVME_QUEUE_ENTRY_SIZE ? NVME_QUEUE_ENTRY_SIZE : copy_sz);
	thd_ctx->idx++;
	__dpa_thread_window_writeback();
}

static void workload_checksum(char *rq_data, uint32_t *data_sz,
	struct dpa_thread_context *thd_ctx)
{
	volatile uint16_t check_sum;

	(void)thd_ctx;
	workload_direct_forward(rq_data, data_sz, thd_ctx);
	check_sum = calculate_checksum((uint16_t *)rq_data, *data_sz / 2);
	(void)check_sum;
}

// 只swap mac 需要 1400 cycle 左右+-10 
static void process_packet(struct flexio_dev_thread_ctx *dtctx, struct dpa_thread_context* thd_ctx)
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
	rq_wqe_idx = be16_to_cpu((volatile __be16)thd_ctx->rq_cq_ctx.cqe->wqe_counter);
	data_sz = be32_to_cpu((volatile __be32)thd_ctx->rq_cq_ctx.cqe->byte_cnt);

	/* Get the RQ WQE pointed to by the CQE */
	rwqe = &(thd_ctx->rq_ctx.rq_ring[rq_wqe_idx & RQ_IDX_MASK]);

	/* Extract data (whole packet) pointed to by the RQ WQE */
	rq_data = (void *)be64_to_cpu((volatile __be64)rwqe->addr);

	uint32_t tenant_id = get_packet_tenant_id(rq_data);
	uint32_t workload = get_tenant_workload(tenant_id);
	if (workload == TENANT_WORKLOAD_MEMCPY)
		workload_memcpy(rq_data, &data_sz, thd_ctx);
	else if (workload == TENANT_WORKLOAD_CHECKSUM)
		workload_checksum(rq_data, &data_sz, thd_ctx);
	else
		workload_direct_forward(rq_data, &data_sz, thd_ctx);
	
	swqe = &(thd_ctx->sq_ctx.sq_ring[(thd_ctx->sq_ctx.sq_wqe_seg_idx + 2) & SQ_IDX_MASK]);
	thd_ctx->sq_ctx.sq_wqe_seg_idx += 4;
	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, thd_ctx->sq_lkey, (uint64_t)rq_data);

	/* Ring DB */
	__dpa_thread_memory_writeback();
	flexio_dev_qp_sq_ring_db(dtctx, ++thd_ctx->sq_ctx.sq_pi, thd_ctx->sq_ctx.sq_number);
	flexio_dev_dbr_rq_inc_pi(thd_ctx->rq_ctx.rq_dbr);
}


#define test_size_in_bytes 102400000
#define test_packet_count 100000

flexio_dev_event_handler_t flexio_pp_dev_1;
__dpa_global__ void flexio_pp_dev_1(uint64_t thread_arg)
{
	// uint64_t thread_start = __dpa_thread_cycles();
	struct host2dev_packet_processor_data1 *data_from_host = (void *)thread_arg;
	struct flexio_dev_thread_ctx *dtctx;
	int i = data_from_host->thd_id;
	// flexio_uintptr_t result = 0;
	struct dpa_thread_context* this_thd_ctx = &(thd_ctx[i]);

	flexio_dev_get_thread_ctx(&dtctx);

	if (!data_from_host->not_first_run) {
		flexio_dev_print("start thread %d\n", i);
		thd_ctx_init(dtctx, data_from_host);
		data_from_host->not_first_run = 1;
	}

	size_t time_interval = 10;
	register size_t cycle_interval = time_interval * DPA_FREQ_HZ;
	size_t start_cycle = __dpa_thread_cycles();
	while (dtctx != NULL) {
		while (flexio_dev_cqe_get_owner(this_thd_ctx->rq_cq_ctx.cqe) != this_thd_ctx->rq_cq_ctx.cq_hw_owner_bit) {
			process_packet(dtctx, this_thd_ctx);
			com_step_cq(&(this_thd_ctx->rq_cq_ctx));

			size_t acm_cycle = __dpa_thread_cycles() - start_cycle;
			if (acm_cycle >= cycle_interval) {
				// flexio_dev_print("Thread 1\n");
				flexio_dev_print("Thread %d\n", i);
				__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
				flexio_dev_cq_arm(dtctx, this_thd_ctx->rq_cq_ctx.cq_idx, this_thd_ctx->rq_cq_ctx.cq_number);
				flexio_dev_thread_reschedule();
				return;
			}
		}
	}

	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	flexio_dev_cq_arm(dtctx, thd_ctx[i].rq_cq_ctx.cq_idx, thd_ctx[i].rq_cq_ctx.cq_number);
	flexio_dev_thread_reschedule();
}


flexio_dev_event_handler_t flexio_pp_dev_2;
__dpa_global__ void flexio_pp_dev_2(uint64_t thread_arg)
{
	// uint64_t thread_start = __dpa_thread_cycles();
	struct host2dev_packet_processor_data1 *data_from_host = (void *)thread_arg;
	struct flexio_dev_thread_ctx *dtctx;
	int i = data_from_host->thd_id;
	// flexio_uintptr_t result = 0;
	struct dpa_thread_context* this_thd_ctx = &(thd_ctx[i]);

	flexio_dev_get_thread_ctx(&dtctx);

	if (!data_from_host->not_first_run) {
		flexio_dev_print("start thread %d\n", i);
		thd_ctx_init(dtctx, data_from_host);
		data_from_host->not_first_run = 1;
	}

	size_t time_interval = 10;
	register size_t cycle_interval = time_interval * DPA_FREQ_HZ;
	size_t start_cycle = __dpa_thread_cycles();
	while (dtctx != NULL) {
		while (flexio_dev_cqe_get_owner(this_thd_ctx->rq_cq_ctx.cqe) != this_thd_ctx->rq_cq_ctx.cq_hw_owner_bit) {
			// flexio_dev_print("Thread 2\n");
			process_packet(dtctx, this_thd_ctx);
			com_step_cq(&(this_thd_ctx->rq_cq_ctx));

			size_t acm_cycle = __dpa_thread_cycles() - start_cycle;
			if (acm_cycle >= cycle_interval) {
				// flexio_dev_print("Thread 2\n");
				flexio_dev_print("Thread %d\n", i);
				// flexio_dev_print("thd1 rqcq db: %d\n", *(thd_ctx[0].rq_cq_ctx.cq_dbr));
				__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
				flexio_dev_cq_arm(dtctx, this_thd_ctx->rq_cq_ctx.cq_idx, this_thd_ctx->rq_cq_ctx.cq_number);
				flexio_dev_thread_reschedule();
				return;
			}
		}
	}

	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	flexio_dev_cq_arm(dtctx, thd_ctx[i].rq_cq_ctx.cq_idx, thd_ctx[i].rq_cq_ctx.cq_number);
	flexio_dev_thread_reschedule();
}
