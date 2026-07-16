# 30 — Backend: MtlContextImpl anatomy

`driver/backend/mtl_context_impl.h` opens with the 10 load-bearing
architecture decisions — read that header first; this doc is the operational
map. Implementation is split: `mtl_context_record.cpp` (passes, clears, draws,
submit, frame-deferred machinery) and `mtl_context_transfer.cpp` (copies,
blits, resolve, PSO resolve, resource flush, MetalFX). **Single-threaded: CS
thread only.**

## Dirty-bit flow (decision 2)

Every seam setter folds state into members and sets bits from
`ContextDirtyBit`: `DirtyPso | DirtyVertexBuffers | DirtyIndexBuffer |
DirtyResources | DirtyPush | DirtyViewport | DirtyDepthStencil |
DirtyBlendColor | DirtyStencilRef | DirtyDepthBias | DirtyRasterizer`.

`draw()/drawIndexed()` → `prepareDraw()`:
`ensurePass (materializePass)` → `resolvePso()` (DirtyPso) →
`flushVertexBuffers()` → `flushResources()` (DirtyResources/DirtyPush) →
`flushDynamicState()` → append the draw into the CmdChunk. Draws with no
binding-shaped dirty state skip table writes entirely (the prepareDraw gate).

Finer-grained per-slot masks under the coarse bits:
`StageBindings::dirtyTextures/dirtySamplers/dirtyCbvs` (bit i = slot i) and
`m_dirtyVertexStreams` — the frontend touches ~1-2 slots/draw; walking all
slots per draw was the dominant CPU cost (~97k appendTable calls/frame).
**Mask iteration is correct ONLY while the same physical argument-table pair
stays bound**; a rotation forces all masks to `~0`, and a PSO change forces
the CBV walk wide (cbvSlotMap remaps slots per shader).

## Pass lifecycle (decision 1)

- `bindRenderTargets()` records only a `PendingPass` (`m_pass.pending`) —
  and FLUSHES any folded clears of the outgoing pass first (the clear-drop
  fix). No encoder yet.
- `clearRenderTarget()` folds into the pending pass's load actions when the
  attachment matches (clear-then-draw = free on TBDR; attachment identity is
  by backing texture + mip/layer, never Rc pointer), else runs a mini clear
  pass (`flushPendingClears`).
- First draw → `materializePass()`: opens the `MTL4RenderCommandEncoder`,
  applies folded clears as loadActions, resets per-encoder trim state
  (`m_dynValid`, `m_lastEncodedPso`, `m_argTablesBound`).
- `closePass()`: flush the CmdChunk, `emitProducerBarrier` (see below),
  endEncoding. Reasons are profiled: `passClose/f: submit|rt|clear|xfer|
  barrier|blit` — on TBDR every close = full tile store + reopen = full tile
  load, so pass count is the main GPU lever (GTA: 66-80 passes/frame).
- Offscreen passes split into their own command buffer when encoded work
  crosses the watchdog budget — split BEFORE the drawable is acquired, never
  after (v2 GPU-hang fix).

**Barriers:** Metal 4 has no driver hazard tracking. `emitProducerBarrier`
(coarse queue barrier: producer's afterStages ordered before ALL later queue
stages) runs on every outgoing encoder before endEncoding — compute-copy
uploads before draws, RT writes before sampling, before present/readback
copies. Coarse-but-correct; narrowing the stage masks is a known TODO.

## CmdChunk batching + replay contract

`mtl_cmd_chunk.h`: encoder commands are NOT thunk calls — they are `wmtcmd_*`
structs chained through `next` pointers in a 64 KiB-block arena, replayed
unix-side in ONE `MTL4RenderCommandEncoder_encodeCommands` crossing per pass
(or per `kCmdChunkFlushBytes = 4 MiB` arena cap; a crossing costs ~14-17 µs so
fewer/larger wins). Flushing at any command boundary is order-preserving.

