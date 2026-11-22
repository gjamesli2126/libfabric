/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Amazon.com, Inc. or its affiliates. All
 * rights reserved. */

#include "efa_unit_tests.h"
#include "rdm/protocols/efa_rdm_proto_eager_rma.h"
#include "efa_rdm_rma.h"
#include "efa_rdm_pke_nonreq.h"
#include "efa_rdm_cq.h"

static bool test_efa_rdm_rma_should_write_using_rdma_helper(
		struct efa_rdm_ep *ep, struct efa_rdm_peer *peer,
		struct efa_rdm_ope *txe, bool use_device_rdma,
		bool homogeneous_peers, bool use_p2p,
		bool use_unsolicited_write_recv)
{
	uint32_t efa_device_caps_orig;
	bool result;

	efa_device_caps_orig = g_efa_selected_device_list[0].device_caps;
	g_efa_selected_device_list[0].device_caps |= EFADV_DEVICE_ATTR_CAPS_RDMA_WRITE;

	ep->use_device_rdma = use_device_rdma;
	ep->homogeneous_peers = homogeneous_peers;
	if (use_device_rdma)
		ep->extra_info[0] |= EFA_RDM_EXTRA_FEATURE_RDMA_WRITE;
	if (use_unsolicited_write_recv)
		ep->extra_info[0] |= EFA_RDM_EXTRA_FEATURE_UNSOLICITED_WRITE_RECV;

	result = efa_rdm_rma_should_write_using_rdma(ep, txe, peer, use_p2p);

	g_efa_selected_device_list[0].device_caps = efa_device_caps_orig;
	return result;
}

/**
 * @brief Test that FI_REMOTE_CQ_DATA with multiple IOVs returns false
 *
 * When FI_REMOTE_CQ_DATA is set and iov_count > 1, the function should
 * return false to use send emulation instead of RDMA write.
 */
void test_efa_rdm_rma_should_write_using_rdma_remote_cq_data_multiple_iovs_returns_false(
		void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_peer peer = {0};
	struct efa_rdm_ope txe = {0};
	bool result;
	struct efa_rdm_ep *ep;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);
	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid.fid);

	peer.flags = EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer.extra_info[0] = EFA_RDM_EXTRA_FEATURE_RDMA_WRITE;
	txe.fi_flags = FI_REMOTE_CQ_DATA;
	txe.iov_count = 2;
	txe.rma_iov_count = 1;

	result = test_efa_rdm_rma_should_write_using_rdma_helper(
			ep, &peer, &txe, true, true, true, true);
	assert_false(result);
}

/**
 * @brief Test that FI_REMOTE_CQ_DATA with multiple RMA IOVs returns false
 *
 * When FI_REMOTE_CQ_DATA is set and rma_iov_count > 1, the function should
 * return false to use send emulation instead of RDMA write.
 */
void test_efa_rdm_rma_should_write_using_rdma_remote_cq_data_multiple_rma_iovs_returns_false(
		void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_peer peer = {0};
	struct efa_rdm_ope txe = {0};
	bool result;
	struct efa_rdm_ep *ep;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);
	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid.fid);

	peer.flags = EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer.extra_info[0] = EFA_RDM_EXTRA_FEATURE_RDMA_WRITE;
	txe.fi_flags = FI_REMOTE_CQ_DATA;
	txe.iov_count = 1;
	txe.rma_iov_count = 2;

	result = test_efa_rdm_rma_should_write_using_rdma_helper(
			ep, &peer, &txe, true, true, true, true);
	assert_false(result);
}

/**
 * @brief Test that use_device_rdma false returns false
 *
 * When use_device_rdma is false, the function should return false
 * regardless of other conditions.
 */
void test_efa_rdm_rma_should_write_using_rdma_use_device_rdma_false_returns_false(
		void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_peer peer = {0};
	struct efa_rdm_ope txe = {0};
	bool result;
	struct efa_rdm_ep *ep;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);
	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid.fid);

	peer.flags = EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer.extra_info[0] = EFA_RDM_EXTRA_FEATURE_RDMA_WRITE;
	txe.fi_flags = 0;
	txe.iov_count = 1;
	txe.rma_iov_count = 1;

	result = test_efa_rdm_rma_should_write_using_rdma_helper(
			ep, &peer, &txe, false, true, true, true);
	assert_false(result);
}

/**
 * @brief Test that peer without RDMA write support returns false
 *
 * When peer doesn't support RDMA write, the function should return false
 * even when other conditions are favorable.
 */
void test_efa_rdm_rma_should_write_using_rdma_peer_no_rdma_write_support_returns_false(
		void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_peer peer = {0};
	struct efa_rdm_ope txe = {0};
	bool result;
	struct efa_rdm_ep *ep;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);
	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid.fid);

	peer.flags = EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer.extra_info[0] = 0; // No RDMA write support
	txe.fi_flags = 0;
	txe.iov_count = 1;
	txe.rma_iov_count = 1;

	result = test_efa_rdm_rma_should_write_using_rdma_helper(
			ep, &peer, &txe, true, false, true, true);
	assert_false(result);
}

/**
 * @brief Test that lack of P2P support returns false
 *
 * When use_p2p is false, the function should return false regardless
 * of RDMA write support status.
 */
void test_efa_rdm_rma_should_write_using_rdma_no_p2p_support_returns_false(
		void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_peer peer = {0};
	struct efa_rdm_ope txe = {0};
	bool result;
	struct efa_rdm_ep *ep;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);
	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid.fid);

	peer.flags = EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer.extra_info[0] = EFA_RDM_EXTRA_FEATURE_RDMA_WRITE;
	txe.fi_flags = 0;
	txe.iov_count = 1;
	txe.rma_iov_count = 1;

	result = test_efa_rdm_rma_should_write_using_rdma_helper(
			ep, &peer, &txe, true, true, false, true);
	assert_false(result);
}

/**
 * @brief Test that P2P and RDMA write support returns true
 *
 * When use_p2p is true and both endpoint and peer support RDMA write,
 * the function should return true to use RDMA write.
 */
void test_efa_rdm_rma_should_write_using_rdma_p2p_and_rdma_write_support_returns_true(
		void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_peer peer = {0};
	struct efa_rdm_ope txe = {0};
	bool result;
	struct efa_rdm_ep *ep;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);
	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid.fid);

	peer.flags = EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer.extra_info[0] = EFA_RDM_EXTRA_FEATURE_RDMA_WRITE;
	txe.fi_flags = 0;
	txe.iov_count = 1;
	txe.rma_iov_count = 1;

	result = test_efa_rdm_rma_should_write_using_rdma_helper(
			ep, &peer, &txe, true, true, true, true);
	assert_true(result);
}

/**
 * @brief Test that FI_REMOTE_CQ_DATA with single IOVs and RDMA support returns true
 *
 * When FI_REMOTE_CQ_DATA is set with single IOV/RMA IOV and both P2P and
 * RDMA write are supported, the function should return true.
 */
void test_efa_rdm_rma_should_write_using_rdma_remote_cq_data_single_iovs_with_rdma_support(
		void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_peer peer = {0};
	struct efa_rdm_ope txe = {0};
	bool result;
	struct efa_rdm_ep *ep;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);
	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid.fid);

	peer.flags = EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer.extra_info[0] = EFA_RDM_EXTRA_FEATURE_RDMA_WRITE | EFA_RDM_EXTRA_FEATURE_UNSOLICITED_WRITE_RECV;
	txe.fi_flags = FI_REMOTE_CQ_DATA;
	txe.iov_count = 1;
	txe.rma_iov_count = 1;

	result = test_efa_rdm_rma_should_write_using_rdma_helper(
			ep, &peer, &txe, true, true, true, true);
	assert_true(result);
}

/**
 * @brief Test that FI_REMOTE_CQ_DATA with peer w/o unsolicited write recv return false
 *
 * When FI_REMOTE_CQ_DATA is set with single IOV/RMA IOV and both P2P and
 * RDMA write are supported, but peer doesn't support unsolicited write recv,
 * the function should return false.
 */
