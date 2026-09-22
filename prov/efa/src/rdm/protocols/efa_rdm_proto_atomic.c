/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Amazon.com, Inc. or its affiliates. All rights reserved. */

#include "efa_rdm_proto_atomic.h"
#include <ofi_proto.h>
#include "efa_rdm_ep.h"
#include "efa_rdm_ope.h"
#include "efa_rdm_peer.h"
#include "efa_rdm_pke_rta.h"

static int efa_rdm_proto_atomic_req_pkt_type(struct efa_rdm_ope *txe)
{
	const struct efa_rdm_atomic_proto *proto = txe->atomic_proto;

	assert(proto);

	return (txe->fi_flags & FI_DELIVERY_COMPLETE) ?
	       proto->req_pkt_type_dc : proto->req_pkt_type;
}

static void
efa_rdm_proto_atomic_write_handle_send_completion(struct efa_rdm_pke *pkt_entry)
{
	struct efa_rdm_ope *txe = pkt_entry->ope;

	assert(txe);
	assert(txe->op == ofi_op_atomic);

	if (txe->internal_flags & EFA_RDM_TXE_DELIVERY_COMPLETE_REQUESTED) {
		/*
		 * RECEIPT reports the application completion as soon as the
		 * destination has applied the atomic. The send completion only
		 * controls when the TXE can be released.
		 */
		if (efa_rdm_txe_with_remote_ack_ready_for_release(txe))
			efa_rdm_txe_release(txe);
	} else {
		efa_rdm_ope_handle_send_completed(txe);
	}

	efa_rdm_pke_release_tx(pkt_entry);
}

static size_t
efa_rdm_proto_atomic_write_get_req_data_size(struct efa_rdm_ope *txe)
{
	return txe->total_len;
}

static ssize_t
efa_rdm_proto_atomic_write_init_req_pke(struct efa_rdm_pke *pkt_entry,
					struct efa_rdm_ope *txe)
{
	if (txe->req_pkt_type == EFA_RDM_DC_WRITE_RTA_PKT)
		return efa_rdm_pke_init_dc_write_rta(pkt_entry, txe);

	assert(txe->req_pkt_type == EFA_RDM_WRITE_RTA_PKT);
	return efa_rdm_pke_init_write_rta(pkt_entry, txe);
}

const struct efa_rdm_atomic_proto efa_rdm_atomic_write_proto = {
	.req_pkt_type = EFA_RDM_WRITE_RTA_PKT,
	.req_pkt_type_dc = EFA_RDM_DC_WRITE_RTA_PKT,
	.get_req_data_size =
		&efa_rdm_proto_atomic_write_get_req_data_size,
	.init_req_pke = &efa_rdm_proto_atomic_write_init_req_pke,
	.handle_send_completion =
		&efa_rdm_proto_atomic_write_handle_send_completion,
};

static ssize_t efa_rdm_proto_atomic_construct_tx_pke(struct efa_rdm_ope *txe)
{
	struct efa_rdm_ep *ep = txe->ep;
	struct efa_rdm_pke *pkt_entry;
	size_t max_payload, req_data_size;
	ssize_t err;

	assert(txe->atomic_proto);
	assert(txe->atomic_proto->get_req_data_size);
	assert(txe->atomic_proto->init_req_pke);

	if (efa_rdm_ep_get_available_tx_pkts(ep) == 0)
		return -FI_EAGAIN;

	max_payload = efa_rdm_txe_max_req_data_capacity(
		ep, txe, txe->req_pkt_type);
	req_data_size = txe->atomic_proto->get_req_data_size(txe);
	/*
	 * Each atomic protocol reports its request data size. WRITE carries
	 * only the operand; a future COMPARE protocol can include compare data
	 * as well. An RTA request must fit in one packet.
	 */
	if (req_data_size > max_payload)
		return -FI_ETRUNC;

	pkt_entry = efa_rdm_pke_alloc(ep, ep->efa_tx_pkt_pool,
				      EFA_RDM_PKE_FROM_EFA_TX_POOL);
	if (OFI_UNLIKELY(!pkt_entry))
		return -FI_EAGAIN;

	err = txe->atomic_proto->init_req_pke(pkt_entry, txe);
	if (OFI_UNLIKELY(err)) {
		efa_rdm_pke_release_tx(pkt_entry);
		return err;
	}

	pkt_entry->handle_pke = txe->atomic_proto->handle_send_completion;
	ep->send_pkt_entry_vec[0] = pkt_entry;
	ep->send_pkt_entry_vec_size = 1;
	return FI_SUCCESS;
}

ssize_t efa_rdm_proto_atomic_post(struct efa_rdm_ope *txe)
{
	struct efa_rdm_ep *ep = txe->ep;
	ssize_t err;

	assert(txe->atomic_proto);

	txe->req_pkt_type = efa_rdm_proto_atomic_req_pkt_type(txe);
	err = efa_rdm_proto_atomic_construct_tx_pke(txe);
	if (err)
		return err;

	err = efa_rdm_pke_sendv(ep->send_pkt_entry_vec, 1, 0);
	if (OFI_UNLIKELY(err)) {
		/*
		 * A post failure means the packet was not handed to the device,
		 * so this layer still owns it. The caller retains ownership of
		 * the TXE and rolls back msg_id.
		 */
		efa_rdm_pke_release_tx(ep->send_pkt_entry_vec[0]);
		return err;
	}

	txe->peer->flags |= EFA_RDM_PEER_REQ_SENT;
	return FI_SUCCESS;
}
