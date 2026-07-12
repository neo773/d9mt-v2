# kk-audit.md — kosmickrisp (Vulkan→Metal 4) vs d9mt v2 (D3D9→Metal 4)

Study of Mesa's `src/kosmickrisp` — a clean, upstream Metal 4 Vulkan driver — to
find what our D3D9→Metal 4 driver does wrong. kk is doubly relevant: it targets
**Metal 4** (like us) and is **Vulkan-shaped** (our backend is DXVK-derived, i.e.
Vulkan-shaped too). Reference tree: `d9mt/v2/mesa/src/kosmickrisp/`
(`bridge/*.m` = our winemetal bridge; `vulkan/kk_*.c` = our driver/backend).

Three parallel Opus audits: (1) queue submission + completion, (2) WSI/present,
(3) sync + residency + command buffers.

---

# Section 1 — Command Submission & GPU Completion

Scope: how each driver submits an MTL4 command buffer, learns it completed, maps
fences/timelines onto Metal 4 events, and does CPU-side waits.

## TL;DR — three defects, most impactful first

1. **Per-lock full-drain crawl (FIX FIRST).** `WaitForResource` drains the
   *entire* GPU on every Lock because `MtlResource::isInUse()` is hardcoded
   `return false`. The per-resource serial infrastructure already exists and is
   stamped — just unused. Wire it up and scope the wait to the resource's own
   serial.
2. **No completion-error path → cmdbufs that "never signal".** We track
   completion *only* via the queue's `signalEvent` timeline. A faulted/timed-out
   cmdbuf never signals, so `completedSerial()` never advances and `waitFor`
   burns its full ~10s budget then abandons. kk additionally registers an
   **MTL4 commit feedback handler** that fires even on GPU error and marks the
   device lost — we have no equivalent and our WMT bridge doesn't even expose
   commit options.
3. **Minor ordering nit** around residency/commit — we are actually close to kk
   here; noted for completeness.

## 1. Completion mechanism — exact Metal API each uses

**kk** — two independent channels:

- *Completion / fence value*: the queue timeline. `kk_queue_submit` calls
  `mtl_signal_event(queue->mtl_handle, sync->mtl_handle, signal->signal_value)`
  (`kk_queue.c:104`) → `[q signalEvent:e value:value]` on the
  `id<MTL4CommandQueue>` (`mtl_command_queue.m:100-108`). The fence is an
  `MTLSharedEvent` (`kk_sync.c:21`, `mtl_sync.m:32-39`). CPU reads it with
  `event.signaledValue` (`mtl_sync.m:59-66`). **This channel only fires if the
  cmdbuf actually completes** — same failure mode we have.
- *Error / liveness*: a **commit feedback handler**. `kk_queue_submit` calls
  `mtl_commit_options_add_feedback_handler(queue->commit_options,
  commit_callback, dev)` immediately before `mtl_command_queue_commit(...)`
  (`kk_queue.c:86-89`). The bridge does `[opt addFeedbackHandler:^(
  id<MTL4CommitFeedback> feedback){…}]` (`mtl_command_queue.m:42-66`),
  extracting `feedback.error` (and GPU start/end times). On error
  `commit_callback` calls `vk_device_set_lost(...)` (`kk_queue.c:20-29`). **This
  block runs even when the cmdbuf faults/times out**
  (`MTL4CommandQueueErrorTimeout`, `…DeviceRemoved`, mapped at
  `mtl_command_queue.m:21-40`).

**Ours** — one channel only. `SubmissionTracker::onSubmit` →
`MTL4CommandQueue_signalEvent(queue, m_event, serial)` after commit
(`mtl_submission.cpp:14-18`). `completedSerial()` reads
`MTLSharedEvent_signaledValue` (`mtl_submission.cpp:20-28`). `waitFor` blocks on
`MTLSharedEvent_waitUntilSignaledValue` in 5×2s slices then gives up
(`mtl_submission.cpp:70-90`). Our commit is the bare
`MTL4CommandQueue_commit(queue, &cb, 1u)` with **no options argument**
(`mtl_context_impl.cpp:199`) — the WMT bridge exposes no
commit-options/feedback-handler entry point at all.

