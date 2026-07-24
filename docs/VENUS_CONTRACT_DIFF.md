# VENUS CONTRACT DIFF — Mesa Venus cross-process GPU protocol vs. d9mt wire

Sources: `mesa/src/virtio/vulkan/vn_*.{c,h}` (guest driver), `venus-protocol/docs/VK_EXT_command_serialization.txt` (spec).
Ours: `v2/driver/mtl/mtl_wire.h` header contract, `v2/server/HANDOFF-S3.md` (V13–V19), `HANDOFF-FLOWCONTROL.md`, `HANDOFF-QUERIES.md`.
All paths below relative to `/Users/huzef/Desktop/L4D2-launcher/mesa/src/virtio/vulkan/` unless noted.

## 1. Mapped memory model — content NEVER rides the command stream

- **Host allocates, guest maps the same pages.** `vkAllocateMemory` is serialized to the host asynchronously over the ring (`vn_device_memory.c:27-48`, seqno recorded). CPU access is NOT a copy path: the guest creates a virtio-gpu **blob resource from the host's VkDeviceMemory** — `virtgpu_bo_create_from_device_memory` (`vn_renderer_virtgpu.c:631-…`) passes `blob_id = mem_id` (the Venus object id of the host VkDeviceMemory) with `VIRTGPU_BLOB_FLAG_USE_MAPPABLE|SHAREABLE` (`vn_renderer_virtgpu.c:656`); the map ioctl then mmaps the host's pages into the guest (`virtgpu_ioctl_map`, mmap PROT_READ|WRITE at `vn_renderer_virtgpu.c:273`).
- **Lazy bo creation at first map**: `vn_MapMemory2` (`vn_device_memory.c:443-505`) defers blob creation to first `vkMapMemory`; the load-bearing comment (`vn_device_memory.c:459-463`): *"vn_renderer_bo_map will block until the renderer creates the resource and injects the pages into the guest."* App pointer = `mmap_ptr + offset` (`vn_device_memory.c:501`).
- **Flush/invalidate are NOPs**: `virtgpu_bo_flush`/`virtgpu_bo_invalidate` — *"nop because kernel makes every mapping coherent"* (`vn_renderer_virtgpu.c:422-436`). `vn_FlushMappedMemoryRanges` (`vn_device_memory.c:513-531`) just forwards to those nops.
- **Ordering, not data**: the only cross-channel sync is `vkWaitRingSeqnoMESA` encoded into the blob-create execbuffer batch (`vn_device_memory.c:76-90,85,113`) so blob creation waits for the async alloc to be consumed.
- **Answer to our #1 question**: mapped-write content NEVER travels through the command stream — the spec forbids it (`venus-protocol/docs/VK_EXT_command_serialization.txt:126-133`, "pointers to mapped memory object regions" are not serializable). CPU-write→GPU-visible is solved entirely by page-table aliasing.

## 2. Ring + flow control

- Ring = SPSC circular buffer in a shmem blob; cacheline-separated `alignas(64)` atomic `head/tail/status` + data (`vn_ring.c:253-260`, `vn_ring.h:27-40`). Free-running u32 byte cursors; **consumer publishes `head` (consumed), producer publishes `tail` (produced)** — naming inverted vs ours. Release/acquire pairing (`vn_ring.c:89-101`).
- **Producer blocks ONLY on ring-full**: `vn_ring_has_space`: `cur + size - head <= buffer_size` (`vn_ring.c:209-221`); `vn_ring_wait_space` polls with `vn_relax` (`vn_ring.c:223-246`).
- **Backoff ladder** (`vn_common.c:181-293`): profiles `{base_sleep_us=160, busy_wait_order=8, warn_order=12, abort_order=16}` (`vn_common.c:200-222`). `vn_relax`: `thrd_yield()` for 2^busy_wait iters, then exponential `os_time_sleep(base << shift)` (`vn_common.c:291-292`), warn at ~3.5 s slept, **abort() at ~895 s** (`vn_common.c:285-288`).
- **Large payloads leave the ring**: commands bigger than `direct_size = buffer_size >> direct_order` (`vn_ring.c:319`) are copied into a side shmem via the ring's 1 MiB `upload` encoder (`vn_ring.c:322-323`); the ring carries only a `VkCommandStreamDescriptionMESA{resourceId, offset, size}` descriptor (`vn_ring.c:490-500`, `vn_ring_submit_locked` `vn_ring.c:617-628`).
- **Doorbell only when consumer idle**: consumer busy-polls with a 1 ms idle timeout; guest submits `vkNotifyRingMESA` through virtio-gpu ONLY if `VK_RING_STATUS_IDLE_BIT_MESA` is set, rate-limited to one per `VN_RING_IDLE_TIMEOUT_NS` = 1 ms (`vn_ring.c:18,467-479,639-646`).
- **Producer-ahead bound = bytes only.** No frame pacing in the transport; GPU-lag pacing is left to the app's own fence/acquire waits (returned via feedback, §3). Sync points: replies (below) and roundtrips (`vkSubmitVirtqueueSeqnoMESA` out-of-band + `vkWaitVirtqueueSeqnoMESA` on-ring, `vn_ring.c:740-762`).
- **Replies**: `vn_ring_submit_command` (`vn_ring.c:687-730`) — allocate reply shmem, tell host via `vkSetReplyCommandStreamMESA` (`vn_ring.c:667-685`), submit, then `vn_ring_wait_seqno` and decode the reply from the shmem's `mmap_ptr` (`vn_ring.c:718-723`). Reply presence is per-command (`VK_COMMAND_GENERATE_REPLY_BIT_EXT`, spec:27-28).

