/* Copyright Amazon.com, Inc. or its affiliates. All rights reserved. */
/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */

#include "efa_rdm_proto.h"
#include "efa.h"
#include "efa_rdm_domain.h"
#include "efa_rdm_ope.h"
#include "efa_rdm_pke_nonreq.h"
#include "protocols/efa_rdm_proto_eager.h"
#include "protocols/efa_rdm_proto_eager_write.h"
#include "protocols/efa_rdm_proto_longcts.h"
#include "protocols/efa_rdm_proto_longread.h"
#include "protocols/efa_rdm_proto_longread_write.h"
#include "protocols/efa_rdm_proto_longcts_write.h"
#include "protocols/efa_rdm_proto_medium.h"
#include "protocols/efa_rdm_proto_runtread.h"
#include "protocols/efa_rdm_proto_longcts_rtr.h"
#include "protocols/efa_rdm_proto_short_rtr.h"
#include "efa_rdm_msg.h"

static bool efa_rdm_proto_iface_is_valid(enum fi_hmem_iface iface)
{
	return (int) iface >= 0 && iface < OFI_HMEM_MAX;
}

int efa_rdm_proto_set_min_bound(struct efa_rdm_proto *proto,
				enum fi_hmem_iface iface, size_t value)
{
	if (!proto || !efa_rdm_proto_iface_is_valid(iface))
		return -FI_EINVAL;

	if (value > proto->bounds[iface].max)
		return -FI_EINVAL;

	proto->bounds[iface].min = value;
	return FI_SUCCESS;
}

int efa_rdm_proto_set_max_bound(struct efa_rdm_proto *proto,
				enum fi_hmem_iface iface, size_t value)
{
	if (!proto || !efa_rdm_proto_iface_is_valid(iface))
		return -FI_EINVAL;

	if (value < proto->bounds[iface].min)
		return -FI_EINVAL;

	proto->bounds[iface].max = value;
	return FI_SUCCESS;
}

int efa_rdm_proto_set_bound_unsupported(struct efa_rdm_proto *proto,
					enum fi_hmem_iface iface, size_t value)
{
	(void) value;

	if (!proto || !efa_rdm_proto_iface_is_valid(iface))
		return -FI_EINVAL;

	return -FI_EOPNOTSUPP;
}

#if HAVE_CUDA || HAVE_NEURON || HAVE_ROCR
static size_t efa_rdm_proto_max_eager_msg_size_with_largest_header(void)
{
	assert(g_efa_selected_device_list);
	return g_efa_selected_device_list[0].ibv_port_attr.max_msg_sz -
	       efa_rdm_pkt_type_get_max_hdr_size();
}
#else
static size_t efa_rdm_proto_max_eager_msg_size_with_largest_header(void)
{
	return 0;
}
#endif

static void
efa_rdm_proto_set_bounds(struct efa_rdm_proto *proto,
			 enum fi_hmem_iface iface, size_t min, size_t max)
{
	proto->bounds[iface].min = min;
	proto->bounds[iface].max = max;
}

static void
efa_rdm_proto_apply_bound_override(
	struct efa_rdm_proto *proto, enum fi_hmem_iface iface,
	const struct efa_env_size_override *override,
	int (*setter)(struct efa_rdm_proto *, enum fi_hmem_iface, size_t))
{
	struct efa_rdm_proto_bounds defaults = proto->bounds[iface];
	int ret;

	if (!override->is_set)
		return;

	ret = setter(proto, iface, override->value);
	if (ret) {
		proto->bounds[iface] = defaults;
		EFA_WARN(FI_LOG_CORE,
			 "Ignoring rejected bound override %zu for %s on %s\n",
			 override->value, proto->name,
			 fi_tostr(&iface, FI_TYPE_HMEM_IFACE));
	}
}