**Consequence.** If a cmdbuf hangs or faults, our shared-event value is frozen
forever. `completedSerial()` is stuck, so *every* subsequent `waitFor`/
`isComplete` for that serial or later stalls, and `retireAfter` callbacks
(cmdbuf release, upload-ring recycle, MAP_DISCARD backing release) never run →
cascading leak + the "command buffers sometimes never signal completion"
symptom. kk converts the same GPU failure into a prompt `VK_ERROR_DEVICE_LOST`
that unblocks waiters.

**Fix (defect 2).**
- Add a WMT bridge entry point for `MTL4CommitOptions` + `addFeedbackHandler`
  (mirror `mtl_command_queue.m:42-66`), and pass options into
  `MTL4CommandQueue_commit`.
- In the feedback handler, on `feedback.error != nil`: (a) log/flag
  device-lost, and (b) **force-advance the completion cache to this serial** so
  `completedSerial()` moves past a faulted cmdbuf and all downstream waiters/
  retirements unblock instead of hanging.

## 2. Waiting for ONE submission without draining the whole GPU

**kk** waits on a *specific* timeline value. `kk_timeline_wait`
(`kk_sync.c:55-83`) calls `mtl_shared_event_wait_until_signaled_value(
timeline->mtl_handle, wait_value, timeout_ms)` — `wait_value` is the fence value
of the one submission the app cares about (`mtl_sync.m:41-48`). `vkWaitForFences`
on one fence blocks only until that fence's value; unrelated later work is
irrelevant. `vkQueueWaitIdle` is the *only* drain-everything path.

**Ours** turns *every* resource lock into `vkQueueWaitIdle`. `WaitForResource`
(`d3d9_device.cpp:4810-4835`) unconditionally does `SynchronizeCsThread(All)` +
`ExecuteFlush` + `m_dxvkDevice->waitForIdle()`, and `waitForIdle()` waits for
`m_tracker->lastSubmittedSerial()` — **the newest serial**, i.e. all in-flight
work (`mtl_device_impl.cpp:410-417`). The tracker already has the right
primitive: `waitFor(serial)` / `isComplete(serial)` scope to a single serial
(`mtl_submission.cpp:70`, `mtl_submission.h:51-53`). We just don't call it with
the resource's serial.

**Consequence.** Locking a tiny dynamic buffer or a query readback blocks the
CPU until the *most recent* frame's cmdbuf finishes — serializing CPU and GPU
every Lock. That is the "every readback does a full GPU drain that crawls the
app" symptom (confirmed: visual-suite crawl, `value=9 cur=7 signaled=1`).

**Fix (part of defect 1).** In `WaitForResource`, after the CS sync, wait for the
*resource's* serial, not the queue's newest:
```
uint64_t s = Resource.trackingSerial();
if (s == 0 || submissions().isComplete(s)) return true;   // idle already
if (MapFlags & D3DLOCK_DONOTWAIT) return false;           // WASSTILLDRAWING
submissions().waitFor(s);
```
Only `ExecuteFlush` if `s >= nextSerial()` (last use not yet committed).

## 3. Per-resource last-use serial ("is this buffer in use?")

**kk** does **not** track per-resource use serials — it's a bare Vulkan driver,
so "is this resource idle" is the *application's* responsibility via fences. kk
has nothing to port here beyond the fence-scoping in §2.

**Ours** *is* the D3D9 runtime, so per-resource tracking is our job, and the
scaffolding exists but is stubbed:
- `MtlResource::trackingSerial()` / `setTrackingSerial()` store last-use serial
  (`mtl_resource.h:157-158`).
- `MtlResource::isInUse()` is hardcoded `return false` (`mtl_resource.h:164`) —
  comment even says it should "compare trackingSerial against the device
  completed serial."
