/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Amazon.com, Inc. or its affiliates. All
 * rights reserved. */

#include "efa_unit_tests.h"
#include "rdm/efa_rdm_pke_nonreq.h"
#include "rdm/efa_rdm_pke_rta.h"
#include "rdm/efa_rdm_pke_utils.h"
#include "rdm/efa_rdm_proto.h"
#include "rdm/protocols/efa_rdm_proto_atomic.h"
#include "rdm/protocols/efa_rdm_proto_eager.h"
#include "rdm/protocols/efa_rdm_proto_zero_copy.h"

/**
 * @brief Helper to set up an endpoint, peer, and TXE for protocol selection
 * tests.
 *
 * Returns the efa_rdm_ep. Caller must provide a peer_addr output and a
 * pre-allocated txe pointer output.
 */
static struct efa_rdm_ep *setup_proto_select_test(struct efa_resource *resource,
						  fi_addr_t *peer_addr)
{
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(raw_addr);
	struct efa_rdm_ep *ep;

	efa_unit_test_resource_construct_rdm_shm_disabled(resource);

	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	assert_int_equal(
		fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len), 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	assert_int_equal(
		fi_av_insert(resource->av, &raw_addr, 1, peer_addr, 0, NULL),
		1);

	return ep;
}

static size_t atomic_bufpool_free_count(struct ofi_bufpool *pool)
{
	struct slist_entry *item;
	size_t count = 0;

	for (item = pool->free_list.entries.head; item; item = item->next)
		count++;

	return count;
}

static void
construct_atomic_write_msg(struct fi_msg_atomic *msg, struct fi_ioc *ioc,
			   struct fi_rma_ioc *rma_ioc, void **desc,
			   void *buf, size_t count, fi_addr_t peer_addr)
{
	ioc->addr = buf;
	ioc->count = count;
	rma_ioc->addr = 0x1000;
	rma_ioc->count = count;
	rma_ioc->key = 0x1234;

	memset(msg, 0, sizeof(*msg));
	msg->msg_iov = ioc;
	msg->desc = desc;
	msg->iov_count = 1;
	msg->addr = peer_addr;
	msg->rma_iov = rma_ioc;
	msg->rma_iov_count = 1;
	msg->datatype = FI_UINT8;
	msg->op = FI_ATOMIC_WRITE;
}

static struct efa_rdm_pke *
post_atomic_write(struct efa_resource *resource,
		  struct efa_unit_test_buff *send_buff, size_t size,
		  uint8_t fill, uint64_t flags,
		  struct efa_rdm_ep **ep_out, struct efa_rdm_peer **peer_out)
{
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct fi_msg_atomic msg;
	struct fi_ioc ioc;
	struct fi_rma_ioc rma_ioc;
	void *desc;
	fi_addr_t peer_addr;
	int ret;

	ep = setup_proto_select_test(resource, &peer_addr);
	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	efa_unit_test_buff_construct(send_buff, resource, size);
	memset(send_buff->buff, fill, send_buff->size);

	desc = fi_mr_desc(send_buff->mr);
	construct_atomic_write_msg(&msg, &ioc, &rma_ioc, &desc,
				   send_buff->buff, send_buff->size, peer_addr);

	g_efa_unit_test_mocks.efa_qp_post_send =
		&efa_mock_efa_qp_post_send_return_mock;
	will_return_int_always(efa_mock_efa_qp_post_send_return_mock, 0);

	ret = fi_atomicmsg(resource->ep, &msg, flags);
	assert_int_equal(ret, 0);
	assert_int_equal(ep->send_pkt_entry_vec_size, 1);

	*ep_out = ep;
	if (peer_out)
		*peer_out = peer;
	return ep->send_pkt_entry_vec[0];
}

static struct efa_rdm_pke *
alloc_atomic_receipt(struct efa_rdm_ep *ep, struct efa_rdm_peer *peer,
		     uint32_t tx_id)
{
	struct efa_rdm_pke *receipt_pkt;
	struct efa_rdm_receipt_hdr *receipt_hdr;

	receipt_pkt = efa_rdm_pke_alloc(ep, ep->rx_unexp_pkt_pool,
					EFA_RDM_PKE_FROM_UNEXP_POOL);
	assert_non_null(receipt_pkt);
	receipt_pkt->peer = peer;
	receipt_hdr = efa_rdm_pke_get_receipt_hdr(receipt_pkt);
	receipt_hdr->type = EFA_RDM_RECEIPT_PKT;
	receipt_hdr->tx_id = tx_id;
	return receipt_pkt;
}

