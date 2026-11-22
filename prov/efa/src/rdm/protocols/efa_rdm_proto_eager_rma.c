/* Copyright Amazon.com, Inc. or its affiliates. All rights reserved. */
/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */

#include "efa_rdm_proto_eager_rma.h"
#include "efa.h"
#include "efa_cntr.h"
#include "efa_rdm_cq.h"
#include "efa_rdm_msg.h"
#include "efa_rdm_rma.h"

/*
 * Eager read / eager write
 *
 * Happy path
 *   post: alloc 1 eager RMA entry, post 1 WQE with
 *         wr_id = entry | TAG | gen, ep->efa_outstanding_tx_ops++
 *   CQE:  wr_id -> entry (gen checked) -> fi_cq entry -> free entry
 *
 * Why no RNR handling
 *   RDMA read, and RDMA write without immediate data, use no receive
 *   buffer at the target, so the device never reports RNR for them. A
 *   write with immediate data is eligible only when it cannot see RNR
 *   either: see efa_rdm_eager_rma_imm_ok(). Every other write with CQ
 *   data stays on the TXE path and keeps its RNR queue and backoff.
 *
 * Why no handshake handling
 *   An op is eligible only when the peer's capabilities are known
 *   (handshake received, or homogeneous peers). Otherwise the caller falls
 *   back to the TXE path, which triggers and queues behind the handshake.
 *
 * Why no per-peer tracking
 *   The entry never dereferences the peer after the post, so fi_av_remove
 *   while a WQE is in flight leaves nothing dangling: the completion is
 *   reported (or dropped on a closing ep) from the entry alone.
 *
 * Every error is final and is reported as one CQ error entry.
 */

/* Op flags an eager RMA honors without extra state. */
#define EFA_RDM_EAGER_RMA_READ_FLAGS \
	(FI_COMPLETION | FI_TRANSMIT_COMPLETE | FI_MORE)
#define EFA_RDM_EAGER_RMA_WRITE_FLAGS \
	(EFA_RDM_EAGER_RMA_READ_FLAGS | FI_REMOTE_CQ_DATA)

/* PoC diagnostics: FI_EFA_RMA_FAST_PATH=2 logs the first fallback reasons. */
static inline ssize_t efa_rdm_eager_rma_fallback(const char *op,
						 const char *why,
						 uint64_t tx_flags)
{
	static int logged;

	if (OFI_UNLIKELY(efa_env.rma_fast_path >= 2) && logged < 4) {
		logged++;
		EFA_WARN(FI_LOG_EP_DATA,
			 "eager %s fallback: %s (tx_flags 0x%" PRIx64 ")\n",
			 op, why, tx_flags);
	}
	return -FI_ENOSYS;
}

/**
 * @brief Check the conditions shared by eager read and eager write.
 *
 * @return NULL if eligible, otherwise the reason it is not.
 */
static inline const char *
efa_rdm_eager_rma_ineligible(struct efa_rdm_ep *ep, struct efa_rdm_peer *peer,
			     const struct fi_msg_rma *msg, uint64_t tx_flags,
			     uint64_t allowed_flags, size_t segment_size)
{
	size_t len;

	if (msg->iov_count != 1 || msg->rma_iov_count != 1)
		return "iov count";
	if (!msg->desc || !msg->desc[0])
		return "no desc";
	/* MR tracking needs an ope to record the MR. */
	if (efa_env.track_mr)
		return "track_mr";
	if (peer->is_self)
		return "self";
	/* Capabilities are only known after the handshake. */
	if (!ep->homogeneous_peers &&
	    !(peer->flags & EFA_RDM_PEER_HANDSHAKE_RECEIVED))
		return "no handshake";
	if (tx_flags & ~allowed_flags)
		return "op flags";

	len = msg->msg_iov[0].iov_len;
	if (!len || len != msg->rma_iov[0].len)
		return "length";
	if (len > MIN(segment_size,
		      efa_rdm_ep_domain(ep)->device->max_rdma_size))
		return "more than one WQE";
	return NULL;
}

/**
 * @brief Can a write with immediate data skip software RNR handling?
 *
 * Both sides must agree on unsolicited write recv (as on the TXE path).
 * Then the write never sees RNR when it takes no receive buffer
 * (unsolicited write recv on), or when the device retries RNR forever
 * (rnr_retry 7, as NIXL sets it).
 */
static inline bool efa_rdm_eager_rma_imm_ok(struct efa_rdm_ep *ep,
					    struct efa_rdm_peer *peer)
{
	bool local = efa_rdm_ep_support_unsolicited_write_recv(ep);

	if (!ep->homogeneous_peers &&
	    local != efa_rdm_peer_support_unsolicited_write_recv(peer))
		return false;

	return local || ep->base_ep.rnr_retry == EFA_RNR_INFINITE_RETRY;
}

/**
 * @brief Allocate and fill the entry of one eager RMA WQE.
 *
 * @param[out] wr_id	the WQE's work request ID
 */