- `setTrackingSerial(serial)` is called **only** for `m_keepAlive` resources at
  submit (`mtl_context_impl.cpp:216`), not every referenced resource.

**Fix (completes defect 1).**
1. Stamp `setTrackingSerial(currentSerial)` on **every** resource bound/written/
   read into the open cmdbuf (at bind/copy record time), not just keep-alive.
   `m_currentSerial` is already captured in `ensureCommandBuffer`
   (`mtl_context_impl.cpp:124`).
2. Implement `isInUse()` as `m_trackingSerial > device.completedSerial()`.
3. Then `WaitForResource` short-circuits idle resources with zero GPU sync, and
   `D3DLOCK_DONOTWAIT` correctly returns `D3DERR_WASSTILLDRAWING`.

## 4. Ordering around commit / signal / residency

**kk** per-submit order: wait external events → `kk_device_make_resources_
resident(dev)` (`kk_queue.c:67`) → per cmdbuf: add feedback handler + `commit`
(`:86-89`) → present drawable if any (`:92-97`) → `signalEvent` (`:104`).

**Ours** (`mtl_context_impl.cpp:191-207`): `endCommandBuffer` →
`MTLResidencySet_commit` + `requestResidency` → `MTL4CommandQueue_commit` →
`onSubmit` = `signalEvent`. Same residency-before-commit, signal-after-commit
ordering — **this part is correct.** Caveat: the residency-epoch gate
(`beginRecording:100`) and allocator-reset gate (`:85`) both read
`completedSerial()`, so they also wedge behind a never-signalling cmdbuf —
another reason the feedback handler must force-advance the completion cache.

## Recommended implementation order

1. **Defect 1** (per-resource serial + scoped `WaitForResource`) — removes the
   crawl, low-risk (primitives already exist).
2. **Defect 2** (commit feedback handler + force-advance completed cache) —
   removes the "never signals" hangs and hardens every `completedSerial()`
   reader.
3. **Defect 4** needs no separate work once 2 lands.

---

# Section 2 — WSI / Present path

Both target Metal 4 (`newMTL4CommandQueue`, `signalDrawable:`). The present
*mechanism* is nearly identical; the divergences are (1) **direct-render vs.
copy**, (2) **drawable residency**, (3) **maximumDrawableCount / timeout**.

## 1. Direct-to-drawable (kk) vs offscreen + compute-copy (us) — ROOT DIVERGENCE

