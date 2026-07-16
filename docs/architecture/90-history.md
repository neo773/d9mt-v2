# 90 — History: debugging sagas as case studies

Each entry: symptom → root cause → fix → the transferable lesson. Commits are
in this repo (`git show <sha>`); several also live as agent memories.

## The clear-drop / mandala (RT feedback loop)

Render-to-texture content accumulated frame over frame (a growing radial
"mandala") — later, GTA's whole 3D world was black. A texture bound as both
RT and sampler tripped DXVK's hazard path (`ensureTextureUsage` recreate +
framebuffer re-dirty) AFTER `Clear()` had folded its clear into the pending
pass; the re-issued `bindRenderTargets` rebuilt `m_pass` and **wiped the
folded clearColor** → loadAction=Load. An interim band-aid (auto-unbind
sampled RTs in SetRenderTarget) violated D3D9 sampler persistence and nulled
GTA's tonemap scene texture every frame → adaptation death spiral → black
world. Root fix (35d32d0): `bindRenderTargets` flushes folded pending clears
before replacing the pass; band-aid deleted; clear-fold attachment identity
compares backing texture + mip/layer, not Rc pointers.
**Lesson: state that is "pending" must survive every path that rebuilds its
container; and never fix a backend ordering bug by breaking API semantics
upstream.**

## The zero-stride PSO crash

GTA IV died in Metal: "Attribute … references a buffer at index 3 that has
no stride." `resolvePso` used the vertex-declaration stride, which D3D9
allows to be 0/stale — the real stride comes from `SetStreamSource`. Fix:
prefer the live `m_vertexStreams[binding].stride` in the PSO layout
(it already dirties the PSO key). The cube demo had only passed because its
two strides happened to match.
**Lesson: D3D9 splits one concept across two APIs (decl vs stream); always
bind the live one. A passing demo can be a coincidence of equal values.**

## Staging VA exhaustion

1276× "staging allocation failed (address space?)" then unix call
0xc0000005. The single 4 MiB `MtlStagingBuffer` returned empty slices once
full and **nobody ever reset or replaced it** — after the first 4 MiB every
upload failed forever and a null-backed buffer eventually crossed the bridge.
Fix: `AllocStagingBuffer` retires the full backing and `reinit()`s fresh;
retired bytes folded into throttle statistics (c3c8363 family: VirtualAlloc'd
backings also had to be freed on retirement).
**Lesson: every "caller allocates a fresh one" comment needs a caller that
actually does; exhaustion paths are correctness paths in a 32-bit VA.**

## The frame-2198 residency freeze

NVIDIA Shadows demo hard-froze at EXACTLY presented frame 2198, 0% CPU, no
GPU fault. Two independent diseases: (1) `RcObject`/`MtlAllocation` had **no
virtual destructor**, so Rc deleting through the base pointer never ran
`~MtlAllocationImpl` — residency removals were literally ZERO for the run;
(2) drawable textures were added to the device-lifetime residency set, so
CoreAnimation could never recycle them → after ~9 GB of drawables
`nextDrawable` starved forever. Also: `ctx->signal()` kept the fence alive
but never signaled it (staging throttle would deadlock). Fix (21e5d85): the
`MtlResidencyTracker` rewrite — the four invariants in mtl_residency.h
(50-resources.md) — plus `virtual ~MtlAllocation()`.
**Lessons: a count-deterministic freeze is a resource-pool exhaustion, count
the pool; and diff adds vs removes — a leak where removals are exactly zero
is a destructor-never-ran bug, not a slow leak.**

## Push-block aliasing (invisible peds/vehicles)

GTA world rendered but skinned/palette meshes were degenerate or invisible.
`air_builder` dropped `DclPushData`'s stage-mask operand, so the VS per-stage
push block landed at byte 0 **aliased onto the shared block** — the VS read
fogColor/alphaRef where cFSize/blend params should be. Adjacent fix: partial
push updates must still republish the full extent (`pushExtent` watermark) or
the shader reads past the fresh slice's end; composite-index byte offsets
must come from `ir::Type::byteOffset`, not load-size scaling (082960e,
b610c22).
**Lesson: layout bugs present as "some objects wrong, most fine" — suspect
every operand you ignore during IR walking. Regression tests for this are
STILL unwritten (air/tests loopback case + code_skin ladder canary).**

## The AB swap that didn't move fps (profiler lied)

The instrumented profiler (`D9MT_PROFILE_INSTRUMENT`) ranked per-slot binding
(BitMask iteration, WMTMemoryPointer::set) as the top cost, so the
argument-buffer binding model was ported (8432d36) — correct, kept, but fps
was FLAT: those functions inline to nothing in release; instrumentation had
manufactured the cost. Release-build bucket profiling (`D9MT_PROFILE`)
showed GTA co-bound (GPU 10-11 ms ≈ CPU 11-13 ms) — the real lever was GPU
pass count.
**Lesson: bucket-profile a RELEASE build before believing instrumented
rankings; instrumentation inflates exactly the small hot functions it
wraps. Read relative rank + call counts, never absolutes.**

## MSAA mosaic (Civ IV tiles)

