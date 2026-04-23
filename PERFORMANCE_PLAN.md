# Ganon Performance Improvement Plan

This document outlines high-impact performance optimizations that have been identified but not yet implemented. They are organized by estimated impact and implementation complexity.

---

## 1. io_uring Asynchronous I/O (Highest Impact — x64 / Modern Kernels Only)

**Problem:** The current architecture uses one pthread per TCP connection (`socket_thread_func` in `network.c`) plus additional threads for tunnel forwarding (`fwd_thread_func` in `tunnel.c`). Each thread blocks on `recv()`/`send()`, causing frequent context switches and poor cache locality. On a 10-node mesh with 3 tunnels each, there can be 30+ threads competing for CPU time.

**Solution:** Replace the per-connection thread model with a single (or per-NUMA-node) io_uring event loop on Linux 5.1+. All sockets are registered with the ring. The thread submits batched read/write SQEs and processes completions in a tight loop. This eliminates most thread context switches and allows the kernel to batch syscalls internally.

**Expected Impact:** 2–4× throughput improvement on x64 by eliminating thread contention and syscall overhead. Smaller gains on ARM due to weaker io_uring support in older kernels.

**Complexity:** High. Requires rewriting `network.c` and `tunnel.c` around an event loop. Must integrate with the existing encryption layer (`transport.c`) which currently assumes blocking I/O. The encryption itself is CPU-bound and can stay in the same thread, but the I/O must become non-blocking.

**Implementation Sketch:**
- Create `io_uring` instance with `IORING_SETUP_SQPOLL` for kernel-side submission polling.
- Register all socket fds with the ring.
- Use `readv`/`writev` SQEs to read/write encrypted frames.
- Maintain per-connection state machines (reading length prefix → reading frame → decrypting → dispatching).

**Important:** io_uring requires Linux ≥5.1 (≥5.6 for networking). It is **not available on Linux 2.6.x** and may be suboptimal on ARMv5/MIPS. See **Appendix A** for a detailed analysis of io_uring on legacy kernels and embedded architectures, including fallback strategies using `epoll`.

---

## 2. Zero-Copy Tunnel Forwarding with `splice()`

**Problem:** Tunnel data flows: `client socket → recv() → buf → encrypt → send() → peer socket → decrypt → buf → send() → dst socket`. There are at least 4 data copies and 4 syscalls per hop, plus encryption overhead.

