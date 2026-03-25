/* Header file with utilities for DPA.
 */

#ifndef __COM_DEV_H__
#define __COM_DEV_H__


/* Flex IO SDK device side API header. */
#include <libflexio-dev/flexio_dev.h>

#ifndef NULL
#define NULL (void *)0
#endif

#define STUCK() \
	do { \
		flexio_dev_thread_reschedule(); \
	} while (1)

/* Convert logarithm to value */
#define L2V(l) (1UL << (l))
/* Convert logarithm to mask */
#define L2M(l) (L2V(l) - 1)

#define CQE_OPCODE_REQUESTER 0x0

#define DPA_FREQ_HZ 1800000000ULL  // 1.8GHz

static uint64_t zero_mac = 0x400432c288a0;
static uint64_t mask = 0x0000ffffffffffff;

/* Sample DPA CQ metadata structure.
 * Contains all data needed to DPA to work with CQ.
 */
typedef struct {
	uint32_t cq_number;
	struct flexio_dev_cqe64 *cq_ring, *cqe;
	uint32_t cq_idx;
	uint8_t cq_hw_owner_bit;
	uint32_t *cq_dbr;
	uint32_t log_cq_depth;
} cq_ctx_t;

/* Sample DPA RQ metadata structure.
 * Contains all data needed to DPA to work with RQ.
 */
typedef struct {
	uint32_t rq_number;
	struct flexio_dev_wqe_rcv_data_seg *rq_ring;
	flexio_uintptr_t rqd_host_addr;
	flexio_uintptr_t rqd_dpa_addr;
	uint32_t *rq_dbr;
} rq_ctx_t;

/* Sample DPA SQ metadata structure.
 * Contains all data needed to DPA to work with SQ.
 */
typedef struct {
	uint32_t sq_number;
	union flexio_dev_sqe_seg *sq_ring;
	flexio_uintptr_t sqd_addr;
	flexio_uintptr_t sqd_host_addr;
	flexio_uintptr_t sqd_dpa_addr;
	uint32_t sq_wqe_seg_idx;
	uint32_t sq_pi;
} sq_ctx_t;

/* Sample DPA send metadata structure.
 * Contains SQ ring and data buffer for sending.
 */
typedef struct {
	void *sq_tx_buff;
	uint32_t tx_buff_idx;
} dt_ctx_t;

/* Sample DPA EQ metadata structure.
 * Contains all data needed to DPA to work with EQ.
 */
typedef struct {
	uint32_t eq_number;
	struct flexio_dev_eqe *eq_ring, *eqe;
	uint32_t eq_idx;
	uint8_t eq_hw_owner_bit;
} eq_ctx_t;

/* Fill the cq_ctx_t structure with input parameters
 *  cq_ctx - structure to be filled
 *  num - cq_number
 *  log_depth - CQ log depth
 *  ring_addr - address of the ring
 *  dbr_addr - address of the DBR
 */
void com_cq_ctx_init(cq_ctx_t *cq_ctx, uint32_t num, uint32_t log_depth,
		     flexio_uintptr_t ring_addr, flexio_uintptr_t dbr_addr);

/* Fill the rq_ctx_t structure with input parameters
 *  rq_ctx - structure to be filled
 *  num - rq_number
 *  ring_addr - address of the ring
 *  dbr_addr - address of the DBR
 */
void com_rq_ctx_init(rq_ctx_t *rq_ctx, uint32_t num, flexio_uintptr_t ring_addr, 
	flexio_uintptr_t dbr_addr);

/* Fill the sq_ctx_t structure with input parameters
 *  sq_ctx - structure to be filled
 *  num - sq_number
 *  ring_addr - address of the ring
 */
void com_sq_ctx_init(sq_ctx_t *sq_ctx, uint32_t num, flexio_uintptr_t ring_addr);

/* Fill the eq_ctx_t structure with input parameters
 *  eq_ctx - structure to be filled
 *  num - sq_number
 *  ring_addr - address of the ring
 */
void com_eq_ctx_init(eq_ctx_t *eq_ctx, uint32_t num, flexio_uintptr_t ring_addr);

/* Fill structure dt_ctx_t with input parameters
 *  dt_ctx - structure to be filled
 *  buff_addr - address of the buffer
 */
void com_dt_ctx_init(dt_ctx_t *dt_ctx, flexio_uintptr_t buff_addr);

/* Update the cq_ctx_t structure and set the DBR for the CQE.
 *  cq_ctx - pointer to cq_ctx_t structure to update
 */
void com_step_cq(cq_ctx_t *cq_ctx);

/* Get the pointer to the next data entry.
 *  dt_ctx - the pointer to the dt_ctx_t structure.
 *  dt_idx_mask - mask of indexes.
 *  log_dt_entry_sz - logarithm size of the data entry
 */
void *get_next_dte(dt_ctx_t *dt_ctx, uint32_t dt_idx_mask, uint32_t log_dt_entry_sz);

/* Get the pointer to the next SQ entry.
 *  sq_ctx - the pointer to the sq_ctx_t structure.
 *  sq_idx_mask - mask of indexes.
 */
void *get_next_sqe(sq_ctx_t *sq_ctx, uint32_t sq_idx_mask);

/* Update the eq_ctx_t structure and EQ with no DBR.
 *  dtctx - context of the thread.
 *  eq_ctx - pointer to the eq_ctx_t structure.
 *  eq_idx_mask - mask of indexes.
 */
void com_step_eq(struct flexio_dev_thread_ctx *dtctx, eq_ctx_t *eq_ctx, uint32_t eq_idx_mask);

/* Poll CQ
 *  Return: 0 on success and -1 if it fails.
 *  cq_ctx - the pointer to the cq_ctx_t structure to poll.
 *  consumed_cqes - the number of consumed CQEs.
 */
uint8_t com_cq_poll(cq_ctx_t *cq_ctx, uint32_t *consumed_cqes);

/* Swap source and destination MAC in the packet.
 *  packet - pointer to the packet.
 */
void swap_macs(char *packet);

void swap_mac(char* packet);

void get_swap_mac(char* packet);

void save_set_dstmac(char* packet, uint16_t mac_index);

void dpa_delay_ns(uint64_t nsec);

void dpa_delay_cycles(uint64_t cycles);

uint16_t calculate_checksum(uint16_t *data, int length);

typedef uint32_t uint_test;

inline uint_test calculate_checksum_nrnd(uint_test *data, int length, int round);


#endif
