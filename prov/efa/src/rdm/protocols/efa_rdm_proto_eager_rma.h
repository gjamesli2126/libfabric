/* Copyright Amazon.com, Inc. or its affiliates. All rights reserved. */
/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */

#ifndef _EFA_RDM_PROTO_EAGER_RMA_H
#define _EFA_RDM_PROTO_EAGER_RMA_H

#include "efa.h"

/*
 * Eager read / eager write: one application RMA == one device WQE == one
 * eager RMA entry (a 64-byte "minimal TXE"). No efa_rdm_ope, no PKE.
 *
 * These are native RDMA reads/writes that fit in a single work request.
 * Not to be confused with efa_rdm_proto_eager_write, which is the emulated
 * (send based) eager RTW protocol.
 */

/**
 * @brief The whole TX state of an eager read/write.
 *
 * Everything the CQ path needs to report a completion or an error. It is
 * allocated from ep->eager_rma_pool and named by the WQE's wr_id.
 */
struct efa_rdm_eager_rma_entry {
	void *op_context;	/**< application context for the CQ entry */
	void *buf;		/**< local buffer, for the error entry */
	size_t len;		/**< bytes, for the error entry */
	uint64_t cq_flags;	/**< FI_RMA | FI_READ or FI_RMA | FI_WRITE */
	uint64_t tx_flags;	/**< op flags at post time (FI_COMPLETION) */
	uint8_t gen;		/**< bumped on every completion, checked against wr_id */
};

/*
 * wr_id = entry address | EFA_RDM_EAGER_RMA_WR_ID_TAG | gen
 *
 * The entry is EFA_RDM_BUFPOOL_ALIGNMENT aligned, so the low bits hold the
 * generation, as for a PKE. Bit 63 is never set in a user-space address,
 * so it tells an eager RMA wr_id from a PKE wr_id.
 */
#define EFA_RDM_EAGER_RMA_WR_ID_TAG (1ULL << 63)

static inline bool efa_rdm_eager_rma_is_wr_id(uint64_t wr_id)
{
	return wr_id & EFA_RDM_EAGER_RMA_WR_ID_TAG;
}

static inline struct efa_rdm_eager_rma_entry *
efa_rdm_eager_rma_entry_from_wr_id(uint64_t wr_id)
{
	return (struct efa_rdm_eager_rma_entry *) (uintptr_t)
		(wr_id & ~(EFA_RDM_EAGER_RMA_WR_ID_TAG | EFA_RDM_GEN_MASK));
}

ssize_t efa_rdm_proto_eager_read_post(struct efa_rdm_ep *ep,
				      struct efa_rdm_peer *peer,
				      const struct fi_msg_rma *msg,
				      uint64_t fi_flags);

ssize_t efa_rdm_proto_eager_write_post(struct efa_rdm_ep *ep,
				       struct efa_rdm_peer *peer,
				       const struct fi_msg_rma *msg,
				       uint64_t fi_flags);

void efa_rdm_proto_eager_rma_handle_wc(struct efa_rdm_ep *ep, uint64_t wr_id,
				       enum ibv_wc_status status,
				       int prov_errno);

void efa_rdm_proto_eager_rma_release(struct efa_rdm_ep *ep, uint64_t wr_id);

#endif /* _EFA_RDM_PROTO_EAGER_RMA_H */