static inline struct efa_rdm_eager_rma_entry *
efa_rdm_eager_rma_alloc(struct efa_rdm_ep *ep, const struct fi_msg_rma *msg,
			uint64_t tx_flags, uint64_t cq_flags, uint64_t *wr_id)
{
	struct efa_rdm_eager_rma_entry *entry;

	entry = ofi_buf_alloc(ep->eager_rma_pool);
	if (OFI_UNLIKELY(!entry))
		return NULL;

	entry->op_context = msg->context;
	entry->buf = msg->msg_iov[0].iov_base;
	entry->len = msg->msg_iov[0].iov_len;
	entry->cq_flags = cq_flags;
	entry->tx_flags = tx_flags;
	entry->gen &= EFA_RDM_GEN_MASK;
	*wr_id = (uintptr_t) entry | EFA_RDM_EAGER_RMA_WR_ID_TAG | entry->gen;
	return entry;
}

/**
 * @brief Post an eager read.
 *
 * @return 0 when posted, -FI_EAGAIN when out of resources, -FI_ENOSYS when
 * the read must take the TXE path, or a negative device error.
 */
ssize_t efa_rdm_proto_eager_read_post(struct efa_rdm_ep *ep,
				      struct efa_rdm_peer *peer,
				      const struct fi_msg_rma *msg,
				      uint64_t fi_flags)
{
	struct efa_rdm_eager_rma_entry *entry;
	struct efa_mr *efa_mr;
	struct ibv_sge sge;
	const char *why;
	uint64_t tx_flags, wr_id;
	int use_p2p, err;

	tx_flags = efa_rdm_msg_get_tx_flags(ep, fi_flags);
	why = efa_rdm_eager_rma_ineligible(ep, peer, msg, tx_flags,
					   EFA_RDM_EAGER_RMA_READ_FLAGS,
					   efa_env.efa_read_segment_size);
	if (why)
		return efa_rdm_eager_rma_fallback("read", why, tx_flags);

	efa_mr = msg->desc[0];
	use_p2p = efa_rdm_ep_use_p2p_for_mr(ep, efa_mr);
	if (use_p2p <= 0 ||
	    !efa_rdm_rma_should_read_using_rdma(ep, peer, use_p2p))
		return efa_rdm_eager_rma_fallback("read", "no rdma read", tx_flags);

	if (OFI_UNLIKELY(efa_rdm_ep_get_available_tx_pkts(ep) == 0))
		return -FI_EAGAIN;

	entry = efa_rdm_eager_rma_alloc(ep, msg, tx_flags, FI_RMA | FI_READ,
					&wr_id);
	if (OFI_UNLIKELY(!entry))
		return -FI_EAGAIN;

	sge.addr = (uintptr_t) msg->msg_iov[0].iov_base;
	sge.length = msg->msg_iov[0].iov_len;
	sge.lkey = efa_mr->lkey;
	err = efa_qp_post_read(ep->base_ep.qp, &sge, 1, msg->rma_iov[0].key,
			       msg->rma_iov[0].addr, wr_id, tx_flags & FI_MORE,
			       peer->ah, peer->qpn, peer->qkey);
	if (OFI_UNLIKELY(err)) {
		ofi_buf_free(entry);
		return (err == ENOMEM) ? -FI_EAGAIN : -err;
	}

	ep->efa_outstanding_tx_ops++;
	return 0;
}

/**
 * @brief Post an eager write.
 *
 * @return see efa_rdm_proto_eager_read_post()
 */
ssize_t efa_rdm_proto_eager_write_post(struct efa_rdm_ep *ep,
				       struct efa_rdm_peer *peer,
				       const struct fi_msg_rma *msg,
				       uint64_t fi_flags)
{
	struct efa_rdm_eager_rma_entry *entry;
	struct efa_mr *efa_mr;
	struct ibv_sge sge;
	const char *why;
	uint64_t tx_flags, wr_id;
	int use_p2p, err;

	tx_flags = efa_rdm_msg_get_tx_flags(ep, fi_flags);
	why = efa_rdm_eager_rma_ineligible(ep, peer, msg, tx_flags,
					   EFA_RDM_EAGER_RMA_WRITE_FLAGS,
					   efa_env.efa_write_segment_size);
	if (why)
		return efa_rdm_eager_rma_fallback("write", why, tx_flags);

	if ((tx_flags & FI_REMOTE_CQ_DATA) &&
	    !efa_rdm_eager_rma_imm_ok(ep, peer))
		return efa_rdm_eager_rma_fallback("write", "imm may see RNR",
						  tx_flags);

	efa_mr = msg->desc[0];
	use_p2p = efa_rdm_ep_use_p2p_for_mr(ep, efa_mr);
	if (use_p2p <= 0 || !efa_both_support_rdma_write(ep, peer) ||
	    !efa_both_support_p2p(ep, peer, use_p2p))
		return efa_rdm_eager_rma_fallback("write", "no rdma write",
						  tx_flags);

	if (OFI_UNLIKELY(efa_rdm_ep_get_available_tx_pkts(ep) == 0))
		return -FI_EAGAIN;

	entry = efa_rdm_eager_rma_alloc(ep, msg, tx_flags, FI_RMA | FI_WRITE,
					&wr_id);
	if (OFI_UNLIKELY(!entry))
		return -FI_EAGAIN;

	sge.addr = (uintptr_t) msg->msg_iov[0].iov_base;
	sge.length = msg->msg_iov[0].iov_len;
	sge.lkey = efa_mr->lkey;
	err = efa_qp_post_write(ep->base_ep.qp, &sge, 1, NULL, false,
				msg->rma_iov[0].key, msg->rma_iov[0].addr,
				wr_id, msg->data,
				tx_flags & (FI_MORE | FI_REMOTE_CQ_DATA),
				peer->ah, peer->qpn, peer->qkey, NULL);
	if (OFI_UNLIKELY(err)) {
		ofi_buf_free(entry);
		return (err == ENOMEM) ? -FI_EAGAIN : -err;
	}

	ep->efa_outstanding_tx_ops++;
	return 0;
}

