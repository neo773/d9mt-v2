#ifndef D9MT_MTL_WIRE_H
#define D9MT_MTL_WIRE_H

/*
 * mtl_wire.h — POD wire format for the native-backend transport
 * (docs/NATIVE_BACKEND_PLAN.md, Phase 0/1).
 *
 * One fixed-layout record per HOT-LOOP MtlContext call — the per-draw state
 * machine (binds, state setters, clears, draws). Cold calls (resource
 * creates, copies/blits/resolves, queries, init/upload, frame lifecycle,
 * present/submit) get NO wire records: they keep their existing direct
 * paths. The PE-side MtlWireContext (driver/backend/mtl_wire_context.cpp)
 * serializes every hot call into the SPSC ring below; the unix-side
 * consumer thread (bridge/unix/winemetal_unix.c) decodes, validates and
 * counts them. Phase 1 is ECHO mode: the PE side still executes every call
 * locally on the real MtlContextImpl, the consumer only proves the
 * transport (decode + count + discard).
 *
 * THIS HEADER COMPILES ON BOTH SIDES OF THE BOUNDARY:
 *   - mingw i686 C++17 (driver d3d9.dll, the PE producer — 32-bit PE)
 *   - clang x86_64 ObjC/C11 (bridge unix consumer) and arm64 (d9mt-serverd)
 * ABI rules (NATIVE_BACKEND_PLAN "Invariants"):
 *   - fixed-width integer/float types ONLY; no pointers, no bool, no enums
 *     as struct fields (opcodes/enum values travel as uint16/uint32),
 *   - explicit padding to 8-byte multiples, natural alignment (no #pragma
 *     pack needed: every struct is tiled from 4/8-byte units),
 *   - D9MT_WIRE_ASSERT(sizeof(...) == literal) on every struct, evaluated
 *     by BOTH compilers.
 *
 * RESOURCE IDENTITY: the Metal objects already live unix-side behind
 * obj_handle_t — that IS the handle system. Records carry the obj_handle_t
 * (u64) plus the fields the hot loop actually consumes downstream:
 * gpuResourceID / gpuAddress mirrors the PE wrappers already hold, and the
 * POD key bits (format, extent, stride, offsets). No pointer-valued fields
 * anywhere; the consumer dereferences nothing in Phase 1.
 *
 * ---------------------------------------------------------------------------
 * RING (single producer = PE CS thread, single consumer = d9mt-serverd, the
 * native arm64 render-server process — docs/RENDER_SERVER_SPEC.md Phase S1)
 *
 * Memory is ONE page-aligned PE region handed across via
 * WireRing_attach(base, bytes):
 *   [ struct d9mt_wire_ring (192 B control block) ][ data: capacity bytes ]
 * The unix side relocates it into the MAP_SHARED file $TMPDIR/d9mt-<pid>/ring
 * (mmap MAP_FIXED over the same VA) and spawns the server; when no server is
 * available it stays plain memory with NO consumer and the producer's
 * ring-full degradation applies (stalls -> permanent forward-only).
 * capacity is a power of two. head/tail are FREE-RUNNING byte counters
 * (never wrapped); a cursor's ring index is (cursor & (capacity-1)).
 *
 *   producer (PE):  space check   head - tail_acquire <= capacity - need
 *                   (spin + stalls++ while full; consumer only counts, so
 *                   full means the consumer thread is starved, not slow)
 *                   write record at (head & mask), THEN head.store(release)
 *   consumer:       head.load(acquire), decode records in [tail, head),
 *                   tail.store(release) after each record — tail doubles as
 *                   the PROGRESS COUNTER the PE side may read (acquire).
 *
 * Records are length-prefixed (d9mt_wire_rec_hdr.size = total bytes incl.
 * header + trailing payload, 8-byte multiple) and never straddle the wrap
 * point: when a record does not fit in the bytes remaining to the end of
 * the data area, the producer writes a d9mt_wire_op_pad header there and
 * advances head to the next capacity boundary. On seeing op_pad the
 * consumer advances tail by (capacity - (tail & mask)) — the pad record's
 * size field is ignored (the skip can exceed uint16).
 *
 * KICK / PARK PROTOCOL:
 *   - S1: the cross-process consumer parks on a 1 ms poll (no kick channel
 *     over the process boundary yet — server/README.md caveats), so
 *     WireRing_kick() is currently a no-op. The producer still calls it
 *     ONLY on the empty->nonempty transition (at record-write time it
 *     sampled tail == previous head), keeping the protocol intact for the
 *     S2 kick channel (pipe/kqueue/futex).
 *   - SHUTDOWN: PE writes the Detach record, sets ring->shutdown = 1
 *     (release) and kicks; the server drains what remains, publishes tail
 *     (mirrored into status.progress), sets consumer_exited=1 (release) and
 *     exits. The PE side then calls WireRing_detach() — bounded < 1 s: wait
 *     <=500 ms for the drain, SIGTERM + reap the server if still alive,
 *     unlink the shm dir — and only after that releases the ring region.
 * ---------------------------------------------------------------------------
 */