**kk renders the frame DIRECTLY into the drawable's texture. No backbuffer, no
copy.** `kk_bind_drawable_to_vkimage` binds `CAMetalDrawable.texture` *as* the
swapchain `VkImage`'s plane storage: `plane->mtl_handle = mtl_retain(texture);
plane->addr = mtl_texture_get_gpu_resource_id(texture)` (`kk_wsi.c:31-49`). The
app render pass writes straight to the drawable as a **color attachment**.
Present is deferred bookkeeping: `kk_encode_drawable_present` stashes
`cmd->drawable = drawable` (`kk_wsi.c:52-58`, field `kk_cmd_buffer.h:216`).

**We keep a separate backbuffer and blit it into the drawable with a compute
encoder every Present** (`d3d9_swapchain.cpp` PresentImage:
`ctx->copyTexture(cDrawable.texture, …, cSrcView->image, …)`). MTL4 has no blit
encoder, so this is a **compute dispatch writing the drawable as a general
read/write resource**.

**Consequence:** a compute write to the drawable requires the drawable texture
to be **explicitly resident** on the MTL4 queue (§2) and exact size clamping
(hence our `std::min` clamps + drawableSize re-pin hacks). kk sidesteps all of
it: a render-pass color attachment needs no manual residency and can't be
size-mismatched because the attachment *is* the drawable. Our copy stage is the
source of partial/black content and the GPU faults that wedge.

**Fix (highest impact):** render the final frame directly into the acquired
drawable's texture as a color attachment. If D3D9 semantics force a backbuffer,
at minimum make the present copy a **render pass** (fullscreen draw into the
drawable as attachment) rather than a compute write to a general texture, so
residency/framebufferOnly stop mattering.

## 2. Drawable residency on the MTL4 queue — LIKELY DIRECT CAUSE OF WEDGE

**kk:** one device-wide residency set added to the queue **once** at queue
creation (`mtl_command_queue_add_residency_set`, `kk_queue.c` init; bridge
`mtl_command_queue.m:78-86` = `[queue addResidencySet:s]`), refreshed via
`kk_device_make_resources_resident(dev)` before every submit. kk **never adds
the drawable texture** to any residency set — it only writes the drawable as a
render-pass attachment (implicitly handled).

**us:** per-context set bound per cmdbuf via `MTL4CommandBuffer_useResidencySet`
(`mtl_context_impl.cpp:51-54,123,194-195`). **We never add the acquired
drawable's texture to the set** before the compute copy writes it.

**Consequence:** under Metal 4 a non-resident resource touched by a shader
reads/writes zeros or faults. Compute copy into a **non-resident drawable** →
black/partial; a fault → queue timeline stalls → `signalDrawable` never advances
→ drawable never recycled → after `maximumDrawableCount` acquisitions
`nextDrawable` blocks → **deadlock after a few frames.** Matches our symptom
exactly (stuck at 3 presents).

**Fix:** if keeping the compute copy, add `cDrawable.texture` to the residency
set (or wire `CAMetalLayer.residencySet` / `addResidencySet`) and commit it
**before** the copy submits. Adopting §1 (render-pass write) removes the need —
which is why kk never wires drawable residency.

## 3. Present call — identical and correct

kk after commit: `mtl_command_queue_signal_drawable(queue, drawable);
mtl_drawable_present(drawable);` (`kk_queue.c:92-96`; bridge
`mtl_command_queue.m:134-149`) — CPU `[drawable present]`, **not**
`cmdbuf.presentDrawable:`. Ours in `synchronizeWsi`:
`MTL4CommandQueue_signalDrawable` + `MetalDrawable_present`
(`mtl_context_impl.cpp:272-273`). **Matches kk, correct.** Note: the *other*
Mesa path `wsi_metal_layer_blit_and_present` uses `[cmdbuf presentDrawable:]` on
a private queue — that's the generic CPU-copy fallback, NOT kk's path. Don't
model present on the blit path. Our removal of the pre-commit `waitForDrawable`
also matches kk (kk does no waitForDrawable). Good.

## 4. Acquire timing / throttle — layer config gaps

**kk layer config** (`wsi_metal_layer_configure`, `wsi_common_metal_layer.m`):
```
metal_layer.allowsNextDrawableTimeout = YES;   // "So acquire timeout works"
metal_layer.framebufferOnly           = NO;
metal_layer.maximumDrawableCount      = image_count;
metal_layer.drawableSize              = {width, height};
metal_layer.displaySyncEnabled        = !enable_immediate;
```

**Our applyProps** sets contents_scale=1.0, drawable size, opaque,
display_sync, framebuffer_only=false, pixel_format. **Never sets
`maximum_drawable_count`** — and the bridge only applies it when non-zero
(`winemetal_unix.c:3753`), so we run at the CA default of **3** — exactly where
we deadlock. **Never sets `allowsNextDrawableTimeout`** (no such field in
`WMTLayerProps`); CA default YES saves us from a hard block, but we rely on an
unset default.

**Fixes:**
- Set `props.maximum_drawable_count = imageCount` so it matches acquire cadence
  and isn't silently pinned at 3.
- Add `allowsNextDrawableTimeout` to `WMTLayerProps`, set `YES` explicitly, so a
  stalled frame yields a nil drawable (our `acquireNextImage` already handles
  nil) instead of a hard hang.
- Leave `presentsWithTransaction` unset (YES forces blocking CPU present).
- `contentsScale=1.0` + drawableSize re-pin is a CrossOver workaround only
  necessary *because of the copy path* — direct-render removes the size-mismatch
  bug class entirely.

## 5. Residency wiring summary (concrete Metal-4 rule)

- kk: `addResidencySet` on the queue once, `make_resources_resident` per submit;
  drawable never added (render attachment only).
- us: `useResidencySet` per cmdbuf, rebuilt per epoch, but **drawable texture
  never inserted** before the compute copy. Bridge already exposes
  `MTLResidencySet_addAllocation` (`winemetal 6136`) — we just don't call it on
  the drawable.

**Net WSI recommendation:** adopt kk's model — render directly into the drawable
as a color attachment, drop the separate backbuffer + compute copy. That one
change removes the residency gap, the size-clamp/re-pin hacks, and the
framebufferOnly dependency at once. If a backbuffer must persist for D3D9
GetFrontBufferData, make the present a **render-pass draw** into the drawable
(not a compute write), and set `maximumDrawableCount=imageCount` +
`allowsNextDrawableTimeout=YES`. **The residency omission (§2) is the most
probable direct cause of "partial content then wedge at ~3 frames."**

---

# Section 3 — Residency / Allocator / Barriers

## Bug 1 (TOP — non-resident hang / GPU fault): we wipe the residency set every epoch and re-add lazily; kk never does

**kk:** ONE persistent, device-lifetime residency set (`kk_device.c:311`),
attached to the **command queue once** at queue creation
(`mtl_command_queue_add_residency_set`, `kk_queue.c` `kk_queue_init`), covering
*every* command buffer forever. Allocations are added at **resource-allocation
time** and removed only at free (`kk_bo.c:61` add on `kk_alloc_bo`,
`kk_bo.c:80-82` remove on destroy; `kk_device_memory.c:131/138`).
`commit`+`requestResidency` run once per submit in
`kk_device_make_resources_resident` (`kk_device.c ~465`), called before every
commit. The set is **never cleared during its life** — any resource the GPU can
touch (including bindless `gpuResourceID` in argument tables) is guaranteed
resident because *all* memory is in the set.

**us:** the set is **torn down and rebuilt** in `beginRecording` whenever the
epoch retires — `MTLResidencySet_removeAllAllocations` + `commit` +
`m_residentSet.clear()` (`mtl_context_impl.cpp:98-107`). After the wipe,
resources are re-added **lazily, per-resource, when re-touched** via
`makeResident` (`mtl_context_impl2.cpp:307-315`, from
`flushVertexBuffers`/`flushResources`/`flushDynamicState` and each transfer).
Correctness now depends on **every referenced resource being re-`makeResident`ed
every single frame**. The code already documents one instance of the failure
(the `m_visBuffer` "BIF0 page fault" special-case, `:104-111`) — that is the
general bug, patched for exactly one buffer.

**Consequence:** any GPU-referenced resource not re-added on the current frame
after an epoch wipe is **non-resident → GPU page fault / hang.** Highest risk:
(a) resources reached only via **bindless `MTLResourceID` in the argument
table** (bound once, not re-run through `makeResident` this frame); (b) the
drawable/attachment when `residentTextureView` falls back to a view handle whose
base image "carries no owned allocation" (`:317-329`); (c) any clean frame that
skips the flush path. `useResidencySet` at cmdbuf open (`:123`) is *coverage*,
not *contents* — attaching an incomplete set changes nothing.

**Fix (adopt kk's model):**
1. Add allocations to the residency set **at resource creation**
   (buffer/texture/heap alloc), remove at destroy — not during encode.
2. Attach the set to the **queue once** (`MTL4CommandQueue addResidencySet`) or
   keep `useResidencySet` but stop wiping.
3. **Delete the `removeAllAllocations`/rebuild in `beginRecording`**
   (`:98-112`). Keep only `commit`+`requestResidency` per submit (`:194-195`).
4. If memory pressure ever forces eviction, evict only *dead* allocations, never
   a blanket wipe.

## Bug 2 (command-stream corruption): allocator reset gate is log-only

**kk:** each `kk_cmd_buffer` owns its own allocators; `mtl_command_allocator_
reset` (`mtl_command_buffer.m:15`) runs only from `kk_reset_cmd_buffer_internal`
at `vkBeginCommandBuffer`/`vkResetCommandBuffer`. The GPU-idle guarantee comes
from the **application's Vulkan fences** — kk never reuses an allocator whose
work is in flight.

**us:** a **shared 3-slot ring** rotated by `slot = m_frameIndex %
m_allocators.size()`, reset with **no wait** — the completion check
(`mtl_context_impl.cpp:85-92`) only `Logger::warn`s ("BISECT: log-only while
diagnosing") then calls `MTL4CommandAllocator_reset` **unconditionally**. The
comment above (`:74-79`) states the correct rule but the wait was never wired.

**Consequence:** with >ring-depth (3) submissions in flight — the
shader-compile-stall / multi-flush-per-frame case — `reset` recycles command
memory the GPU is still reading → command-stream corruption, intermittent GPU
faults. Matches the "GTA boot death."

**Fix:** before reset, block on the slot's serial:
```cpp
if (submissions().completedSerial() < m_allocatorSerial[slot])
    submissions().waitFor(m_allocatorSerial[slot]);