void test_efa_rdm_rma_should_write_using_rdma_unsolicited_write_recv_not_match(
		void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope txe = {0};
	bool result;
	struct efa_rdm_ep *ep;
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(struct efa_ep_addr);
	int err, num_addr;
	fi_addr_t peer_addr;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);
	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid.fid);
	/* create a fake peer */
	err = fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len);
	assert_int_equal(err, 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	num_addr = fi_av_insert(resource->av, &raw_addr, 1, &peer_addr, 0, NULL);
	assert_int_equal(num_addr, 1);

	/*
	 * Fake a peer that has made handshake and
	 * does not support unsolicited write recv
	 */
	peer = efa_rdm_ep_get_peer_explicit(ep, peer_addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer->extra_info[0] |= EFA_RDM_EXTRA_FEATURE_RDMA_WRITE;
	peer->extra_info[0] &= ~EFA_RDM_EXTRA_FEATURE_UNSOLICITED_WRITE_RECV;
	/* make sure shm is not used */
	peer->is_local = false;

	txe.fi_flags = FI_REMOTE_CQ_DATA;
	txe.iov_count = 1;
	txe.rma_iov_count = 1;

	result = test_efa_rdm_rma_should_write_using_rdma_helper(
			ep, peer, &txe, true, false, true, true);
	assert_false(result);
}

/* RDM 0-byte operation tests */
void test_efa_rdm_rma_read_0_byte_no_shm(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_ep *efa_rdm_ep;
	fi_addr_t addr;
	int ret;

	efa_unit_test_rdm_0byte_prep(resource, &addr);
	efa_rdm_ep = container_of(resource->ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid);

	assert_int_equal(efa_rdm_ep->efa_outstanding_tx_ops, 0);
	ret = fi_read(resource->ep, NULL, 0, NULL, addr, 0x87654321, 123456, NULL);
	assert_int_equal(ret, 0);
	assert_int_equal(efa_rdm_ep->efa_outstanding_tx_ops, 1);

}

void test_efa_rdm_rma_readv_0_byte_no_shm(void **state)
{
	struct efa_resource *resource = *state;
	fi_addr_t addr;
	struct iovec iov = {0};
	int ret;

	efa_unit_test_rdm_0byte_prep(resource, &addr);

	ret = fi_readv(resource->ep, &iov, NULL, 0, addr, 0x87654321, 123456, NULL);
	assert_int_equal(ret, 0);

}

void test_efa_rdm_rma_readmsg_0_byte_no_shm(void **state)
{
	struct efa_resource *resource = *state;
	fi_addr_t addr;
	struct iovec iov = {0};
	struct fi_msg_rma msg = {0};
	struct fi_rma_iov rma_iov = {.addr = 0x87654321, .len = 0, .key = 123456};
	int ret;

	efa_unit_test_rdm_0byte_prep(resource, &addr);

	efa_unit_test_construct_msg_rma(&msg, &iov, NULL, 0, addr, &rma_iov, 1, NULL, 0);

	ret = fi_readmsg(resource->ep, &msg, 0);
	assert_int_equal(ret, 0);

}

void test_efa_rdm_rma_write_0_byte_no_shm(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_ep *efa_rdm_ep;
	fi_addr_t addr;
	int ret;

	efa_unit_test_rdm_0byte_prep(resource, &addr);
	efa_rdm_ep = container_of(resource->ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid);

	assert_int_equal(efa_rdm_ep->efa_outstanding_tx_ops, 0);
	ret = fi_write(resource->ep, NULL, 0, NULL, addr, 0x87654321, 123456, NULL);
	assert_int_equal(ret, 0);
	assert_int_equal(efa_rdm_ep->efa_outstanding_tx_ops, 1);
}

void test_efa_rdm_rma_writev_0_byte_no_shm(void **state)
{
	struct efa_resource *resource = *state;
	fi_addr_t addr;
	struct iovec iov = {0};
	int ret;

	efa_unit_test_rdm_0byte_prep(resource, &addr);

	ret = fi_writev(resource->ep, &iov, NULL, 0, addr, 0x87654321, 123456, NULL);
	assert_int_equal(ret, 0);

}

void test_efa_rdm_rma_writemsg_0_byte_no_shm(void **state)
{
	struct efa_resource *resource = *state;
	fi_addr_t addr;
	struct iovec iov = {0};
	struct fi_msg_rma msg = {0};
	struct fi_rma_iov rma_iov = {.addr = 0x87654321, .len = 0, .key = 123456};
	int ret;

	efa_unit_test_rdm_0byte_prep(resource, &addr);

	efa_unit_test_construct_msg_rma(&msg, &iov, NULL, 0, addr, &rma_iov, 1, NULL, 0);

	ret = fi_writemsg(resource->ep, &msg, 0);
	assert_int_equal(ret, 0);

}

void test_efa_rdm_rma_writedata_0_byte_no_shm(void **state)
{
	struct efa_resource *resource = *state;
	fi_addr_t addr;
	int ret;

	efa_unit_test_rdm_0byte_prep(resource, &addr);

	ret = fi_writedata(resource->ep, NULL, 0, NULL, 0, addr, 0x87654321, 123456, NULL);
	assert_int_equal(ret, 0);

}

void test_efa_rdm_rma_inject_write_0_byte_no_shm(void **state)
{
	struct efa_resource *resource = *state;
	fi_addr_t addr;
	int ret;

	efa_unit_test_rdm_0byte_prep(resource, &addr);

	ret = fi_inject_write(resource->ep, NULL, 0, addr, 0x87654321, 123456);
	assert_int_equal(ret, 0);

}

void test_efa_rdm_rma_inject_writedata_0_byte_no_shm(void **state)
{
	struct efa_resource *resource = *state;
	fi_addr_t addr;
	int ret;

	efa_unit_test_rdm_0byte_prep(resource, &addr);

	ret = fi_inject_writedata(resource->ep, NULL, 0, 0, addr, 0x87654321, 123456);
	assert_int_equal(ret, 0);

}

void test_efa_rdm_rma_write_0_byte_with_inject_flag(void **state)
{
	struct efa_resource *resource = *state;
	struct iovec iov = {0};
	struct fi_msg_rma msg = {0};
	struct fi_rma_iov rma_iov = {.addr = 0x87654321, .len = 0, .key = 123456};
	fi_addr_t addr;
	int ret;

	efa_unit_test_rdm_0byte_prep(resource, &addr);

	efa_unit_test_construct_msg_rma(&msg, &iov, NULL, 0, addr, &rma_iov, 1, NULL, 0);

	ret = fi_writemsg(resource->ep, &msg, FI_INJECT);
	assert_int_equal(ret, 0);
}
/**
 * @brief Test that partial RDMA write post failure doesn't release the txe
 *
 * When efa_rdm_ope_post_remote_write() posts the first segment of a
 * multi-segment write successfully but fails on a subsequent segment,
 * the caller efa_rdm_rma_generic_writemsg() must not release the txe
 * because the in-flight segment still references it. Releasing it causes
 * a double-free when the completion arrives.
 *
 * This test forces a 2-segment write by shrinking max_rdma_size, then
 * saturates the tx queue so the second segment fails. It verifies that
 * the txe is NOT released before the in-flight completion arrives, and
 * IS released cleanly by the completion path (no leak).
 */
void test_efa_rdm_rma_post_remote_write_partial_fail_no_txe_release(
		void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_ep *efa_rdm_ep;
	struct efa_rdm_peer *peer;
	struct efa_unit_test_buff send_buff;
	struct iovec iov;
	struct fi_msg_rma msg = {0};
	struct fi_rma_iov rma_iov;
	struct efa_rdm_pke *inflight_pke;
	fi_addr_t addr;
	struct efa_ep_addr raw_addr;
	size_t raw_addr_len = sizeof(raw_addr);
	size_t max_rdma_size_orig;
	uint64_t wr_id;
	void *desc;
	int ret;

	/* Skip test on platforms that don't support RDMA write. */
	if (!efa_device_support_rdma_write()) {
		skip();
		return;
	}

	efa_unit_test_resource_construct_rdm_shm_disabled(resource);
	efa_rdm_ep = container_of(resource->ep, struct efa_rdm_ep,
				  base_ep.util_ep.ep_fid);

	/* Set up peer with RDMA write support */
	ret = fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len);
	assert_int_equal(ret, 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	ret = fi_av_insert(resource->av, &raw_addr, 1, &addr, 0, NULL);
	assert_int_equal(ret, 1);

	peer = efa_rdm_ep_get_peer_explicit(efa_rdm_ep, addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer->extra_info[0] |= EFA_RDM_EXTRA_FEATURE_RDMA_WRITE |
				EFA_RDM_EXTRA_FEATURE_RDMA_READ |
				EFA_RDM_EXTRA_FEATURE_UNSOLICITED_WRITE_RECV;

	/* Enable RDMA write and set device caps */
	efa_rdm_ep->use_device_rdma = true;

	/* Create a 128-byte registered buffer */
	efa_unit_test_buff_construct(&send_buff, resource, 128);
	desc = fi_mr_desc(send_buff.mr);

	/* Force 2-segment write: 128 bytes with max_rdma_size=64 */
	max_rdma_size_orig = efa_rdm_ep_domain(efa_rdm_ep)->device->max_rdma_size;
	efa_rdm_ep_domain(efa_rdm_ep)->device->max_rdma_size = 64;

	/* Mock: first segment's efa_qp_post_write succeeds */
	g_efa_unit_test_mocks.efa_qp_post_write = &efa_mock_efa_qp_post_write_return_mock;
	will_return(efa_mock_efa_qp_post_write_return_mock, 0);

	/*
	 * Leave room for exactly 1 tx op. The first segment will post and
	 * fill the queue, then the second segment hits -FI_EAGAIN.
	 */
	efa_rdm_ep->efa_outstanding_tx_ops = efa_rdm_ep->efa_max_outstanding_tx_ops - 1;

	iov.iov_base = send_buff.buff;
	iov.iov_len = 128;
	rma_iov.addr = 0x87654321;
	rma_iov.len = 128;
	rma_iov.key = 123456;
	efa_unit_test_construct_msg_rma(&msg, &iov, &desc, 1, addr,
					&rma_iov, 1, NULL, 0);

	assert_int_equal(g_ibv_submitted_wr_id_cnt, 0);
	ret = fi_writemsg(resource->ep, &msg, 0);
	assert_int_equal(ret, -FI_EAGAIN);
	assert_int_equal(g_ibv_submitted_wr_id_cnt, 1);

	/*
	 * With the fix, the txe must still be live because the first
	 * segment is in-flight (efa_outstanding_tx_ops > 0).
	 */
	assert_int_equal(efa_unit_test_get_ope_list_length(efa_rdm_ep, EFA_RDM_TXE), 1);
	assert_int_equal(efa_rdm_ep->efa_outstanding_tx_ops,
			 efa_rdm_ep->efa_max_outstanding_tx_ops);
	assert_int_equal(peer->efa_outstanding_tx_ops, 1);

	/*
	 * Drive the in-flight segment's completion through the real
	 * completion handler. Pre-fix, this dereferences the freed txe
	 * (SEGV/UAF). Post-fix, it releases the txe cleanly because the
	 * generic writemsg clamped bytes_write_total_len to match what
	 * was submitted.
	 */
	wr_id = (uint64_t) g_ibv_submitted_wr_id_vec[0];
	inflight_pke = efa_rdm_cq_get_pke_from_wr_id_solicited(wr_id);
	efa_rdm_ep_record_tx_op_completed(efa_rdm_ep, inflight_pke);
	efa_rdm_pke_handle_rma_completion(inflight_pke);

	/* The txe was released by the completion path. No leak. */
	assert_int_equal(efa_unit_test_get_ope_list_length(efa_rdm_ep, EFA_RDM_TXE), 0);
	assert_int_equal(efa_rdm_ep->efa_outstanding_tx_ops,
			 efa_rdm_ep->efa_max_outstanding_tx_ops - 1);
	assert_int_equal(peer->efa_outstanding_tx_ops, 0);
	assert_true(dlist_empty(&peer->outstanding_tx_pkts));

	/* Restore */
	efa_rdm_ep_domain(efa_rdm_ep)->device->max_rdma_size = max_rdma_size_orig;
	efa_unit_test_buff_destruct(&send_buff);
}

/**
 * @brief Reproduce the rma_pingpong double-free under app retry
 *
 * Mirrors the pingpong application pattern: after fi_writemsg returns
 * -FI_EAGAIN on a multi-segment write, the application retries the same
 * operation. A second fi_writemsg allocates a second txe while the first
 * txe's segment-1 packet is still in-flight. Pre-fix, the first txe is
 * already freed by the unconditional release in efa_rdm_rma_generic_writemsg,
 * so when segment 1 completes, efa_rdm_pke_handle_rma_completion dereferences
 * freed memory and eventually calls free() twice (double-free / UAF).
 *
 * This test is expected to SEGV or trip ASan / glibc tcache pre-fix, and
 * pass cleanly post-fix.
 */
void test_efa_rdm_rma_partial_post_retry_no_double_free(
		void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_ep *efa_rdm_ep;
	struct efa_rdm_peer *peer;
	struct efa_unit_test_buff send_buff;
	struct iovec iov;
	struct fi_msg_rma msg = {0};
	struct fi_rma_iov rma_iov;
	struct efa_rdm_pke *inflight_pke;
	fi_addr_t addr;
	struct efa_ep_addr raw_addr;
	size_t raw_addr_len = sizeof(raw_addr);
	size_t max_rdma_size_orig;
	uint64_t first_wr_id;
	void *desc;
	int ret;

	/* Skip test on platforms that don't support RDMA write. */
	if (!efa_device_support_rdma_write()) {
		skip();
		return;
	}

	efa_unit_test_resource_construct_rdm_shm_disabled(resource);
	efa_rdm_ep = container_of(resource->ep, struct efa_rdm_ep,
				  base_ep.util_ep.ep_fid);

	ret = fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len);
	assert_int_equal(ret, 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	ret = fi_av_insert(resource->av, &raw_addr, 1, &addr, 0, NULL);
	assert_int_equal(ret, 1);

	peer = efa_rdm_ep_get_peer_explicit(efa_rdm_ep, addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer->extra_info[0] |= EFA_RDM_EXTRA_FEATURE_RDMA_WRITE |
				EFA_RDM_EXTRA_FEATURE_RDMA_READ |
				EFA_RDM_EXTRA_FEATURE_UNSOLICITED_WRITE_RECV;

	efa_rdm_ep->use_device_rdma = true;

	efa_unit_test_buff_construct(&send_buff, resource, 128);
	desc = fi_mr_desc(send_buff.mr);

	/* Force 2-segment write: 128 bytes with max_rdma_size=64 */
	max_rdma_size_orig = efa_rdm_ep_domain(efa_rdm_ep)->device->max_rdma_size;
	efa_rdm_ep_domain(efa_rdm_ep)->device->max_rdma_size = 64;

	/*
	 * Saturate tx queue so every call's first segment posts and second
	 * segment hits -FI_EAGAIN. Each call leaves exactly 1 in-flight pkt
	 * but fails overall.
	 */
	efa_rdm_ep->efa_outstanding_tx_ops = efa_rdm_ep->efa_max_outstanding_tx_ops - 1;

	/* Only the first call posts — it will fill the queue to max. */
	g_efa_unit_test_mocks.efa_qp_post_write = &efa_mock_efa_qp_post_write_return_mock;
	will_return(efa_mock_efa_qp_post_write_return_mock, 0);

	iov.iov_base = send_buff.buff;
	iov.iov_len = 128;
	rma_iov.addr = 0x87654321;
	rma_iov.len = 128;
	rma_iov.key = 123456;
	efa_unit_test_construct_msg_rma(&msg, &iov, &desc, 1, addr,
					&rma_iov, 1, NULL, 0);

	/* First attempt: posts segment 1, fails on segment 2. */
	ret = fi_writemsg(resource->ep, &msg, 0);
	assert_int_equal(ret, -FI_EAGAIN);
	assert_int_equal(g_ibv_submitted_wr_id_cnt, 1);
	first_wr_id = (uint64_t) g_ibv_submitted_wr_id_vec[0];

	/*
	 * Simulate app retry BEFORE the in-flight completion arrives. The
	 * queue is still saturated so even segment 1 of the retry gets
	 * -FI_EAGAIN immediately (nothing posted). The retry must not
	 * disturb the first in-flight txe.
	 */
	ret = fi_writemsg(resource->ep, &msg, 0);
	assert_int_equal(ret, -FI_EAGAIN);
	assert_int_equal(g_ibv_submitted_wr_id_cnt, 1);
	/* txe1 must still be live; retry's txe2 was released inline. */
	assert_int_equal(efa_unit_test_get_ope_list_length(efa_rdm_ep, EFA_RDM_TXE), 1);
	assert_int_equal(peer->efa_outstanding_tx_ops, 1);

	/*
	 * Now drive the in-flight completion. Pre-fix, the first txe was
	 * freed and context_pkt_entry->ope points into freed memory — the
	 * handler's write to txe->bytes_write_completed and subsequent
	 * efa_rdm_txe_release trigger UAF / double-free.
	 */
	inflight_pke = efa_rdm_cq_get_pke_from_wr_id_solicited(first_wr_id);
	efa_rdm_ep_record_tx_op_completed(efa_rdm_ep, inflight_pke);
	efa_rdm_pke_handle_rma_completion(inflight_pke);

	/* Post-fix: both txes have been released; peer/ep state is clean. */
	assert_int_equal(efa_unit_test_get_ope_list_length(efa_rdm_ep, EFA_RDM_TXE), 0);
	assert_int_equal(peer->efa_outstanding_tx_ops, 0);
	assert_true(dlist_empty(&peer->outstanding_tx_pkts));

	efa_rdm_ep_domain(efa_rdm_ep)->device->max_rdma_size = max_rdma_size_orig;
	efa_unit_test_buff_destruct(&send_buff);
}

/**
 * @brief Test that partial RDMA read post failure doesn't release the txe
 *
 * Same bug pattern as the write test: efa_rdm_ope_post_read() can post
 * the first segment successfully then fail on a subsequent segment.
 * The caller efa_rdm_rma_generic_readmsg() must not release the txe
 * when segments are in-flight. The in-flight completion must release
 * the txe cleanly (no leak).
 */
void test_efa_rdm_rma_post_remote_read_partial_fail_no_txe_release(
		void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_ep *efa_rdm_ep;
	struct efa_rdm_peer *peer;
	struct efa_unit_test_buff recv_buff;
	struct iovec iov;
	struct fi_msg_rma msg = {0};
	struct fi_rma_iov rma_iov;
	struct efa_rdm_pke *inflight_pke;
	fi_addr_t addr;
	struct efa_ep_addr raw_addr;
	size_t raw_addr_len = sizeof(raw_addr);
	size_t max_rdma_size_orig;
	uint32_t vendor_part_id_orig;
	uint64_t wr_id;
	void *desc;
	int ret;

	/* Skip test on platforms that don't support RDMA read. */
	if (!efa_device_support_rdma_read()) {
		skip();
		return;
	}

	efa_unit_test_resource_construct_rdm_shm_disabled(resource);
	efa_rdm_ep = container_of(resource->ep, struct efa_rdm_ep,
				  base_ep.util_ep.ep_fid);

	/* Set up peer with RDMA read support */
	ret = fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len);
	assert_int_equal(ret, 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	ret = fi_av_insert(resource->av, &raw_addr, 1, &addr, 0, NULL);
	assert_int_equal(ret, 1);

	peer = efa_rdm_ep_get_peer_explicit(efa_rdm_ep, addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer->extra_info[0] |= EFA_RDM_EXTRA_FEATURE_RDMA_READ;
	/* Match device versions so efa_rdm_interop_rdma_read returns true */
	vendor_part_id_orig = g_efa_selected_device_list[0].ibv_attr.vendor_part_id;
	g_efa_selected_device_list[0].ibv_attr.vendor_part_id = 0xEFA1;
	peer->device_version = 0xEFA1;

	/* Enable RDMA read */
	efa_rdm_ep->use_device_rdma = true;

	/* Create a 128-byte registered buffer */
	efa_unit_test_buff_construct(&recv_buff, resource, 128);
	desc = fi_mr_desc(recv_buff.mr);

	/* Force 2-segment read: 128 bytes with max_rdma_size=64 */
	max_rdma_size_orig = efa_rdm_ep_domain(efa_rdm_ep)->device->max_rdma_size;
	efa_rdm_ep_domain(efa_rdm_ep)->device->max_rdma_size = 64;

	/* Mock: first segment's efa_qp_post_read succeeds */
	g_efa_unit_test_mocks.efa_qp_post_read = &efa_mock_efa_qp_post_read_return_mock;
	will_return(efa_mock_efa_qp_post_read_return_mock, 0);

	/*
	 * Leave room for exactly 1 tx op so the second segment fails.
	 */
	efa_rdm_ep->efa_outstanding_tx_ops = efa_rdm_ep->efa_max_outstanding_tx_ops - 1;

	iov.iov_base = recv_buff.buff;
	iov.iov_len = 128;
	rma_iov.addr = 0x87654321;
	rma_iov.len = 128;
	rma_iov.key = 123456;
	efa_unit_test_construct_msg_rma(&msg, &iov, &desc, 1, addr,
					&rma_iov, 1, NULL, 0);

	assert_int_equal(g_ibv_submitted_wr_id_cnt, 0);
	ret = fi_readmsg(resource->ep, &msg, 0);
	/*
	 * Restore the spoofed device state immediately so that a mid-test
	 * assertion failure below does not leak state into the next test.
	 * efa_rdm_interop_rdma_read() is only consulted during the protocol
	 * selection inside fi_readmsg(), so the spoofed values are no longer
	 * needed once it has returned.
	 */
	efa_rdm_ep_domain(efa_rdm_ep)->device->max_rdma_size = max_rdma_size_orig;
	g_efa_selected_device_list[0].ibv_attr.vendor_part_id = vendor_part_id_orig;

	assert_int_equal(ret, -FI_EAGAIN);
	assert_int_equal(g_ibv_submitted_wr_id_cnt, 1);

	/*
	 * With the fix, the txe must still be live because the first
	 * segment is in-flight.
	 */
	assert_int_equal(efa_unit_test_get_ope_list_length(efa_rdm_ep, EFA_RDM_TXE), 1);
	assert_int_equal(efa_rdm_ep->efa_outstanding_tx_ops,
			 efa_rdm_ep->efa_max_outstanding_tx_ops);
	assert_int_equal(peer->efa_outstanding_tx_ops, 1);

	/*
	 * Drive the in-flight segment's completion through the real
	 * completion handler. Pre-fix, this dereferences the freed txe
	 * (SEGV/UAF). Post-fix, it releases the txe cleanly because the
	 * generic readmsg clamped bytes_read_total_len to match what was
	 * submitted.
	 */
	wr_id = (uint64_t) g_ibv_submitted_wr_id_vec[0];
	inflight_pke = efa_rdm_cq_get_pke_from_wr_id_solicited(wr_id);
	efa_rdm_ep_record_tx_op_completed(efa_rdm_ep, inflight_pke);
	efa_rdm_pke_handle_rma_completion(inflight_pke);

	/* The txe was released by the completion path. No leak. */
	assert_int_equal(efa_unit_test_get_ope_list_length(efa_rdm_ep, EFA_RDM_TXE), 0);
	assert_int_equal(efa_rdm_ep->efa_outstanding_tx_ops,
			 efa_rdm_ep->efa_max_outstanding_tx_ops - 1);
	assert_int_equal(peer->efa_outstanding_tx_ops, 0);
	assert_true(dlist_empty(&peer->outstanding_tx_pkts));

	efa_unit_test_buff_destruct(&recv_buff);
}

/**
 * @brief Retry variant of the RDMA read partial-post test.
 *
 * Mirror of test_efa_rdm_rma_partial_post_retry_no_double_free for the
 * read path. Proves that when fi_readmsg() fails mid-multi-segment,
 * a subsequent retry does not reuse (and thereby corrupt) a freed txe
 * that the in-flight segment's completion still references.
 */
void test_efa_rdm_rma_partial_post_retry_no_double_free_read(
		void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_ep *efa_rdm_ep;
	struct efa_rdm_peer *peer;
	struct efa_unit_test_buff recv_buff;
	struct iovec iov;
	struct fi_msg_rma msg = {0};
	struct fi_rma_iov rma_iov;
	struct efa_rdm_pke *inflight_pke;
	fi_addr_t addr;
	struct efa_ep_addr raw_addr;
	size_t raw_addr_len = sizeof(raw_addr);
	size_t max_rdma_size_orig;
	uint32_t vendor_part_id_orig;
	uint64_t first_wr_id;
	void *desc;
	int ret;

	/* Skip test on platforms that don't support RDMA read. */
	if (!efa_device_support_rdma_read()) {
		skip();
		return;
	}

	efa_unit_test_resource_construct_rdm_shm_disabled(resource);
	efa_rdm_ep = container_of(resource->ep, struct efa_rdm_ep,
				  base_ep.util_ep.ep_fid);

	ret = fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len);
	assert_int_equal(ret, 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	ret = fi_av_insert(resource->av, &raw_addr, 1, &addr, 0, NULL);
	assert_int_equal(ret, 1);

	peer = efa_rdm_ep_get_peer_explicit(efa_rdm_ep, addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer->extra_info[0] |= EFA_RDM_EXTRA_FEATURE_RDMA_READ;
	vendor_part_id_orig = g_efa_selected_device_list[0].ibv_attr.vendor_part_id;
	g_efa_selected_device_list[0].ibv_attr.vendor_part_id = 0xEFA1;
	peer->device_version = 0xEFA1;

	efa_rdm_ep->use_device_rdma = true;

	efa_unit_test_buff_construct(&recv_buff, resource, 128);
	desc = fi_mr_desc(recv_buff.mr);

	max_rdma_size_orig = efa_rdm_ep_domain(efa_rdm_ep)->device->max_rdma_size;
	efa_rdm_ep_domain(efa_rdm_ep)->device->max_rdma_size = 64;

	/*
	 * Saturate tx queue so each call's first segment posts once and the
	 * second segment hits -FI_EAGAIN.
	 */
	efa_rdm_ep->efa_outstanding_tx_ops = efa_rdm_ep->efa_max_outstanding_tx_ops - 1;

	g_efa_unit_test_mocks.efa_qp_post_read = &efa_mock_efa_qp_post_read_return_mock;
	will_return(efa_mock_efa_qp_post_read_return_mock, 0);

	iov.iov_base = recv_buff.buff;
	iov.iov_len = 128;
	rma_iov.addr = 0x87654321;
	rma_iov.len = 128;
	rma_iov.key = 123456;
	efa_unit_test_construct_msg_rma(&msg, &iov, &desc, 1, addr,
					&rma_iov, 1, NULL, 0);

	/* First attempt: posts segment 1, fails on segment 2. */
	ret = fi_readmsg(resource->ep, &msg, 0);
	assert_int_equal(ret, -FI_EAGAIN);
	assert_int_equal(g_ibv_submitted_wr_id_cnt, 1);
	first_wr_id = (uint64_t) g_ibv_submitted_wr_id_vec[0];

	/*
	 * Retry BEFORE the in-flight completion arrives. Queue is still
	 * saturated so the retry's segment 1 immediately returns -FI_EAGAIN
	 * (nothing posted). The retry must not disturb the first in-flight
	 * txe.
	 */
	ret = fi_readmsg(resource->ep, &msg, 0);
	assert_int_equal(ret, -FI_EAGAIN);
	assert_int_equal(g_ibv_submitted_wr_id_cnt, 1);
	assert_int_equal(efa_unit_test_get_ope_list_length(efa_rdm_ep, EFA_RDM_TXE), 1);
	assert_int_equal(peer->efa_outstanding_tx_ops, 1);

	/*
	 * Restore spoofed device state now that fi_readmsg has consumed it.
	 * Done before the final assertions so a failure there does not
	 * leak into subsequent tests.
	 */
	efa_rdm_ep_domain(efa_rdm_ep)->device->max_rdma_size = max_rdma_size_orig;
	g_efa_selected_device_list[0].ibv_attr.vendor_part_id = vendor_part_id_orig;

	/*
	 * Drive the in-flight completion. Pre-fix, txe1 was freed by the
	 * first fi_readmsg's error path; the completion handler's writes
	 * to txe->bytes_read_completed UAF the freed slab slot (which the
	 * retry may have re-used).
	 */
	inflight_pke = efa_rdm_cq_get_pke_from_wr_id_solicited(first_wr_id);
	efa_rdm_ep_record_tx_op_completed(efa_rdm_ep, inflight_pke);
	efa_rdm_pke_handle_rma_completion(inflight_pke);

	/* Clean drain: both txes released, peer/ep state clean. */
	assert_int_equal(efa_unit_test_get_ope_list_length(efa_rdm_ep, EFA_RDM_TXE), 0);
	assert_int_equal(peer->efa_outstanding_tx_ops, 0);
	assert_true(dlist_empty(&peer->outstanding_tx_pkts));

	efa_unit_test_buff_destruct(&recv_buff);
}


/**
 * @brief Test that efa_rdm_rma_should_read_using_rdma returns false
 *        when peer has p2p_supported=false
 */
void test_efa_rdm_rma_should_read_using_rdma_peer_p2p_false(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_peer peer = {0};
	struct efa_rdm_ep *ep;
	uint32_t efa_device_caps_orig;
	uint32_t vendor_part_id_orig;
	bool result;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);
	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	efa_device_caps_orig = g_efa_selected_device_list[0].device_caps;
	g_efa_selected_device_list[0].device_caps |= EFADV_DEVICE_ATTR_CAPS_RDMA_READ;
	vendor_part_id_orig = g_efa_selected_device_list[0].ibv_attr.vendor_part_id;
	g_efa_selected_device_list[0].ibv_attr.vendor_part_id = 0xEFA1;

	ep->use_device_rdma = true;

	peer.flags = EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer.extra_info[0] = EFA_RDM_EXTRA_FEATURE_RDMA_READ;
	peer.device_version = 0xEFA1;
	peer.p2p_supported = false;

	result = efa_rdm_rma_should_read_using_rdma(ep, &peer, true);
	assert_false(result);

	g_efa_selected_device_list[0].ibv_attr.vendor_part_id = vendor_part_id_orig;
	g_efa_selected_device_list[0].device_caps = efa_device_caps_orig;
}

/**
 * @brief Test that efa_rdm_rma_should_read_using_rdma returns true
 *        when peer has p2p_supported=true
 */
void test_efa_rdm_rma_should_read_using_rdma_peer_p2p_true(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_peer peer = {0};
	struct efa_rdm_ep *ep;
	uint32_t efa_device_caps_orig;
	uint32_t vendor_part_id_orig;
	bool result;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);
	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	efa_device_caps_orig = g_efa_selected_device_list[0].device_caps;
	g_efa_selected_device_list[0].device_caps |= EFADV_DEVICE_ATTR_CAPS_RDMA_READ;
	vendor_part_id_orig = g_efa_selected_device_list[0].ibv_attr.vendor_part_id;
	g_efa_selected_device_list[0].ibv_attr.vendor_part_id = 0xEFA1;

	ep->use_device_rdma = true;

	peer.flags = EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer.extra_info[0] = EFA_RDM_EXTRA_FEATURE_RDMA_READ;
	peer.device_version = 0xEFA1;
	peer.p2p_supported = true;

	result = efa_rdm_rma_should_read_using_rdma(ep, &peer, true);
	assert_true(result);

	g_efa_selected_device_list[0].ibv_attr.vendor_part_id = vendor_part_id_orig;
	g_efa_selected_device_list[0].device_caps = efa_device_caps_orig;
}

/**
 * @brief Test that efa_rdm_rma_should_write_using_rdma returns false
 *        when peer has p2p_supported=false
 */
void test_efa_rdm_rma_should_write_using_rdma_peer_p2p_false_returns_false(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_peer peer = {0};
	struct efa_rdm_ope txe = {0};
	bool result;
	struct efa_rdm_ep *ep;
	uint32_t efa_device_caps_orig;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);
	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	efa_device_caps_orig = g_efa_selected_device_list[0].device_caps;
	g_efa_selected_device_list[0].device_caps |= EFADV_DEVICE_ATTR_CAPS_RDMA_WRITE;

	ep->use_device_rdma = true;
	ep->homogeneous_peers = false;

	peer.flags = EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer.extra_info[0] = EFA_RDM_EXTRA_FEATURE_RDMA_WRITE;
	peer.p2p_supported = false;
	txe.fi_flags = 0;
	txe.iov_count = 1;
	txe.rma_iov_count = 1;

	result = efa_rdm_rma_should_write_using_rdma(ep, &txe, &peer, true);
	assert_false(result);

	g_efa_selected_device_list[0].device_caps = efa_device_caps_orig;
}

/**
 * @brief Test that efa_rdm_rma_should_read_using_rdma works for homogeneous
 *        peers without requiring a handshake, using local ep p2p state.
 *
 * When ep has FI_HMEM with hmem_p2p_opt=DISABLED, should return false
 * even without handshake received.
 */
void test_efa_rdm_rma_should_read_using_rdma_homogeneous_no_p2p(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_peer peer = {0};
	struct efa_rdm_ep *ep;
	uint32_t efa_device_caps_orig;
	bool result;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);
	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	efa_device_caps_orig = g_efa_selected_device_list[0].device_caps;
	g_efa_selected_device_list[0].device_caps |= EFADV_DEVICE_ATTR_CAPS_RDMA_READ;

	ep->use_device_rdma = true;
	ep->homogeneous_peers = true;
	ep->base_ep.info->caps |= FI_HMEM;
	ep->hmem_p2p_opt = FI_HMEM_P2P_DISABLED;

	/* Peer has no handshake - homogeneous path should not require it */
	peer.flags = 0;

	result = efa_rdm_rma_should_read_using_rdma(ep, &peer, true);
	assert_false(result);

	g_efa_selected_device_list[0].device_caps = efa_device_caps_orig;
}

/**
 * @brief Test that efa_rdm_rma_should_read_using_rdma returns true for
 *        homogeneous peers when local ep supports p2p (no FI_HMEM).
 */
void test_efa_rdm_rma_should_read_using_rdma_homogeneous_with_p2p(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_peer peer = {0};
	struct efa_rdm_ep *ep;
	uint32_t efa_device_caps_orig;
	bool result;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);
	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	efa_device_caps_orig = g_efa_selected_device_list[0].device_caps;
	g_efa_selected_device_list[0].device_caps |= EFADV_DEVICE_ATTR_CAPS_RDMA_READ;

	ep->use_device_rdma = true;
	ep->homogeneous_peers = true;

	/* No FI_HMEM → always p2p. No handshake needed for homogeneous. */
	peer.flags = 0;

	result = efa_rdm_rma_should_read_using_rdma(ep, &peer, true);
	assert_true(result);

	g_efa_selected_device_list[0].device_caps = efa_device_caps_orig;
}

