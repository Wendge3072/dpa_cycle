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
	/* lkey - local memory key */
	uint32_t sq_lkey;
	uint32_t rq_lkey;
	int buffer_location;
	uint32_t window_id;
	uint32_t idx;
	// NVMe related
	flexio_uintptr_t host_buffer;
	flexio_uintptr_t result;
	// Packet statistics for each round (1000000 packets per round)
	uint64_t packet_stats_round;

	cq_ctx_t rq_cq_ctx;     /* RQ CQ */
	rq_ctx_t rq_ctx;        /* RQ */
	sq_ctx_t sq_ctx;        /* SQ */
	cq_ctx_t sq_cq_ctx;     /* SQ CQ */
	dt_ctx_t dt_ctx;        /* SQ Data ring */
};

/* The structure of the sample DPA application contains global data that the application uses */
struct dpa_sche_context {
	/* Packet count - used for debug message */
	uint64_t packets_count;
	int buffer_location;
	uint32_t window_id;
	uint32_t idx;
	// Packet statistics for each round (1000000 packets per round)
	uint64_t packet_stats_round;
	struct flexio_dpa_dev_queue queues[MAX_SCHEDULER_QUEUES];
	long long thrput_deficit[MAX_TENANT_NUM];
	uint16_t n_packet [MAX_TENANT_NUM];
	size_t tenant_cycle_used[MAX_TENANT_NUM];
	size_t tenant_cycle_target[MAX_TENANT_NUM];
};

#define ATOMIC_COMMUNICATE

typedef uint8_t eu_status;

enum {
	EU_OFF  = 0,
    EU_FREE = 1,
    EU_HANG = 2,
    EU_BUSY = 3,
    EU_OVER = 4,
};

struct offload_dispatch_info {
	struct flexio_dpa_dev_queue* tenant;
	size_t busy_cycle[MAX_TENANT_NUM];
	uint8_t restrict_tenant[MAX_TENANT_NUM];
	uint32_t rq_wqe_idx;
	eu_status status;
	uint8_t reserved[1];
};

extern struct dpa_thread_context dpa_thds_ctx[190];
extern struct dpa_sche_context dpa_schs_ctx[32];
extern struct offload_dispatch_info offload_info[190];

#define SCHED_PERIOD_CYCLES DPA_FREQ_HZ / 1000
#define SCHED_PERIOD_CYCLES_PERCENT SCHED_PERIOD_CYCLES / 100
#define SCHED_PERIOD_CYCLES_BUDGET SCHED_PERIOD_CYCLES_PERCENT * 85

static size_t scheduler_num;
static uint32_t thrput_quantum = 15625;
static uint16_t q_packet = 670;
static uint32_t thrput_weights[MAX_TENANT_NUM] = {30, 30};
static uint32_t cycle_weights[MAX_TENANT_NUM] = {10, 0};

flexio_dev_status_t change_status(uint16_t thd_id, eu_status old_status, eu_status new_status);

void spin_on_status(uint16_t thd_id, eu_status expected_status);

void process_packet(struct flexio_dev_thread_ctx *dtctx, struct dpa_thread_context* tenant);

int pp_queue(struct flexio_dev_thread_ctx *dtctx, struct dpa_thread_context* this_thd_ctx, struct flexio_dpa_dev_queue* tenant, int thd_id, uint_test &result);

flexio_dev_rpc_handler_t thd_ctx_init;
__dpa_rpc__ uint64_t thd_ctx_init(uint64_t data);

#endif /* __FLEXIO_PP_DEV_UTILS_H__ */
