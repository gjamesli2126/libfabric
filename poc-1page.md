# Eager read / eager write for efa-proto (1-page)

## Problem

On efa-proto, every `fi_read` / `fi_write` is treated as if it might be large,
might need retry, and might need to wait for a handshake. So each op carries a
full TX entry (TXE, 912 bytes) plus a packet entry (PKE). For small, one-WQE
RMA (NIXL KV-cache transfers: 16 KB blocks), that bookkeeping costs more than
the transfer. efa-proto reaches only 73% (read) and 56% (write) of efa-direct.

## Idea

Most RMA ops are "one op = one work request (WQE)". Such an op needs no
segmentation and, under the right conditions, no recovery. Give it the
smallest possible state:

> **one application op = one WQE = one 64-byte entry**

The entry holds only what is needed to tell the application "this op is done"
(or "this op failed"): its context, read vs. write, whether it wants a
completion, and length/buffer for the error report. No TXE, no PKE.

This is the same shape as the existing eager *send* protocol (one op, one
packet), applied to native RDMA: **eager read** and **eager write**.

## When an op may take the eager path

Only when nothing can go wrong that software would have to fix:

| Condition | Why |
|---|---|
| 1 local + 1 remote buffer, fits one WQE | no segmentation |
| peer handshake already done | capabilities known; no waiting |
| no RNR possible | no software retry needed |
| supported op flags only | WRITE allows delivery-complete. Inject falls back. |

Reads and plain writes can never see RNR (they use no receive buffer). A write
with remote CQ data may, so it is eligible only if the target does not need a
receive buffer, or the device retries RNR forever. Anything else uses the
existing path unchanged.

## Errors

On the eager path every error is final: it becomes one CQ error entry with the
application's context. There is no retry and no queueing.

## Result (p5en, NIXL, 16 KB blocks, GPU memory, median of 3)

| | READ | WRITE |
|---|---|---|
| efa-proto today | 73% | 56% |
| **eager read / write** | **89%** | **86%** |

(% of efa-direct bandwidth; target was 80%.)

## Cost

About 330 lines of code, off by default (`FI_EFA_RMA_FAST_PATH=1`), and
no change to the wire protocol or to other ops.