/**
 * @brief Test that efa_rdm_rma_should_write_using_rdma works for self peer
 *        without requiring a handshake, using local ep p2p state.
 */
void test_efa_rdm_rma_should_write_using_rdma_self_no_p2p(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_peer peer = {0};
	struct efa_rdm_ope txe = {0};
	struct efa_rdm_ep *ep;
	uint32_t efa_device_caps_orig;
	bool result;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);
	ep = container_of(resource->ep, struct efa_rdm_ep,
			  base_ep.util_ep.ep_fid);

	efa_device_caps_orig = g_efa_selected_device_list[0].device_caps;
	g_efa_selected_device_list[0].device_caps |= EFADV_DEVICE_ATTR_CAPS_RDMA_WRITE;

	ep->use_device_rdma = true;
	ep->base_ep.info->caps |= FI_HMEM;
	ep->hmem_p2p_opt = FI_HMEM_P2P_DISABLED;

	/* Self peer - no handshake needed */
	peer.is_self = true;
	peer.flags = 0;
	txe.fi_flags = 0;
	txe.iov_count = 1;
	txe.rma_iov_count = 1;

	result = efa_rdm_rma_should_write_using_rdma(ep, &txe, &peer, true);
	assert_false(result);

	g_efa_selected_device_list[0].device_caps = efa_device_caps_orig;
}

