# 40 — Bridge: the winemetal PE/unix split

`bridge/` is a fork of DXMT's winemetal — the ONLY path from the PE `d3d9.dll`
to Metal. Policy (SOURCE_OF_TRUTH §3): DXMT is the gold standard for this
seam; our changes are **additive patches in DXMT's exact style** so `diff` vs
upstream (`bridge/UPSTREAM` pin) stays cherry-pickable. LGPL: winemetal stays
its own dll/so; the MIT driver links it dynamically.

## The split

- **PE side** (Windows code, x86): `winemetal_thunks.{h,c}` — one exported
  `WINEMETAL_API` C function per operation; fills a param struct; calls
  `UNIX_CALL(code, &params)` (`WINE_UNIX_CALL`; debug builds assert on
  failure, except retain/release at teardown). Built as `winemetal64.dll` +
  `winemetal32.dll`. `main.c` = DllMain (+ an FH4 fiber-data workaround).
- **unix side** (macOS x86_64 Mach-O): `unix/winemetal_unix.c` (~7k lines) —
  one `static NTSTATUS _Name(void *obj)` impl per operation doing the real
  ObjC Metal calls, registered in TWO dispatch tables:
  `__wine_unix_call_funcs[]` and `__wine_unix_call_wow64_funcs[]`
  (winemetal_unix.c ~6988 / ~7219).

**Dispatch-table lockstep rule (ABI):** the dispatch code IS the array index.
Append only, never renumber, and append to BOTH tables in the same change.
Historical blocks: upstream 0..147 (incl. apitrace 136..142), DXSO 148..152,
d9mt additions 153..156 (vertex-descriptor PSO + d9mtair AIR compile), the
Metal 4 surface 157..210, wire transport 211..214.
**Current highest index: 214 (`WireRing_detach`).** The next addition is 215.

## Thunk anatomy — 7-step recipe for adding one

Example pattern: `WireRing_stats` / `MTL4ArgumentTable_setAddress`.

1. **Param struct** in `bridge/winemetal_thunks.h` (`#pragma pack(push, 8)`
   region): `struct unixcall_<name> { ... }`. Every member 8-byte /
   arch-invariant (`obj_handle_t`, `uint64_t`, `WMTMemoryPointer` /
   `WMTConstMemoryPointer` for PE pointers, floats padded) → the 64-bit thunk
   is wow64 byte-compatible and needs NO thunk32 twin. Reuse the generic
   structs (`unixcall_generic_obj_obj_ret` etc.) when the shape fits.
2. **Public decl** in `bridge/winemetal.h` (and any `WMT*` info structs it
   needs, DXMT-style).
3. **PE thunk** in `bridge/winemetal_thunks.c`: fill params
   (`WMT_MEMPTR_SET` for pointers), `UNIX_CALL(<next index>, &params)`,
   return out-fields.
4. **Unix impl** in `bridge/unix/winemetal_unix.c`: `static NTSTATUS
   _Name(void *obj)` — cast, do the ObjC work, write results into the param
   struct. Respect the ownership contract (below).
5. **Register in BOTH dispatch tables** at the same (new, appended) index,
   as the `_pooled`-wrapped name (the `D9MT_POOLED` macro generates the
   `@autoreleasepool` wrapper).
6. If the driver calls it: declare in whatever backend header consumes it
   (handles are plain `obj_handle_t`; `backend/wmt_handle.h` provides the
   `WmtRef` RAII wrappers).
7. Rebuild + install: `build-bridge.sh` (smoke test must pass) then
   `install-bridge-builtins.sh` (system-wide CrossOver copy — without it you
   run the STALE bridge), then a clean driver build if headers changed.

## ABI discipline

- No pointers on the wire except `WMTMemoryPointer` (PE addresses the unix
  side may dereference — the wmtcmd precedent: unix reads PE memory, never
  the reverse for 32-bit PE).
- Results for 32-bit PE callers go INTO caller-provided PE buffers
  (`d9mtair` returns metallib bytes this way — a 32-bit PE cannot read unix
  pointers).
- Handles are `obj_handle_t` (u64) wrapping the ObjC pointer unix-side.
- Struct layout changes to any `unixcall_*`/`WMT*`/`wmtcmd_*` = rebuild
  bridge AND driver (and `rm -rf build/driver/obj`).

## Ownership / autorelease rules (commit 2ed9b58 — the leak fix)

Wine threads have no NSAutoreleasePool, so **every dispatch entry runs inside
a per-call `@autoreleasepool`** via the generated `_pooled` wrappers.
Unwrapped exceptions: `_NSObject_retain`, `_NSObject_release` (hottest, pure
refcount) and `_NSAutoreleasePool_alloc_init` (a PE-created pool must not
nest inside the wrapper pool — out-of-order pop corrupts the pool stack).