## 3. Feedback / completion — host GPU writes, guest polls shared memory

- One pattern for fences/semaphores/events/queries: a **coherent, permanently-mapped feedback buffer** (`vn_feedback_buffer_create`, `vn_feedback.c:32-100`; HOST_VISIBLE|COHERENT at `vn_feedback.c:77`; struct comment "coherent buffer with bound and mapped memory", `vn_feedback.h:59-66`), suballocated into slots (`vn_feedback.h:38-50`: `{offset, buf_handle, union {status, counter}}`).
- The guest **appends GPU commands to the app's own submission** that write results into the slot: `vkCmdFillBuffer(slot, status)` bracketed by barriers (`vn_feedback.c:367-399`); query feedback copies results+availability (64-bit, availability bit included — `vn_query_pool.c:192`) at submit time (`vn_queue.c:790-830`).
- Guest reads = a mapped-pointer load: `vn_sync_feedback_query` reads `slot->counter` (`vn_feedback.c:619-631`); `vn_GetQueryPoolResults` polls availability bits with `vn_relax` (`vn_query_feedback_wait_ready`, `vn_query_pool.c:288-355`). **Zero round trips; the transport is never involved in completion.**

## 4. Never-serialized list (spec "Others", `VK_EXT_command_serialization.txt:126-134`)

| Refused on the wire | Replacement mechanism |
|---|---|
| Pointers to mapped memory regions | blob page aliasing (§1) |
| WSI handles | guest-side WSI; images bound to blob-backed memory |
| External handles (spec:117-124) | `res_id` / dma-buf export-import (`vn_renderer.h:156-160`) |
| Function pointers | n/a (never crosses) |
| All object handles travel as u64 object ids (spec:73-77) | guest-chosen ids, no host pointers |

## 5. Liveness / error model

- **FATAL bit**: host sets `VK_RING_STATUS_FATAL_BIT_MESA` in ring status; guest checks after every submit (`vn_ring.c:453-458`) and inside waits (`vn_common.c:267-272`) → `abort()`.
- **Positive-ack heartbeat**: during any long wait the guest *clears* `VK_RING_STATUS_ALIVE_BIT_MESA` (`vn_relax_init`, `vn_common.c:229-242`; `vn_common.c:274-277`); a live host re-sets it while consuming. Single watchdog owner (`vn_watchdog_acquire`, `vn_common.c:135-154`); if the bit stays cleared past the deadline → *"aborting on expired ring alive status"* (`vn_common.c:279-283`). Plus the hard per-wait abort at 2^abort_order (~895 s). Renderer-level waits return `VK_ERROR_DEVICE_LOST` (`vn_renderer.h:123-128`).

## THE DIFF