/**
 * @brief Test that a successful inject RDMA write bumps the write counter
 *
 * The inject entry points (fi_inject_write / fi_inject_writedata) set 
 * NO_COMPLETION
 * on the txe to suppress the success CQ entry per inject semantics, but
 * the op still counts as a completed transfer, so the FI_WRITE counter
 * must be incremented when the device write completes.
 *
 * This test binds a counter with FI_WRITE, issues fi_inject_write over
 * device RDMA (mocked efa_qp_post_write), drives the completion through
 * the real completion handler, and asserts: counter == 1, no CQ entry,
 * txe released.
 */
void test_efa_rdm_rma_inject_write_bumps_tx_counter(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_rdm_ep *efa_rdm_ep;
	struct efa_rdm_peer *peer;
	struct efa_rdm_pke *inflight_pke;
	struct fid_cntr *cntr;
	struct fi_cntr_attr cntr_attr = {0};
	struct fi_cq_data_entry cq_entry;
	fi_addr_t addr;
	struct efa_ep_addr raw_addr;
	size_t raw_addr_len = sizeof(raw_addr);
	uint64_t wr_id;
	char buf[32] = {0};
	bool shm_permitted = false;
	int ret;

	/* Skip test on platforms that don't support RDMA write. */
	if (!efa_device_support_rdma_write()) {
		skip();
		return;
	}

	/*
	 * Mirror efa_unit_test_resource_construct_rdm_shm_disabled(), but
	 * bind a counter before enabling the endpoint.
	 */
	resource->hints = efa_unit_test_alloc_hints(FI_EP_RDM, EFA_FABRIC_NAME);
	assert_non_null(resource->hints);
	efa_unit_test_resource_construct_with_hints(resource, FI_EP_RDM,
						    FI_VERSION(1, 14),
						    resource->hints, false, true);
	ret = fi_setopt(&resource->ep->fid, FI_OPT_ENDPOINT,
			FI_OPT_SHARED_MEMORY_PERMITTED, &shm_permitted,
			sizeof(shm_permitted));
	assert_int_equal(ret, 0);

	ret = fi_cntr_open(resource->domain, &cntr_attr, &cntr, NULL);
	assert_int_equal(ret, 0);
	ret = fi_ep_bind(resource->ep, &cntr->fid, FI_WRITE);
	assert_int_equal(ret, 0);

	ret = fi_enable(resource->ep);
	assert_int_equal(ret, 0);

	efa_rdm_ep = container_of(resource->ep, struct efa_rdm_ep,
				  base_ep.util_ep.ep_fid);

	/* Set up peer with RDMA write support */
	ret = fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len);
	assert_int_equal(ret, 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	ret = fi_av_insert(resource->av, &raw_addr, 1, &addr, 0, NULL);
	assert_int_equal(ret, 1);

	peer = efa_rdm_ep_get_peer_explicit(efa_rdm_ep, addr);
	peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	peer->extra_info[0] |= EFA_RDM_EXTRA_FEATURE_RDMA_WRITE |
				EFA_RDM_EXTRA_FEATURE_UNSOLICITED_WRITE_RECV;

	efa_rdm_ep->use_device_rdma = true;

	/* Mock: the device write posts successfully */
	g_efa_unit_test_mocks.efa_qp_post_write = &efa_mock_efa_qp_post_write_return_mock;
	will_return(efa_mock_efa_qp_post_write_return_mock, 0);

	assert_int_equal(g_ibv_submitted_wr_id_cnt, 0);
	ret = fi_inject_write(resource->ep, buf, sizeof(buf), addr,
			      0x87654321, 123456);
	assert_int_equal(ret, 0);
	assert_int_equal(g_ibv_submitted_wr_id_cnt, 1);

	/* No counter update before the device completion arrives */
	assert_int_equal(fi_cntr_read(cntr), 0);

	/* Drive the device completion through the real completion handler */
	wr_id = (uint64_t) g_ibv_submitted_wr_id_vec[0];
	inflight_pke = efa_rdm_cq_get_pke_from_wr_id_solicited(wr_id);
	efa_rdm_ep_record_tx_op_completed(efa_rdm_ep, inflight_pke);
	efa_rdm_pke_handle_rma_completion(inflight_pke);

	/* The write counter must be bumped exactly once... */
	assert_int_equal(fi_cntr_read(cntr), 1);

	/* ...without a CQ entry (inject semantics)... */
	assert_int_equal(fi_cq_read(resource->cq, &cq_entry, 1), -FI_EAGAIN);

	/* ...and the txe must be released (no leak) */
	assert_int_equal(efa_unit_test_get_ope_list_length(efa_rdm_ep, EFA_RDM_TXE), 0);
	assert_int_equal(peer->efa_outstanding_tx_ops, 0);

	/* ep must be closed before the counter */
	fi_close(&resource->ep->fid);
	resource->ep = NULL;
	fi_close(&cntr->fid);
}