**Contract for every handle written to `params->ret` / `ret_error`:**
1. owned +1 (alloc/new/copy or explicit `[... retain]` — encoders,
   commandBuffer, NSError out-params, strings do this); the PE side must
   `NSObject_release` it eventually; OR
2. parent-owned borrowed (property getters: `drawable.texture`,
   `layer.residencySet`, `NSArray_object`) — valid only while the parent
   handle lives.

Returning a bare autoreleased object = use-after-free at pool drain (crash
shape: objc_msgSend EXC_BAD_ACCESS inside a `_pooled` fn or at next PE use).
Residual OBJC_DEBUG_MISSING_POOLS noise from winemac.so / Metal HUD is NOT
ours — don't chase it.

## The execute_on_main hazard (deadlock class)

`execute_on_main()` (winemetal_unix.c ~1523) does `dispatch_sync` to the main
queue. **Never call it (or any dispatch_sync-to-main) from a path a D3D9
Reset can hold.** The proven deadlock (fixed in acc2270): during a display-
mode change, wine's main thread blocks SendMessage'ing `WM_DISPLAYCHANGE` to
the app thread — which sits in `Reset` (device lock held) waiting on a bridge
call. If that bridge call sync-hops to main, the pair deadlocks.
`_MetalLayer_setProps` is the template for the fix: copy params **by value**
(the caller's struct is gone once the call returns), `[layer retain]`
manually (no ARC in that TU), apply via `dispatch_async`. Layer props are
configuration, not per-frame state — a beat of latency is harmless. Prefer
async + value-copy + retain for ANY main-thread work reachable from device
paths.

## The wire transport (codes 211..214)

PE producer (`backend/mtl_wire_context.{h,cpp}`) ↔ consumer (now the
render-server process; 70-render-server.md). Record/ring ABI:
`driver/mtl/mtl_wire.h` — compiles on BOTH sides, fixed-width fields only,
`D9MT_WIRE_ASSERT(sizeof)` everywhere.

- **Ring:** one page-aligned region = `d9mt_wire_ring` control block (192 B:
  magic/capacity/data_offset; producer line: head, stalls; consumer line:
  tail(=progress), records, bytes, decode_errors, shutdown/consumer_exited)
  + 4 MiB data. head/tail are FREE-RUNNING byte counters (index =
  `cursor & (capacity-1)`); acquire/release atomics (Rosetta preserves x86
  ordering). Records are length-prefixed (`d9mt_wire_rec_hdr{opcode,size}`),
  8-byte multiples, never straddle the wrap: an `op_pad` header skips to the
  boundary (its size field is ignored — the skip can exceed uint16).
- **MAP_FIXED-over-VirtualAlloc trick** (THE 32-bit constraint): d3d9.dll is
  i686 — the unix side canNOT hand back a fresh mmap pointer above 4 GiB.
  So the PE side `VirtualAlloc`s the region (page-aligned) and
  `WireRing_attach` (211) mmaps the shm FILE (`$TMPDIR/d9mt-<pid>/ring`) over
  that exact VA with `MAP_SHARED|MAP_FIXED`. Same pointer before/after,
  file-backed after. Never munmap that VA unix-side — on fallback restore
  anonymous MAP_FIXED memory; the PE `VirtualFree` (after `WireRing_detach`,
  214) drops the last file ref.
- **Kick/park:** producer calls `WireRing_kick` (212) ONLY on the
  empty→nonempty transition; currently a no-op (server polls at 1 ms — an S2
  kick channel slots in without touching the producer). Shutdown: Detach
  record → `ring->shutdown=1` (release) → `WireRing_detach` (bounded <1 s:
  ≤500 ms drain wait, SIGTERM+reap, SIGKILL last resort, unlink dir).
- **Stats:** `WireRing_stats` (213) fills `WMTWireStats` (records/bytes/
  stalls/decode_errors + heartbeat + server_error from the status block);
  surfaces on the PROF line once per 256-frame window.
- Opcodes (`d9mt_wire_op`, uint16, append-only): pad=0, detach=1, then the
  hot loop — bind_shader/texture/sampler/uniform_buffer(_range)/
  vertex_buffer(_range)/index_buffer/render_targets, set_viewports/topology/
  vertex_layout/blend_mode/rasterizer/multisample/depth_stencil/stencil_ref/
  blend_color/depth_bias, push_data, clear_render_target, draw, draw_indexed
  (count=25). Cold calls (creates/copies/blits/queries/lifecycle) get NO
  records — they keep their direct thunk paths.

## Known stale comments (do not be misled)

- `mtl_wire.h` says the PE producer compiles as "mingw x86_64" — the driver
  d3d9.dll is i686 (build-driver.sh, S1 client handoff). The ABI rules make
  this moot, but the comment is wrong.
- Multiple bridge comments predate the render server and describe the
  deleted in-process consumer thread; the consumer lives in
  `server/consumer.m` now.