#ifdef __cplusplus
#include <cstdint>
#include <atomic>
#define D9MT_WIRE_ASSERT(x) static_assert(x, #x)
typedef std::atomic<uint64_t> d9mt_wire_au64;
typedef std::atomic<uint32_t> d9mt_wire_au32;
#else
#include <stdint.h>
#include <stdatomic.h>
#define D9MT_WIRE_ASSERT(x) _Static_assert(x, #x)
typedef _Atomic uint64_t d9mt_wire_au64;
typedef _Atomic uint32_t d9mt_wire_au32;
#endif

D9MT_WIRE_ASSERT(sizeof(d9mt_wire_au64) == 8);
D9MT_WIRE_ASSERT(sizeof(d9mt_wire_au32) == 4);

#define D9MT_WIRE_RING_MAGIC 0x30315257544D3944ull /* "D9MTWR10" LE */

/* --- ring control block (offset 0 of the shared allocation) -------------- */
struct d9mt_wire_ring {
  /* line 0: immutable after attach */
  uint64_t magic;        /* D9MT_WIRE_RING_MAGIC */
  uint64_t capacity;     /* data bytes, power of two */
  uint64_t data_offset;  /* byte offset of the data area == sizeof(*this) */
  uint64_t _pad0[5];
  /* line 1: producer-written */
  d9mt_wire_au64 head;   /* free-running producer cursor (release) */
  d9mt_wire_au64 stalls; /* producer ring-full spin episodes */
  uint64_t _pad1[6];
  /* line 2: consumer-written */
  d9mt_wire_au64 tail;          /* free-running consumer cursor / progress (release) */
  d9mt_wire_au64 records;       /* records decoded (pad records excluded) */
  d9mt_wire_au64 bytes;         /* payload bytes consumed */
  d9mt_wire_au64 decode_errors; /* validation failures (stream dropped to head) */
  d9mt_wire_au32 shutdown;        /* PE -> consumer: drain and exit */
  d9mt_wire_au32 consumer_exited; /* consumer -> PE: safe to free the ring */
  uint64_t _pad2[3];
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_ring) == 192);

/* --- opcodes (uint16 on the wire) ----------------------------------------
 * Hot loop only. Order is ABI: append, never renumber. */
enum d9mt_wire_op {
  d9mt_wire_op_pad = 0,    /* skip to end of ring (see header comment) */
  d9mt_wire_op_detach = 1, /* courtesy end-of-stream marker (shutdown flag is authoritative) */
  d9mt_wire_op_bind_shader = 2,
  d9mt_wire_op_bind_texture = 3,
  d9mt_wire_op_bind_sampler = 4,
  d9mt_wire_op_bind_uniform_buffer = 5,
  d9mt_wire_op_bind_uniform_buffer_range = 6,
  d9mt_wire_op_bind_vertex_buffer = 7,
  d9mt_wire_op_bind_vertex_buffer_range = 8,
  d9mt_wire_op_bind_index_buffer = 9,
  d9mt_wire_op_bind_render_targets = 10,
  d9mt_wire_op_set_viewports = 11,
  d9mt_wire_op_set_primitive_topology = 12,
  d9mt_wire_op_set_vertex_layout = 13,
  d9mt_wire_op_set_blend_mode = 14,
  d9mt_wire_op_set_rasterizer_state = 15,
  d9mt_wire_op_set_multisample_state = 16,
  d9mt_wire_op_set_depth_stencil_state = 17,
  d9mt_wire_op_set_stencil_reference = 18,
  d9mt_wire_op_set_blend_color = 19,
  d9mt_wire_op_set_depth_bias = 20,
  d9mt_wire_op_push_data = 21,
  d9mt_wire_op_clear_render_target = 22,
  d9mt_wire_op_draw = 23,
  d9mt_wire_op_draw_indexed = 24,

