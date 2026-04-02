#include "com_dev.h"
#include <libflexio-dev/flexio_dev_queue_access.h>
#include <stddef.h>
#include <dpaintrin.h>

#define SWAP(a, b) \
	do { \
		__typeof__(a) tmp;  \
		tmp = a;            \
		a = b;              \
		b = tmp;            \
	} while (0)

/* Swap source and destination MAC addresses in the packet.
 *  packet - pointer to the packet.
 */
void swap_macs(char *packet)
{
	char *dmac, *smac;
	int i;

	dmac = packet;
	smac = packet + 6;
	for (i = 0; i < 6; i++, dmac++, smac++)
		SWAP(*smac, *dmac);
}

void swap_mac(char* packet)
{
	uint64_t src_mac = *((uint64_t *)packet);
	uint64_t dst_mac = *((uint64_t *)(packet + 6));
	*((uint64_t *)packet) = dst_mac;
	*((uint64_t *)(packet + 6)) = (src_mac & mask) | (dst_mac & ~mask);
	// memset(packet + 12, 0, 30);
}

void get_swap_mac(char* packet)
{
	uint64_t dstmac_ori = *((uint64_t *)(packet + 64));
	uint64_t srcmac_ori = *((uint64_t *)(packet + 6));
	// uint64_t dstmac_now = *((uint64_t *)(packet));
	*((uint64_t *)packet) = srcmac_ori;
	*((uint64_t *)(packet + 6)) = (dstmac_ori & mask) | (srcmac_ori & ~mask);
}

/*
	dst mac: 0xa088c2320440
	big end: 0xXX 0xXX 0x40 0x04 0x32 0xc2 0x88 0xa0
*/ 
void save_set_dstmac(char* packet, uint16_t mac_index)
{
	register uint64_t dst_mac = *((uint64_t *)packet);
	*((uint64_t *)(packet + 64)) = dst_mac;
	dst_mac = (dst_mac & ~mask) | (zero_mac + ((uint64_t)mac_index << 40));
	*((uint64_t *)packet) = dst_mac;
}

/* Fill the cq_ctx_t structure with input parameters
 *  cq_ctx - structure to be filled
 *  num - cq_number
 *  log_depth - CQ log depth
 *  ring_addr - address of the ring
 *  dbr_addr - address of the DBR
 */
void com_cq_ctx_init(cq_ctx_t *cq_ctx, uint32_t num, uint32_t log_depth, flexio_uintptr_t ring_addr,
		     flexio_uintptr_t dbr_addr)
{
	cq_ctx->cq_number = num;
	cq_ctx->cq_ring = (struct flexio_dev_cqe64 *)ring_addr;
	cq_ctx->cq_dbr = (uint32_t *)dbr_addr;

	cq_ctx->cqe = cq_ctx->cq_ring;
	cq_ctx->cq_idx = 0;
	cq_ctx->cq_hw_owner_bit = 0x1;
	cq_ctx->log_cq_depth = log_depth;
}

/* Fill the rq_ctx_t structure with input parameters
 *  rq_ctx - structure to be filled
 *  num - rq_number
 *  ring_addr - address of the ring
 *  dbr_addr - address of the DBR
 */
void com_rq_ctx_init(rq_ctx_t *rq_ctx, uint32_t num, flexio_uintptr_t ring_addr,
		     flexio_uintptr_t dbr_addr)
{
	rq_ctx->rq_number = num;
	rq_ctx->rq_ring = (struct flexio_dev_wqe_rcv_data_seg *)ring_addr;
	rq_ctx->rq_dbr = (uint32_t *)dbr_addr;
}

/* Fill the sq_ctx_t structure with input parameters
 *  sq_ctx - structure to be filled
 *  num - sq_number
 *  ring_addr - address of the ring
 */
void com_sq_ctx_init(sq_ctx_t *sq_ctx, uint32_t num, flexio_uintptr_t ring_addr)
{
	sq_ctx->sq_number = num;
	sq_ctx->sq_ring = (union flexio_dev_sqe_seg *)ring_addr;
	sq_ctx->sq_wqe_seg_idx = 0;
}

/* Fill the eq_ctx_t structure with input parameters
 *  eq_ctx - structure to be filled
 *  num - sq_number
 *  ring_addr - address of the ring
 */
void com_eq_ctx_init(eq_ctx_t *eq_ctx, uint32_t num, flexio_uintptr_t ring_addr)
{
	eq_ctx->eq_number = num;
	eq_ctx->eq_ring = (struct flexio_dev_eqe *)ring_addr;

	eq_ctx->eqe = eq_ctx->eq_ring;
	eq_ctx->eq_idx = 0;
	eq_ctx->eq_hw_owner_bit = 0x1;
}

/* Fill structure dt_ctx_t with input parameters
 *  dt_ctx - structure to be filled
 *  buff_addr - address of the buffer
 */