void test_proto_atomic_write_constructs_callback_pke(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_ope *txe;
	struct efa_rdm_pke *pkt_entry;
	struct efa_rdm_rta_hdr *rta_hdr;
	size_t hdr_size;

	pkt_entry = post_atomic_write(resource, &send_buff, 64, 0x5a,
				      FI_COMPLETION, &ep, NULL);
	txe = pkt_entry->ope;
	rta_hdr = efa_rdm_pke_get_rta_hdr(pkt_entry);
	hdr_size = efa_rdm_pke_get_req_hdr_size(pkt_entry);

	assert_ptr_equal(txe->atomic_proto, &efa_rdm_atomic_write_proto);
	assert_non_null(pkt_entry->handle_pke);
	assert_int_equal(rta_hdr->type, EFA_RDM_WRITE_RTA_PKT);
	assert_int_equal(rta_hdr->msg_id, txe->msg_id);
	assert_int_equal(rta_hdr->atomic_datatype, FI_UINT8);
	assert_int_equal(rta_hdr->atomic_op, FI_ATOMIC_WRITE);
	assert_int_equal(rta_hdr->rma_iov_count, 1);
	assert_int_equal(rta_hdr->rma_iov[0].addr, 0x1000);
	assert_int_equal(rta_hdr->rma_iov[0].len, send_buff.size);
	assert_int_equal(rta_hdr->rma_iov[0].key, 0x1234);
	assert_memory_equal(pkt_entry->wiredata + hdr_size,
			    send_buff.buff, send_buff.size);

	efa_rdm_pke_handle_send_completion(pkt_entry);
	assert_int_equal(efa_unit_test_get_ope_list_length(ep, EFA_RDM_TXE),
			 0);

	efa_unit_test_buff_destruct(&send_buff);
}

static void test_proto_atomic_dc_completion_order_common(
	struct efa_resource *resource, bool send_first)
{
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope *txe;
	struct efa_rdm_pke *req_pkt;
	struct efa_rdm_pke *receipt_pkt;
	struct fi_cq_tagged_entry cq_entry;

	req_pkt = post_atomic_write(resource, &send_buff, 32, 0,
				    FI_DELIVERY_COMPLETE | FI_COMPLETION,
				    &ep, &peer);
	txe = req_pkt->ope;
	assert_int_equal(efa_rdm_pkt_type_of(req_pkt),
			 EFA_RDM_DC_WRITE_RTA_PKT);
	assert_int_equal(efa_rdm_pke_get_rta_hdr(req_pkt)->send_id,
			 txe->tx_id);
	receipt_pkt = alloc_atomic_receipt(
		ep, peer, efa_rdm_pke_get_rta_hdr(req_pkt)->send_id);

	if (send_first) {
		efa_rdm_pke_handle_send_completion(req_pkt);
		assert_int_equal(fi_cq_read(resource->cq, &cq_entry, 1),
				 -FI_EAGAIN);
		efa_rdm_pke_handle_receipt_recv(receipt_pkt);
	} else {
		efa_rdm_pke_handle_receipt_recv(receipt_pkt);
		assert_true(txe->internal_flags &
			    EFA_RDM_TXE_REMOTE_ACK_RECEIVED);
		assert_int_equal(fi_cq_read(resource->cq, &cq_entry, 1), 1);
		efa_rdm_pke_handle_send_completion(req_pkt);
	}

	assert_int_equal(efa_unit_test_get_ope_list_length(ep, EFA_RDM_TXE),
			 0);
	if (send_first)
		assert_int_equal(fi_cq_read(resource->cq, &cq_entry, 1), 1);
	assert_int_equal(fi_cq_read(resource->cq, &cq_entry, 1), -FI_EAGAIN);

	efa_unit_test_buff_destruct(&send_buff);
}

void test_proto_atomic_dc_send_first(void **state)
{
	test_proto_atomic_dc_completion_order_common(*state, true);
}

void test_proto_atomic_dc_receipt_first(void **state)
{
	test_proto_atomic_dc_completion_order_common(*state, false);
}

void test_proto_atomic_dc_receipt_then_send_error_reports_success(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope *txe;
	struct efa_rdm_pke *req_pkt;
	struct efa_rdm_pke *receipt_pkt;
	struct fi_cq_tagged_entry cq_entry;
	struct fi_cq_err_entry err_entry = {0};

	req_pkt = post_atomic_write(resource, &send_buff, 32, 0,
				    FI_DELIVERY_COMPLETE | FI_COMPLETION,
				    &ep, &peer);
	txe = req_pkt->ope;
	receipt_pkt = alloc_atomic_receipt(
		ep, peer, efa_rdm_pke_get_rta_hdr(req_pkt)->send_id);

	efa_rdm_pke_handle_receipt_recv(receipt_pkt);
	assert_int_equal(fi_cq_read(resource->cq, &cq_entry, 1), 1);
	assert_int_equal(efa_unit_test_get_ope_list_length(ep, EFA_RDM_TXE),
			 1);

	efa_rdm_pke_handle_tx_error(
		req_pkt, EFA_IO_COMP_STATUS_LOCAL_ERROR_UNREACH_REMOTE);

	assert_int_equal(efa_unit_test_get_ope_list_length(ep, EFA_RDM_TXE),
			 0);
	assert_int_equal(fi_cq_read(resource->cq, &cq_entry, 1), -FI_EAGAIN);
	assert_int_equal(fi_cq_readerr(resource->cq, &err_entry, 0),
			 -FI_EAGAIN);

	efa_unit_test_buff_destruct(&send_buff);
}