  /* --- Phase S2 (render-server executor) COLD records — APPENDED ONLY.
   * Resource identity for S2: records carry BOTH a client-assigned monotonic
   * u64 `wire_id` (docs/RENDER_SERVER_SPEC.md — server-side identity) AND the
   * client-process obj_handle_t (`client_handle`) that the EXISTING hot
   * records (bind_*, buffer_refs) reference — the server resolves hot-record
   * references through client_handle and by-id records (destroy /
   * setRenderTargetsById / presentFrame) through wire_id. */
  d9mt_wire_op_create_buffer = 25,
  d9mt_wire_op_create_texture = 26,
  d9mt_wire_op_create_shader = 27,
  d9mt_wire_op_create_sampler = 28,
  d9mt_wire_op_destroy_resource = 29,
  d9mt_wire_op_set_render_targets_by_id = 30,
  d9mt_wire_op_present_frame = 31,
  d9mt_wire_op_clear_render_target_by_id = 32,

  /* --- Phase S3 (texture/buffer CONTENT via the shared heap; view identity
   * for texture binds) — APPENDED ONLY. Bulk bytes live in the `shared-heap`
   * file (struct d9mt_wire_heap below); records reference heap regions by
   * free-running offset + generation. */
  d9mt_wire_op_upload_buffer = 33,
  d9mt_wire_op_upload_texture = 34,
  d9mt_wire_op_bind_texture_by_id = 35,

  /* --- Phase S3 v5: PROCESS-LIFETIME server. The wire is decoupled from
   * MtlContext lifetime (D3D9 device lifetimes are chaotic: throwaway
   * devices, overlapping lifetimes). Context death sends a QUIESCE session
   * record — the server resets its resource table + folded state and KEEPS
   * RUNNING (acked by a status epoch bump); a later context CONNECTS with a
   * session-begin record. The server dies only with the client process
   * (pid watchdog) or an explicit Detach (tests / final teardown). */
  d9mt_wire_op_session = 36,

  /* --- Phase S3 v9: the GPU-side COPY/BLIT family. The cutover showed the
   * server's backbuffer black while 1.5M records/window executed: menu/legal
   * screens are COMPOSED with texture copies and stretched blits that were
   * "cold: forward only" — they never crossed. Records carry wire ids +
   * client handles (create-at-first-use precedes them); either side being
   * wire-INELIGIBLE (pseudo-resource) skips the record client-side (PROF
   * copySkip=). texture->buffer readbacks stay LOCAL (C2 slice). */
  d9mt_wire_op_copy_texture = 37,
  d9mt_wire_op_copy_buffer = 38,
  d9mt_wire_op_blit_texture_view = 39,
  d9mt_wire_op_resolve_texture = 40,
  d9mt_wire_op_generate_mipmaps = 41,
  d9mt_wire_op_clear_image_view = 42,

  d9mt_wire_op_count = 43
};

/* --- record header -------------------------------------------------------
 * size = total record bytes (header + fixed body + trailing payload),
 * 8-byte multiple, >= 8, and — except for op_pad, whose size is ignored —
 * never larger than the bytes remaining to the end of the data area. */
struct d9mt_wire_rec_hdr {
  uint16_t opcode; /* enum d9mt_wire_op */
  uint16_t size;
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_hdr) == 8);

/* --- shared sub-PODs ------------------------------------------------------ */

/* MtlBufferSlice, resolved to what draws consume: the snapshotted backing's
 * id<MTLBuffer> obj_handle_t, the slice-origin GPU address, offset/length. */
struct d9mt_wire_buffer_ref {
  uint64_t buffer;      /* obj_handle_t of the storage snapshot (0 = undefined) */
  uint64_t gpu_address; /* MTLBuffer.gpuAddress + offset (slice origin) */
  uint64_t offset;
  uint64_t length;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_buffer_ref) == 32);

/* A render-target attachment: the view's obj_handle_t + the POD key bits
 * the pass encoder consumes. */
struct d9mt_wire_rt_ref {
  uint64_t view; /* obj_handle_t of the view texture (0 = unbound) */
  uint32_t format;
  uint32_t width;
  uint32_t height;
  uint32_t sample_count;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rt_ref) == 24);

struct d9mt_wire_clear_value {
  float color[4];
  float depth;
  uint32_t stencil;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_clear_value) == 24);