/**
 * @brief Only works on nodes with EFA devices
 *
 * fi_writemsg to an fi_addr removed by fi_av_remove returns an error instead
 * of crashing.
 *
 * @param[in]	state		struct efa_resource that is managed by the framework
 */
void test_efa_rdm_rma_writemsg_to_removed_addr(void **state)
{
	struct efa_resource *resource = *state;
	struct efa_unit_test_buff local_buff;
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(raw_addr);
	struct iovec iov;
	struct fi_msg_rma msg = {0};
	struct fi_rma_iov rma_iov;
	fi_addr_t addr;
	void *desc;
	int ret;

	efa_unit_test_resource_construct(resource, FI_EP_RDM, EFA_FABRIC_NAME);

	ret = fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len);
	assert_int_equal(ret, 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;

	ret = fi_av_insert(resource->av, &raw_addr, 1, &addr, 0, NULL);
	assert_int_equal(ret, 1);

	assert_int_equal(fi_av_remove(resource->av, &addr, 1, 0), 0);

	efa_unit_test_buff_construct(&local_buff, resource, 4096 /* buff_size */);
	desc = fi_mr_desc(local_buff.mr);
	iov.iov_base = local_buff.buff;
	iov.iov_len = local_buff.size;
	rma_iov.addr = 0x87654321;
	rma_iov.len = local_buff.size;
	rma_iov.key = 123456;
	efa_unit_test_construct_msg_rma(&msg, &iov, &desc, 1, addr, &rma_iov, 1,
					NULL, 0);

	ret = fi_writemsg(resource->ep, &msg, 0);
	assert_int_equal(ret, -FI_EINVAL);

	efa_unit_test_buff_destruct(&local_buff);
}