void com_dt_ctx_init(dt_ctx_t *dt_ctx, flexio_uintptr_t buff_addr)
{
	dt_ctx->sq_tx_buff = (void *)buff_addr;
	dt_ctx->tx_buff_idx = 0;
}

/* Update the cq_ctx_t structure and set the DBR for the CQE.
 *  cq_ctx - pointer to cq_ctx_t structure to update
 */
void com_step_cq(cq_ctx_t *cq_ctx)
{
	uint32_t cq_idx_mask = L2M(cq_ctx->log_cq_depth);

	cq_ctx->cq_idx++;
	cq_ctx->cqe = &cq_ctx->cq_ring[cq_ctx->cq_idx & cq_idx_mask];
	/* check for wrap around */
	if (!(cq_ctx->cq_idx & cq_idx_mask))
		cq_ctx->cq_hw_owner_bit = !cq_ctx->cq_hw_owner_bit;

	flexio_dev_dbr_cq_set_ci(cq_ctx->cq_dbr, cq_ctx->cq_idx);
}

/* Update the eq_ctx_t structure and EQ with no DBR.
 *  dtctx - context of the thread.
 *  eq_ctx - pointer to the eq_ctx_t structure.
 *  eq_idx_mask - mask of indexes.
 */
void com_step_eq(struct flexio_dev_thread_ctx *dtctx, eq_ctx_t *eq_ctx, uint32_t eq_idx_mask)
{
	uint32_t eq_ci;

	eq_ci = eq_ctx->eq_idx++;
	eq_ctx->eqe = &eq_ctx->eq_ring[eq_ctx->eq_idx & eq_idx_mask];
	/* check for wrap around */
	if (!(eq_ctx->eq_idx & eq_idx_mask))
		eq_ctx->eq_hw_owner_bit = !eq_ctx->eq_hw_owner_bit;

	/* No DBR */
	flexio_dev_eq_update_ci(dtctx, eq_ci, eq_ctx->eq_number);
}

/* Get the pointer to the next data entry.
 *  dt_ctx - the pointer to the dt_ctx_t structure.
 *  dt_idx_mask - mask of indexes.
 *  log_dt_entry_sz - logarithm size of the data entry
 */
void *get_next_dte(dt_ctx_t *dt_ctx, uint32_t dt_idx_mask, uint32_t log_dt_entry_sz)
{
	return (char *)(dt_ctx->sq_tx_buff) +
	       ((dt_ctx->tx_buff_idx++ & dt_idx_mask) << log_dt_entry_sz);
}

/* Get the pointer to the next SQ entry.
 *  sq_ctx - the pointer to the sq_ctx_t structure.
 *  sq_idx_mask - mask of indexes.
 */
void *get_next_sqe(sq_ctx_t *sq_ctx, uint32_t sq_idx_mask)
{
	return &sq_ctx->sq_ring[sq_ctx->sq_wqe_seg_idx++ & sq_idx_mask];
}

/* Poll CQ
 *  Return: 0 on success and -1 if it fails.
 *  cq_ctx - the pointer to the cq_ctx_t structure to poll.
 *  consumed_cqes - the number of consumed CQEs.
 */
uint8_t com_cq_poll(cq_ctx_t *cq_ctx, uint32_t *consumed_cqes)
{
	if (!cq_ctx || !consumed_cqes)
		return -1;

	*consumed_cqes = 0;
	while (flexio_dev_cqe_get_owner(cq_ctx->cqe) != cq_ctx->cq_hw_owner_bit) {
		__dpa_thread_fence(__DPA_MEMORY, __DPA_R, __DPA_R);

		if (flexio_dev_cqe_get_opcode(cq_ctx->cqe) != CQE_OPCODE_REQUESTER)
			return -1;
		com_step_cq(cq_ctx);
		(*consumed_cqes)++;
	}

	return 0;
}

// ----==== DPA Workloads ====----

void dpa_delay_ns(uint64_t nsec) {
    uint64_t start = __dpa_thread_cycles();
    uint64_t wait_cycles = DPA_FREQ_HZ * nsec / 1000000000ULL;

    while ((__dpa_thread_cycles() - start) < wait_cycles) {
        // busy wait
    }
}

void dpa_delay_cycles(uint64_t cycles) {
    uint64_t start = __dpa_thread_cycles();

    while ((__dpa_thread_cycles() - start) < cycles) {
        // busy wait
    }
}

__attribute__((optimize("O0")))
uint16_t calculate_checksum(uint16_t *data, int length) {
    uint32_t sum = 0;
    int i;

    for (i = 0; i < length; i++) {
        sum += data[i];
    }

    return ~sum;
}


__attribute__((optimize("O0")))
uint_test calculate_checksum_nrnd(uint_test *data, int length, int round) {
	uint_test sum = 0;
    int i;

	while(round--){
		for (i = 0; i < length; i++) {
			sum += data[i];
		}
	}

    return ~sum;
}