| Contract | Venus law (cite) | Ours (handoffs) | Verdict |
|---|---|---|---|
| Mapped CPU writes | Same physical pages both sides; flush=nop; content never serialized (`vn_renderer_virtgpu.c:427`; spec:126-133) | Re-ship at seams: `noteBufferWrite`/`snapshotDirtyUniforms` → heap slab → server blit into server-owned MTLBuffer (S3 V13/V16) | **GAP — root of the whole saga** |
| Bulk data off the ring | Ring carries descriptors; payload in side shmem (`vn_ring.c:319,490-500`) | Hot records on ring; content via `heap` file slabs (S3 slice 2) | match |
| Ring layout/cursors | Cacheline-split atomic head/tail/status, free-running, rel/acq (`vn_ring.c:24-30,253-260`) | 192 B control block, free-running byte counters, rel/acq (mtl_wire.h) | match |
| Producer block | Ring-full only; yield→exp-sleep→warn→abort (`vn_common.c:245-293`) | Short-spin → futex park on `&tail`, bounded windows (mtl_wire.h KICK/PARK) | match (ours parks in kernel; theirs has abort ladder) |
| Producer-ahead bound | Bytes only; app's fence waits are the real pacing | Frame credits: ≤N presents ahead of GPU-completed serial (FLOWCONTROL §1) | **ours-stronger** (needed: our consumer owns GPU work) |
| Doorbell | Notify only when consumer-declared IDLE, ≥1 ms apart (`vn_ring.c:467-479`) | Futex kick only on empty→nonempty edge (mtl_wire.h) | match (equivalent edge-trigger) |
| Completion return | Host **GPU** writes into guest-mapped coherent buffer; guest polls (`vn_feedback.c`, `vn_query_pool.c:288-355`) | Status block `frame_serial` (completion-gated CPU publish) + query seqlock ring at heap+512 (QUERIES) | match (ours publishes from CPU listener — extra latency, same shape) |
| Replies | Reply shmem + wait-seqno + decode (`vn_ring.c:687-730`) | Cold calls keep direct unixcall paths; hot loop reply-free (mtl_wire.h) | match |
| Never serialize | Mapped ptrs/WSI/externals/function ptrs (spec:126-134) | No pointer-valued fields; obj_handle_t identity (mtl_wire.h) | match |
| Liveness | FATAL bit + ALIVE positive-ack watchdog + hard abort (`vn_common.c:267-288`) | Consumer self-heartbeat 100 Hz + pid checks + `g_serverDead` latch + 3 s progress bail, graceful fallback (V19, FLOWCONTROL) | ours-stronger on outcome; **gap on positive-ack shape** (V19 wedge class = self-reported liveness lying) |

## PORT MAP (one-to-one port targets; blob substrate CONFIRMED: MTLBuffer `newBufferWithBytesNoCopy` over a pre-faulted non-sparse MAP_SHARED file gives direct cross-process CPU-write→GPU-read visibility, zero copies)

### P1. Blob/mapped-memory model → kills the upload class
**Port**: `vn_renderer_shmem`/`vn_renderer_bo` (`vn_renderer.h:11-29` — `{res_id, mmap_size, mmap_ptr}` refcounted), `bo_ops.{create_from_device_memory, map, flush, invalidate}` (`vn_renderer.h:139-175`), `vn_device_memory_bo_init` + `vn_device_memory_needs_wait_alloc` (`vn_device_memory.c:59-90`), `vn_MapMemory2`/`vn_FlushMappedMemoryRanges` (`vn_device_memory.c:443-531`).
**Invariants**: (a) one blob per memory object, created lazily at first map, identified by the *memory object's wire id*; (b) map is idempotent/cached (`vn_renderer_virtgpu.c:449-454`), never freed until bo unref; (c) flush/invalidate are nops — coherency is the substrate's job; (d) blob create is ordered after the async alloc via an on-ring wait token (`vkWaitRingSeqnoMESA`, `vn_device_memory.c:85`).
**Maps onto ours**: heap-file regions become the backing store of the *server's* MTLBuffers: server calls `newBufferWithBytesNoCopy` on the pre-faulted MAP_SHARED region the PE client already writes through its D3D9 Lock pointer. Vulkan→Metal: `VkDeviceMemory`→heap-file region (offset,size); `res_id`→our obj_handle_t; blob map ioctl→both sides' existing mmap of the heap file (already mapped — the "injection" step is free).
**DELETED**: `upload_buffer` emission from `snapshotDirtyUniforms` + `noteBufferWrite` re-ship (V13/V16 client seams), server `handleUploadBuffer` direct/staged split, `deferredXfers` parking + `promoteParkedXfers` + `uplFlushUse` pass closes (V15, FLOWCONTROL §2), BufShadow lastShip dedup/merge (V15 FIX 2), heap-ack lazy re-upload for buffers. Serial discipline (*"no mutation of GPU-visible memory until completed_serial covers its last reader"*, S3 V16) stays — it is Venus's own "works correctly queued after ongoing works" (`vn_ring.h:20-24`) and D3D9 DISCARD renames give fresh regions per generation.

### P2. Ring/seqno/wait discipline
**Port**: `vn_ring_layout`/`vn_ring_get_layout` (`vn_ring.h:27-45`, `vn_ring.c:248-275`), `vn_ring_submit_internal` (`vn_ring.c:435-480` — wait-space per buffer, write, store-tail, FATAL check, idle-gated notify), `vn_ring_wait_space`/`has_space` (`vn_ring.c:209-246`), seqno API `vn_ring_get_seqno_status`/`wait_seqno`/`retire_submits` (`vn_ring.c:160-198`), indirect path `vn_ring_submission_get_cs` + `direct_size` (`vn_ring.c:482-520,319`), `vn_relax` machinery (`vn_common.c:181-293`).
**Invariants**: (a) seqno = free-running producer byte cursor; "consumed" ⇔ `head >= seqno` (wrap-safe compare `vn_ring_ge_seqno`); (b) retire-on-submit frees kept-alive side shmems once consumed (`vn_ring.c:160-173`); (c) notify is *state*-gated (IDLE bit) not edge-gated, rate-limited 1 ms; (d) every wait goes through one relax ladder with profiled yield/sleep/warn/abort orders.
**Maps onto ours**: mtl_wire.h ring already matches ≈1:1 (cursor names swapped). Port targets: (1) replace our fixed spin/park constants with `vn_relax_profile` ladders per wait reason; (2) adopt seqno-status API shape for "has the consumer passed byte X" (we already read tail as progress — formalize `ge_seqno` wrap compare); (3) keep our futex park (superior to their poll-sleep) but add their IDLE-bit semantics to the status word so the *consumer* declares poll-vs-park mode. Adaptation: virtio-gpu execbuffer notify→`WireRing_kick` futex wake; ring shmem blob→our existing ring file.
**DELETED**: ad-hoc stall-window heuristics that duplicate the ladder; per-site spin constants. Frame credits (FLOWCONTROL) are RETAINED — Venus has no equivalent and ours is load-bearing.