static void test_proto_atomic_single_packet_boundary_common(
	struct efa_resource *resource, uint64_t flags, int pkt_type)
{
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope *txe;
	struct efa_rdm_pke *pkt_entry;
	struct efa_rdm_pke *receipt_pkt;
	struct fi_msg_atomic msg;
	struct fi_ioc ioc;
	struct fi_rma_ioc rma_ioc;
	struct fi_cq_tagged_entry cq_entry;
	void *desc;
	fi_addr_t peer_addr;
	uint16_t header_flags = 0;
	uint32_t next_msg_id;
	size_t max_payload;
	int ret;

	ep = setup_proto_select_test(resource, &peer_addr);
	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	if (efa_rdm_peer_need_raw_addr_hdr(peer))
		header_flags |= EFA_RDM_REQ_OPT_RAW_ADDR_HDR;
	else if (efa_rdm_peer_need_connid(peer))
		header_flags |= EFA_RDM_PKT_CONNID_HDR;

	max_payload = ep->mtu_size -
		efa_rdm_pkt_type_get_req_hdr_size(pkt_type,
						  header_flags, 1);
	efa_unit_test_buff_construct(&send_buff, resource, max_payload + 1);
	desc = fi_mr_desc(send_buff.mr);

	g_efa_unit_test_mocks.efa_qp_post_send =
		&efa_mock_efa_qp_post_send_return_mock;
	will_return_int_always(efa_mock_efa_qp_post_send_return_mock, 0);

	construct_atomic_write_msg(&msg, &ioc, &rma_ioc, &desc,
				   send_buff.buff, max_payload, peer_addr);
	ret = fi_atomicmsg(resource->ep, &msg, flags | FI_COMPLETION);
	assert_int_equal(ret, 0);
	pkt_entry = ep->send_pkt_entry_vec[0];
	txe = pkt_entry->ope;
	assert_int_equal(efa_rdm_pkt_type_of(pkt_entry), pkt_type);
	if (flags & FI_DELIVERY_COMPLETE)
		receipt_pkt = alloc_atomic_receipt(ep, peer, txe->tx_id);
	efa_rdm_pke_handle_send_completion(pkt_entry);
	if (flags & FI_DELIVERY_COMPLETE)
		efa_rdm_pke_handle_receipt_recv(receipt_pkt);
	assert_int_equal(fi_cq_read(resource->cq, &cq_entry, 1), 1);

	next_msg_id = peer->next_msg_id;
	construct_atomic_write_msg(&msg, &ioc, &rma_ioc, &desc,
				   send_buff.buff, max_payload + 1, peer_addr);
	ret = fi_atomicmsg(resource->ep, &msg, flags | FI_COMPLETION);
	assert_int_equal(ret, -FI_ETRUNC);
	assert_int_equal(peer->next_msg_id, next_msg_id);
	assert_int_equal(efa_unit_test_get_ope_list_length(ep, EFA_RDM_TXE),
			 0);

	efa_unit_test_buff_destruct(&send_buff);
}

void test_proto_atomic_single_packet_boundary(void **state)
{
	test_proto_atomic_single_packet_boundary_common(
		*state, 0, EFA_RDM_WRITE_RTA_PKT);
}

void test_proto_atomic_dc_single_packet_boundary(void **state)
{
	test_proto_atomic_single_packet_boundary_common(
		*state, FI_DELIVERY_COMPLETE, EFA_RDM_DC_WRITE_RTA_PKT);
}

void test_proto_atomic_post_failure_rolls_back_msg_id(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct fi_msg_atomic msg;
	struct fi_ioc ioc;
	struct fi_rma_ioc rma_ioc;
	void *desc;
	fi_addr_t peer_addr;
	uint32_t next_msg_id;
	size_t tx_pkt_free_before;
	size_t txe_free_before;
	int ret;

	ep = setup_proto_select_test(resource, &peer_addr);
	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	efa_unit_test_buff_construct(&send_buff, resource, 16);
	desc = fi_mr_desc(send_buff.mr);
	construct_atomic_write_msg(&msg, &ioc, &rma_ioc, &desc,
				   send_buff.buff, send_buff.size, peer_addr);
	next_msg_id = peer->next_msg_id;
	tx_pkt_free_before = atomic_bufpool_free_count(ep->efa_tx_pkt_pool);
	txe_free_before = atomic_bufpool_free_count(ep->base_ep.txe_pool);

	g_efa_unit_test_mocks.efa_qp_post_send =
		&efa_mock_efa_qp_post_send_return_mock;
	will_return_int(efa_mock_efa_qp_post_send_return_mock, ENOMEM);

	ret = fi_atomicmsg(resource->ep, &msg, 0);
	assert_int_equal(ret, -FI_EAGAIN);
	assert_int_equal(peer->next_msg_id, next_msg_id);
	assert_int_equal(efa_unit_test_get_ope_list_length(ep, EFA_RDM_TXE),
			 0);
	assert_int_equal(atomic_bufpool_free_count(ep->efa_tx_pkt_pool),
			 tx_pkt_free_before);
	assert_int_equal(atomic_bufpool_free_count(ep->base_ep.txe_pool),
			 txe_free_before);

	efa_unit_test_buff_destruct(&send_buff);
}