/*
 * Eager read / eager write (FI_EFA_RMA_FAST_PATH=1): one RMA == one WQE ==
 * one 64-byte eager RMA entry, no TXE and no PKE. These tests guard the parts of efa-proto the fast path
 * must keep: the CQ entry, the error entry, and the fallback to the TXE
 * path when the op could see RNR.
 */
struct eager_rma_fixture {
	struct efa_rdm_ep *ep;
	struct efa_rdm_peer *peer;
	struct efa_unit_test_buff buff;
	struct iovec iov;
	struct fi_rma_iov rma_iov;
	void *desc;
	fi_addr_t addr;
	int fast_orig, track_mr_orig;
};

static void eager_rma_setup(struct efa_resource *resource,
			    struct eager_rma_fixture *f)
{
	struct efa_ep_addr raw_addr;
	size_t raw_addr_len = sizeof(raw_addr);
	int ret;

	efa_unit_test_resource_construct_rdm_shm_disabled(resource);
	f->ep = container_of(resource->ep, struct efa_rdm_ep,
			     base_ep.util_ep.ep_fid);

	ret = fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len);
	assert_int_equal(ret, 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;
	ret = fi_av_insert(resource->av, &raw_addr, 1, &f->addr, 0, NULL);
	assert_int_equal(ret, 1);

	f->peer = efa_rdm_ep_get_peer_explicit(f->ep, f->addr);
	assert_non_null(f->peer);
	f->peer->flags |= EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	f->peer->extra_info[0] |= EFA_RDM_EXTRA_FEATURE_RDMA_READ |
				  EFA_RDM_EXTRA_FEATURE_RDMA_WRITE;
	f->peer->p2p_supported = true;
	f->peer->device_version =
		efa_rdm_ep_domain(f->ep)->device->ibv_attr.vendor_part_id;
	f->ep->use_device_rdma = true;

	efa_unit_test_buff_construct(&f->buff, resource, 4096);
	f->desc = fi_mr_desc(f->buff.mr);
	f->iov.iov_base = f->buff.buff;
	f->iov.iov_len = f->buff.size;
	f->rma_iov.addr = 0x87654321;
	f->rma_iov.len = f->buff.size;
	f->rma_iov.key = 123456;

	f->fast_orig = efa_env.rma_fast_path;
	f->track_mr_orig = efa_env.track_mr;
	efa_env.rma_fast_path = 1;
	efa_env.track_mr = 0;

	g_efa_unit_test_mocks.efa_qp_post_read =
		&efa_mock_efa_qp_post_read_return_mock;
	g_efa_unit_test_mocks.efa_qp_post_write =
		&efa_mock_efa_qp_post_write_return_mock;
}

