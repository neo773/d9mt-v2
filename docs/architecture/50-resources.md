# 50 — Resources: lifecycle, residency, retirement

Four deadlock/corruption classes, one primitive: the **submission serial**.
Everything lifetime-shaped hangs off `SubmissionTracker`
(`backend/mtl_submission.h`) — upload-ring recycling, rename retirement,
EVENT queries, frame pacing. Every one of these was independently stubbed at
some point in v1/v2 and every stub deadlocked or corrupted.

## Creation seams

- App threads create resources through `MtlDevice`
  (`driver/mtl/mtl_device.h` → `backend/mtl_device_impl.*`): createBuffer /
  createImage / allocateStorage / createSampler / view creation. The CS
  thread only *uses* them (`MtlContextImpl` is CS-only).
- CPU-visible buffer memory obeys the **32-bit PE rule**: `VirtualAlloc` in
  the PE address space, wrapped unix-side by `newBufferWithBytesNoCopy`.
  The VirtualAlloc'd backing is owned by the allocation and freed on
  retirement (leaking it was commit c3c8363's VA-exhaustion class).
- Handles: raw `obj_handle_t` everywhere; `backend/wmt_handle.h` provides
  typed RAII wrappers (`WmtBuffer`, `WmtTexture`, `WmtResidencySet`, …) with
  retain/release semantics matching the bridge ownership contract
  (40-bridge.md).
- Frontend lifetime is `Rc<T>` (`util/rc/`). **`MtlAllocation` (and any Rc
  base deleted through a base pointer) MUST have a virtual destructor** — a
  missing one meant `~MtlAllocationImpl` never ran, zero residency removals
  ever, and the frame-2198 freeze (90-history.md).

## Storage, renames, invalidateBuffer (contract 3a)

- A buffer's live storage can be RENAMED on the app thread (MAP_DISCARD):
  the wrapper points at fresh backing immediately; recorded/bound slices are
  **physical snapshots** and keep reading the old backing.
- The CS-time commit point is `ctx->invalidateBuffer(buffer, oldSlice)`:
  retargets still-bound slices (CBV/vertex-stream dirty masks are set so the
  next flush rebinds) and retires the OLD backing via
  `tracker.retireAfter(currentSerial)` — nothing else (decision 6). The old
  allocation is released only after the last cmdbuf that referenced it
  completes; then its backing recycles (rename pool).
- Diag counters `s_diagVbStale`/`s_diagCbvStale` count draws whose bound
  slice diverged from live storage at flush — must stay 0.

## Residency — MtlResidencyTracker (backend/mtl_residency.{h,cpp})

Single owner of ALL `MTLResidencySet` state. The four invariants (from the
header; violations have each caused a production failure):

1. **ONE driver-owned, device-lifetime set, attached to the MTL4 queue at
   construction.** Queue-level attach covers every cmdbuf — nothing calls
   useResidencySet per cmdbuf. Contents = exactly the driver-created
   allocations alive or awaiting GPU retirement. Never drawables, never
   borrowed handles.
2. **add/removeAllocation are userspace; `commit()` is the expensive kernel
   sync.** Mutations batch under a dirty flag: commit at most once per
   submit (`beforeSubmit()`) + once per retirement sweep (`serialRetired()`),
   only when dirty. `requestResidency` once at creation — later commits
   auto-wire new adds.
3. **Deferred destruction.** MTL4 command buffers do NOT retain resources.
   `allocDestroyed(alloc, hostMem, lastUseSerial)` takes ownership of the +1
   handle ref and the VirtualAlloc'd backing, queues against a serial;
   `serialRetired(completed)` removes + commits, releases the Metal object,
   THEN frees host memory — strictly that order. `lastUseSerial` =
   `SubmissionTracker::nextSerial()` is always safe.
4. **Drawables NEVER enter the driver set.** Each CAMetalLayer owns a
   read-only `residencySet` auto-tracking its drawables;
   `attachLayer()/detachLayer()` attach THAT set at queue level (bridge codes
   205-207). Pinning drawables in a driver set starves `nextDrawable`
   forever (the 2198 freeze). `attachLayer` returns false while the layer's
   set isn't ready (async device apply) — callers retry.

Rule 5 (implementation): residency sets are not thread safe — one mutex
guards all mutation (creation on app threads, submit/retire on CS).
Diagnostics: `netResident()` should plateau (GTA ~77), `pendingRemovals()`
small.

## Submission serials + retirement (backend/mtl_submission.{h,cpp})

- `onSubmit(queue)`: called immediately AFTER `MTL4CommandQueue_commit` —
  MTL4 signals events on the queue timeline, not per-cmdbuf. Returns the
  serial the queue will signal on one shared `MTLSharedEvent`.
- `completedSerial()` reads the event; `retireAfter(serial, fn)` defers; 
  `processRetirements()` runs matured callbacks.
- **The pump thread**: retirement must not depend on future frontend
  activity. An app thread blocked on a fence that only a retirement callback
  signals (staging throttle during loads, no presents in flight) would
  deadlock a CS-driven-only design — so a dedicated pump blocks on the
  shared event for the lowest pending serial. Callbacks run on CS OR pump,
  exactly once, and must be thread-agnostic.
- `waitFor(serial)` CPU-blocks — teardown/Reset/Lock-sync only, never
  per-draw. `drainAndStopPump()` is the teardown gate and must run before
  the residency tracker / upload ring die.
- Frontend waits (`WaitForResource`) compare a resource's `trackingSerial`
  stamp against completed — stamps are written at submit for everything
  `track()`ed, with a per-draw fast path (`m_lastStampSerial`).

## Upload ring (backend/mtl_ring.{h,cpp})

`UploadRing` — per-frame upload/argument memory; THE primitive behind
argument-snapshot semantics ("every draw reads the last draw's constants"
black-screen class) and rename retirement:

- Chain of shared WMTBuffers carved by bump allocation; `allocate(size,
  align)` returns `RingSlice{gpuAddress, cpuPtr, buffer, offset}` — always a
  FRESH slice per draw-visible write.
- `onSubmit(serial)` stamps every chunk touched since last submit;
  `onComplete(completedSerial)` recycles chunks whose stamp passed.
  **Exhaustion GROWS the chain — allocation never blocks the CS thread and
  never reuses a live chunk** (v2's fixed 4 MB arena hung the GPU).
- Chunks register with the residency tracker at creation and hand
  buffer + host backing back at teardown (GPU idle then).
- Ring memory is read by the GPU later — contrast CmdChunk arenas, which are
  consumed synchronously at the encodeCommands crossing and recycle
  immediately (30-backend.md).

## Staging (frontend)

`D3D9DeviceEx::m_stagingBuffer` = one 4 MiB linear `MtlStagingBuffer`. When
`alloc` returns an undefined slice (full), `AllocStagingBuffer` retires the
whole backing and `reinit()`s fresh (outstanding slices keep the old backing
alive via Rc until GPU-done). `m_stagingAllocatedBase` folds retired bytes
into `GetStagingMemoryStatistics()` so `ThrottleAllocation`'s in-flight cap
and the staging fence stay correct. Failure to recycle = 1276 failed
FlushBuffers + 0xc0000005 (90-history.md).

## Texture specifics

- Metal requires `arrayLength >= 1` even for non-array 2D.
- GPU-only (MAP_MODE_NONE) textures are zero-initialized at creation
  (`d3d9_initializer.cpp`) — WDDM parity; uninitialized garbage fed GTA's
  tonemapper.
- `ensureTextureUsage(image, usage)` reallocates + blits when a texture
  needs a usage it wasn't created with (feedback-loop path).
- `copyTextureToBuffer/copyBufferToTexture` pitch comes from
  `computeBufferImagePitch` — pitch mismatches display as diagonal
  skew/tiling (see the Civ IV row-pitch investigation in `handoff/`).

## To change X, edit Y

- Anything freed "too early / never" → check the chain: Rc release →
  `allocDestroyed` → pending removal serial → `serialRetired`. Log
  `netResident()` first.
- New GPU-visible allocation type → register with the residency tracker at
  creation, retire via `allocDestroyed`, stamp `trackingSerial` at submit.
- Lock/upload behavior → `d3d9_device.cpp` (Lock/FlushBuffer/
  AllocStagingBuffer) + `d3d9_common_texture/buffer` mapping mode logic.