int efa_rdm_proto_init_bounds_for_iface(enum fi_hmem_iface target_iface, enum fi_hmem_iface policy_iface){
	size_t medium_max, read_min, read_write_min;

	if (!efa_rdm_proto_iface_is_valid(target_iface) ||
	    !efa_rdm_proto_iface_is_valid(policy_iface))
		return -FI_EINVAL;

	switch (policy_iface) {
	case FI_HMEM_SYSTEM:
		medium_max = EFA_DEFAULT_INTER_MAX_MEDIUM_MESSAGE_SIZE;
		read_min = EFA_DEFAULT_INTER_MIN_READ_MESSAGE_SIZE;
		read_write_min = EFA_DEFAULT_INTER_MIN_READ_WRITE_SIZE;
		break;
	case FI_HMEM_CUDA:
	case FI_HMEM_ROCR:
	case FI_HMEM_ZE:
	case FI_HMEM_NEURON:
		medium_max = 0;
		read_min =
			efa_rdm_proto_max_eager_msg_size_with_largest_header() + 1;
		read_write_min = read_min;
		break;
	case FI_HMEM_SYNAPSEAI:
		medium_max = 0;
		read_min = 0;
		read_write_min = 1;
		break;
	default:
		return -FI_EINVAL;
	}

	efa_rdm_proto_set_bounds(&efa_rdm_proto_eager, target_iface, 0, SIZE_MAX);
	efa_rdm_proto_set_bounds(&efa_rdm_proto_medium, target_iface, 0, medium_max);
	efa_rdm_proto_set_bounds(&efa_rdm_proto_runtread, target_iface, read_min, SIZE_MAX);
	efa_rdm_proto_set_bounds(&efa_rdm_proto_longread, target_iface, read_min, SIZE_MAX);
	efa_rdm_proto_set_bounds(&efa_rdm_proto_longcts, target_iface, 0, SIZE_MAX);
	efa_rdm_proto_set_bounds(&efa_rdm_proto_eager_write, target_iface, 0, SIZE_MAX);
	efa_rdm_proto_set_bounds(&efa_rdm_proto_longread_write, target_iface, read_write_min, SIZE_MAX);
	efa_rdm_proto_set_bounds(&efa_rdm_proto_longcts_write, target_iface, 0, SIZE_MAX);
	efa_rdm_proto_set_bounds(&efa_rdm_proto_short_rtr, target_iface, 0, SIZE_MAX);
	efa_rdm_proto_set_bounds(&efa_rdm_proto_longcts_rtr, target_iface, 0, SIZE_MAX);

	if (policy_iface == FI_HMEM_SYNAPSEAI)
		return FI_SUCCESS;

	efa_rdm_proto_apply_bound_override(&efa_rdm_proto_eager, target_iface,
		&efa_env.inter_max_eager_message_size,
		efa_rdm_proto_eager.set_max_bound);
	if (policy_iface == FI_HMEM_SYSTEM)
		efa_rdm_proto_apply_bound_override(&efa_rdm_proto_medium, target_iface,
			&efa_env.inter_max_medium_message_size,
			efa_rdm_proto_medium.set_max_bound);
	efa_rdm_proto_apply_bound_override(&efa_rdm_proto_runtread, target_iface,
		&efa_env.inter_min_read_message_size,
		efa_rdm_proto_runtread.set_min_bound);
	efa_rdm_proto_apply_bound_override(&efa_rdm_proto_longread, target_iface,
		&efa_env.inter_min_read_message_size,
		efa_rdm_proto_longread.set_min_bound);
	efa_rdm_proto_apply_bound_override(&efa_rdm_proto_eager_write, target_iface,
		&efa_env.inter_max_eager_write_size,
		efa_rdm_proto_eager_write.set_max_bound);
	efa_rdm_proto_apply_bound_override(&efa_rdm_proto_longread_write, target_iface,
		&efa_env.inter_min_read_write_size,
		efa_rdm_proto_longread_write.set_min_bound);

	return FI_SUCCESS;
}