Contract:
- Unix consumes synchronously during encodeCommands → chunk memory recycles
  immediately after the flush (unlike UploadRing memory, which the GPU reads
  later).
- `m_cmdChunk` non-empty ONLY while a pass is open; flushed at closePass, at
  the arena cap, and before any DIRECT call on the pass encoder
  (`emitGraphicsBarrier`) so stream and direct ops stay ordered.
- **CmdChunk is move-only; copy is a deleted compile error.** The implicit
  copy once deep-copied the arena while head/tail/next still pointed into the
  source's blocks — the deferred-pass vector growth silently copied entries
  (std::function move not noexcept under C++17 → move_if_noexcept chose copy)
  and replay walked freed heap (GTA boot crash). `FrameEntry`'s moves are
  explicitly noexcept for the same reason.

## Argument tables: pairs + shadows + rotation (decision 3)

- D3D9's separate VS/PS constant namespaces map to an `ArgTablePair`
  ([vertex, fragment] `MTL4ArgumentTable`s), bound via
  `setArgumentTable:atStages:`. Metal snapshots table contents at each
  ENCODED draw, so one pair is safely mutated between draws within an
  encoder.
- The pair **rotates once per materialized pass** (`m_argTables` ring,
  retire-by-serial: a pair is reused only when its `serial` has completed) —
  NOT per draw. Rotation exists only to prevent rewriting a pair an earlier
  *submission* may still be encoding against. History: a single shared table
  mutated per draw under overlapping execution made every in-flight draw read
  the LAST draw's bindings (the FF strobe; 90-history.md).
- `ArgTableShadow` mirrors each table's persistent contents (31 buffer
  addresses, 16 texture ids, 16 sampler ids). `appendTable{Address,Texture,
  Sampler}` diff against the shadow and append only on change. Null is a
  value: unbind diffs to a 0-write; correctness is value-based (a skipped
  write leaves the identical gpuAddress/ResourceID). The shadow is exact
  because tables are written exclusively through these helpers and every
  append is replayed before any later draw.
- Per-draw uniform/push data goes into FRESH `UploadRing` slices at draw time;
  the table gets the slice gpuAddress. Ring memory is never rewritten under a
  queued draw (snapshot semantics).
- AB model: `flushResources` patches only dirty (kind,reg) words in the
  persistent `abShadowWords`, memcpys the whole shadow into a fresh ring
  slice, appends one setAddress at `kAbIndex=21`. `abFnId` detects word-layout
  (shader) changes → full recompute. `abActive` forces a full reconcile on
  per-slot↔AB model switches (FF ↔ translated shaders).
- The meta-blit path has its OWN `BlitArgTable` ring — a blit must never
  advance the per-draw pool under in-flight draws.

Slot constants (keep in lockstep with AirBuilder — see 20-shaders.md):
`kVertexBufferBaseIndex=3`, `kPushConstantIndex=20`, `kAbIndex=21`,
`kPushSharedBlockSize=32`.

## PSO cache + snapshot MRU (decision 9)

`mtl_pso_cache.h`: `RenderPsoKey` = packed POD (vs/fs cookies, 20 attrs +
layouts incl. **strides**, 4 color attachments + blend, sampleCount,
topologyClass, alphaToCoverage; **NO depth/stencil formats — Metal 4 removed
them from the descriptor**), FNV-hashed, one-entry memo before the map. Build:
`MTL4Compiler_newRenderPipelineState` with (library, entryName) per stage —
synchronous on miss (CntPsoMiss; ≈0 in GTA steady state).

In front of the key build sits the **PsoSnapshot MRU**
(mtl_context_impl.h): a ~64 B identity of the key INPUTS (shader pointers,
attachment formats BY VALUE — never view pointers, recycled addresses
false-hit — plus generation counters `m_layoutGen/m_strideGen/m_blendGen`,
msaa, topology). 4-entry memcmp MRU short-circuits the 600-byte key build +
hash + probe for A↔B material alternation: resolvePso 3.0 → 0.34 ms/frame.
Every setter that mutates key inputs must bump its generation counter.
`D9MT_NO_PSOMRU=1` disables.