### P3. Feedback pattern
**Port**: `vn_feedback_pool`/`vn_feedback_slot` (`vn_feedback.h:11-50` — slot suballocation, free-list recycling), `vn_feedback_buffer_create` (`vn_feedback.c:32-100`), GPU-write injection `vn_event_feedback_cmd_record` shape (`vn_feedback.c:343-399`: barrier→FillBuffer(status)→barrier appended to the app's own submission), query feedback read `vn_query_feedback_wait_ready`/`vn_get_query_pool_feedback` (`vn_query_pool.c:288-355`), poll `vn_sync_feedback_query` (`vn_feedback.c:619-631`).
**Invariants**: (a) results are written by the *GPU in submission order* into the shared buffer — publication is inherently completion-ordered, no CPU listener; (b) results are 64-bit with an availability bit (`vn_query_pool.c:192`); (c) slots are recycled only after the consuming wait observes the value (pending→free lists, `vn_feedback.h:75-96`).
**Maps onto ours**: our query seqlock ring + status block already match the poll side (QUERIES: "the client poll is two atomic loads"). The 1:1 port is the *write* side: server encodes a GPU write (blit/`MTL4` fill or SharedEvent-driven copy) of `{counter, frame_serial}` into the heap-file slot inside the same submission, replacing the CPU `MTLSharedEvent` listener publication (FLOWCONTROL §1's "publication is completion-gated by the listener"). Adaptation: `vkCmdFillBuffer`→blit-encoder fill into the NoCopy heap buffer; slot pool→our fixed heap offsets.
**DELETED**: listener-thread publication latency from the credit path; the CPU-summed visibility-segment publish keeps its logic but writes through the same slot discipline.

### P4 (small, high value). Liveness port
**Port**: `vn_watchdog` + ALIVE-bit handshake (`vn_common.c:120-154,274-283`) and FATAL bit checks (`vn_ring.c:453-458`, `vn_common.c:267-272`). Guest-clears/host-re-sets replaces our self-reported heartbeat for *wedge* detection (V19 class); keep our graceful `g_serverDead` fallback instead of their `abort()`.

## TOP 3 IMPLIED CHANGES (ranked vs the 434 passes/frame upload-flush equilibrium + mapped-write saga)

1. **P1 — make the mapped pages BE the GPU buffer** (Venus: flush=nop, content never serialized). Every dynamic VB/IB/constant byte becomes GPU-visible the instant the app's Lock-pointer write lands; upload records, slab blits, and their pass closes cease to exist as a class. Feasibility: CONFIRMED by the NoCopy PoC on pre-faulted non-sparse MAP_SHARED; remaining work is allocation-granularity (page-align regions, pre-fault at create) and keeping the V16 serial discipline for reuse.
2. **P1½ — redirect instead of copy for residual staged ranges** (Venus commands reference `resourceId+offset` into shared pages, no copy — `vn_ring.c:496-499`): bind slab-slice GPU addresses directly for exactly-covered CBV/stream ranges (the S3 V16 "next lever"), deleting the last `stg/useFlush` producers. Feasible today — the slab is already a server MTLBuffer; immutability by serial discipline.
3. **P3+P4 — GPU-written feedback slots + positive-ack liveness**: completion publication ordered by the GPU itself (removes listener latency inside the credit wait), and the ALIVE-bit handshake that detects a wedged-but-heartbeating consumer — precisely the V19/2198 failure our self-heartbeat cannot see. Low-risk, additive status-word bits.

**Most load-bearing sentence found**: *"vn_renderer_bo_map will block until the renderer creates the resource and injects the pages into the guest"* (`vn_device_memory.c:459-463`) — together with its enforcement twin, *"nop because kernel makes every mapping coherent"* (`vn_renderer_virtgpu.c:427`): the entire mapped-memory problem is solved by page aliasing at map time, never by the command stream.