/**
 * @brief Check the wr_id generation and return the entry it names.
 *
 * A mismatch means a stale or duplicated completion; as for a PKE there is
 * no safe way to continue.
 */
static inline struct efa_rdm_eager_rma_entry *
efa_rdm_eager_rma_get_entry(uint64_t wr_id)
{
	struct efa_rdm_eager_rma_entry *entry =
		efa_rdm_eager_rma_entry_from_wr_id(wr_id);

	if (OFI_UNLIKELY(entry->gen != (wr_id & EFA_RDM_GEN_MASK))) {
		EFA_WARN(FI_LOG_CQ,
			 "eager RMA entry %p: expected gen %d received gen %d\n",
			 entry, entry->gen, (int) (wr_id & EFA_RDM_GEN_MASK));
		abort();
	}
	return entry;
}

static inline void efa_rdm_eager_rma_free(struct efa_rdm_ep *ep,
					  struct efa_rdm_eager_rma_entry *entry)
{
	assert(ep->efa_outstanding_tx_ops > 0);
	ep->efa_outstanding_tx_ops--;
	entry->gen = (entry->gen + 1) & EFA_RDM_GEN_MASK;
	ofi_buf_free(entry);
}

/**
 * @brief Free the entry of a completed eager RMA without reporting it.
 *
 * Used by the closing-endpoint path.
 */
void efa_rdm_proto_eager_rma_release(struct efa_rdm_ep *ep, uint64_t wr_id)
{
	efa_rdm_eager_rma_free(ep, efa_rdm_eager_rma_get_entry(wr_id));
}

/**
 * @brief Report the completion of an eager read/write and free its entry.
 */
void efa_rdm_proto_eager_rma_handle_wc(struct efa_rdm_ep *ep, uint64_t wr_id,
				       enum ibv_wc_status status,
				       int prov_errno)
{
	struct efa_rdm_eager_rma_entry *entry = efa_rdm_eager_rma_get_entry(wr_id);
	struct util_ep *util_ep = &ep->base_ep.util_ep;
	struct fi_cq_err_entry err_entry = {0};
	void *op_context = entry->op_context;
	uint64_t cq_flags = entry->cq_flags;
	uint64_t tx_flags = entry->tx_flags;
	int ret;

	if (OFI_UNLIKELY(status != IBV_WC_SUCCESS)) {
		err_entry.op_context = op_context;
		err_entry.flags = cq_flags;
		err_entry.len = entry->len;
		err_entry.buf = entry->buf;
		err_entry.err = to_fi_errno(prov_errno);
		err_entry.prov_errno = prov_errno;
		efa_rdm_eager_rma_free(ep, entry);
		EFA_WARN(FI_LOG_CQ, "eager %s failed: %s (%d)\n",
			 (cq_flags & FI_READ) ? "read" : "write",
			 efa_strerror(prov_errno), prov_errno);
		efa_rdm_cq_write_error(&ep->base_ep, util_ep->tx_cq, &err_entry,
				       "eager RMA");
		efa_cntr_report_error(util_ep, cq_flags);
		return;
	}

	efa_rdm_eager_rma_free(ep, entry);

	if (tx_flags & FI_COMPLETION) {
		/* TX completions do not carry a source address. */
		if (util_ep->caps & FI_SOURCE)
			ret = ofi_cq_write_src(util_ep->tx_cq, op_context,
					       cq_flags, 0, NULL, 0, 0,
					       FI_ADDR_NOTAVAIL);
		else
			ret = ofi_cq_write(util_ep->tx_cq, op_context,
					   cq_flags, 0, NULL, 0, 0);
		if (OFI_UNLIKELY(ret)) {
			EFA_WARN(FI_LOG_CQ,
				 "Unable to write eager RMA completion: %s\n",
				 fi_strerror(-ret));
			err_entry.op_context = op_context;
			err_entry.flags = cq_flags;
			err_entry.err = -ret;
			err_entry.prov_errno = FI_EFA_ERR_WRITE_SEND_COMP;
			efa_rdm_cq_write_error(&ep->base_ep, util_ep->tx_cq,
					       &err_entry, "eager RMA");
			efa_cntr_report_error(util_ep, cq_flags);
			return;
		}
	}

	efa_cntr_report_tx_completion(util_ep, cq_flags);
}