void test_proto_atomic_inject_copies_operand(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope *txe;
	struct efa_rdm_pke *pkt_entry;
	struct efa_rdm_pke *receipt_pkt;
	struct fi_cq_tagged_entry cq_entry;
	fi_addr_t peer_addr;
	uint8_t operand[16];
	uint8_t expected[sizeof(operand)];
	size_t hdr_size;
	int ret;

	ep = setup_proto_select_test(resource, &peer_addr);
	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	ep->base_ep.util_ep.tx_op_flags |= FI_DELIVERY_COMPLETE;
	memset(operand, 0xa5, sizeof(operand));
	memcpy(expected, operand, sizeof(expected));

	g_efa_unit_test_mocks.efa_qp_post_send =
		&efa_mock_efa_qp_post_send_return_mock;
	will_return_int_always(efa_mock_efa_qp_post_send_return_mock, 0);

	ret = fi_inject_atomic(resource->ep, operand, sizeof(operand),
			       peer_addr, 0x1000, 0x1234,
			       FI_UINT8, FI_ATOMIC_WRITE);
	assert_int_equal(ret, 0);

	pkt_entry = ep->send_pkt_entry_vec[0];
	txe = pkt_entry->ope;
	hdr_size = efa_rdm_pke_get_req_hdr_size(pkt_entry);
	assert_int_equal(efa_rdm_pkt_type_of(pkt_entry),
			 EFA_RDM_DC_WRITE_RTA_PKT);
	assert_true(txe->internal_flags & EFA_RDM_TXE_NO_COMPLETION);
	assert_true(txe->internal_flags &
		    EFA_RDM_TXE_DELIVERY_COMPLETE_REQUESTED);

	memset(operand, 0, sizeof(operand));
	assert_memory_equal(pkt_entry->wiredata + hdr_size,
			    expected, sizeof(expected));

	receipt_pkt = alloc_atomic_receipt(ep, peer, txe->tx_id);
	efa_rdm_pke_handle_send_completion(pkt_entry);
	assert_int_equal(efa_unit_test_get_ope_list_length(ep, EFA_RDM_TXE),
			 1);
	efa_rdm_pke_handle_receipt_recv(receipt_pkt);
	assert_int_equal(efa_unit_test_get_ope_list_length(ep, EFA_RDM_TXE),
			 0);
	assert_int_equal(fi_cq_read(resource->cq, &cq_entry, 1), -FI_EAGAIN);
}

void test_proto_atomic_rnr_retry_preserves_callback(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_ope *txe;
	struct efa_rdm_pke *pkt_entry;
	void (*handle_pke)(struct efa_rdm_pke *);
	int ret;

	pkt_entry = post_atomic_write(resource, &send_buff, 16, 0,
				      FI_COMPLETION, &ep, NULL);
	txe = pkt_entry->ope;
	handle_pke = pkt_entry->handle_pke;

	efa_rdm_pke_handle_tx_error(
		pkt_entry, EFA_IO_COMP_STATUS_REMOTE_ERROR_RNR);
	assert_true(txe->internal_flags & EFA_RDM_OPE_QUEUED_RNR);
	assert_ptr_equal(pkt_entry->handle_pke, handle_pke);
	assert_ptr_equal(pkt_entry->ope, txe);

	ret = efa_rdm_ope_process_queued_ope(txe);
	assert_int_equal(ret, 0);
	assert_false(txe->internal_flags & EFA_RDM_OPE_QUEUED_RNR);
	assert_ptr_equal(pkt_entry->handle_pke, handle_pke);

	efa_rdm_pke_handle_send_completion(pkt_entry);
	assert_int_equal(efa_unit_test_get_ope_list_length(ep, EFA_RDM_TXE),
			 0);

	efa_unit_test_buff_destruct(&send_buff);
}

void test_proto_atomic_non_rnr_error_releases_txe(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_pke *pkt_entry;
	struct fi_cq_data_entry cq_entry;
	struct fi_cq_err_entry cq_err_entry = {0};

	pkt_entry = post_atomic_write(resource, &send_buff, 16, 0,
				      FI_COMPLETION, &ep, NULL);

	efa_rdm_pke_handle_tx_error(
		pkt_entry, EFA_IO_COMP_STATUS_LOCAL_ERROR_UNREACH_REMOTE);

	assert_int_equal(efa_unit_test_get_ope_list_length(ep, EFA_RDM_TXE),
			 0);
	assert_int_equal(fi_cq_read(resource->cq, &cq_entry, 1), -FI_EAVAIL);
	assert_int_equal(fi_cq_readerr(resource->cq, &cq_err_entry, 0), 1);
	assert_int_equal(cq_err_entry.prov_errno,
			 EFA_IO_COMP_STATUS_LOCAL_ERROR_UNREACH_REMOTE);

	efa_unit_test_buff_destruct(&send_buff);
}

/**
 * @brief Test that eager protocol is selected for small messages.
 */
