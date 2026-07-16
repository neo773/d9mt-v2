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
 *   - mingw x86_64 C++17 (driver, PE producer)
 *   - clang arm64/x86_64 ObjC/C11 (bridge unix, consumer)
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
 * RING (single producer = PE CS thread, single consumer = unix wire thread)
 *
 * Memory is ONE PE allocation handed across via WireRing_attach(base, bytes):
 *   [ struct d9mt_wire_ring (192 B control block) ][ data: capacity bytes ]
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
 * KICK / PARK PROTOCOL (no lost wakeups):
 *   - The consumer parks on a pthread condvar. Before parking it takes the
 *     wire mutex and RE-CHECKS head != tail (and !shutdown) under the lock.
 *   - The producer calls WireRing_kick() — which takes the same mutex and
 *     signals — ONLY on the empty->nonempty transition: at record-write
 *     time it sampled tail == previous head (consumer had drained
 *     everything). Any record published after the consumer's locked
 *     re-check therefore came with a kick; any record published before is
 *     seen by the re-check. Over-kicking (consumer still draining) is
 *     harmless.
 *   - SHUTDOWN: PE sets ring->shutdown = 1 (release) and kicks; the
 *     consumer drains what remains, publishes tail, sets consumer_exited=1
 *     (release) and returns. The PE side waits (bounded) for
 *     consumer_exited before freeing the ring memory, and leaks the ring
 *     on timeout rather than freeing under a live consumer.
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

  d9mt_wire_op_count = 25
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

#endif /* D9MT_MTL_WIRE_H */