## Deferred-pass architecture (decision 10) — OPT-IN `D9MT_DEFERRED=1`

Ported from mtld3d's passes.rs. When ON: `closePass()` parks
`FrameEntry{targets, folded clears, CmdChunks}`; transfers/blits/mini-clears
park as Direct entries (captured encode closures) so replay order == record
order. `flushFramePasses()` runs inside `submit()` (before endCommandBuffer —
every flush/readback/query/Reset/present funnels through submit, so nothing
observes un-replayed work) and applies whole-frame load/store optimization:

- Rule A (diverges from mtld3d): first use w/o clear keeps load=Load, never
  DontCare — D3D9 backbuffers persist across frames (GTA relies on it).
- Rule B (`D9MT_NO_RULE_B`): depth/stencil store=DontCare on the last pass,
  only at a present-boundary flush (`m_frameBoundaryFlush` armed by endFrame)
  and only for structurally un-sampleable, unread textures.
- Rule C (`D9MT_NO_RULE_C`): color store=DontCare when the next pass fully
  clears; blocked by `m_frameSampled` (this batch) and `m_everTransferRead`
  (persistent — the present blit reads the backbuffer every frame, so the
  backbuffer is permanently exempt).
- Rule E: clear-only passes fold into the next pass's load actions.
- Rule F: draw-less, store-less passes are culled.
- Merge (`D9MT_NO_PASSMERGE`): adjacent Render entries with identical
  attachments and no clears on the second replay into one encoder (the safe
  replacement for the reverted `D9MT_RTSKIP` hack — merge happens at flush
  time where a Reset can never interleave; entries hold Rc keepalives).

Serial correctness: replay happens in the SAME command buffer/serial the
entries were recorded under, so tracking stamps, ring slices, table rotation
and query serials all stay valid.

**Why default OFF:** batching the whole frame into one flush serializes CPU
and GPU — frame time = CPU + GPU instead of max(CPU, GPU); measured hard fps
regression in GTA, and Rules B/C never fire there (GTA samples everything it
stores). Re-default once submission overlap exists (render-server consumer
submitting pass batches while later ones record).
**Stale comment warning:** the decision-10 text in mtl_context_impl.h still
says "D9MT_NO_DEFERRED=1 reverts" — the actual gate is `deferredPassesEnabled()`
reading `D9MT_DEFERRED` (opt-in) since commit 3512ff5.

## Special paths

- **Copies** (decision 8): all via `MTL4ComputeCommandEncoder` copy* — Metal 4
  has no blit encoder. `copyBuffer/copyTexture/copyBufferToTexture/
  copyTextureToBuffer` in mtl_context_transfer.cpp.
- **blitTextureView / generateMipmaps(filtered) / clearImageView:** meta-blit
  render pipeline (build-time metallib `mtl_blit_metallib.h`; `m_blitVs/
  m_blitFs`, `m_fillFs` solid-color, PSOs ride m_psoCache keyed by blit
  cookies + dst format; samplers [0]=Nearest [1]=Linear).
- **Scissored depth/stencil clear:** `clearDepthStencilRect` uses
  `m_depthClearVs` (depth-writing VS variant) — Metal load-action clears are
  full-attachment only.
- **resolveTexture** (§6.5): draw-less pass with
  Store/LoadActionMultisampleResolve (TBDR-native hardware resolve); partial
  region = resolve-full-then-copy.
- **Triangle-fan emulation:** Metal has no fan primitive. One shared 192 KiB
  index buffer holds the pattern (0,1,2, 0,2,3, …); a fan of N verts consumes
  its first 3*(N-2) indices (prefix property) with base_vertex = firstVertex
  (`m_fanIndices`, `kFanMaxVerts=16386`). Indexed fans warn-once, unhandled.