void test_proto_select_eager_for_small_msg(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope *txe;
	struct efa_rdm_proto *proto = NULL;
	fi_addr_t peer_addr;
	struct fi_msg msg = {0};
	struct iovec iov;

	ep = setup_proto_select_test(resource, &peer_addr);
	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;

	iov.iov_base = NULL;
	iov.iov_len = 64; /* Small message, fits in eager */
	efa_unit_test_construct_msg(&msg, &iov, 1, peer_addr, NULL, 0, NULL);

	txe = ofi_buf_alloc(ep->base_ep.txe_pool);
	assert_non_null(txe);

	efa_rdm_proto_select_send_protocol(ep, peer, &msg, ofi_op_msg, 0, txe,
					   &proto);
	assert_non_null(proto);
	assert_ptr_equal(proto, &efa_rdm_proto_eager);

	ofi_buf_free(txe);
}

/**
 * @brief Test that zero-length messages select eager protocol.
 */
void test_proto_select_eager_for_zero_len_msg(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope *txe;
	struct efa_rdm_proto *proto = NULL;
	fi_addr_t peer_addr;
	struct fi_msg msg = {0};
	struct iovec iov;

	ep = setup_proto_select_test(resource, &peer_addr);
	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;

	iov.iov_base = NULL;
	iov.iov_len = 0;
	efa_unit_test_construct_msg(&msg, &iov, 1, peer_addr, NULL, 0, NULL);

	txe = ofi_buf_alloc(ep->base_ep.txe_pool);
	assert_non_null(txe);

	efa_rdm_proto_select_send_protocol(ep, peer, &msg, ofi_op_msg, 0, txe,
					   &proto);
	assert_ptr_equal(proto, &efa_rdm_proto_eager);

	ofi_buf_free(txe);
}

/**
 * @brief Test that eager construct_tx_pkes produces exactly 1 PKE with
 * the correct callback set.
 */
void test_proto_eager_construct_pkes_single_pke(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope *txe;
	struct efa_rdm_proto *proto = NULL;
	fi_addr_t peer_addr;
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(raw_addr);
	struct fi_msg msg = {0};
	struct iovec iov;
	int err;
	uint64_t pke_send_flags;

	efa_unit_test_resource_construct_rdm_shm_disabled(resource);
	efa_unit_test_buff_construct(&send_buff, resource, 64);

	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	assert_int_equal(
		fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len), 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	assert_int_equal(
		fi_av_insert(resource->av, &raw_addr, 1, &peer_addr, 0, NULL),
		1);

	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;

	iov.iov_base = send_buff.buff;
	iov.iov_len = send_buff.size;
	efa_unit_test_construct_msg(&msg, &iov, 1, peer_addr, NULL, 0,
				    (void **) &send_buff.mr);

	txe = ofi_buf_alloc(ep->base_ep.txe_pool);
	assert_non_null(txe);

	/*
	 * Fill the TXE as generic_send would before calling construct_tx_pkes.
	 * The selection step is what populates txe->req_pkt_type, txe->desc[]
	 * and the MR generation snapshot, all of which construct_tx_pkes()
	 * reads, so run it rather than setting up a subset by hand: a freshly
	 * allocated txe_pool slot holds whatever its previous occupant left
	 * behind.
	 */
	efa_rdm_proto_select_send_protocol(ep, peer, &msg, ofi_op_msg, 0, txe,
					   &proto);
	assert_ptr_equal(proto, &efa_rdm_proto_eager);
	efa_rdm_proto_txe_fill(txe, ep, peer, &msg, ofi_op_msg, 0, 0, 0, proto);
	txe->msg_id = peer->next_msg_id++;

	err = efa_rdm_proto_eager.construct_tx_pkes(ep, txe, &pke_send_flags);
	assert_int_equal(err, 0);
	assert_int_equal(ep->send_pkt_entry_vec_size, 1);
	assert_non_null(ep->send_pkt_entry_vec[0]);
	assert_non_null(ep->send_pkt_entry_vec[0]->handle_pke);
	assert_ptr_equal(ep->send_pkt_entry_vec[0]->ope, txe);

	/* Clean up */
	efa_rdm_pke_release_tx(ep->send_pkt_entry_vec[0]);
	efa_rdm_txe_release(txe);
	efa_unit_test_buff_destruct(&send_buff);
}

/**
 * @brief Test that eager send completion callback releases TXE and PKE
 * for non-DC messages.
 */