/**
 * @brief Undo the memory registrations the selection loop made.
 *
 * efa_rdm_ope_try_fill_desc() registers the source buffer so a read based
 * protocol can be evaluated. Hand those registrations back when no protocol
 * can carry the operation.
 */
static void efa_rdm_proto_release_selection_mrs(struct efa_rdm_ope *txe)
{
	int i, err;

	for (i = 0; i < txe->iov_count; ++i) {
		if (!txe->mr[i])
			continue;

		err = fi_close((struct fid *) txe->mr[i]);
		if (OFI_UNLIKELY(err))
			EFA_WARN(FI_LOG_EP_DATA,
				 "mr dereg failed during protocol selection. err=%d\n",
				 err);

		txe->mr[i] = NULL;
		txe->desc[i] = NULL;
	}
}

/* List of supported protocols.
 * The protocols listed here will be tried in the order they're listed.
 * The first protocol that can be used for the TX operation will be used.
 *
 * Only efa_rdm_proto_select_send_protocol() below walks this array, so it is
 * internal to this file and its length is whatever the initializer holds.
 */
static struct efa_rdm_proto * const efa_rdm_protocols[] = {
	&efa_rdm_proto_eager,
	&efa_rdm_proto_medium,
	&efa_rdm_proto_runtread,
	&efa_rdm_proto_longread,
	/* Long CTS takes any send the others above decline, so it is last. */
	&efa_rdm_proto_longcts,
};

/*
 * Emulated write protocols, tried in order during selection.
 */
static struct efa_rdm_proto * const efa_rdm_emulated_write_protocols[] = {
	&efa_rdm_proto_eager_write,
	&efa_rdm_proto_longread_write,
	&efa_rdm_proto_longcts_write,
};

/*
 * Emulated read protocols, tried in order during selection.
 */
static struct efa_rdm_proto * const efa_rdm_emulated_read_protocols[] = {
	&efa_rdm_proto_short_rtr,
	&efa_rdm_proto_longcts_rtr,
};

void efa_rdm_proto_handle_receipt_recv(struct efa_rdm_pke *pkt_entry)
{
	struct efa_rdm_receipt_hdr *receipt_hdr =
		efa_rdm_pke_get_receipt_hdr(pkt_entry);
	struct efa_rdm_ope *txe =
		efa_rdm_ep_live_txe_from_id(pkt_entry->ep, receipt_hdr->tx_id);

	if (!txe) {
		EFA_INFO(FI_LOG_CQ,
			 "RECEIPT names a send that is no longer live, dropping it\n");
		efa_rdm_pke_release_rx(pkt_entry);
		return;
	}

	/* Write send completion immediately to preserve DC semantics. */
	efa_rdm_txe_report_completion(txe);

	if (txe->state == EFA_RDM_OPE_SEND)
		dlist_remove(&txe->entry);

	/*
	 * The TXE is released either here or when the request/CTSDATA packet's
	 * send completes, whichever happens last.
	 */
	txe->internal_flags |= EFA_RDM_TXE_REMOTE_ACK_RECEIVED;
	if (efa_rdm_txe_with_remote_ack_ready_for_release(txe))
		efa_rdm_txe_release(txe);

	efa_rdm_pke_release_rx(pkt_entry);
}

void efa_rdm_proto_handle_eor_recv(struct efa_rdm_pke *pkt_entry)
{
	struct efa_rdm_eor_hdr *eor_hdr =
		(struct efa_rdm_eor_hdr *) pkt_entry->wiredata;
	struct efa_rdm_ope *txe =
		efa_rdm_ep_live_txe_from_id(pkt_entry->ep, eor_hdr->send_id);

	if (!txe) {
		EFA_INFO(FI_LOG_CQ,
			 "EOR names a send that is no longer live, dropping it\n");
		efa_rdm_pke_release_rx(pkt_entry);
		return;
	}

	efa_rdm_txe_release_read_msg_slot(txe);

	txe->bytes_acked += txe->total_len - txe->bytes_runt;
	if (txe->bytes_acked == txe->total_len) {
		efa_rdm_txe_report_completion(txe);
		txe->internal_flags |= EFA_RDM_TXE_REMOTE_ACK_RECEIVED;
		if (efa_rdm_txe_with_remote_ack_ready_for_release(txe))
			efa_rdm_txe_release(txe);
	}

	efa_rdm_pke_release_rx(pkt_entry);
}