struct d9mt_wire_viewport {
  float origin_x, origin_y;
  float width, height;
  float znear, zfar;
  uint32_t scissor_x, scissor_y, scissor_w, scissor_h;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_viewport) == 40);

struct d9mt_wire_vertex_attr {
  uint32_t location;
  uint32_t binding;
  uint32_t format; /* MtlFormat */
  uint32_t offset;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_vertex_attr) == 16);

struct d9mt_wire_vertex_layout {
  uint32_t binding;
  uint32_t stride;
  uint32_t per_instance; /* bool */
  uint32_t divisor;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_vertex_layout) == 16);

struct d9mt_wire_stencil_face {
  uint32_t fail_op;
  uint32_t depth_fail_op;
  uint32_t pass_op;
  uint32_t compare_op;
  uint32_t read_mask;
  uint32_t write_mask;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_stencil_face) == 24);

struct d9mt_wire_draw_args { /* MtlDrawArgs */
  uint32_t vertex_count;
  uint32_t instance_count;
  uint32_t first_vertex;
  uint32_t first_instance;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_draw_args) == 16);

struct d9mt_wire_draw_indexed_args { /* MtlDrawIndexedArgs */
  uint32_t index_count;
  uint32_t instance_count;
  uint32_t first_index;
  int32_t vertex_offset;
  uint32_t first_instance;
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_draw_indexed_args) == 24);

/* --- records --------------------------------------------------------------
 * Naming: d9mt_wire_rec_<opcode>. Trailing payloads are documented per
 * record; hdr.size covers them. */

/* header-only records: pad, detach */
struct d9mt_wire_rec_nul {
  struct d9mt_wire_rec_hdr hdr;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_nul) == 8);

struct d9mt_wire_rec_bind_shader {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t stage; /* MtlShaderStage */
  uint32_t _pad0;
  uint64_t library; /* obj_handle_t of the shader's MTLLibrary */
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_bind_shader) == 24);

struct d9mt_wire_rec_bind_texture {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t stages; /* MtlStageMask */
  uint32_t slot;
  uint64_t view;            /* obj_handle_t of the view texture (0 = unbind) */
  uint64_t gpu_resource_id; /* what the argument-table flush writes */
  uint32_t format;          /* MtlFormat (view key) */
  uint32_t view_type;       /* MtlTextureViewType */
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_bind_texture) == 40);

struct d9mt_wire_rec_bind_sampler {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t stages;
  uint32_t slot;
  uint64_t sampler;         /* obj_handle_t (0 = unbind) */
  uint64_t gpu_resource_id; /* what the argument-table flush writes */
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_bind_sampler) == 32);

struct d9mt_wire_rec_bind_uniform_buffer {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t stages;
  uint32_t slot;
  struct d9mt_wire_buffer_ref buf;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_bind_uniform_buffer) == 48);

struct d9mt_wire_rec_bind_uniform_buffer_range {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t stages;
  uint32_t slot;
  uint64_t offset;
  uint64_t length;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_bind_uniform_buffer_range) == 32);

struct d9mt_wire_rec_bind_vertex_buffer {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t binding;
  uint32_t stride;
  struct d9mt_wire_buffer_ref buf;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_bind_vertex_buffer) == 48);

struct d9mt_wire_rec_bind_vertex_buffer_range {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t binding;
  uint32_t stride;
  uint64_t offset;
  uint64_t length;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_bind_vertex_buffer_range) == 32);

struct d9mt_wire_rec_bind_index_buffer {
  struct d9mt_wire_rec_hdr hdr;
  struct d9mt_wire_buffer_ref buf;
  uint32_t index_type; /* MtlIndexType */
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_bind_index_buffer) == 48);

struct d9mt_wire_rec_bind_render_targets {
  struct d9mt_wire_rec_hdr hdr;
  struct d9mt_wire_rt_ref color[4];
  struct d9mt_wire_rt_ref depth_stencil;
  uint32_t feedback_loop; /* MtlFeedbackLoopMask */
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_bind_render_targets) == 136);

/* Trailing: count * d9mt_wire_viewport (40 B each). */
struct d9mt_wire_rec_set_viewports {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t count;
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_set_viewports) == 16);

struct d9mt_wire_rec_set_primitive_topology {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t topology; /* MtlPrimitiveTopology */
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_set_primitive_topology) == 16);