void test_proto_eager_send_completion_releases_txe(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct efa_rdm_pke *pkt_entry;
	fi_addr_t peer_addr;
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(raw_addr);
	int err;

	efa_unit_test_resource_construct_rdm_shm_disabled(resource);
	efa_unit_test_buff_construct(&send_buff, resource, 64);

	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	assert_int_equal(
		fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len), 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	assert_int_equal(
		fi_av_insert(resource->av, &raw_addr, 1, &peer_addr, 0, NULL),
		1);

	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;

	/* Mock efa_qp_post_send to succeed */
	g_efa_unit_test_mocks.efa_qp_post_send =
		&efa_mock_efa_qp_post_send_return_mock;
	will_return_int_maybe(efa_mock_efa_qp_post_send_return_mock, 0);

	/* Send a message via fi_send which goes through the new code path */
	err = fi_send(resource->ep, send_buff.buff, send_buff.size,
		      fi_mr_desc(send_buff.mr), peer_addr, NULL);
	assert_int_equal(err, 0);
	assert_int_equal(efa_unit_test_get_ope_list_length(ep, EFA_RDM_TXE), 1);

	/* Get the TXE and PKE */
	pkt_entry = ep->send_pkt_entry_vec[0];
	assert_non_null(pkt_entry->handle_pke);

	/* Simulate send completion: record_tx_op_completed + callback */
	efa_rdm_ep_record_tx_op_completed(ep, pkt_entry);
	pkt_entry->handle_pke(pkt_entry);

	/* TXE should be released */
	assert_int_equal(efa_unit_test_get_ope_list_length(ep, EFA_RDM_TXE), 0);

	efa_unit_test_buff_destruct(&send_buff);
}

/**
 * @brief Test that eager assigns msg_id from peer->next_msg_id.
 */
void test_proto_eager_assigns_msg_id(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	fi_addr_t peer_addr;
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(raw_addr);
	uint32_t initial_msg_id;
	int err;

	efa_unit_test_resource_construct_rdm_shm_disabled(resource);
	efa_unit_test_buff_construct(&send_buff, resource, 64);

	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	assert_int_equal(
		fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len), 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	assert_int_equal(
		fi_av_insert(resource->av, &raw_addr, 1, &peer_addr, 0, NULL),
		1);

	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	initial_msg_id = peer->next_msg_id;

	g_efa_unit_test_mocks.efa_qp_post_send =
		&efa_mock_efa_qp_post_send_return_mock;
	will_return_int_maybe(efa_mock_efa_qp_post_send_return_mock, 0);

	err = fi_send(resource->ep, send_buff.buff, send_buff.size,
		      fi_mr_desc(send_buff.mr), peer_addr, NULL);
	assert_int_equal(err, 0);

	/* msg_id should have been assigned and next_msg_id incremented */
	struct efa_rdm_ope *txe =
		efa_unit_test_get_first_ope(ep, EFA_RDM_TXE);
	assert_int_equal(txe->msg_id, initial_msg_id);
	assert_int_equal(peer->next_msg_id, initial_msg_id + 1);

	efa_unit_test_buff_destruct(&send_buff);
}

/**
 * @brief Test that a send is queued before handshake and dequeued after
 * handshake completes when the peer may have zero-copy mode enabled.
 */
void test_proto_eager_queue_dequeue_handshake(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope *txe;
	fi_addr_t peer_addr;
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(raw_addr);
	struct fi_cq_tagged_entry cq_entry;
	int ret;

	efa_unit_test_resource_construct_rdm_shm_disabled(resource);
	efa_unit_test_buff_construct(&send_buff, resource, 64);

	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	assert_int_equal(
		fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len), 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	assert_int_equal(
		fi_av_insert(resource->av, &raw_addr, 1, &peer_addr, 0, NULL),
		1);

	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags &= ~EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	ep->peer_may_have_zcpy_rx = true;

	/*
	 * The handshake trigger and the queued send's repost each post at
	 * least once, and the progress engine may retry, so let every post
	 * succeed rather than queueing a fixed number of return values. The
	 * mock reads its value with mock_int(), so it needs the int variant.
	 */
	g_efa_unit_test_mocks.efa_qp_post_send = &efa_mock_efa_qp_post_send_return_mock;
	will_return_int_always(efa_mock_efa_qp_post_send_return_mock, 0);

	ret = fi_send(resource->ep, send_buff.buff, send_buff.size,
		      fi_mr_desc(send_buff.mr), peer_addr, NULL);
	assert_int_equal(ret, 0);

	/* Verify the OPE is in the queued list */
	assert_int_equal(ep->ope_queued_before_handshake_cnt, 1);
	txe = container_of(ep->ope_queued_list.next,
			   struct efa_rdm_ope, queued_entry);
	assert_true(dlist_entry_in_list(&txe->queued_entry,
					&ep->ope_queued_list));

	/* Simulate handshake received */
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;

	/* Progress via fi_cq_read which calls efa_domain_progress */
	ret = fi_cq_read(resource->cq, &cq_entry, 1);
	assert_int_equal(ret, -FI_EAGAIN);

	/* Verify the OPE was dequeued and sent */
	assert_int_equal(ep->ope_queued_before_handshake_cnt, 0);
	assert_true(dlist_empty(&ep->ope_queued_list));

	efa_unit_test_buff_destruct(&send_buff);
}

/**
 * @brief An eager txe queued before the handshake survives the MR generation
 *        check when it is reposted.
 *
 * efa_rdm_ope_process_queued_ope() gates the repost on
 * efa_rdm_mr_gen_check_ope(), which asserts the dispatch-time snapshot in
 * ope->desc_gen[] was initialized and compares it against the live MR
 * generation. The refactored TXE setup must take that snapshot, exactly as
 * efa_rdm_txe_construct() does; otherwise the check reads a stale value from
 * the recycled ope pool slot and cancels a healthy transfer with a spurious
 * FI_EFA_ERR_PEER_ABORTED.
 */