void efa_rdm_proto_txe_init_buffers(struct efa_rdm_ep *ep,
						  const struct fi_msg *msg,
						  struct efa_rdm_ope *txe)
{
	txe->ep = ep;
	txe->iov_count = msg->iov_count;
	memcpy(txe->iov, msg->msg_iov, sizeof(struct iovec) * msg->iov_count);
	memset(txe->mr, 0, sizeof(*txe->mr) * msg->iov_count);

	efa_rdm_mr_gen_init_ope_desc(txe);
	if (msg->desc) {
		memcpy(txe->desc, msg->desc, sizeof(*msg->desc) * msg->iov_count);
		efa_rdm_mr_gen_capture_in_ope_desc(txe);
	} else {
		memset(txe->desc, 0, sizeof(*txe->desc) * msg->iov_count);
	}
	txe->total_len = ofi_total_iov_len(msg->msg_iov, msg->iov_count);
}

int efa_rdm_proto_select_send_protocol(struct efa_rdm_ep *ep,
				       struct efa_rdm_peer *peer,
				       const struct fi_msg *msg, uint32_t op,
				       uint64_t flags, struct efa_rdm_ope *txe,
				       struct efa_rdm_proto **proto)
{
	struct efa_rdm_proto *selected_proto;
	int req_pkt_type, iface, err;
	bool use_p2p, mr_attempted = false;
	uint16_t header_flags = 0;
	uint64_t effective_flags, mr_access_flags;

	/*
	 * efa_rdm_msg_generic_send() sends a peer that only accepts headerless
	 * packets straight to the zero-copy protocol, so every protocol
	 * considered here writes a REQ header.
	 */
	assert(!efa_rdm_peer_expects_zero_hdr_data_transfer(peer));

	/*
	 * Resolve the endpoint's tx_op_flags now: an endpoint-level
	 * FI_DELIVERY_COMPLETE must steer protocol selection just as a
	 * per-operation flag does.
	 */
	effective_flags = efa_rdm_msg_get_tx_flags(ep, flags);

	efa_rdm_proto_txe_init_buffers(ep, msg, txe);

	/* can_use_protocol() reads the peer off the txe, before txe_fill runs. */
	txe->peer = peer;

	/*
	 * The predicates need the effective flags: the runt read protocol has no
	 * delivery complete REQ variant, so it has to rule out a
	 * FI_DELIVERY_COMPLETE send. efa_rdm_txe_construct_common() assigns the
	 * same value again later, from the same helper.
	 */
	txe->fi_flags = effective_flags;

	iface = (msg->desc && msg->desc[0]) ?
			((struct efa_mr *) msg->desc[0])->iface :
			FI_HMEM_SYSTEM;

	/*
	 * The read based protocols can only be used when the device can access
	 * the source buffer directly, so resolve p2p availability once here
	 * instead of in each predicate. A negative return means the transfer
	 * cannot be performed at all.
	 */
	err = efa_rdm_ep_use_p2p_for_mr(ep, txe->desc[0]);
	if (err < 0)
		return err;
	use_p2p = err;
	mr_access_flags = FI_SEND | (use_p2p ? FI_REMOTE_READ : 0);
	header_flags = efa_rdm_proto_req_header_flags(peer, effective_flags);

