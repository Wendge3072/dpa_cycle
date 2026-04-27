#ifndef __FLEXIO_PP_DEV_UTILS_H__
#define __FLEXIO_PP_DEV_UTILS_H__

#include "com_dev.h"
// #include "../../common/dev/com_dev.h"
#include <libflexio-dev/flexio_dev_err.h>
#include <libflexio-dev/flexio_dev_queue_access.h>
#include <libflexio-dev/flexio_dev_debug.h>
#include <libflexio-libc/string.h>
#include <stddef.h>
#include <dpaintrin.h>
/* Shared header file for packet processor sample */
#include "../flexio_pp_com.h"

/*
0 表示使用调度器分配的队列 ✅
1 表示使用线程私有队列 ❌
*/
#define WORKER_TX_USE_PRIVATE_SQ 0

/*
worker 线程每 reschedule 汇报数据包平均处理cycle的开关
*/
#define WORKER_QUEUE_CYCLE_REPORT 0

/*
sch 线程每秒钟汇报租户 cycle 使用量的开关
*/
#define SCH_CYCLE_USAGE_REPORT 1

/* 
sch 线程每秒钟汇报 设置资源预算 开销的开关
*/
#define SCH_ROLLOVER_COST_REPORT 1

/* 
sch 线程每秒钟汇报每调度周期“资源检查”循环迭代次数的开关
*/
#define SCH_LOOP_ITER_REPORT 0

/*
sch 线程每秒钟汇报租户实际主导资源使用比例 D 的开关
*/
#define SCH_DRF_D_REPORT 1

/*
sch 线程 rollover 模式切换开关 - 是否切换到 work-conserving 模式
*/
#define SCH_ROLLOVER_WORK_CONSERVING 1

#define assert_debug 0

#define DEFAULT_LINK_BANDWIDTH_BPS 80000000000ULL
#define MAX_CYCLE_PERCENTAGE 7815
#define MAX_CYCLE_TOTAL 10000
#define WORKER_BATCH_SIZE 1048576UL
#define WORKER_QUEUE_BURST_SIZE (128)
#define SCHED_PERIOD_CYCLES (DPA_FREQ_HZ / 1000)
#define WC_BUDGET_CAP_NUM 2
#define WC_BUDGET_CAP_DEN 1
#define DRF_SHIFT 20
#define DRF_CAP_EXTRA_Q20 \
	(WC_BUDGET_CAP_NUM > WC_BUDGET_CAP_DEN ? \
	 ((((uint64_t)WC_BUDGET_CAP_NUM - WC_BUDGET_CAP_DEN) << DRF_SHIFT) / WC_BUDGET_CAP_DEN) : 0)

#define TENANT_RESTRICT_NONE 0
#define TENANT_RESTRICT_CYCLE 1
#define TENANT_RESTRICT_BW 2

static uint32_t cycle_weights[MAX_TENANT_NUM] = {60, 40};
// static uint32_t bandwidth_weights[MAX_TENANT_NUM] = {80, 10}; 
static uint32_t bandwidth_weights[MAX_TENANT_NUM] = {40, 60};

struct flexio_dpa_dev_queue {
	/* lkey - local memory key */
	uint32_t sq_lkey;
	uint32_t rq_lkey;
	cq_ctx_t rq_cq_ctx;     /* RQ CQ */
	rq_ctx_t rq_ctx;        /* RQ */
	sq_ctx_t sq_ctx;        /* SQ */
	cq_ctx_t sq_cq_ctx;     /* SQ CQ */
	dt_ctx_t dt_ctx;        /* SQ Data ring */
};

/* The structure of the sample DPA application contains global data that the application uses */
struct dpa_thread_context {
	/* Packet count - used for debug message */
	uint64_t packets_count;
	int buffer_location;
	uint32_t window_id;
	uint32_t idx;
	// NVMe related
	flexio_uintptr_t host_buffer;
	flexio_uintptr_t result;
	uint8_t restrict_probe_shadow[WORKER_QUEUES_PER_THREAD];
#if WORKER_QUEUE_CYCLE_REPORT
	size_t queue_cycle_sum[WORKER_QUEUES_PER_THREAD];
	size_t queue_pkt_count[WORKER_QUEUES_PER_THREAD];
#endif
	struct flexio_dpa_dev_queue queue;
};