/* Trailing: n_attr * d9mt_wire_vertex_attr, then n_layout *
 * d9mt_wire_vertex_layout (both 16 B => already 8-aligned). */
struct d9mt_wire_rec_set_vertex_layout {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t n_attr;
  uint32_t n_layout;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_set_vertex_layout) == 16);

struct d9mt_wire_rec_set_blend_mode {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t rt;
  uint32_t enable; /* bool */
  uint32_t color_src;
  uint32_t color_dst;
  uint32_t color_op;
  uint32_t alpha_src;
  uint32_t alpha_dst;
  uint32_t alpha_op;
  uint32_t write_mask;
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_set_blend_mode) == 48);

struct d9mt_wire_rec_set_rasterizer_state {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t cull_mode;
  uint32_t fill_mode;
  uint32_t front_face;
  uint32_t depth_clip;   /* bool */
  uint32_t flat_shading; /* bool */
  uint32_t sample_count;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_set_rasterizer_state) == 32);

struct d9mt_wire_rec_set_multisample_state {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t sample_count;
  uint32_t sample_mask;
  uint32_t alpha_to_coverage; /* bool */
  uint32_t alpha_to_one;      /* bool */
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_set_multisample_state) == 24);

struct d9mt_wire_rec_set_depth_stencil_state {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t depth_compare_op;
  uint32_t depth_test;     /* bool */
  uint32_t depth_write;    /* bool */
  uint32_t stencil_enable; /* bool */
  struct d9mt_wire_stencil_face front;
  struct d9mt_wire_stencil_face back;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_set_depth_stencil_state) == 72);

struct d9mt_wire_rec_set_stencil_reference {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t reference;
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_set_stencil_reference) == 16);

struct d9mt_wire_rec_set_blend_color {
  struct d9mt_wire_rec_hdr hdr;
  float color[4];
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_set_blend_color) == 24);

struct d9mt_wire_rec_set_depth_bias {
  struct d9mt_wire_rec_hdr hdr;
  float constant_factor;
  float slope_factor;
  float clamp;
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_set_depth_bias) == 24);

/* Trailing: size bytes of push data, padded to 8. */
struct d9mt_wire_rec_push_data {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t stages;
  uint32_t offset;
  uint32_t size;
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_push_data) == 24);

struct d9mt_wire_rec_clear_render_target {
  struct d9mt_wire_rec_hdr hdr;
  struct d9mt_wire_rt_ref rt;
  uint32_t clear_aspects;
  uint32_t discard_aspects;
  struct d9mt_wire_clear_value value;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_clear_render_target) == 64);

/* Trailing: count * d9mt_wire_draw_args (16 B each). */
struct d9mt_wire_rec_draw {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t count;
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_draw) == 16);

/* Trailing: count * d9mt_wire_draw_indexed_args (24 B each). */
struct d9mt_wire_rec_draw_indexed {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t count;
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_draw_indexed) == 16);

/* --- Phase S2 cold records (opcodes 25..32) --------------------------------
 * Emitted lazily on the CS thread at first wire USE of a resource (binds /
 * render-target binds), except destroy_resource which is queued from
 * arbitrary-thread destructors and drained CS-side before the next record —
 * so a create always precedes any reference and a destroy always follows the
 * last one. S2: NO buffer/texture CONTENT crosses (server buffers are
 * zero-filled, textures empty); constants ride push_data payloads. */

/* client_handle = the obj_handle_t of the backing id<MTLBuffer> — the value
 * d9mt_wire_buffer_ref.buffer carries. One record per PHYSICAL backing
 * (Discard renames mint a new backing = a new record).
 * heap_offset: S3 shared-heap placement; always 0 in S2 (reserved). */
struct d9mt_wire_rec_create_buffer {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t wire_id;
  uint64_t client_handle;
  uint64_t size;
  uint32_t usage;      /* MtlUsage bits (informational) */
  uint32_t cpu_access; /* MtlCpuAccess (informational; server allocates private) */
  uint64_t heap_offset;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_create_buffer) == 48);

/* client_handle = obj_handle_t of the id<MTLTexture> (MtlTextureHandle.tex).
 * POD mirror of MtlImageCreateInfo. */
struct d9mt_wire_rec_create_texture {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t wire_id;
  uint64_t client_handle;
  uint32_t type;   /* MtlImageType */
  uint32_t format; /* MtlFormat (== WMTPixelFormat == MTLPixelFormat value) */
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t mip_levels;
  uint32_t array_layers;
  uint32_t sample_count;
  uint32_t usage; /* MtlUsage bits */
  uint32_t cube;  /* bool */
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_create_texture) == 64);

