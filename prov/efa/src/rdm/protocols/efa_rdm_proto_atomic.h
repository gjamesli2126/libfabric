/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Amazon.com, Inc. or its affiliates. All rights reserved. */

#ifndef _EFA_RDM_PROTO_ATOMIC_H
#define _EFA_RDM_PROTO_ATOMIC_H

#include "efa.h"

struct efa_rdm_ope;
struct efa_rdm_pke;

/**
 * @brief Atomic TX protocol interface.
 *
 * Atomic requests use fi_msg_atomic and carry RMA IOVs in their request
 * headers. Keeping this interface separate from the message protocol avoids
 * coupling the two paths.
 */
struct efa_rdm_atomic_proto {
	int req_pkt_type;
	int req_pkt_type_dc;
	size_t (*get_req_data_size)(struct efa_rdm_ope *txe);
	ssize_t (*init_req_pke)(struct efa_rdm_pke *pkt_entry,
				struct efa_rdm_ope *txe);
	void (*handle_send_completion)(struct efa_rdm_pke *pkt_entry);
};

extern const struct efa_rdm_atomic_proto efa_rdm_atomic_write_proto;

ssize_t efa_rdm_proto_atomic_post(struct efa_rdm_ope *txe);

#endif /* _EFA_RDM_PROTO_ATOMIC_H */
