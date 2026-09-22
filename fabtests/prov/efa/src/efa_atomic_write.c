/*
 * Copyright (c) 2026 Amazon.com, Inc. or its affiliates.
 *
 * This software is available to you under a choice of one of two
 * licenses. You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file COPYING
 * in the main directory of this source tree, or the BSD license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES.
 */

#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <rdma/fi_atomic.h>
#include <rdma/fi_errno.h>

#include <shared.h>

#define ATOMIC_TEST_BUF_SIZE	(64 * 1024)
#define ATOMIC_TEST_IOV_COUNT	4
#define ATOMIC_TEST_TIMEOUT_MS	5000

static uint8_t expected[ATOMIC_TEST_BUF_SIZE];
static struct fi_context2 atomic_ctx;

static int verify_no_tx_cq_entry(unsigned int duration_ms);

static void fill_pattern(void *buf, size_t len, uint8_t seed)
{
	uint8_t *bytes = buf;
	size_t i;

	for (i = 0; i < len; i++)
		bytes[i] = seed + (uint8_t)(i * 17);
}

static int exchange_status(int local_status)
{
	int peer_status;

	peer_status = ft_sock_sync(oob_sock, local_status);
	if (peer_status)
		return peer_status;
	return local_status;
}

static int wait_for_target(const void *value, size_t len)
{
	uint64_t deadline = ft_gettime_ms() + ATOMIC_TEST_TIMEOUT_MS;

	do {
		ft_force_progress();
		if (!memcmp(rx_buf, value, len))
			return 0;
		usleep(100);
	} while (ft_gettime_ms() < deadline);

	fprintf(stderr, "target data did not reach the expected value\n");
	return -FI_ENODATA;
}

static int verify_no_tx_completion(unsigned int duration_ms)
{
	uint64_t count;
	uint64_t deadline;

	if (!(opts.options & FT_OPT_TX_CQ) && !txcntr)
		return 0;

	if (opts.options & FT_OPT_TX_CQ)
		return verify_no_tx_cq_entry(duration_ms);

	deadline = ft_gettime_ms() + duration_ms;
	do {
		ft_force_progress();
		count = fi_cntr_read(txcntr);
		if (count > tx_seq) {
			fprintf(stderr,
				"unexpected extra transmit counter update\n");
			return -FI_EOTHER;
		}
		if (!duration_ms)
			break;
		usleep(100);
	} while (ft_gettime_ms() < deadline);

	return 0;
}

static int verify_no_tx_cq_entry(unsigned int duration_ms)
{
	struct fi_cq_tagged_entry comp;
	uint64_t deadline;
	ssize_t ret;

	if (!txcq)
		return 0;

	deadline = ft_gettime_ms() + duration_ms;
	do {
		ret = fi_cq_read(txcq, &comp, 1);
		if (ret > 0) {
			fprintf(stderr, "unexpected transmit CQ entry\n");
			return -FI_EOTHER;
		}
		if (ret == -FI_EAVAIL) {
			fprintf(stderr, "unexpected transmit CQ error\n");
			return -FI_EOTHER;
		}
		if (ret < 0 && ret != -FI_EAGAIN) {
			FT_PRINTERR("fi_cq_read", ret);
			return ret;
		}
		if (!duration_ms)
			break;
		usleep(100);
	} while (ft_gettime_ms() < deadline);

	return 0;
}

static uint64_t completion_flags(void)
{
	return opts.options & FT_OPT_TX_CQ ? FI_COMPLETION : 0;
}

static int wait_for_one_tx_completion(void)
{
	int ret;

	tx_seq++;
	ret = ft_get_tx_comp(tx_seq);
	if (ret)
		return ret;

	return verify_no_tx_completion(1);
}