- **presentUpscale** (`D9MT_METALFX=1`): MTL4FX spatial scalers memoized per
  (in-dims, out-dims, formats) in `m_fxScalers`; falls back to sampled blit.
- **Queries** (decision 7): occlusion = per-pass visibility-result buffer
  slots (`m_visBuffer`); EVENT/timestamp = submission-serial compare via
  `SubmissionTracker`. No bespoke fences, ever.

## Submission spine (decision 5) — see also 50-resources.md

`beginRecording()`: rotate the 3-deep `MTL4CommandAllocator` ring, reset the
caught-up slot (waitFor its serial — a stall here = GPU-bound/pacing).
`submit()`: flushFramePasses (deferred) → endCommandBuffer → residency
`beforeSubmit()` → `MTL4CommandQueue_commit` → `SubmissionTracker::onSubmit`
(queue signalEvent(serial)) → `UploadRing::onSubmit` → keep-alive →
`retireAfter(serial)`. `endFrame()`: processRetirements + residency
`serialRetired` + ring `onComplete` + profiler frame tick. `synchronizeWsi`:
the 4-step present handshake (60-present.md).

## Reading a PROF line (D9MT_PROFILE, every 256 frames)

```
PROF frame=F win=256 ms(avg/f): beginRecording=T(t) prepareDraw=… resolvePso=…
 flushVB=… flushRes=… flushDyn=… encodeDraw=… flushChunk=… submit=… syncWsi=…
 endFrame=… | draws=N(n/f) chunkFlush=N(n/f) psoMiss=N argRotate=N(n/f)
 abRebuild=N(n/f) psoKeyBuild=N(n/f) passClose/f: submit=… rt=… clear=… xfer=…
 barrier=… blit=… | framePasses=…/f storeDontCare=…/f | wire: rec=… bytes=…
 stalls=… decodeErr=… hb=… srvErr=…
```

- Each bucket: window-total ms `(per-frame avg)`. **Buckets NEST** —
  prepareDraw contains resolvePso/flushVB/flushRes/flushDyn (+flushChunk at
  the arena cap); syncWsi contains the present submit; read sub-buckets as
  "of which", never additive across the frame.
- `beginRecording` high = GPU-bound or pacing (allocator-slot wait).
- `psoMiss` = synchronous hot-path compiles; `argRotate` = binding-dirty
  draws; `abRebuild` = AB word-slice full rebuilds; `psoKeyBuild` = DirtyPso
  resolves that missed the MRU.
- `passClose/f` reasons rank the TBDR pass-count levers.
- wire suffix: `hb` = server heartbeat delta per window (0 with a server
  attached = server DEAD), `srvErr` = status-block error code, `stalls` =
  producer ring-full episodes.
- Lines are wide — parse with python, not grep (truncation).
- Counters accumulate on the CS thread only; granularity is function-level by
  design (clock reads under Rosetta aren't free — no Scopes in per-slot
  loops).

Diagnostics that must stay 0: `s_diagVbStale`/`s_diagCbvStale` (recorded
slice diverged from live storage at flush — the recorded-vs-live retargeting
bug class).

## To change X, edit Y

- New encoder command → `wmtcmd_mtl4_*` struct in bridge/winemetal.h, replay
  case in `_MTL4RenderCommandEncoder_encodeCommands` (winemetal_unix.c),
  appender in mtl_context_record.cpp. Rebuild bridge + install.
- Pass split/store behavior → closePass/recordPassEntry/flushFramePasses in
  mtl_context_record.cpp.
- Binding flow → flushResources/flushVertexBuffers (mtl_context_transfer.cpp)
  + the slot constants + AirBuilder (20-shaders.md) in lockstep.
- New profiler bucket/counter → mtl_profiler.h enums + kNames + the PROF
  format string (all three).