void test_proto_eager_queued_before_handshake_survives_mr_gen_check(
	void **state)
{
	struct efa_resource *resource = *state;
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope *txe;
	struct fi_cq_tagged_entry cq_entry;
	struct fi_cq_err_entry err_entry;
	fi_addr_t peer_addr;
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(raw_addr);
	unsigned int i;
	int ret;

	efa_unit_test_resource_construct_rdm_shm_disabled(resource);
	efa_unit_test_buff_construct(&send_buff, resource, 64);

	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	assert_int_equal(
		fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len), 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	assert_int_equal(
		fi_av_insert(resource->av, &raw_addr, 1, &peer_addr, 0, NULL),
		1);

	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags &= ~EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	ep->peer_may_have_zcpy_rx = true;

	/* Let every post succeed; see test_proto_eager_queue_dequeue_handshake. */
	g_efa_unit_test_mocks.efa_qp_post_send =
		&efa_mock_efa_qp_post_send_return_mock;
	will_return_int_always(efa_mock_efa_qp_post_send_return_mock, 0);

	/* The send is queued because the handshake has not arrived yet. */
	ret = fi_send(resource->ep, send_buff.buff, send_buff.size,
		      fi_mr_desc(send_buff.mr), peer_addr, NULL);
	assert_int_equal(ret, 0);
	assert_int_equal(ep->ope_queued_before_handshake_cnt, 1);

	txe = container_of(ep->ope_queued_list.next, struct efa_rdm_ope,
			   queued_entry);

	/*
	 * The MR generation snapshot must be populated, and must match the
	 * live generation of every source MR -- the transfer is healthy.
	 */
	for (i = 0; i < txe->iov_count; i++) {
		if (!txe->desc[i])
			break;
		assert_true(efa_rdm_mr_gen_value_is_valid(txe->desc_gen[i]));
	}
	assert_true(efa_rdm_mr_gen_check_ope(txe));

	/* Handshake arrives; the queued send is reposted, not canceled. */
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;

	ret = fi_cq_read(resource->cq, &cq_entry, 1);
	assert_int_equal(ret, -FI_EAGAIN);

	assert_int_equal(ep->ope_queued_before_handshake_cnt, 0);
	assert_true(dlist_empty(&ep->ope_queued_list));

	/* No error completion: the repost must not be mistaken for an abort. */
	memset(&err_entry, 0, sizeof(err_entry));
	assert_int_equal(fi_cq_readerr(resource->cq, &err_entry, 0), -FI_EAGAIN);

	efa_unit_test_buff_destruct(&send_buff);
}

/**
 * @brief A failure inside the eager construct_tx_pkes rolls peer->next_msg_id
 *        back by exactly one.
 *
 * The txe is owned by efa_rdm_msg_generic_send(), which releases it and rolls
 * back the msg_id when posting fails. construct_tx_pkes() must therefore not
 * do the same cleanup itself: doing so releases a pooled txe twice and
 * decrements next_msg_id twice, desynchronizing the per-peer message sequence
 * the receiver's reorder window depends on.
 */
void test_proto_eager_construct_pkes_failure_rolls_back_msg_id(
	void **state)
{
	struct efa_resource *resource = *state;
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	fi_addr_t peer_addr;
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(raw_addr);
	uint32_t initial_msg_id;
	int ret;

	efa_unit_test_resource_construct_rdm_shm_disabled(resource);
	efa_unit_test_buff_construct(&send_buff, resource, 64);

	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	assert_int_equal(
		fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len), 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	assert_int_equal(
		fi_av_insert(resource->av, &raw_addr, 1, &peer_addr, 0, NULL),
		1);

	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	initial_msg_id = peer->next_msg_id;

	/* Fail the device post so the send path takes its error branch. */
	g_efa_unit_test_mocks.efa_qp_post_send =
		&efa_mock_efa_qp_post_send_return_mock;
	will_return_int_maybe(efa_mock_efa_qp_post_send_return_mock, -FI_ENOMEM);

	ret = fi_send(resource->ep, send_buff.buff, send_buff.size,
		      fi_mr_desc(send_buff.mr), peer_addr, NULL);
	assert_int_not_equal(ret, 0);

	/* Rolled back exactly once, and the txe was released exactly once. */
	assert_int_equal(peer->next_msg_id, initial_msg_id);
	assert_int_equal(efa_unit_test_get_ope_list_length(ep, EFA_RDM_TXE), 0);

	efa_unit_test_buff_destruct(&send_buff);
}

/**
 * @brief Test that the zero-copy protocol's construct_tx_pkes produces a
 * headerless PKE with the EFA_RDM_PKE_SEND_TO_USER_RECV_QP flag.
 */