static int finish_data_test(const char *name, int local_status,
			    size_t expected_len)
{
	int ret;

	if (!opts.dst_addr && !local_status)
		local_status = wait_for_target(expected, expected_len);

	ret = exchange_status(local_status);
	if (ret)
		return ret;

	/*
	 * Give a late duplicate operation or completion a chance to surface.
	 * The receiver keeps progressing while the initiator checks its CQ.
	 */
	if (opts.dst_addr)
		local_status = verify_no_tx_completion(20);
	else {
		uint64_t deadline = ft_gettime_ms() + 20;

		do {
			ft_force_progress();
			usleep(100);
		} while (ft_gettime_ms() < deadline);

		if (memcmp(rx_buf, expected, expected_len)) {
			fprintf(stderr, "%s target changed after completion\n", name);
			local_status = -FI_EOTHER;
		}
	}

	ret = exchange_status(local_status);
	if (!ret && !opts.dst_addr)
		printf("PASS: %s\n", name);
	return ret;
}

static int post_atomicmsg(struct fi_ioc *ioc, void **desc, size_t iov_count,
			  struct fi_rma_ioc *rma_iov, size_t rma_iov_count,
			  enum fi_datatype datatype, enum fi_op op,
			  uint64_t flags)
{
	struct fi_msg_atomic msg = {
		.msg_iov = ioc,
		.desc = desc,
		.iov_count = iov_count,
		.addr = remote_fi_addr,
		.rma_iov = rma_iov,
		.rma_iov_count = rma_iov_count,
		.datatype = datatype,
		.op = op,
		.context = &atomic_ctx,
	};
	int ret;

	do {
		ret = fi_atomicmsg(ep, &msg, flags);
		if (ret == -FI_EAGAIN)
			ft_force_progress();
	} while (ret == -FI_EAGAIN);

	if (ret)
		return ret;
	return wait_for_one_tx_completion();
}

/*
 * fi_atomic and fi_atomicv funnel into the same fi_msg_atomic provider path,
 * so one fragmented fi_atomicmsg write covers the WRITE_RTA data path. The
 * pytest suite reruns the whole binary with -U, which turns every operation
 * here into a DC_WRITE_RTA.
 */
static int test_atomicmsg(void)
{
	const size_t src_offsets[ATOMIC_TEST_IOV_COUNT] = {0, 48, 112, 192};
	const size_t src_counts[ATOMIC_TEST_IOV_COUNT] = {19, 29, 37, 43};
	struct fi_ioc ioc[ATOMIC_TEST_IOV_COUNT];
	struct fi_rma_ioc rma_iov = {0};
	void *desc[ATOMIC_TEST_IOV_COUNT];
	size_t i, total = 0;
	int ret = 0;

	memset(rx_buf, 0xcc, ATOMIC_TEST_BUF_SIZE);
	memset(tx_buf, 0, ATOMIC_TEST_BUF_SIZE);
	memset(expected, 0xcc, sizeof(expected));

	for (i = 0; i < ATOMIC_TEST_IOV_COUNT; i++) {
		ioc[i].addr = tx_buf + src_offsets[i];
		ioc[i].count = src_counts[i];
		desc[i] = mr_desc;
		fill_pattern(ioc[i].addr, ioc[i].count, 0x40 + i * 13);
		memcpy(expected + total, ioc[i].addr, ioc[i].count);
		total += ioc[i].count;
	}

	ret = ft_sync();
	if (ret)
		return ret;

	if (opts.dst_addr) {
		rma_iov.addr = remote.addr;
		rma_iov.count = total;
		rma_iov.key = remote.key;
		ret = post_atomicmsg(ioc, desc, ATOMIC_TEST_IOV_COUNT,
				     &rma_iov, 1,
				     FI_UINT8, FI_ATOMIC_WRITE,
				     completion_flags());
	}

	return finish_data_test("fi_atomicmsg fragmented operand", ret, total);
}

