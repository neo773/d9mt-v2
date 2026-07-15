# Metal 4 CPU-Side Performance — Research Findings

**Context:** d9mt v2 is a D3D9→Metal 4 translation layer running under CrossOver/Wine on
Apple Silicon. GTA IV is **CPU-bound** in daylight city scenes; goal is to lift ~60fps
toward v1's (Metal 3) 80–90fps. This document captures a deep multi-agent research pass
(100 agents, adversarial 3-vote verification, primary Apple sources) on Metal 4 CPU-savings
techniques, ranked by likely impact for a **draw-call-heavy, immediate-mode-translated**
workload.

Every finding below is verified 3-0 (unanimous) unless noted, and grounded in official
Apple documentation / WWDC sessions. **Caveat Apple ships no quantified CPU-savings numbers**
— the ranking is engineering inference, not measured deltas.

---

## Status legend
- ✅ **DONE** — already implemented in d9mt v2 (this research validates the approach)
- 🔜 **NEXT** — not yet done; concrete next win
- 🧩 **BIG** — larger restructure; high ceiling, more risk/effort

---

## Executive summary

The highest-leverage Metal 4 CPU savings come from its redesigned **binding** and
**submission** model:
1. Persistent, reusable `MTL4ArgumentTable`; update only the changed slot per draw; bind
   buffers by raw 64-bit GPU address (`setAddress`) for cheap upload-ring suballocation.
2. Batch queue submission — encode on multiple worker threads (each its own
   `MTL4CommandAllocator`), then one grouped `MTL4CommandQueue commit:count:`.
3. On the TBDR GPU, merge render encoders via **Suspend/Resume** + **color-attachment
   mapping** to eliminate intermediate tile store/load and cut encoder/pass count.
4. Remove PSO-compile hitches with `MTL4Compiler` **async** creation + **binary archives**.
5. Attach stable `MTLResidencySet`s to the **queue once**; prefer fewer/larger sets.

Notably, ICB / GPU-driven encoding did **not** surface as a top lever for this workload —
the argument-table + commit-batch + encoder-merge path dominates. We do **not** need the
large ICB rewrite yet.

---

## Findings (ranked)

### 0. Persistent, reusable argument table; update only the changed slot ✅ DONE
Metal 4 encoders have **no** `setBuffer`/`setTexture`/`useResource`/`useHeap`. Resources
live in an `MTL4ArgumentTable` (buffers, textures, samplers) bound to stages via
`setArgumentTable(atStages:)`. One table can be created up front at max size, shared across
encoders and frames, reused indefinitely, and kept off the encoding critical path. Per-class
setters (`setAddress`/`setTexture`/`setSamplerState`) persist, so **only the changed slot is
updated per draw**. A single argument buffer can address thousands of resources (bindless).

> Apple: "encoders don't have methods that bind individual buffers, textures, and heaps…
> configure resource bindings in an argument table and then bind that table"; "reuse that
> argument table indefinitely… memory and runtime savings add up with each common resource";
> "Creating argument tables before encoding lets you move resource binding off of your
> critical path."

**d9mt status:** This is exactly the **dirty-tracking** commit (`8744a88`) — per-physical-table
shadow, unchanged draws append zero binding structs, dirty draws diff-write only changed slots.
Research confirms this is the single highest-value technique for our workload. **Validated.**

Sources:
- https://developer.apple.com/documentation/Metal/understanding-the-metal-4-core-api
- https://developer.apple.com/documentation/metal/mtl4argumenttable
- https://developer.apple.com/videos/play/wwdc2025/254/

---

### 1. Bind by raw GPU address (`setAddress`) for upload-ring suballocation ✅ DONE
Bind suballocated/upload-ring buffers by 64-bit GPU address via
`MTL4ArgumentTable.setAddress(_:index:)` (`buffer.gpuAddress + offset`) instead of by
`MTLBuffer` object — avoids per-draw buffer-object references. `setAddress(_:attributeStride:
atIndex:)` supports dynamic vertex strides (needs `descriptor.supportAttributeStrides`).