Civ IV displayed as a scrambled puzzle of misplaced/rotated tiles.
The app requested a multisampled backbuffer; Present copied the raw MSAA
texture into the single-sample drawable — invalid in Metal, rendered as raw
sample-block memory. `resolveTexture` was still a stub, so no resolve
existed anywhere (StretchRect resolves silently no-op'd too). Fix (2a905c3):
real resolveTexture (store-action resolve pass) + a single-sample
`m_resolveBuffer` the swapchain presents from.
**Lesson: audit MTL_STUB no-ops when output is garbled — a silent stub in a
copy path shows up as memory-layout art, and only for apps whose settings
exercise it.**

## Layer geometry / cmd-tab zoom

After minimize/restore (cmd-tab), the window showed the top-left quarter
magnified — or presents got 4× more expensive. The presenter wrote
`contentsScale=1.0` + a pinned `drawableSize` on every applyProps, racing
winemac's view layout which recomputes both; whoever wrote last won.
Fix (a7ae922 + acc2270): geometry belongs to the window system —
contentsScale NEVER written, drawableSize only at create/recreate (a
mode-change Reset must re-pin), zero-fields mean leave-untouched in the
bridge, and the scaled present blit absorbs whatever size layout imposes.
**Lesson: when two writers race for the same property, don't write faster —
decide who OWNS it and make the other writer structurally unable to clobber.**

## WM_DISPLAYCHANGE deadlock

Display-mode change froze the process. Wine's main thread was blocked
SendMessage'ing WM_DISPLAYCHANGE to the app thread; the app thread sat in
D3D9 `Reset` (device lock held) inside a bridge call that `dispatch_sync`'d
to the main thread. Classic ABBA across a process-architecture boundary.
Fix (acc2270): `_MetalLayer_setProps` copies props by value, retains the
layer, applies **async** on main.
**Lesson: never dispatch_sync to the main thread from any path a device
lock/Reset can hold; configuration work tolerates async, deadlocks don't.**

## CmdChunk copy corruption

GTA IV boot crash: unix replay died on "unhandled mtl4 render command type".
`std::vector<FrameEntry>` growth used move_if_noexcept; under C++17
`std::function`'s move ctor isn't noexcept, so FrameEntry's implicit move
wasn't either → vector growth **copied** entries. CmdChunk's implicit copy
deep-copied the block arena while head/tail/next still pointed at the
source's blocks; the source died, replay walked freed heap (frame 1
survived on stale-but-intact bytes). Fix (570cad0): CmdChunk copy deleted
(compile error forever), FrameEntry moves explicitly noexcept.
**Lesson: any struct owning raw intra-links must delete copy; and
`noexcept`-ness of moves is load-bearing for vector-held types — assert it.**

## Fan wedges

GTA's minimap/waypoint polygons drew as disconnected wedges (every 3rd
triangle). Vulkan has native triangle fans so the DXVK frontend passes them
through; Metal doesn't, and the backend's topology switch fell into the
triangle-list default. Fix (17f7ec8): non-indexed fans draw indexed off one
shared prefix-pattern buffer (0,1,2, 0,2,3, …) with base_vertex=firstVertex.
**Lesson: enumerate the source API's primitive/feature set against the
target's — silent fallthrough on an unhandled enum is a rendering bug that
looks content-specific.**

## The -O1 discovery

Every perf measurement to that date had run on a `-O1 -g` build — the
"release" flags since inception (af34995). Under Rosetta, unoptimized code
pays double. One flag change (-O2, plus `-fno-strict-aliasing` to protect
the PE↔unix struct punning) shifted the entire baseline.
**Lesson: before optimizing anything, read the build flags; and pin
aliasing rules when raw-struct ABIs cross a boundary at -O2.**

## Deferred-pass serialization

The frame-deferred pass architecture (decision 10, mtld3d port) landed with
whole-frame load/store optimization — and regressed GTA fps hard. Batching
all encoding into one flush at submit serializes CPU and GPU: frame time =
CPU + GPU instead of overlapped max(CPU, GPU). The store/load rules also
never fire on deferred renderers (GTA samples everything it stores).
Shipped default-OFF behind `D9MT_DEFERRED=1` (3512ff5); re-defaults when the
render server gives submission overlap.
**Lesson: an optimization that changes WHEN work is submitted changes the
pipeline shape; measure end-to-end fps, not the optimized quantity.**

## The falsified native premise

The plan to escape Rosetta by moving the CS hot loop "to the native unix
side" (NATIVE_BACKEND_PLAN) died the day it was verified: the whole wine
process — including winemetal.so and in-process Metal.framework code — runs
x86_64-translated; macOS cannot mix ISAs in a process. Phases 2-3 cancelled
same-day (1faf24f); the Phase 0/1 wire transport survived (process-agnostic
by design) and became the render server's foundation (70-render-server.md).
**Lesson: verify the load-bearing premise FIRST with a 10-minute probe
(`activity monitor: "X86-64 (translated)"`) before building phases on it —
and design transports process-agnostic so a falsified premise doesn't waste
the plumbing.**

## Honorable mention: the shared argument table (FF strobe)

FF games flickered "all states of the spinning at once" while a GPU capture
(serialized) showed a perfect frame. ONE mutable MTL4ArgumentTable was
reused across draws; under overlapping execution draw N+1's setAddress
mutated the table draw N was still reading. Fix (b5bc4b8): the per-pass
rotated pool + retire-by-serial (30-backend.md). The decision-3 comment had
claimed snapshot safety for the ring MEMORY — true — while the TABLE OBJECT
was shared — false.
**Lesson: serialized captures hide overlapping-execution races; and audit
which OBJECT is shared, not just which memory.**