static int test_inject(void)
{
	const size_t count = 32;
	int ret = 0;

	memset(rx_buf, 0xcc, ATOMIC_TEST_BUF_SIZE);
	fill_pattern(tx_buf, count, 0x91);
	memset(expected, 0xcc, sizeof(expected));
	memcpy(expected, tx_buf, count);

	ret = ft_sync();
	if (ret)
		return ret;

	if (opts.dst_addr) {
		do {
			ret = fi_inject_atomic(ep, tx_buf, count, remote_fi_addr,
					       remote.addr, remote.key, FI_UINT8,
					       FI_ATOMIC_WRITE);
			if (ret == -FI_EAGAIN)
				ft_force_progress();
		} while (ret == -FI_EAGAIN);

		/*
		 * The application may immediately reuse an inject buffer. Also
		 * verify that inject never creates an application CQ entry.
		 */
		memset(tx_buf, 0, count);
		if (!ret && !(opts.options & FT_OPT_TX_CQ))
			tx_seq++;
		if (!ret)
			ret = verify_no_tx_cq_entry(20);
	}

	return finish_data_test("fi_inject_atomic buffer lifetime and no CQ",
				ret, count);
}

static int post_write(size_t count, uint8_t fill)
{
	struct fi_ioc ioc = {
		.addr = tx_buf,
		.count = count,
	};
	struct fi_rma_ioc rma_iov = {
		.addr = remote.addr,
		.count = count,
		.key = remote.key,
	};
	void *desc = mr_desc;

	memset(tx_buf, fill, count);
	return post_atomicmsg(&ioc, &desc, 1, &rma_iov, 1, FI_UINT8,
			      FI_ATOMIC_WRITE, completion_flags());
}

/*
 * Verify the one-packet invariant: the advertised atomic count succeeds, and
 * an oversized request fails with -FI_ETRUNC without touching target memory.
 * The exact boundary does not matter, so probing doubles the count until the
 * provider rejects it instead of binary-searching the precise limit.
 */
static int test_single_packet_boundary(void)
{
	size_t count = 0, probe, fail_count = 0;
	int local_status = 0, peer_value, ret;

	memset(rx_buf, 0xcc, ATOMIC_TEST_BUF_SIZE);

	ret = ft_sync();
	if (ret)
		return ret;

	if (opts.dst_addr) {
		ret = fi_atomicvalid(ep, FI_UINT8, FI_ATOMIC_WRITE, &count);
		if (ret)
			local_status = ret;
		else if (!count || count >= ATOMIC_TEST_BUF_SIZE)
			local_status = -FI_EOVERFLOW;

		if (!local_status)
			local_status = post_write(count, 0x5a);

		probe = count;
		while (!local_status) {
			probe = MIN(probe * 2, ATOMIC_TEST_BUF_SIZE - 1);
			ret = post_write(probe, 0x5a);
			if (ret == -FI_ETRUNC) {
				fail_count = probe;
				break;
			}
			if (ret) {
				local_status = ret;
			} else if (probe == ATOMIC_TEST_BUF_SIZE - 1) {
				fprintf(stderr,
					"no atomic size was rejected within the test buffer\n");
				local_status = -FI_EOVERFLOW;
			} else {
				count = probe;
			}
		}
	}

	/*
	 * The server receives the verified count while progressing EFA in
	 * ft_sock_sync(). A negative value communicates an initiator failure.
	 */
	peer_value = ft_sock_sync(oob_sock,
				  opts.dst_addr ?
				  (local_status ? local_status : (int)count) :
				  0);
	if (!opts.dst_addr) {
		if (peer_value < 0) {
			local_status = peer_value;
		} else if (!peer_value ||
			   (size_t)peer_value >= ATOMIC_TEST_BUF_SIZE) {
			local_status = -FI_EOVERFLOW;
		} else {
			count = (size_t)peer_value;
			memset(rx_buf, 0xcc, ATOMIC_TEST_BUF_SIZE);
			memset(expected, 0xcc, sizeof(expected));
			memset(expected, 0x6d, count);
		}
	} else if (peer_value) {
		local_status = peer_value;
	}

	ret = exchange_status(local_status);
	if (ret)
		return ret;

	/*
	 * The largest successful write (count bytes of 0x6d) plus one sentinel
	 * byte beyond it detect any later full or partial application of the
	 * oversized 0xb4 request.
	 */
	if (opts.dst_addr)
		local_status = post_write(count, 0x6d);
	else
		local_status = wait_for_target(expected, count + 1);

	ret = exchange_status(local_status);
	if (ret)
		return ret;

	if (opts.dst_addr) {
		ret = post_write(fail_count, 0xb4);
		if (ret != -FI_ETRUNC) {
			fprintf(stderr,
				"oversize atomic returned %d instead of -FI_ETRUNC\n",
				ret);
			local_status = ret ? ret : -FI_EOTHER;
		} else {
			local_status = verify_no_tx_completion(20);
		}
	} else {
		uint64_t deadline = ft_gettime_ms() + 20;

		do {
			ft_force_progress();
			usleep(100);
		} while (ft_gettime_ms() < deadline);

		if (memcmp(rx_buf, expected, count + 1)) {
			fprintf(stderr,
				"oversize atomic changed target memory\n");
			local_status = -FI_EOTHER;
		}
	}

	ret = exchange_status(local_status);
	if (!ret && !opts.dst_addr)
		printf("PASS: single-packet boundary (%zu bytes verified)\n",
		       count);
	return ret;
}