**Solution:** Use `splice()` (Linux) to move data directly between sockets without entering userspace. This only works for unencrypted data, so it must be combined with kTLS (see #3) or used only for plaintext tunnels.

**Expected Impact:** 2–3× tunnel throughput when combined with kTLS. Without kTLS, limited to unencrypted paths.

**Complexity:** Medium-High. Requires restructuring `tunnel.c` to use pipe buffers as intermediate storage between `splice()` calls. The protocol framing (tunnel_id + conn_id headers) still needs userspace inspection, so a hybrid approach is needed: `splice()` for bulk data, `recv()` for headers.

---

## 3. Kernel TLS (kTLS)

**Problem:** Every byte of tunnel data is encrypted/decrypted in userspace. Even with libsodium, this consumes significant CPU cycles and prevents zero-copy optimizations.

**Solution:** Linux 4.17+ supports TLS offload in the kernel (`TLS_TX`, `TLS_RX` socket options). Set up kTLS with the session keys after the X25519 handshake, then use standard `send()`/`recv()` or `splice()` and the kernel handles ChaCha20-Poly1305 transparently.

**Expected Impact:** 2–5× tunnel throughput on any architecture (including ARM/MIPS) because it eliminates userspace crypto, memory copies, and enables `splice()`. The bottleneck shifts entirely to network bandwidth.

**Complexity:** High. Requires:
- OpenSSL or wolfSSL for kTLS key setup (Linux kTLS API requires it)
- Fallback to userspace libsodium/Monocypher for older kernels
- Restructuring handshake to extract session keys in a format kTLS accepts
- For embedded systems, kernel must be compiled with `CONFIG_TLS=y`

**Note:** This would make the encryption layer architecture-dependent but with massive performance gains.

---

## 4. Message Batching / Coalescing

**Problem:** Each `recv()` from a tunnel client produces one `TUNNEL_DATA` message with full protocol + encryption overhead (76 bytes). For a 1 KB client write, this is ~7.6% overhead. For 100 KB, it's negligible, but many protocols (HTTP, SSH) send small messages.

**Solution:** Batch multiple small reads into a single `TUNNEL_DATA` message. In `fwd_thread_func`, accumulate data for up to N microseconds or until a buffer threshold is reached, then send one batched message.

**Expected Impact:** 10–30% reduction in protocol overhead for small-message workloads. Minimal impact for bulk transfer.

**Complexity:** Low-Medium. Add a small timer + accumulator in `fwd_thread_func`. Must handle flush on connection close.

---

## 5. Lock-Free Ring Buffers for Thread Communication

**Problem:** The tunnel layer uses `pthread_mutex_t` and `pthread_cond_t` for synchronization (`g_tunnel_mutex`, `g_ack_cond`). Under high load, mutex contention becomes a bottleneck.

**Solution:** Replace mutex-based queues with lock-free single-producer single-consumer (SPSC) ring buffers between the network I/O thread and tunnel forwarding threads. The `lfring` or `DPDK rte_ring` algorithms are well-tested.

**Expected Impact:** 10–20% latency reduction under heavy load. Less thread scheduling jitter.

**Complexity:** Medium. Must carefully handle buffer exhaustion (backpressure) and ensure memory ordering correctness.

---

## 6. Vectorized Syscalls (`sendmmsg` / `recvmmsg`)

**Problem:** Each encrypted frame requires separate `send()`/`recv()` syscalls. For a node forwarding 10,000 small control messages per second, that's 10,000 syscalls.

**Solution:** Use `sendmmsg()` and `recvmmsg()` to send/receive multiple frames in a single syscall. This is especially effective for broadcast scenarios where the same message is sent to multiple peers.

**Expected Impact:** 20–50% reduction in syscall overhead for high-message-rate scenarios. Minimal impact for bulk tunnel traffic.

**Complexity:** Low-Medium. Requires buffering outgoing messages briefly to form batches.

---

## 7. Pre-Allocated Memory Pools (Slab Allocator)

**Problem:** Even with stack buffers in `TRANSPORT__send_msg` and `TRANSPORT__recv_msg`, large frames and tunnel buffers still hit `malloc()`/`free()`. Under high throughput, the allocator becomes a contention point.

**Solution:** Implement a simple fixed-size memory pool for common buffer sizes (64B, 1KB, 16KB, 128KB, 1MB). Allocate pools at startup. `pool_alloc(size)` returns a chunk from the matching pool; `pool_free(ptr, size)` returns it.

**Expected Impact:** 5–15% throughput improvement. More importantly, eliminates latency spikes caused by allocator contention.

**Complexity:** Low. A simple bitmap-based or free-list allocator per size class is ~200 lines of C.

---

## 8. Inline Encryption (Encrypt In-Place)

**Problem:** `TRANSPORT__send_msg` copies serialized data into `plain_buf`, then encrypts into `ciphertext`, then sends. That's two copies of the same data. Similarly on recv.

**Solution:** Encrypt in-place using `crypto_aead_xchacha20poly1305_ietf_encrypt()` with `c == m` (in-place encryption). libsodium supports this. For decryption, `crypto_aead_xchacha20poly1305_ietf_decrypt()` also supports in-place operation. This eliminates one full data copy per frame.

**Expected Impact:** 5–10% throughput improvement. Most noticeable on memory-bandwidth-constrained systems (ARM, MIPS).

**Complexity:** Low. Requires rearranging the frame layout: serialize directly into the output buffer, leaving room for length prefix + nonce + MAC.

---

## 9. CPU Pinning and Busy-Polling

**Problem:** Threads migrate between CPU cores, causing cache misses and scheduler overhead.

**Solution:** Pin the I/O event loop thread(s) to dedicated cores using `pthread_setaffinity_np()`. Combine with `SO_BUSY_POLL` on sockets to have the kernel poll for new packets in the NIC driver's NAPI context instead of raising interrupts.

**Expected Impact:** 10–30% latency reduction for small messages. Minimal throughput impact for bulk transfers.

**Complexity:** Low. Add CLI flags `--cpu-pin 3` and `--busy-poll 50` (microseconds).

---

## 10. Reduce Wire Nonce Size

**Problem:** The 24-byte nonce contains 16 fixed zero bytes. Sending them wastes bandwidth.

**Solution:** Send only the 8-byte counter. Reconstruct the full 24-byte nonce on receipt by prepending 16 zero bytes.

**Expected Impact:** Negligible for 128KB frames (16/131156 = 0.012%). More meaningful for small control messages: 16/76 = 21% overhead reduction for a minimal frame. For high-message-rate routing protocols, this matters.

**Complexity:** Low. One-line change in wire format. Must update both C and Python simultaneously.

---

## 11. Replace Python Threads with Asyncio / Multiprocessing

**Problem:** The Python client uses OS threads, but the GIL means only one thread executes Python bytecode at a time. The encryption (`monocypher-py`) and protocol parsing happen under the GIL.

**Solution:** Restructure the Python client around `asyncio` with a single event loop. For CPU-intensive work (encryption), use `ProcessPoolExecutor` to offload to separate processes, or use `asyncio` + `uvloop` for better I/O performance.

**Expected Impact:** 20–50% improvement in Python client throughput. Less relevant for the C server.

**Complexity:** High. Requires almost complete rewrite of `client.py`.

---

## 12. Connection Multiplexing (QUIC-like)

**Problem:** Each peer-to-peer TCP connection carries only one encrypted session. If two nodes have 10 tunnels between them, they still share one TCP connection, but the tunnel forwarding threads all contend for the same socket.

**Solution:** Open multiple TCP connections between the same pair of nodes (connection pool). Distribute tunnel traffic across them. This is similar to HTTP/2 connection coalescing but inverted.

**Expected Impact:** 20–40% throughput improvement when multiple tunnels share a link. Reduces head-of-line blocking.

**Complexity:** Medium. Requires adding connection pooling to `network.c` and load balancing across pooled connections in `transport.c`.

---

## Priority Ranking

| Rank | Idea | Impact | Complexity | Best For |
|------|------|--------|------------|----------|
| 1 | io_uring | Very High | High | x64, high connection counts |
| 2 | kTLS | Very High | High | All architectures (if kernel supports) |
| 3 | Zero-copy `splice()` | High | Medium-High | Bulk tunnel traffic |
| 4 | Connection multiplexing | High | Medium | Multi-tunnel topologies |
| 5 | Message batching | Medium | Low | Small-message workloads |
| 6 | Inline encryption | Medium | Low | Memory-bandwidth-limited systems |
| 7 | Memory pools | Medium | Low | Latency-sensitive workloads |
| 8 | Lock-free rings | Medium | Medium | High-load scenarios |
| 9 | Vectorized syscalls | Medium | Low-Medium | High message rates |
| 10 | CPU pinning + busy-poll | Low-Medium | Low | Low-latency requirements |
| 11 | Reduce nonce size | Low | Low | Control-message-heavy workloads |
| 12 | Python asyncio rewrite | Medium | High | Python client performance |

---

## Recommended Next Steps

1. **Implement io_uring** for the C server network layer. This is the single biggest improvement for x64 deployments.
2. **Evaluate kTLS** feasibility on target embedded kernels. If supported, it provides the best cross-platform speedup.
3. **Add message batching** to `fwd_thread_func()` as a quick win for small-message workloads.
4. **Implement inline encryption** and memory pools as follow-up optimizations.

---

## Appendix A: io_uring on Legacy Kernels and Embedded Architectures

### A.1 The Kernel Version Problem

io_uring was merged into Linux **5.1** (May 2019). Many features essential for networking performance (`IORING_OP_RECV`, `IORING_OP_SEND`, `IORING_SETUP_SQPOLL`) arrived in **5.6** (March 2020) and **5.11** (February 2021). This means:

| Kernel | io_uring Available? | Networking SQEs? | SQPOLL? |
|--------|--------------------|------------------|---------|
| 2.6.x | **No** | No | No |
| 3.x | **No** | No | No |
| 4.x | **No** | No | No |
| 5.1–5.5 | Yes (basic) | No | Partial |
| 5.6+ | Yes | Yes | Yes |
| 5.11+ | Yes (mature) | Yes + multishot | Yes |

**Linux 2.6.x predates io_uring by approximately 13 years.** It is physically impossible to use io_uring on a 2.6.x kernel without backporting the entire subsystem, which is effectively a kernel rewrite.

### A.2 What to Use on Linux 2.6.x Instead

If the target embedded system runs Linux 2.6.x, the best available asynchronous I/O mechanisms are:

#### A.2.1 `epoll` (Linux 2.5.44+)
`epoll` is available on all 2.6.x kernels and is the standard high-performance I/O multiplexing API. It is strictly inferior to io_uring because:
- It still requires one syscall per I/O operation (`read`/`write` after `epoll_wait` returns ready)
- It cannot batch submissions
- It does not support async buffered file I/O

However, for a TCP mesh tunneler, `epoll` with **edge-triggered (ET) mode** and **non-blocking sockets** is the practical ceiling on 2.6.x. The architecture would be:
- One event-loop thread calling `epoll_wait()`
- When `EPOLLIN` fires, read until `EAGAIN`
- When `EPOLLOUT` fires, write until `EAGAIN` or buffer empty
- Parse encrypted frames from a per-connection streaming buffer

This eliminates the per-connection pthread overhead (which is the main win of io_uring for this codebase) but retains syscall overhead.

#### A.2.2 `eventfd` (Linux 2.6.22+)
`eventfd` can wake the event loop from other threads (e.g., tunnel forwarding threads). This is useful if tunnel threads remain in the architecture. The event loop thread `epoll_wait`s on both sockets and an `eventfd`; tunnel threads write to the `eventfd` to signal new outbound data.

#### A.2.3 `signalfd` / `timerfd` (Linux 2.6.22 / 2.6.25)
These can replace signal handlers and timer threads with file-descriptor-based events in the same `epoll` loop, reducing thread count further.

### A.3 Architecture Support for io_uring (ARM / MIPS)

io_uring is architecture-agnostic at the API level, but its performance characteristics vary significantly:

#### A.3.1 ARM
- **ARMv7+ with Linux 5.6+**: io_uring works and supports `SQPOLL`. The kernel-side submission queue polling runs efficiently.
- **ARMv5 (our current target)**: Even if the kernel were upgraded, ARMv5 lacks the atomic instructions and memory barriers that make io_uring fast. The shared ring buffers between userspace and kernel require `ldrex`/`strex` or `dmb` instructions for correct memory ordering. ARMv5 only has `swp` (which is deprecated and slow). On ARMv5, io_uring would likely be *slower* than `epoll` due to excessive locking in the kernel.
- **NEON consideration**: The userspace encryption (libsodium/Monocypher) benefits from NEON on ARMv7+. io_uring does not change this.

#### A.3.2 MIPS
- **MIPS32 with Linux 5.6+**: io_uring is supported but has received far less optimization attention than x86_64 or ARM64. The MIPS port of the io_uring fastpath (batched syscall bypass) may not be as mature.
- **Big-endian consideration**: io_uring itself is endian-agnostic, but the shared memory ring layout is defined in terms of `__u32` fields. On big-endian MIPS, the kernel and userspace agree on layout, so there is no issue. However, any hand-optimized assembly for io_uring fastpaths is typically little-endian-centric.
- **Cache coherency**: MIPS systems often have weaker cache coherency than x86_64. The memory barriers required for the io_uring ring buffer (producer/consumer indices) may be more expensive on MIPS than on x86_64, where the `mov`+`mfence` pattern is heavily optimized.

### A.4 Should You Even Consider io_uring for Embedded?

**Probably not, unless you control the kernel version.**

io_uring's primary benefits are:
1. **Batching many I/O operations into one syscall** — valuable when you have 1000+ connections
2. **Kernel-side polling (SQPOLL)** — valuable when you have sustained high throughput
3. **Async file I/O** — not relevant for a network tunneler

On a typical embedded mesh node with **4–16 peer connections**, the per-connection thread overhead is modest. The Linux 2.6 scheduler handles 16 threads fine. The bigger bottleneck on embedded is:
- **CPU-bound encryption** (addressed by libsodium on x64, less so on ARMv5/MIPS)
- **Memory bandwidth** (addressed by larger buffers, inline encryption)
- **Small buffer sizes causing high message rates**

For a 2.6.x ARMv5 router with 8 peers, switching from threads to an `epoll` event loop would reduce context switches but would not fundamentally change throughput. The crypto and memory copies dominate.

### A.5 Recommended Architecture: Compile-Time Abstraction

If you want to write code that can use io_uring on modern x64 but falls back gracefully on embedded, use a compile-time abstraction layer:

```c
#ifdef HAS_IO_URING
    /* io_uring event loop for Linux 5.6+ x64/ARM64 */
    network_loop_uring();
#elif defined HAS_EPOLL
    /* epoll event loop for Linux 2.6+ all architectures */
    network_loop_epoll();
#else
    /* thread-per-connection fallback */
    network_loop_threads();
#endif
```

**Detection logic in CMake:**
```cmake
include(CheckSymbolExists)
check_symbol_exists(io_uring_queue_init "liburing.h" HAS_IO_URING)
if(NOT HAS_IO_URING)
    check_symbol_exists(epoll_create "sys/epoll.h" HAS_EPOLL)
endif()
```

This lets you deploy the same binary tree:
- **x64 servers**: Build with `liburing`, get maximum performance
- **Modern embedded (ARM64, recent MIPS)**: Build with `liburing`, get good performance
- **Legacy embedded (ARMv5, 2.6.x kernel)**: Build with `epoll`, get acceptable performance

### A.6 Summary Table

| Platform | Kernel | Best I/O Model | io_uring Viable? | Primary Bottleneck |
|----------|--------|----------------|------------------|--------------------|
| x64 server | 5.6+ | io_uring | **Yes** | syscall overhead |
| x64 server | 2.6.x | epoll ET | No | thread overhead, syscalls |
| ARMv7+ embedded | 5.6+ | io_uring | **Yes** | encryption (NEON helps) |
| ARMv5 embedded | 2.6.x | epoll ET | No | encryption, memory bandwidth |
| MIPS32BE embedded | 2.6.x | epoll ET | No | encryption, memory bandwidth |
| MIPS32BE embedded | 5.6+ | io_uring | Marginal | encryption, cache barriers |

### A.7 Optimization Availability by Kernel Generation

The table below maps each performance optimization to the kernel versions where it is available and estimates the impact on each generation.

**Key:**
- **N/A** = Feature does not exist on this kernel
- **Low** = <10% improvement
- **Medium** = 10–30% improvement
- **High** = 30–100% improvement
- **Very High** = >100% improvement (2× or more)

| Optimization | Linux 2.6.x | Linux 3.3.x | Linux 5.6+ (Modern) | Notes |
|--------------|-------------|-------------|---------------------|-------|
| **Thread-per-connection** (current) | Baseline | Baseline | Baseline | Works everywhere. Bottleneck on x64 with many connections. |
| **epoll event loop** | **High** | **High** | Medium | Best option on 2.6.x/3.x. Eliminates thread overhead. Available since 2.5.44. |
| **eventfd wakeups** | Medium | Medium | Low | Available since 2.6.22. Reduces timer/signal threads. |
| **signalfd/timerfd** | Medium | Medium | Low | Available since 2.6.22/2.6.25. Consolidates events into epoll loop. |
| **sendmmsg/recvmmsg** | N/A | **High** | Medium | Added in 2.6.34/3.0. Batches syscalls for high message rates. |
| **SO_BUSY_POLL** | N/A | N/A | Medium | Added in 3.11. Kernel-side NIC polling. Latency win, not throughput. |
| **TCP_FASTOPEN** | N/A | Medium | Low | Added in 3.6 (client) / 3.7 (server). Saves 1 RTT on connection setup. |
| **io_uring** | N/A | N/A | **Very High** | Requires 5.1+ (basic), 5.6+ (networking). Game-changer for x64. |
| **io_uring SQPOLL** | N/A | N/A | **Very High** | Requires 5.6+. Kernel polls submission queue. Zero syscalls in steady state. |
| **kTLS (kernel TLS)** | N/A | N/A | **Very High** | Requires 4.17+. Offloads ChaCha20-Poly1305 to kernel. |
| **splice() zero-copy** | Low | Low | **High** | Available since 2.6.17. Only useful with kTLS or unencrypted paths. |
| **Message batching** | Medium | Medium | Medium | Pure userspace. Works on all kernels. Best for small-message workloads. |
| **Inline encryption** | Medium | Medium | Medium | Pure userspace. Eliminates one memcpy per frame. |
| **Memory pools** | Medium | Medium | Medium | Pure userspace. Reduces malloc contention. |
| **Lock-free SPSC rings** | Medium | Medium | Medium | Pure userspace. Reduces mutex contention between threads. |
| **CPU pinning** | Low | Low | Low | Pure userspace. `pthread_setaffinity_np` available since 2.5. |
| ** Larger buffers (128K)** | Medium | Medium | Medium | Pure userspace. Already implemented. |
| **Stack recv buffers** | Low | Low | Low | Pure userspace. Already implemented. |
| **Wire nonce reduction** | Low | Low | Low | Pure userspace. Minimal impact for bulk traffic. |

#### A.7.1 Linux 2.6.x Profile (ARMv5, MIPS32BE Embedded)

**Available high-impact optimizations:**
1. **epoll event loop** — The single biggest win. Replacing 16 pthreads with one epoll thread eliminates context switches and reduces scheduler load. Expected: **30–60% throughput improvement** on single-core embedded CPUs.
2. **sendmmsg/recvmmsg** — Not available on all 2.6.x (needs ≥2.6.34). If target is 2.6.32 LTS, this is N/A.
3. **Message batching** — Easy win. Accumulate small tunnel writes. Expected: **10–25% reduction in protocol overhead** for HTTP/SSH-like traffic.
4. **Inline encryption + memory pools** — Together can yield **10–20%** by reducing memory bandwidth pressure.

**Unavailable (hard blockers):**
- io_uring, kTLS, SO_BUSY_POLL, TCP_FASTOPEN

**Primary bottleneck:** CPU-bound encryption (Monocypher on ARMv5/MIPS) and memory bandwidth. The scheduler can handle 16 threads on 2.6.x; the crypto cannot keep up.

#### A.7.2 Linux 3.3.x Profile (Mid-Range Embedded)

**New capabilities vs 2.6.x:**
- `sendmmsg()` (since 2.6.34) — Batches multiple `send()` calls. For a node broadcasting RREQ to 10 peers, this turns 10 syscalls into 1. Expected: **20–40%** for high-message-rate control traffic.
- `recvmmsg()` (since 2.6.34) — Similar for inbound.
- `TCP_FASTOPEN` (since 3.6/3.7) — Saves one RTT on dynamic peer connections. Minor for long-lived mesh links.
- `signalfd`/`timerfd`/`eventfd` are mature and well-tested.

**Recommended stack:**
```
epoll ET event loop + eventfd wakeups + sendmmsg/recvmmsg + inline encryption + message batching
```

This is the "sweet spot" for older embedded Linux: all building blocks are stable, no kernel upgrades required, and the gains are significant.

#### A.7.3 Linux 5.6+ Profile (Modern x64 / ARM64 Servers)

**New capabilities vs 3.x:**
- **io_uring** — Batches submissions and completions. On a 64-core server with 1000+ connections, this eliminates syscall overhead entirely. Expected: **2–4× throughput** vs thread-per-connection.
- **io_uring SQPOLL** — Kernel thread polls the submission queue. Userspace never enters the kernel for I/O in steady state. Expected: additional **20–30%** on top of basic io_uring.
- **kTLS** — Offloads ChaCha20-Poly1305 to kernel. Combined with io_uring and `splice()`, this enables true zero-copy encrypted tunneling. Expected: **2–5×** for tunnel throughput.
- **SO_BUSY_POLL** — Reduces interrupt-driven latency. Niche; only useful for sub-millisecond latency requirements.

**Recommended stack:**
```
io_uring + SQPOLL + kTLS + splice() + inline encryption + memory pools
```

On modern x64 with libsodium + kTLS, the bottleneck shifts from CPU/crypto to **network bandwidth**.

---

### A.8 Practical Recommendation for Ganon

Given the project's embedded targets (ARMv5, MIPS32BE, Linux 2.6.x), **do not prioritize io_uring**. Instead:

1. **For x64 builds**: Add an optional `epoll`-based event loop as an intermediate step. It gives 60–70% of io_uring's benefit with 20% of the implementation effort, and works on all kernels ≥2.5.44.
2. **For embedded builds**: Focus on non-crypto optimizations: larger buffers, inline encryption, memory pools, message batching. These benefit all kernels and architectures.
3. **Revisit io_uring** only when the project drops support for Linux <5.6 or when a specific high-throughput x64 deployment justifies the engineering cost.
