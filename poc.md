# Eager read / eager write for efa-proto: proof of concept

Ticket: [P555154859](https://t.corp.amazon.com/P555154859/communication)

## 1. Problem

NIXL has to use efa-proto (not efa-direct) so it can talk across all Nitro
generations. For KV-cache transfers it posts many one-sided RMA ops of 16 KB.
On efa-proto these run far below efa-direct:

| p5en, NIXL, 16 KB, GPU memory | READ | WRITE |
|---|---|---|
| efa-direct | 34.8 GB/s | 44.8 GB/s |
| efa-proto | 25.4 GB/s (73%) | 25.1 GB/s (56%) |

The hardware work is the same in both cases: one RDMA read or write per op.
The difference is software bookkeeping. Goal: reach at least 80% of efa-direct
on efa-proto, without making efa-proto become efa-direct.

## 2. Why efa-proto is slow for small RMA

efa-proto must handle every RMA, including the hard ones:

- an op larger than the device limit (1 GB) is split into many WQEs;
- a peer whose capabilities are unknown needs a handshake first;
- a peer that cannot do RDMA needs an emulated protocol (send based);
- a WQE that hits Receiver-Not-Ready (RNR) must be queued and reposted;
- a peer removed from the address vector must have its ops cleaned up.

To support all of this, every op gets a TX entry (TXE, 912 bytes) that
records the whole op, and every WQE gets a packet entry (PKE) that records
enough to repost that WQE. Both are allocated, filled, linked into peer and
endpoint lists, and released on completion.

For a 16 KB op that fits in one WQE, needs no handshake, and cannot see RNR,
none of that state is ever used. It is pure overhead, paid about two million
times per second.

## 3. The idea: one op = one WQE = one small entry

Split RMA into two classes:

| Class | Needs | Path |
|---|---|---|
| **eager** | one WQE, no possible recovery | new: minimal state |
| everything else | segmentation, handshake, emulation, retry | existing TXE path |

This mirrors the eager *send* protocol, where a message that fits one packet
takes a one-packet path. We add the RMA equivalents: **eager read** and
**eager write**.

For an eager op, the only question the completion has to answer is *"which
application op was this, and did it succeed?"*. So the state shrinks to a
single 64-byte entry:

| Field | Used for |
|---|---|
| application context | the CQ entry the application reads |
| read or write | the CQ entry flags (`FI_READ` / `FI_WRITE`), counters |
| op flags | whether the application asked for a completion |
| length, buffer | the error entry, if the op fails |
| generation | detect a stale or duplicate completion |

This entry is the "necessary TXE" for eager RMA. Everything else in the TXE
(peer pointer, IOV arrays, byte counters, list links, protocol state, RNR
state, MR bookkeeping) is dropped.

## 4. How one entry stands for one WQE (happy path)

The device returns the work request ID (`wr_id`) of each WQE in its
completion (CQE). Today `wr_id` names a PKE, and the PKE leads to the TXE.
For eager RMA, `wr_id` names the 64-byte entry directly:

```
post:  take entry -> fill it -> post 1 WQE, wr_id = entry
CQE:   wr_id -> entry -> write CQ entry for the application -> free entry
```

One bit of `wr_id` marks it as an eager entry, so the completion path can
tell it apart from a PKE before reading anything. The generation in the low
bits catches a completion that arrives for an entry already reused.

### Options we compared

| Where the state lives | READ | WRITE | Verdict |
|---|---|---|---|
| TXE + PKE (today) | 73% | 56% | baseline |
| one PKE, no TXE (A) | 87% | 84% | works; uses a large send buffer as a note |
| **one 64-byte entry (B)** | **89%** | **86%** | **chosen** |
| no state, context in `wr_id` (Z) | 94% | 86% | upper bound only; see below |

Z is the absolute floor of bookkeeping, measured only to know the limit. It
is not safe: it cannot detect a stale completion, cannot report the failed
op's length, and steals bits from the application's pointer. B keeps those
properties for 64 bytes, at a cost of about 5-8% on READ and nothing on WRITE.

## 5. Error paths

The rule that makes this small: **an op takes the eager path only if no error
can need software recovery.** Then every error is simply reported.

| Case | Can it happen on an eager op? | Handling |
|---|---|---|
| RNR on read | No: a read uses no receive buffer at the target | - |
| RNR on write | No: a plain write uses no receive buffer | - |
| RNR on write with remote CQ data | Only if the target takes a receive buffer | eligible only if it does not, or the device retries RNR forever (NIXL sets this) |
| handshake not done | The op is not eligible | existing path; it starts the handshake and waits |
| peer cannot do RDMA | The op is not eligible | existing emulated protocol |
| op larger than one WQE | The op is not eligible | existing segmented path |
| bad key, unreachable peer, any device error | Yes | one CQ error entry with the app context; no retry |
| an RNR still arrives (defensive) | Should not | reported as an error, never queued |
| peer removed while the op is in flight | Yes | completion is still reported; the entry does not point at the peer |
| endpoint closed while the op is in flight | Yes | the entry is freed; nothing reported |
| MR tracking enabled | The op is not eligible | existing path, which tracks the MR |

So the eager path has exactly one error behaviour, and every hard case stays
on the path that already handles it.

## 6. What is eligible

An RMA op takes the eager path when all of these hold; otherwise nothing
changes for it:

- one local buffer and one remote buffer of the same length;
- the length fits in one WQE (below the read/write segment size);
- the peer's capabilities are known (handshake done, or all peers are the
  same);
- the device can do RDMA read / write to this peer for this memory;
- op flags are only completion, transmit-complete, "more to come", and for
  writes, remote CQ data;
- the remote CQ data rule above holds;
- not an op to self, and MR tracking is off.

NIXL's 16 KB reads and writes meet all of these after the first few ops of a
connection (which go through the handshake).

## 7. Results

p5en.48xlarge x2, NIXL nixlbench, VRAM to VRAM, 16 KB blocks, batch 22500,
1000 iterations, interleaved runs, median of 3 (GB/s, % of efa-direct).

| | READ | WRITE |
|---|---|---|
| efa-direct | 34.8 | 44.8 |
| efa-proto, eager off | 25.4 (73%) | 25.1 (56%) |
| earlier zero-state prototype | 33.6 (97%) | 25.3 (56%) * |
| eager, one PKE (A) | 30.3 (87%) | 37.8 (84%) |
| **eager, 64-byte entry (B)** | **31.0 (89%)** | **38.7 (86%)** |

\* The earlier prototype refused writes with remote CQ data and "more to
come", which are exactly what NIXL uses, so its writes took the slow path.

Batch 15000 gives the same picture (B: READ 89%, WRITE 86%). NIXL's data
check passes on B for both READ and WRITE. 697 unit tests pass.

## 8. What is left

- **READ, last 5-8%.** It is the cost of taking and returning the entry. It
  could go to zero by keeping the entry inside the application's
  `fi_context2`, as efa-direct does; that needs NIXL to request
  `FI_CONTEXT2` on efa-proto too.
- **WRITE, last 14%.** Not caused by the entry (B equals Z). The likely
  cause is that NIXL disables unsolicited write recv, so every write with
  CQ data takes a receive buffer at the target. To be investigated.
- **More coverage.** Real-device error injection, fabtests, multi-thread,
  MR close during flight.
- **Default.** The path is off unless `FI_EFA_RMA_FAST_PATH=1`.

## 9. Data

All raw logs are on devCluster-p5en under `~/rma-poc/results/`:
`summary-m1.txt` (A), `summary-m2.txt` (all builds, both batch sizes),
`summary-m3.txt` (Z and the data checks).