static void eager_rma_teardown(struct eager_rma_fixture *f)
{
	efa_env.rma_fast_path = f->fast_orig;
	efa_env.track_mr = f->track_mr_orig;
	efa_unit_test_buff_destruct(&f->buff);
}

static void eager_rma_mock_poll(struct efa_rdm_ep *ep, int status, int n)
{
	int i;

	g_efa_unit_test_mocks.efa_ibv_cq_start_poll =
		&efa_mock_efa_ibv_cq_start_poll_use_saved_send_wr_with_mock_status;
	g_efa_unit_test_mocks.efa_ibv_cq_next_poll =
		&efa_mock_efa_ibv_cq_next_poll_use_saved_send_wr_with_mock_status;
	g_efa_unit_test_mocks.efa_ibv_cq_end_poll =
		&efa_mock_efa_ibv_cq_end_poll_check_mock;
	g_efa_unit_test_mocks.efa_ibv_cq_wc_read_qp_num =
		&efa_mock_efa_ibv_cq_wc_read_qp_num_return_mock;
	will_return_int(
		efa_mock_efa_ibv_cq_start_poll_use_saved_send_wr_with_mock_status,
		status);
	for (i = 1; i < n; i++)
		will_return_int(
			efa_mock_efa_ibv_cq_next_poll_use_saved_send_wr_with_mock_status,
			status);
	for (i = 0; i < n; i++)
		will_return_uint(efa_mock_efa_ibv_cq_wc_read_qp_num_return_mock,
				 ep->base_ep.qp->qp_num);
	expect_function_call(efa_mock_efa_ibv_cq_end_poll_check_mock);
}

static uint64_t eager_rma_wr_id(int i)
{
	return (uint64_t) g_ibv_submitted_wr_id_vec[i];
}

/* Happy path: read and write each use 1 entry and no TXE, and the CQ
 * entry is built from the entry alone. */