/* client_handle = obj_handle_t of the id<MTLLibrary> — the value
 * d9mt_wire_rec_bind_shader.library carries. metallib byte streams exceed the
 * uint16 record size, so the payload is CHUNKED: each record carries
 * [chunk_offset, chunk_offset+chunk_size) of total_size; the server creates
 * the library once the last byte lands. Trailing payload layout:
 *   chunk_offset == 0: ab_word_count AB words (2 bytes each: u8 kind per
 *     MtlShaderFunction::AbWordKind, u8 reg), padded to 8, THEN chunk_size
 *     metallib bytes;
 *   chunk_offset  > 0: chunk_size metallib bytes only.
 * cbv_slot_map: MtlShaderFunction::cbvSlotMap (-1 = undeclared); for AB
 * shaders (ab_word_count > 0) values are AB word indices. */
struct d9mt_wire_rec_create_shader {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t wire_id;
  uint64_t client_handle;
  uint32_t stage; /* MtlShaderStage */
  uint32_t ab;    /* bool: argument-buffer binding model */
  uint64_t total_size;
  uint64_t chunk_offset;
  uint32_t chunk_size;
  uint32_t ab_word_count;
  uint32_t ab_table_index;
  uint32_t _pad0;
  int8_t cbv_slot_map[8];
  char entry[32]; /* NUL-terminated AIR entry point */
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_create_shader) == 104);

/* client_handle = obj_handle_t of the id<MTLSamplerState> — the value
 * d9mt_wire_rec_bind_sampler.sampler carries. POD mirror of MtlSamplerKey. */
struct d9mt_wire_rec_create_sampler {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t wire_id;
  uint64_t client_handle;
  uint32_t mag_filter;
  uint32_t min_filter;
  uint32_t mip_mode;
  uint32_t address_u;
  uint32_t address_v;
  uint32_t address_w;
  uint32_t max_anisotropy;
  float mip_lod_bias;
  float min_lod;
  float max_lod;
  uint32_t compare_op;
  uint32_t compare_enable;
  uint32_t border_color;
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_create_sampler) == 80);

struct d9mt_wire_rec_destroy_resource {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t wire_id;
  uint64_t client_handle;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_destroy_resource) == 24);

#define D9MT_WIRE_RT_FLAG_BACKBUFFER 0x1u /* server substitutes its IOSurface slot */

/* A render-target attachment named by the IMAGE's wire_id (not the view —
 * views have no wire identity in S2; base_mip/base_layer carry the view's
 * subresource selection). */
struct d9mt_wire_rt_id_ref {
  uint64_t image_wire_id; /* 0 = unbound */
  uint32_t flags;         /* D9MT_WIRE_RT_FLAG_* */
  uint32_t format;        /* view format (MtlFormat) */
  uint32_t width;
  uint32_t height;
  uint32_t sample_count;
  uint32_t base_mip;
  uint32_t base_layer;
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rt_id_ref) == 40);

/* Emitted immediately after every bind_render_targets (op 10) from the same
 * call; the S2 server executes THIS one and counts op 10. */
struct d9mt_wire_rec_set_render_targets_by_id {
  struct d9mt_wire_rec_hdr hdr;
  struct d9mt_wire_rt_id_ref color[4];
  struct d9mt_wire_rt_id_ref depth_stencil;
  uint32_t feedback_loop;
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_set_render_targets_by_id) == 216);

/* Emitted from the present path (synchronizeWsi, once per present).
 * frame_serial is the client's monotonic present counter (1-based); the
 * server closes the frame, commits, rotates its IOSurface slot and publishes
 * {slot, frame_serial} in status v2 (server/d9mt_status.h). */
struct d9mt_wire_rec_present_frame {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t frame_serial;
  uint64_t backbuffer_wire_id; /* 0 = unknown (no present blit source seen yet) */
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_present_frame) == 24);

/* Emitted immediately after every clearRenderTarget record (op 22) from the
 * same call; the S2 server executes THIS one (op 22 carries only the view
 * handle, which has no server identity) and counts op 22. */
struct d9mt_wire_rec_clear_render_target_by_id {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t image_wire_id;
  uint32_t flags; /* D9MT_WIRE_RT_FLAG_* */
  uint32_t clear_aspects;
  uint32_t discard_aspects;
  uint32_t _pad0;
  struct d9mt_wire_clear_value value;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_clear_render_target_by_id) == 56);