void test_proto_zero_copy_construct_pkes(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope *txe;
	fi_addr_t peer_addr;
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(raw_addr);
	struct fi_msg msg = {0};
	struct iovec iov;
	int err;
	uint64_t pke_send_flags;

	efa_unit_test_resource_construct_rdm_shm_disabled(resource);
	efa_unit_test_buff_construct(&send_buff, resource, 64);

	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	assert_int_equal(
		fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len), 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	assert_int_equal(
		fi_av_insert(resource->av, &raw_addr, 1, &peer_addr, 0, NULL),
		1);

	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	/* Mark peer as expecting zero-copy transfer */
	peer->extra_info[0] |= EFA_RDM_EXTRA_FEATURE_REQUEST_USER_RECV_QP;
	peer->user_recv_qp.qpn = 99;
	peer->user_recv_qp.qkey = 0xABCD;

	iov.iov_base = send_buff.buff;
	iov.iov_len = send_buff.size;
	efa_unit_test_construct_msg(&msg, &iov, 1, peer_addr, NULL, 0,
				    (void **) &send_buff.mr);

	txe = ofi_buf_alloc(ep->base_ep.txe_pool);
	assert_non_null(txe);

	/* Fill TXE as generic_send would before calling construct_tx_pkes */
	efa_rdm_proto_txe_init_buffers(ep, &msg, txe);
	efa_rdm_proto_txe_fill(txe, ep, peer, &msg, ofi_op_msg, 0, 0, 0,
			       &efa_rdm_proto_zero_copy);
	txe->msg_id = peer->next_msg_id++;

	err = efa_rdm_proto_zero_copy.construct_tx_pkes(ep, txe,
							&pke_send_flags);
	assert_int_equal(err, 0);
	assert_int_equal(ep->send_pkt_entry_vec_size, 1);

	/* Verify headerless packet properties */
	struct efa_rdm_pke *pke = ep->send_pkt_entry_vec[0];
	assert_true(pke->flags & EFA_RDM_PKE_SEND_TO_USER_RECV_QP);
	assert_true(pke->flags & EFA_RDM_PKE_HAS_NO_BASE_HDR);
	assert_int_equal(pke->pkt_size, 64);
	assert_int_equal(pke->payload_size, 64);

	ofi_buf_free(pke);
	efa_unit_test_buff_destruct(&send_buff);
}

/**
 * @brief A send queued before the handshake switches to the zero-copy protocol
 *        when the handshake reveals a headerless peer.
 *
 * Whether a peer accepts only headerless packets is carried in its handshake,
 * so a send dispatched before the handshake arrived selected a protocol that
 * writes a REQ header. The repost must revisit that choice; otherwise the
 * headered packet lands on the peer's control QP, which a peer in zero-copy
 * receive mode rejects.
 */
void test_proto_zero_copy_reselected_after_handshake(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_unit_test_buff send_buff;
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope *txe;
	struct efa_rdm_pke *pkt_entry;
	struct fi_cq_tagged_entry cq_entry;
	fi_addr_t peer_addr;
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(raw_addr);
	int ret;

	efa_unit_test_resource_construct_rdm_shm_disabled(resource);
	efa_unit_test_buff_construct(&send_buff, resource, 64);

	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	assert_int_equal(
		fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len), 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	assert_int_equal(
		fi_av_insert(resource->av, &raw_addr, 1, &peer_addr, 0, NULL),
		1);

	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags &= ~EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	ep->peer_may_have_zcpy_rx = true;

	/* Let every post succeed; see test_proto_eager_queue_dequeue_handshake. */
	g_efa_unit_test_mocks.efa_qp_post_send =
		&efa_mock_efa_qp_post_send_return_mock;
	will_return_int_always(efa_mock_efa_qp_post_send_return_mock, 0);

	/* The send is queued because the handshake has not arrived yet. */
	ret = fi_send(resource->ep, send_buff.buff, send_buff.size,
		      fi_mr_desc(send_buff.mr), peer_addr, NULL);
	assert_int_equal(ret, 0);
	assert_int_equal(ep->ope_queued_before_handshake_cnt, 1);

	txe = container_of(ep->ope_queued_list.next, struct efa_rdm_ope,
			   queued_entry);
	assert_ptr_equal(txe->proto, &efa_rdm_proto_eager);

	/* The handshake arrives and reports the peer is in zero-copy mode. */
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer->extra_info[0] |= EFA_RDM_EXTRA_FEATURE_REQUEST_USER_RECV_QP;
	peer->user_recv_qp.qpn = 99;
	peer->user_recv_qp.qkey = 0xABCD;

	ret = fi_cq_read(resource->cq, &cq_entry, 1);
	assert_int_equal(ret, -FI_EAGAIN);

	assert_int_equal(ep->ope_queued_before_handshake_cnt, 0);
	assert_true(dlist_empty(&ep->ope_queued_list));

	/* The reposted packet is headerless and bound for the user_recv_qp. */
	assert_ptr_equal(txe->proto, &efa_rdm_proto_zero_copy);
	pkt_entry = ep->send_pkt_entry_vec[0];
	assert_ptr_equal(pkt_entry->ope, txe);
	assert_true(pkt_entry->flags & EFA_RDM_PKE_SEND_TO_USER_RECV_QP);
	assert_true(pkt_entry->flags & EFA_RDM_PKE_HAS_NO_BASE_HDR);

	efa_unit_test_buff_destruct(&send_buff);
}