void test_efa_rdm_rma_eager_rma_happy_path(void **state)
{
	struct efa_resource *resource = *state;
	struct eager_rma_fixture f;
	struct fi_cq_data_entry cq_entries[2] = {0};
	struct fi_msg_rma read_msg, write_msg;
	void *read_context = (void *) 0x1234560;
	void *write_context = (void *) 0x2345670;
	size_t outstanding;
	int ret;

	if (!efa_device_support_rdma_read() || !efa_device_support_rdma_write()) {
		skip();
		return;
	}
	eager_rma_setup(resource, &f);
	efa_unit_test_construct_msg_rma(&read_msg, &f.iov, &f.desc, 1, f.addr,
					&f.rma_iov, 1, read_context, 0);
	efa_unit_test_construct_msg_rma(&write_msg, &f.iov, &f.desc, 1, f.addr,
					&f.rma_iov, 1, write_context, 0);
	outstanding = f.ep->efa_outstanding_tx_ops;

	will_return_int(efa_mock_efa_qp_post_read_return_mock, 0);
	will_return_int(efa_mock_efa_qp_post_write_return_mock, 0);
	assert_int_equal(fi_readmsg(resource->ep, &read_msg, FI_COMPLETION), 0);
	assert_int_equal(fi_writemsg(resource->ep, &write_msg, FI_COMPLETION), 0);

	assert_int_equal(g_ibv_submitted_wr_id_cnt, 2);
	assert_true(efa_rdm_eager_rma_is_wr_id(eager_rma_wr_id(0)));
	assert_true(efa_rdm_eager_rma_is_wr_id(eager_rma_wr_id(1)));
	assert_ptr_equal(efa_rdm_eager_rma_entry_from_wr_id(eager_rma_wr_id(0))->op_context,
			 read_context);
	assert_ptr_equal(efa_rdm_eager_rma_entry_from_wr_id(eager_rma_wr_id(1))->op_context,
			 write_context);
	assert_int_equal(efa_unit_test_get_ope_list_length(f.ep, EFA_RDM_TXE), 0);
	assert_int_equal(f.ep->efa_outstanding_tx_ops, outstanding + 2);

	eager_rma_mock_poll(f.ep, IBV_WC_SUCCESS, 2);
	ret = fi_cq_read(resource->cq, cq_entries, 2);
	assert_int_equal(ret, 2);
	assert_ptr_equal(cq_entries[0].op_context, read_context);
	assert_int_equal(cq_entries[0].flags, FI_RMA | FI_READ);
	assert_ptr_equal(cq_entries[1].op_context, write_context);
	assert_int_equal(cq_entries[1].flags, FI_RMA | FI_WRITE);
	assert_int_equal(f.ep->efa_outstanding_tx_ops, outstanding);

	eager_rma_teardown(&f);
}

/* Error path: a failed WQE becomes exactly one CQ error entry with the
 * application context, and the entry is freed (no retry, no TXE). */
static void eager_rma_error_case(struct efa_resource *resource, int vendor_err)
{
	struct eager_rma_fixture f;
	struct fi_cq_data_entry cq_entry;
	struct fi_cq_err_entry err_entry = {0};
	struct fi_msg_rma msg;
	void *context = (void *) 0x3456780;
	size_t outstanding;
	int ret;

	eager_rma_setup(resource, &f);
	efa_unit_test_construct_msg_rma(&msg, &f.iov, &f.desc, 1, f.addr,
					&f.rma_iov, 1, context, 0);
	outstanding = f.ep->efa_outstanding_tx_ops;

	will_return_int(efa_mock_efa_qp_post_write_return_mock, 0);
	assert_int_equal(fi_writemsg(resource->ep, &msg, FI_COMPLETION), 0);
	assert_true(efa_rdm_eager_rma_is_wr_id(eager_rma_wr_id(0)));

	g_efa_unit_test_mocks.efa_ibv_cq_wc_read_vendor_err =
		&efa_mock_efa_ibv_cq_wc_read_vendor_err_return_mock;
	will_return_uint(efa_mock_efa_ibv_cq_wc_read_vendor_err_return_mock,
			 vendor_err);
	eager_rma_mock_poll(f.ep, IBV_WC_GENERAL_ERR, 1);

	ret = fi_cq_read(resource->cq, &cq_entry, 1);
	assert_int_equal(ret, -FI_EAVAIL);
	ret = fi_cq_readerr(resource->cq, &err_entry, 0);
	assert_int_equal(ret, 1);
	assert_ptr_equal(err_entry.op_context, context);
	assert_int_equal(err_entry.flags, FI_RMA | FI_WRITE);
	assert_int_equal(err_entry.prov_errno, vendor_err);
	assert_int_equal(err_entry.len, f.buff.size);
	assert_int_equal(f.ep->efa_outstanding_tx_ops, outstanding);
	assert_int_equal(f.ep->efa_rnr_queued_pkt_cnt, 0);
	assert_int_equal(efa_unit_test_get_ope_list_length(f.ep, EFA_RDM_TXE), 0);

	eager_rma_teardown(&f);
}

void test_efa_rdm_rma_eager_rma_error_bad_address(void **state)
{
	if (!efa_device_support_rdma_write()) {
		skip();
		return;
	}
	eager_rma_error_case(*state, EFA_IO_COMP_STATUS_REMOTE_ERROR_BAD_ADDRESS);
}

/* RNR cannot happen on an eligible eager op; if the device still reports
 * it, it is reported like any other error, never queued. */
void test_efa_rdm_rma_eager_rma_error_rnr_is_final(void **state)
{
	if (!efa_device_support_rdma_write()) {
		skip();
		return;
	}
	eager_rma_error_case(*state, EFA_IO_COMP_STATUS_REMOTE_ERROR_RNR);
}

/* Missing handshake: the op must take the TXE path, which owns the
 * handshake wait. */
void test_efa_rdm_rma_eager_rma_no_handshake_uses_txe(void **state)
{
	struct efa_resource *resource = *state;
	struct eager_rma_fixture f;
	struct fi_msg_rma msg;
	int i;

	if (!efa_device_support_rdma_read()) {
		skip();
		return;
	}
	eager_rma_setup(resource, &f);
	f.peer->flags &= ~EFA_RDM_PEER_HANDSHAKE_RECEIVED;
	f.ep->homogeneous_peers = false;
	efa_unit_test_construct_msg_rma(&msg, &f.iov, &f.desc, 1, f.addr,
					&f.rma_iov, 1, (void *) 0x10, 0);

	/* The TXE path sends a handshake request with efa_qp_post_send. */
	g_efa_unit_test_mocks.efa_qp_post_send =
		&efa_mock_efa_qp_post_send_return_mock;
	will_return_int_maybe(efa_mock_efa_qp_post_send_return_mock, 0);

	fi_readmsg(resource->ep, &msg, FI_COMPLETION);
	/* The read has a TXE (the handshake request may own another one) ... */
	assert_true(efa_unit_test_get_ope_list_length(f.ep, EFA_RDM_TXE) >= 1);
	/* ... and nothing was posted on the eager path. */
	for (i = 0; i < g_ibv_submitted_wr_id_cnt; i++)
		assert_false(efa_rdm_eager_rma_is_wr_id(eager_rma_wr_id(i)));

	eager_rma_teardown(&f);
}

/* Write with remote CQ data: eager only if it cannot see RNR. */
void test_efa_rdm_rma_eager_rma_imm_needs_no_rnr(void **state)
{
	struct efa_resource *resource = *state;
	struct eager_rma_fixture f;
	struct fi_msg_rma msg;
	int ret;

	if (!efa_device_support_rdma_write()) {
		skip();
		return;
	}
	eager_rma_setup(resource, &f);
	f.ep->homogeneous_peers = false;
	f.ep->extra_info[0] &= ~EFA_RDM_EXTRA_FEATURE_UNSOLICITED_WRITE_RECV;
	f.peer->extra_info[0] &= ~EFA_RDM_EXTRA_FEATURE_UNSOLICITED_WRITE_RECV;
	efa_unit_test_construct_msg_rma(&msg, &f.iov, &f.desc, 1, f.addr,
					&f.rma_iov, 1, (void *) 0x20, 0xbeef);

	/* Finite RNR retry and the write takes an rx buffer: TXE path. */
	f.ep->base_ep.rnr_retry = 3;
	will_return_int(efa_mock_efa_qp_post_write_return_mock, 0);
	ret = fi_writemsg(resource->ep, &msg, FI_COMPLETION | FI_REMOTE_CQ_DATA);
	assert_int_equal(ret, 0);
	assert_false(efa_rdm_eager_rma_is_wr_id(eager_rma_wr_id(0)));
	assert_int_equal(efa_unit_test_get_ope_list_length(f.ep, EFA_RDM_TXE), 1);

	/* Infinite RNR retry (NIXL's setting): eager. */
	f.ep->base_ep.rnr_retry = EFA_RNR_INFINITE_RETRY;
	will_return_int(efa_mock_efa_qp_post_write_return_mock, 0);
	ret = fi_writemsg(resource->ep, &msg, FI_COMPLETION | FI_REMOTE_CQ_DATA);
	assert_int_equal(ret, 0);
	assert_true(efa_rdm_eager_rma_is_wr_id(eager_rma_wr_id(1)));
	assert_int_equal(efa_unit_test_get_ope_list_length(f.ep, EFA_RDM_TXE), 1);

	eager_rma_teardown(&f);
}