**d9mt status:** We bind via `MTL4ArgumentTable_setAddress` with record-time gpuAddress
snapshots (the physical-slice `MtlBufferSlice` contract) and use `support_attribute_strides`.
**Validated.** *(Minor: the object-based `setResource` path coexists; we already avoid it on
the hot path.)*

Sources:
- https://developer.apple.com/documentation/metal/mtl4argumenttable

---

### 2. Batch queue submission + multi-threaded encoding 🔜/🧩 NEXT
Encode command buffers across multiple CPU worker threads (each with its own **non-thread-safe**
`MTL4CommandAllocator`, which reuses command-buffer memory and avoids dynamic allocations while
encoding), then submit them all with a single `MTL4CommandQueue commit:count:`. The queue
"reduces CPU runtime and memory overhead by sending work to the GPU when you commit" and allows
submitting from any thread.

**d9mt status:**
- **commit:count: batching** — we evaluated and **deliberately skipped** it: `ExecuteFlush`
  signals per-cmdbuf fences that app threads block on (flush throttle, staging budget, query
  polls), so deferring commits regresses mid-frame latency more than the few saved queue
  crossings. That reasoning still holds for naive batching. Revisit only if the fence model is
  restructured so a batched commit doesn't delay fence delivery.
- **Multi-threaded encoding** — 🧩 NOT done. Our encoding is single-CS-thread. This is a bigger
  restructure (per-thread allocators, work partition, ordering) but a real ceiling-raiser for
  draw-heavy scenes. High effort.

Sources:
- https://developer.apple.com/documentation/Metal/understanding-the-metal-4-core-api
- https://developer.apple.com/documentation/metal/mtl4commandqueue/commit:count:
- https://developer.apple.com/videos/play/wwdc2025/254/

---

### 3. Merge render encoders — Suspend/Resume + color-attachment mapping 🔜 NEXT (top GPU-side win)
Two mechanisms to cut render-pass/encoder count and TBDR tile store/load:
- **(a) Suspend/Resume:** suspend an encoder in one command buffer, resume in another;
  submitting them in one commit tells Metal to **merge the passes**, avoiding the store/load of
  intermediate tile results each separate pass incurs. *(The genuinely new Metal 4 capability is
  suspend/resume spanning separate command buffers; consecutive same-descriptor merging predates
  Metal 4.)*
- **(b) Color-attachment mapping:** `MTL4RenderPassDescriptor.supportColorAttachmentMapping`
  with `MTLLogicalToPhysicalColorAttachmentMap` remaps logical shader-output indices to physical
  attachments, so switching pipelines with different color outputs **doesn't require a new
  encoder** — fewer encoders, fewer passes, less CPU encoding overhead.

**d9mt status:** 🔜 NOT done. This directly targets the **~15 single-draw post-process render
encoders per frame** we observed in GTA traces — each currently pays a full tile store+reload.
GPU-side win that complements our CPU-side batching; becomes the dominant lever once we stop
being purely CPU-bound. **Strong next candidate.**

> Apple: "Metal 4 provides Suspend/Resume option to merge multiple render encoders… Submitting
> the render encoders in one commit instructs Metal to merge the render passes." "With color
> attachment mapping, you don't need a second encoder… reduce encoding overhead and improve GPU
> efficiency by reducing the amount of render passes."

Sources:
- https://developer.apple.com/videos/play/wwdc2025/254/
- game-porting-skills:creating-metal4-shader-pipelines

---

### 4. Async PSO compilation (`MTL4Compiler`) 🔜 NEXT (kills mid-frame hitches)
`MTL4Compiler` gives fine-grained control over **when** each shader compiles + per-task
progress. Use async creation — `makeRenderPipelineState`/`makeComputePipelineState` with
`compilerTaskOptions` returning an `MTL4CompilerTask` — which returns immediately and runs on a
background dispatch queue at a chosen QoS, avoiding long CPU stalls. `MTL4Compiler` is `Sendable`
(thread-safe, shareable across threads for concurrent compilation).

**d9mt status:** 🔜 We have a synchronous PSO cache. When a game hits a new shader/state combo
mid-frame, we compile on the hot path → the dips you see when new content appears. Async
compilation (with a placeholder/skip for the first-encounter frame) removes those stalls.
Critical for an immediate-mode translator that discovers PSOs mid-frame. **Strong next candidate.**