	/* Synapse AI only supports long read */
	if (iface == FI_HMEM_SYNAPSEAI) {
		selected_proto = &efa_rdm_proto_longread;
		req_pkt_type = efa_rdm_proto_req_pkt_type(
			selected_proto, op, effective_flags, peer);

		if ((effective_flags & FI_INJECT) ||
		    !efa_rdm_proto_msg_size_in_bounds(
			    selected_proto, iface, txe->total_len))
			goto no_match;

		if (efa_is_cache_available(efa_rdm_ep_rdm_domain(ep)))
			efa_rdm_ope_try_fill_desc(txe, 0, mr_access_flags);

		*proto = selected_proto;
		txe->proto = selected_proto;
		txe->req_pkt_type = req_pkt_type;
		return FI_SUCCESS;
	}

	for (int i = 0; i < ARRAY_SIZE(efa_rdm_protocols); ++i) {
		selected_proto = efa_rdm_protocols[i];

		if (!efa_rdm_proto_msg_size_in_bounds(
			    selected_proto, iface, txe->total_len))
			continue;

		if ((effective_flags & FI_INJECT) &&
		    selected_proto != &efa_rdm_proto_eager)
			continue;

		req_pkt_type = efa_rdm_proto_req_pkt_type(
			selected_proto, op, effective_flags, peer);

		/* If a protocol can benefit from memory registration, attempt
		 * to register the memory.
		 *
		 * Ask for FI_REMOTE_READ whenever p2p is available, because
		 * read based protocols require it.
		 *
		 * TODO: Move efa_rdm_ope_try_fill_desc to efa_rdm_proto.c
		 */
		if (!mr_attempted && selected_proto->wants_mr) {

			if (efa_is_cache_available(efa_rdm_ep_rdm_domain(ep)))
				efa_rdm_ope_try_fill_desc(txe, 0, mr_access_flags);
			mr_attempted = true;
		}

		if (selected_proto->can_use_protocol(txe, req_pkt_type,
						     header_flags, iface,
						     use_p2p)) {
			*proto = selected_proto;
			txe->proto = selected_proto;
			txe->req_pkt_type = req_pkt_type;
			EFA_DBG(FI_LOG_EP_DATA,
				"Selected the %s protocol for a %zu byte send\n",
				selected_proto->name, txe->total_len);
			return FI_SUCCESS;
		}
	}

no_match:
	if (mr_attempted)
		efa_rdm_proto_release_selection_mrs(txe);

	*proto = NULL;
	txe->proto = NULL;
	return -FI_EOPNOTSUPP;
}

/* Utility funcions */

void efa_rdm_proto_txe_fill(struct efa_rdm_ope *txe, struct efa_rdm_ep *ep,
			    struct efa_rdm_peer *peer, const struct fi_msg *msg,
			    uint32_t op, uint64_t tag, uint64_t flags,
			    uint32_t internal_flags, struct efa_rdm_proto *proto)
{
	/*
	 * txe->mr, txe->desc and the MR generation snapshot were already
	 * populated by efa_rdm_proto_select_send_protocol(), which needs them
	 * to decide whether a protocol can be used, so use the construct
	 * helper that leaves them alone.
	 */
	efa_rdm_txe_construct_common(txe, ep, peer, msg, op, flags,
				     internal_flags);

	if (op == ofi_op_tagged) {
		txe->cq_entry.tag = tag;
		txe->tag = tag;
	}
}

void efa_rdm_proto_select_emulated_write_protocol(struct efa_rdm_ep *ep,
						  struct efa_rdm_peer *peer,
						  struct efa_rdm_ope *txe,
						  bool use_p2p,
						  struct efa_rdm_proto **proto)
{
	struct efa_rdm_proto *selected_proto;
	uint16_t header_flags = 0;
	int req_pkt_type, iface, i;
	bool mr_attempted = false;

	iface = txe->desc[0] ?
			((struct efa_mr *) txe->desc[0])->iface :
			FI_HMEM_SYSTEM;