static int run(void)
{
	int ret, finalize_ret;

	ret = ft_init_fabric();
	if (ret)
		return ret;

	if (fi->tx_attr->iov_limit < ATOMIC_TEST_IOV_COUNT ||
	    fi->tx_attr->rma_iov_limit < 1) {
		fprintf(stderr, "provider does not support required atomic IOVs\n");
		ret = -FI_ENOSYS;
		goto out;
	}

	ret = ft_exchange_keys(&remote);
	if (ret)
		goto out;

	ret = test_atomicmsg();
	if (ret)
		goto out;
	ret = test_inject();
	if (ret)
		goto out;
	ret = test_single_packet_boundary();

out:
	finalize_ret = ft_finalize();
	return ret ? ret : finalize_ret;
}

static void print_usage(const char *name)
{
	ft_csusage((char *)name,
		   "EFA atomic WRITE/DC_WRITE/inject protocol tests");
	FT_PRINT_OPTS_USAGE("-U",
			    "set FI_DELIVERY_COMPLETE as the endpoint default");
}

int main(int argc, char **argv)
{
	int op, ret, cleanup_ret;

	opts = INIT_OPTS;
	opts.options |= FT_OPT_SIZE | FT_OPT_OOB_SYNC;
	opts.transfer_size = ATOMIC_TEST_BUF_SIZE;
	opts.window_size = 1;

	hints = fi_allocinfo();
	if (!hints)
		return EXIT_FAILURE;

	while ((op = getopt_long(argc, argv, "Uh" ADDR_OPTS INFO_OPTS CS_OPTS,
				 long_opts, &lopt_idx)) != -1) {
		switch (op) {
		case 'U':
			hints->tx_attr->op_flags |= FI_DELIVERY_COMPLETE;
			break;
		case '?':
		case 'h':
			print_usage(argv[0]);
			ft_longopts_usage();
			return EXIT_FAILURE;
		default:
			if (!ft_parse_long_opts(op, optarg))
				continue;
			ft_parse_addr_opts(op, optarg, &opts);
			ft_parseinfo(op, optarg, hints, &opts);
			ft_parsecsopts(op, optarg, &opts);
			break;
		}
	}

	if (optind < argc)
		opts.dst_addr = argv[optind];

	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_MSG | FI_ATOMICS;
	hints->mode = FI_CONTEXT | FI_CONTEXT2;
	hints->domain_attr->mr_mode = opts.mr_mode;
	hints->addr_format = opts.address_format;

	ret = run();
	cleanup_ret = ft_free_res();
	return ft_exit_code(ret ? ret : cleanup_ret);
}
