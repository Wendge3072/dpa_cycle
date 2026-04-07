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
	struct flexio_dpa_dev_queue queues[MAX_SCHEDULER_QUEUES];
};

typedef uint8_t eu_status;

enum {
	EU_OFF  = 0,
    EU_FREE = 1,
    EU_HANG = 2,
};

struct offload_dispatch_info {
	struct flexio_dpa_dev_queue* tenant;
	eu_status status;
};

extern struct dpa_thread_context dpa_thds_ctx[190];
extern struct dpa_sche_context dpa_schs_ctx[32];
extern struct offload_dispatch_info offload_info[190];

void spin_on_status(uint16_t thd_id, eu_status expected_status);

void pp_queue(struct flexio_dev_thread_ctx *dtctx,
	      struct flexio_dpa_dev_queue *rq_queue,
	      struct flexio_dpa_dev_queue *sch_queue,
	      struct dpa_thread_context *thd_queue);

flexio_dev_rpc_handler_t thd_ctx_init;
__dpa_rpc__ uint64_t thd_ctx_init(uint64_t data);

#endif /* __FLEXIO_PP_DEV_UTILS_H__ */