MTL4CommandAllocator_reset(m_allocators[slot].handle());
```
(A deeper ring reduces stall frequency.) Do **not** ship the log-only gate.

## Bug 3 (residency add ordering — minor)

Ordering within a single frame is OK — `makeResident` adds during encode, then
`submit` does `commit`+`requestResidency` before `MTL4CommandQueue_commit`
(`:194-199`). The hazard is purely Bug 1 (the wipe), not this ordering.

## Bug 4 (barriers — largely correct, tighten later)

`emitProducerBarrier` calls `MTL4CommandEncoder_barrier(enc, afterStages,
WMTStageAll, VisibilityOptionDevice)` (`mtl_context_impl2.cpp:342-349`). This is
**not** broken "producer-only": `barrierAfterStages:beforeQueueStages:` *is* the
complete queue barrier (`after`=producer, `before`=All = conservative consumer).
`closePass` ends the render encoder and emits a fragment producer barrier
(`mtl_context_impl.cpp:408-419`), matching kk's tear-down-on-transition. Correct
and safe. Minor: kk uses `after = All` at every encoder stop
(`kk_cmd_buffer.c:335`); we use `after = Fragment` in `closePass` — fine for
D3D9. Consider centralizing barrier emission at one encoder-close choke point
(kk's `kk_stop_encoder`) so no close path bypasses it.

---

# Consolidated priority (all three sections)

The three audits **converge on the residency model** as the primary
architectural fault. Recommended order:

1. **Residency: stop wiping (Sec 3 Bug 1).** Persistent device-lifetime set,
   add-at-alloc / remove-at-free, attached to the queue once. Kills the
   intermittent non-resident GPU faults (GTA boot deaths, the drawable-write
   fault that wedges present). **Highest leverage — do first.**
2. **Present: render direct-into-drawable, or at least add the drawable to the
   residency set + set `maximumDrawableCount=imageCount` +
   `allowsNextDrawableTimeout=YES` (Sec 2 §1-2,4).** Kills the partial-content +
   wedge-at-3-frames.
3. **Per-resource wait scoping (Sec 1 defect 1).** Stamp `trackingSerial` on
   every referenced resource; implement `isInUse()`; scope `WaitForResource` to
   the resource's serial. Kills the per-lock full-drain crawl.
4. **Commit feedback handler (Sec 1 defect 2).** Bridge `MTL4CommitOptions` +
   `addFeedbackHandler`; on GPU error force-advance the completion cache + flag
   device-lost. Turns "never signals" hangs into prompt errors and hardens every
   `completedSerial()` reader.
5. **Allocator reset wait (Sec 3 Bug 2).** Restore the real `waitFor` gate;
   remove the log-only BISECT stub.

Items 1 + 2 are the two that most plausibly explain the black/partial/wedge
GTA + visual-suite behavior; item 3 explains the crawl; item 4 is the safety net
that stops any residual GPU fault from wedging the whole app.