/* The structure of the sample DPA application contains global data that the application uses */
struct dpa_sche_context {
	/* Packet count - used for debug message */
	uint64_t packets_count;
	int buffer_location;
	uint32_t window_id;
	uint32_t idx;
	struct flexio_dpa_dev_queue queues[MAX_SCHEDULER_QUEUES];
	size_t tenant_cycle_target[MAX_TENANT_NUM];
	size_t tenant_cycle_consumed[MAX_TENANT_NUM];
	size_t tenant_bw_target[MAX_TENANT_NUM];
	size_t tenant_bw_consumed[MAX_TENANT_NUM];
	size_t tenant_cycle_budget[MAX_TENANT_NUM];
	size_t tenant_cycle_budget_cap[MAX_TENANT_NUM];
	size_t tenant_bw_budget[MAX_TENANT_NUM];
	size_t tenant_bw_budget_cap[MAX_TENANT_NUM];
	uint8_t restrict_tenant[MAX_TENANT_NUM];
#if SCH_CYCLE_USAGE_REPORT
	size_t tenant_cycle_report_used[MAX_TENANT_NUM];
#endif
#if SCH_LOOP_ITER_REPORT
	size_t sched_loop_current;
	size_t sched_loop_report_periods;
	size_t sched_loop_report_total;
#endif
#if SCH_ROLLOVER_COST_REPORT
	size_t rollover_cost_report_periods;
	size_t rollover_cost_report_total_cycles;
#endif
#if SCH_DRF_D_REPORT
	size_t tenant_d_report_periods[MAX_TENANT_NUM];
	size_t tenant_d_report_total_q20[MAX_TENANT_NUM];
#endif
};

typedef uint8_t eu_status;

enum {
	EU_OFF  = 0,
    EU_FREE = 1,
    EU_HANG = 2,
};

struct offload_dispatch_info {
	struct flexio_dpa_dev_queue *assigned_queues[WORKER_QUEUES_PER_THREAD];
	struct dpa_sche_context *sch_ctx;
	uint32_t wakeup_cq_num;
	eu_status status;
};

extern struct dpa_thread_context dpa_thds_ctx[190];
extern struct dpa_sche_context dpa_schs_ctx[32];
extern struct offload_dispatch_info offload_info[190];

void spin_on_status(uint16_t thd_id, eu_status expected_status);
void sch_ctx_init(struct flexio_dev_thread_ctx *dtctx,
             struct host2dev_packet_processor_data_sch *data_from_host);

static inline __attribute__((always_inline)) uint32_t
pp_get_packet_size(struct flexio_dpa_dev_queue *rq_queue)
{
	return be32_to_cpu((volatile __be32)rq_queue->rq_cq_ctx.cqe->byte_cnt);
}

static inline __attribute__((always_inline)) uint32_t
pp_queue(struct flexio_dev_thread_ctx *dtctx,
	 struct dpa_sche_context *sch_ctx,
	 uint32_t tenant_id,
	 struct flexio_dpa_dev_queue *rq_queue,
	 sq_ctx_t *tx_sq_ctx,
	 uint32_t tx_sq_number)
{
	register cq_ctx_t *rq_cq_ctx = &(rq_queue->rq_cq_ctx);
	register rq_ctx_t *rq_ctx = &(rq_queue->rq_ctx);
	register struct flexio_dev_wqe_rcv_data_seg *rwqe;
	register union flexio_dev_sqe_seg *swqe;
	register uint32_t rq_wqe_idx;
	register uint32_t data_sz;
	register char *rq_data;

	(void)sch_ctx;
	(void)tenant_id;

	rq_wqe_idx = be16_to_cpu((volatile __be16)rq_cq_ctx->cqe->wqe_counter);
	// data_sz = pp_get_packet_size(rq_queue);
	data_sz = be32_to_cpu((volatile __be32)rq_queue->rq_cq_ctx.cqe->byte_cnt);
	rwqe = &(rq_ctx->rq_ring[rq_wqe_idx & RQ_IDX_MASK]);
	rq_data = (void *)be64_to_cpu((volatile __be64)rwqe->addr);

	swap_mac(rq_data);

	swqe = &(tx_sq_ctx->sq_ring[(tx_sq_ctx->sq_wqe_seg_idx + 2) & SQ_IDX_MASK]);
	tx_sq_ctx->sq_wqe_seg_idx += 4;
	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, rq_queue->rq_lkey, (uint64_t)rq_data);

	__dpa_thread_memory_writeback();
	flexio_dev_qp_sq_ring_db(dtctx, ++tx_sq_ctx->sq_pi, tx_sq_number);
	flexio_dev_dbr_rq_inc_pi(rq_ctx->rq_dbr);
	com_step_cq(rq_cq_ctx);

	return data_sz;
}

flexio_dev_rpc_handler_t thd_ctx_init;
__dpa_rpc__ uint64_t thd_ctx_init(uint64_t data);

#endif /* __FLEXIO_PP_DEV_UTILS_H__ */