	if (efa_rdm_peer_need_raw_addr_hdr(peer))
		header_flags |= EFA_RDM_REQ_OPT_RAW_ADDR_HDR;
	else if (efa_rdm_peer_need_connid(peer))
		header_flags |= EFA_RDM_PKT_CONNID_HDR;

	if (txe->fi_flags & FI_REMOTE_CQ_DATA)
		header_flags |= EFA_RDM_REQ_OPT_CQ_DATA_HDR;

	for (i = 0; i < ARRAY_SIZE(efa_rdm_emulated_write_protocols); ++i) {
		selected_proto = efa_rdm_emulated_write_protocols[i];

		if (!efa_rdm_proto_msg_size_in_bounds(
			    selected_proto, iface, txe->total_len))
			continue;

		if ((txe->fi_flags & FI_INJECT) &&
		    selected_proto != &efa_rdm_proto_eager_write)
			continue;

		req_pkt_type = efa_rdm_proto_req_pkt_type(
			selected_proto, txe->op, txe->fi_flags, peer);

		/*
		 * Register the source buffer for a protocol that benefits from
		 * it, asking for FI_REMOTE_READ when p2p is available because
		 * the read based protocols require it.
		 */
		if (!mr_attempted && selected_proto->wants_mr) {
			uint64_t access = FI_SEND |
					  (use_p2p ? FI_REMOTE_READ : 0);

			if (efa_is_cache_available(efa_rdm_ep_rdm_domain(ep)))
				efa_rdm_ope_try_fill_desc(txe, 0, access);
			mr_attempted = true;
		}

		if (selected_proto->can_use_protocol(txe, req_pkt_type,
						     header_flags, iface,
						     use_p2p)) {
			*proto = selected_proto;
			txe->proto = selected_proto;
			txe->req_pkt_type = req_pkt_type;
			return;
		}
	}

	/*
	 * No emulated write protocol matched. This is not expected; the caller
	 * reports an error.
	 */
	*proto = NULL;
	txe->proto = NULL;
}

void efa_rdm_proto_select_emulated_read_protocol(struct efa_rdm_ep *ep,
						 struct efa_rdm_peer *peer,
						 struct efa_rdm_ope *txe,
						 struct efa_rdm_proto **proto)
{
	struct efa_rdm_proto *selected_proto;
	uint16_t header_flags = 0;
	int req_pkt_type, iface, i;

	iface = txe->desc[0] ?
			((struct efa_mr *) txe->desc[0])->iface :
			FI_HMEM_SYSTEM;

	if (efa_rdm_peer_need_raw_addr_hdr(peer))
		header_flags |= EFA_RDM_REQ_OPT_RAW_ADDR_HDR;
	else if (efa_rdm_peer_need_connid(peer))
		header_flags |= EFA_RDM_PKT_CONNID_HDR;

	if (txe->fi_flags & FI_REMOTE_CQ_DATA)
		header_flags |= EFA_RDM_REQ_OPT_CQ_DATA_HDR;

	for (i = 0; i < ARRAY_SIZE(efa_rdm_emulated_read_protocols); ++i) {
		selected_proto = efa_rdm_emulated_read_protocols[i];

		if (!efa_rdm_proto_msg_size_in_bounds(
			    selected_proto, iface, txe->total_len))
			continue;

		req_pkt_type = efa_rdm_proto_req_pkt_type(
			selected_proto, txe->op, txe->fi_flags, peer);

		if (selected_proto->can_use_protocol(txe, req_pkt_type,
						     header_flags, iface,
						     false /* use_p2p */)) {
			*proto = selected_proto;
			txe->proto = selected_proto;
			txe->req_pkt_type = req_pkt_type;
			return;
		}
	}

	/*
	 * No emulated read protocol matched. The caller reports an error.
	 */
	*proto = NULL;
	txe->proto = NULL;
}
