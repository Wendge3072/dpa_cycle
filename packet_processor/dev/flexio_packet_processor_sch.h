#ifndef __FLEXIO_PACKET_PROCESSOR_SCH_H__
#define __FLEXIO_PACKET_PROCESSOR_SCH_H__

#include "com_dev.h"
#include <libflexio-dev/flexio_dev_err.h>
#include <libflexio-dev/flexio_dev_queue_access.h>
#include <libflexio-dev/flexio_dev_debug.h>
#include <libflexio-libc/string.h>
#include <stddef.h>
#include <dpaintrin.h>
/* Shared header file for packet processor sample */
#include "../flexio_packet_processor_com.h"

/* Mask for CQ index */
#define CQ_IDX_MASK ((1 << LOG_CQ_DEPTH) - 1)
/* Mask for RQ index */
#define RQ_IDX_MASK ((1 << LOG_RQ_DEPTH) - 1)
/* Mask for SQ index */
#define SQ_IDX_MASK ((1 << (LOG_SQ_DEPTH + LOG_SQE_NUM_SEGS)) - 1)
/* Mask for data index */
#define DATA_IDX_MASK ((1 << (LOG_SQ_DEPTH)) - 1)

/* The structure of the sample DPA application contains global data that the application uses */
static struct dpa_thread_context {
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
} thd_ctx[190];

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
static struct dpa_sche_context {
	/* Packet count - used for debug message */
	uint64_t packets_count;
	int buffer_location;
	uint32_t window_id;
	uint32_t idx;
	// Packet statistics for each round (1000000 packets per round)
	uint64_t packet_stats_round;
	struct flexio_dpa_dev_queue queues[MAX_SCHEDULER_QUEUES];
	long long deficit[MAX_SCHEDULER_QUEUES];
	uint16_t n_packet [MAX_SCHEDULER_QUEUES];
} sch_ctx[32];

static size_t tenant_num_per_scheduler;
static size_t scheduler_num;
static size_t worker_threads_num;
static uint32_t quantum = 15625;
static uint16_t q_packet = 670;
static uint32_t weights[MAX_SCHEDULER_QUEUES] = {25, 25};

#define TENANT_WORKLOAD_DIRECT 0
#define TENANT_WORKLOAD_MEMCPY 1
#define TENANT_WORKLOAD_CHECKSUM 2

#define TENANT_0_WORKLOAD TENANT_WORKLOAD_DIRECT
#define TENANT_1_WORKLOAD TENANT_WORKLOAD_DIRECT

#define PACKET_METADATA_ORIG_DMAC_OFFSET 64
#define DEFAULT_WORKER_FORWARD_STRIPE 8

static uint64_t zero_mac = 0x400432c288a0;
static uint64_t mac_prefix_mask = 0xffff000000000000;

static inline uint32_t get_tenant_workload(uint32_t tenant_id)
{
	if (tenant_id == 0)
		return TENANT_0_WORKLOAD;
	if (tenant_id == 1)
		return TENANT_1_WORKLOAD;
	return TENANT_WORKLOAD_DIRECT;
}

static inline uint64_t get_packet_orig_dmac(char *packet)
{
	return *((uint64_t *)(packet + PACKET_METADATA_ORIG_DMAC_OFFSET));
}

static inline uint32_t get_packet_tenant_id(char *packet)
{
	uint64_t saved_dst_mac = get_packet_orig_dmac(packet);
	uint64_t saved_mac_body = saved_dst_mac & ~mac_prefix_mask;

	if (saved_mac_body >= zero_mac) {
		uint64_t mac_index = (saved_mac_body - zero_mac) >> 40;
		return (uint32_t)(mac_index % MAX_SCHEDULER_QUEUES);
	}
	return 0;
}

#endif /* __FLEXIO_PACKET_PROCESSOR_SCH_H__ */