Sources:
- https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api
- https://developer.apple.com/documentation/metal/mtl4compiler
- https://developer.apple.com/videos/play/wwdc2025/205/

---

### 5. Binary archives — persist compiled pipelines across runs 🔜 NEXT
`MTL4Compiler.pipelineDataSetSerializer` stores data for every pipeline it creates
(`MTL4Archive` / `MTL4PipelineDataSetSerializer`); harvested sets serialize to a binary archive
(or a pipeline script precompiled offline via `metal-tt` / `serializeAsPipelinesScriptWithError`).
Offline compilation moves GPU binary generation to **build time** — no runtime codegen, PSO
creation becomes a lightweight load. The Metal 3 `MTLBinaryArchive` path still works (create with
nil-URL descriptor, `addRenderPipelineFunctions`, `serialize(to:)`, attach via
`descriptor.binaryArchives`); both Metal 3 and 4 can load binary archives.
`failOnBinaryArchiveMiss` controls whether a miss recompiles or fails. **Apple: "you always want
to use binaryArchives in Metal."**

**d9mt status:** 🔜 We already persist a PSO cache (`d9mt_pso_cache.bin`) but not as a Metal
binary archive — so a cache "hit" still re-runs Metal's codegen. Moving to real binary archives
makes cross-run PSO load near-free (kills first-launch and first-encounter stutter). Caveat:
skip-compile only for exact-match device+OS-build; a miss recompiles per `failOnBinaryArchiveMiss`.
*(Finding merged 9 sub-claims; one sub-claim was 2-1, rest 3-0.)*

Sources:
- https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api
- https://developer.apple.com/documentation/metal/mtl4compiler
- https://developer.apple.com/documentation/metal/creating-binary-archives-from-device-built-pipeline-state-objects
- https://developer.apple.com/videos/play/wwdc2021/10229/
- https://developer.apple.com/videos/play/wwdc2022/10102/

---

### 6. Residency — queue-attach once, fewer/larger sets ✅ DONE
Attach stable residency sets to the **command queue once** (not per command buffer) for
frequently-needed/lifetime resources; attach to command buffers only sets that change often.
Prefer **fewer sets with more resources each** so Metal processes residency in bulk. Queue
attachment is more efficient than attaching the same set to multiple command buffers; per-resource
residency overhead grows with resource count.

**d9mt status:** Our `MtlResidencyTracker` uses exactly this — ONE device-lifetime,
queue-attached set, `requestResidency` once, batched dirty commits, no per-cmdbuf `useResidencySet`.
**Validated.**

Sources:
- https://developer.apple.com/videos/play/wwdc2025/254/
- https://developer.apple.com/documentation/metal/simplifying-gpu-resource-management-with-residency-sets

---

## Prioritized action plan for d9mt

Already done (validated by research): argument-table dirty-tracking (0), GPU-address binding (1),
queue-attached single residency set (6).

Next wins, in order:
1. **Async PSO compilation (Finding 4)** — removes mid-frame compile stalls (the fps dips on new
   content). Moderate effort, CPU-side, directly attacks the observed dips. **Do first.**
2. **Binary archives (Finding 5)** — near-free cross-run PSO load; kills first-launch/first-encounter
   stutter. Pairs with #1. Moderate effort.
3. **Encoder merging: Suspend/Resume + color-attachment mapping (Finding 3)** — collapse the ~15
   single-draw post passes, eliminate tile store/load. GPU-side; dominant once CPU-bound is relieved.
   Moderate-high effort.
4. **Multi-threaded encoding (Finding 2)** — biggest ceiling-raiser for draw-heavy scenes, biggest
   restructure. Defer until 1–3 land and we re-measure. **Big.**
5. **commit:count: batching (Finding 2)** — still gated by the ExecuteFlush per-cmdbuf fence model;
   revisit only if that's restructured.

ICB / GPU-driven encoding: **not needed yet** — not a top lever for this workload per the research.

---

*Research method: 100-agent deep-research workflow (5 search angles → dedup fetch of top sources →
3-vote adversarial verification, 2/3 refutes to kill → confidence-ranked synthesis). All 7 findings
survived at high confidence. Run ID wf_c9f74cb7-271.*