/* ---------------------------------------------------------------------------
 * Phase S3: shared heap (docs/RENDER_SERVER_SPEC.md "Shared memory layout"
 * item 2) — the bulk-data channel for texture/buffer CONTENT.
 *
 * A second MAP_SHARED file `heap` in the shm dir:
 *   [ struct d9mt_wire_heap (192 B) ][ pad to data_offset ][ data: capacity ]
 * data_offset is D9MT_WIRE_HEAP_DATA_OFFSET (16 KiB) so the data area is
 * host-page-aligned: the server wraps the WHOLE mapping once via
 * newBufferWithBytesNoCopy and blits regions straight out of it (zero CPU
 * copy); region offsets stay 256-aligned relative to the data area.
 *
 * PRODUCER (client, CS thread): sequential bump allocator. `head` is a
 * FREE-RUNNING byte cursor; a region's data lives at (offset & (capacity-1))
 * and NEVER straddles the wrap (the allocator skips to the next capacity
 * boundary instead — the skipped bytes are reclaimed when a later region's
 * generation is acked). Every region is tagged with a GENERATION = the
 * free-running END cursor of the region (monotonic).
 *
 * CONSUMER (server): after the GPU submission that read a region COMPLETES,
 * the server release-stores the region's generation into `consumed` (also
 * mirrored into status v3 heap_consumed for observability). The producer's
 * space check is head - consumed <= capacity - need: acking generation G
 * frees every byte of every region allocated at or before G.
 *
 * FULL HEAP NEVER STALLS: the producer drops the upload (dropped++) and the
 * server keeps its previous content for that resource — visual staleness on
 * the server side only, the client's local execution is untouched. */

#define D9MT_WIRE_HEAP_MAGIC 0x30315048544D3944ull /* "D9MTHP10" LE */
#define D9MT_WIRE_HEAP_DATA_OFFSET 16384u

struct d9mt_wire_heap {
  /* line 0: immutable after attach */
  uint64_t magic;       /* D9MT_WIRE_HEAP_MAGIC */
  uint64_t capacity;    /* data bytes, power of two */
  uint64_t data_offset; /* D9MT_WIRE_HEAP_DATA_OFFSET */
  uint64_t _pad0[5];
  /* line 1: producer-written */
  d9mt_wire_au64 head;    /* free-running region end cursor (release) */
  d9mt_wire_au64 dropped; /* uploads dropped because the heap was full */
  uint64_t _pad1[6];
  /* line 2: consumer-written */
  d9mt_wire_au64 consumed; /* generation ack: every region with generation <=
                            * consumed is GPU-complete and reusable */
  uint64_t _pad2[7];
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_heap) == 192);

/* --- Phase S3 records (opcodes 33..35) --------------------------------------
 * Content records are emitted on the CS thread AFTER the resource's create_*
 * record (ensure* order holds) and BEFORE any draw that consumes the bytes,
 * so executing them in stream order gives the server the same content
 * timeline the client executes locally (dual-execution stays additive). */

/* Buffer content: `size` bytes for [dst_offset, dst_offset+size) of the
 * backing identified by client_handle/wire_id (same identity rules as
 * create_buffer). heap_offset is the region's free-running START cursor. */
struct d9mt_wire_rec_upload_buffer {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t wire_id;
  uint64_t client_handle;
  uint64_t dst_offset;
  uint64_t size;
  uint64_t heap_offset;
  uint64_t heap_generation;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_upload_buffer) == 56);

/* Texture content for ONE subresource box: linear rows at row_pitch/
 * slice_pitch (Metal bytesPerRow/bytesPerImage semantics; slice_pitch is 0
 * for non-3D extents, matching the client's copyBufferToTexture encode). */
struct d9mt_wire_rec_upload_texture {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t wire_id;
  uint64_t client_handle;
  uint32_t mip;
  uint32_t layer;
  uint32_t x, y, z;
  uint32_t width, height, depth;
  uint32_t row_pitch;
  uint32_t slice_pitch;
  uint32_t format; /* MtlFormat of the copy (view of the staging bytes) */
  uint32_t _pad0;
  uint64_t heap_offset;
  uint64_t heap_generation;
  uint64_t data_size;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_upload_texture) == 96);

/* Emitted immediately after every bind_texture (op 3) from the same call:
 * names the bound view's IMAGE by wire id + the view key bits, so the server
 * can resolve a REAL texture instead of the S2 dummy. image_wire_id == 0
 * means unbind (slot reverts to the dummy). */
struct d9mt_wire_rec_bind_texture_by_id {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t stages; /* MtlStageMask */
  uint32_t slot;
  uint64_t image_wire_id;
  uint32_t format; /* view format (MtlFormat) */
  uint32_t view_type;
  uint32_t base_mip;
  uint32_t mip_count;
  uint32_t base_layer;
  uint32_t layer_count;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_bind_texture_by_id) == 48);

/* Session boundary (S3 v5). kind 0 = QUIESCE (the claiming context died:
 * drain, retire every table resource GPU-safely, reset folded state, keep
 * running); kind 1 = BEGIN (a new context connected to the live server:
 * same reset, marks the boundary). Both ack by incrementing status epoch
 * (release) — epoch is 1 at boot-ready and +1 per session record executed.
 * Stale wire ids / client handles from an earlier session MUST fail to
 * resolve afterwards (dummy substitution), never alias. */
struct d9mt_wire_rec_session {
  struct d9mt_wire_rec_hdr hdr;
  uint32_t kind;    /* 0 = quiesce, 1 = begin */
  uint32_t session; /* client-side session counter (diagnostics) */
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_session) == 16);

/* --- Phase S3 v9 records (opcodes 37..42) ---------------------------------- */

/* One subresource box copy (mirror of the local single-slice encode). */
struct d9mt_wire_rec_copy_texture {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t dst_wire_id, dst_handle;
  uint64_t src_wire_id, src_handle;
  uint32_t dst_mip, dst_layer, src_mip, src_layer;
  uint32_t dst_x, dst_y, dst_z;
  uint32_t src_x, src_y, src_z;
  uint32_t width, height, depth;
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_copy_texture) == 96);

/* GPU buffer-to-buffer copy (device-local; CPU-visible content rides
 * upload_buffer instead). */
struct d9mt_wire_rec_copy_buffer {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t dst_wire_id, dst_handle;
  uint64_t src_wire_id, src_handle;
  uint64_t dst_offset, src_offset, bytes;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_copy_buffer) == 64);

/* The meta-blit (sampled stretched rect). The CLIENT already normalized the
 * rects exactly like its local path: dst rect is the final viewport/scissor
 * (dx0<dx1, dy0<dy1, clamped to the dst mip), uv[4] = {u0,v0,u1,v1} in
 * source-normalized coordinates (reversed source = uv0>uv1 mirrors). */
struct d9mt_wire_rec_blit_texture_view {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t dst_wire_id, dst_handle;
  uint64_t src_wire_id, src_handle;
  uint32_t dst_format, dst_mip, dst_layer;
  uint32_t src_format, src_mip, src_layer;
  int32_t dst_x0, dst_y0, dst_x1, dst_y1;
  float uv[4];
  uint32_t filter; /* MtlSamplerFilter: 1 = Linear, else Nearest */
  uint32_t _pad0;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_blit_texture_view) == 104);

/* MSAA resolve: full-subresource, color aspect (the local path's shape —
 * store-action MultisampleResolve, no shader). */
struct d9mt_wire_rec_resolve_texture {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t dst_wire_id, dst_handle;
  uint64_t src_wire_id, src_handle;
  uint32_t src_mip, src_layer, dst_mip, dst_layer;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_resolve_texture) == 56);

struct d9mt_wire_rec_generate_mipmaps {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t wire_id, client_handle; /* the IMAGE (mipgen covers the chain) */
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_generate_mipmaps) == 24);

/* ColorFill / partial clear through a view. The server executes the
 * FULL-subresource color shape (load-action mini-pass); partial rects and
 * depth-stencil aspects are dropped server-side (logged once) — they need
 * the fill/depth-clear draw pipelines, a later slice. */
struct d9mt_wire_rec_clear_image_view {
  struct d9mt_wire_rec_hdr hdr;
  uint64_t wire_id, client_handle;
  uint32_t format, mip, layer;
  uint32_t x, y, width, height;
  uint32_t aspects;
  struct d9mt_wire_clear_value value;
};
D9MT_WIRE_ASSERT(sizeof(struct d9mt_wire_rec_clear_image_view) == 80);

#endif /* D9MT_MTL_WIRE_H */
