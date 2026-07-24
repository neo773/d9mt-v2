/*
 * executor.mm — d9mt-serverd Phase S2/S3 record executor.
 *
 * A native, DIRECT-ObjC METAL 4 ONLY (docs/SOURCE_OF_TRUTH.md §2) port of the
 * driver backend's hot-loop state machine (driver/backend/mtl_context_impl.h
 * decisions 1-3, 5, 9; reference bodies: mtl_context_record.cpp +
 * mtl_context_transfer.cpp; the WMT bridge call bodies in
 * bridge/unix/winemetal_unix.c are the ObjC translations). Simple
 * immediate-mode shape: setRenderTargetsById closes the pass, draws encode
 * into an MTL4RenderCommandEncoder materialized lazily at the first
 * draw/clear.
 *
 * S2 scope (dual-execution validation — the client still renders locally):
 *  - resource table: client obj_handle_t / wire_id -> server-side
 *    MTLBuffer/MTLTexture/MTLSamplerState/shader library (create_* records).
 *  - NO buffer/texture CONTENT crosses: buffers are zero-filled shared
 *    allocations (safe: garbage-free vertex/index fetch), textures empty.
 *    Push-constant data DOES cross (push_data payload) and uploads into a
 *    server-side ring.
 *  - texture BINDS (bind_texture references client VIEW handles, which have
 *    no wire identity in S2) resolve to a 1x1 dummy so sampling is defined.
 *  - the client's swapchain backbuffer (D9MT_WIRE_RT_FLAG_BACKBUFFER)
 *    substitutes the IOSurface texture of the current slot; presentFrame
 *    commits the frame, publishes {slot, frame serial} in status v2 and
 *    rotates the slot.
 *  - errors NEVER kill the stream: they count into status v2
 *    exec_errors/draws_skipped and the affected work is dropped.
 *
 * Phase S3 delta:
 *  - ASYNC PSO builds: MTL4Compiler completion-handler tasks. The consumer
 *    thread NEVER blocks on a compile — draws whose PSO is still in flight
 *    are skipped and counted (status v3 draws_skipped_pending_pso) until the
 *    handler lands the pipeline. This is what keeps ring consumption flowing
 *    through a boot-time shader storm (the S2 sync builds stalled the ring
 *    until the client's watchdog killed the transport).
 *  - CONTENT crosses via the shared heap (mtl_wire.h d9mt_wire_heap):
 *    upload_buffer/upload_texture records name heap regions; the server
 *    memcpys the region into its upload slab ring (native arm64 copy) and
 *    blits from the slab inside an MTL4ComputeCommandEncoder that closes
 *    before the next render pass. NOTE a zero-copy design
 *    (newBufferWithBytesNoCopy over the whole heap mapping) was tried and
 *    is UNSOUND: Metal wires the pages at wrap time, when the sparse file
 *    still resolves to zero-fill pages — client writes that fault in fresh
 *    pagecache pages later are never seen by the GPU mapping (verified:
 *    CPU-visible bytes, GPU blits zeros). The slab copy also frees the heap
 *    region IMMEDIATELY: the ack generation (heap->consumed + status v3
 *    heap_consumed) advances at record execution, no GPU tracking needed.
 *  - texture binds resolve REAL textures: bind_texture_by_id names the image
 *    by wire id; the dummy substitution remains only for unresolved ids and
 *    content-less textures (undefined private storage stays unsampled).
 *  - frame publication is GPU-completion-gated: {present_slot, frame_serial,
 *    frames} are stored from an MTLSharedEventListener block when the
 *    frame's serial signals — status frame_serial now means "pixels done".
 *
 * Threading: single-threaded, called only from the consumer thread (which
 * runs at QOS_CLASS_USER_INTERACTIVE — set in main.m); PSO completion
 * handlers and the frame-completion listener touch ONLY their captured
 * entry/status atomics.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Metal/MTL4CommandQueue.h>
#import <Metal/MTL4CommandAllocator.h>
#import <Metal/MTL4CommandBuffer.h>
#import <Metal/MTL4ArgumentTable.h>
#import <Metal/MTL4RenderCommandEncoder.h>
#import <Metal/MTL4ComputeCommandEncoder.h>
#import <Metal/MTL4RenderPass.h>
#import <Metal/MTL4Compiler.h>
#import <Metal/MTL4RenderPipeline.h>
#import <Metal/MTL4PipelineState.h>
#import <Metal/MTL4LibraryFunctionDescriptor.h>
#import <Metal/MTLResidencySet.h>

#include <mach/mach_time.h>
#include <pthread/qos.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "d9mt_server.h"
#include "../driver/mtl/mtl_futex.h"

/* the shared C/C++ headers use std::atomic under C++; the C11-style
 * atomic_*_explicit calls resolve via ADL but the orders need importing */
using std::memory_order_acquire;
using std::memory_order_relaxed;
using std::memory_order_release;

namespace {

/* Argument-table slot map — MUST mirror the client's
 * (mtl_context_impl.h): CBVs at shader-derived AIR slots (0..2), vertex
 * streams at kVertexBufferBaseIndex + stream, push data at
 * kPushConstantIndex, AB handle words at the shader's abTableIndex. */
constexpr uint32_t kVertexBufferBaseIndex = 3u;
constexpr uint32_t kPushConstantIndex = 20u;
constexpr uint32_t kMaxStreams = 16u;
constexpr uint32_t kMaxTexSlots = 16u;   /* classic argument-TABLE capacity */
/* S3 v10: STATE tracking is wider than the table — FF-shaped shaders bind
 * texture/sampler registers up to 80+ (5 type-arrays x 16) through the AB
 * word path; the old 16-slot arrays silently dummy'd every bind >= 16
 * (black FF composition, the menu-screen class). uint8 reg => 256 covers
 * the whole space. */
constexpr uint32_t kMaxTexTrack = 256u;
constexpr uint32_t kFanMaxVerts = 16386u;
constexpr uint32_t kUploadSlabBytes = 2u << 20;

/* Bridge-numeric MTL4 stage/visibility values (winemetal.h WMTStage*): the
 * bridge casts these straight into MTLStages/MTL4VisibilityOptions, so the
 * numbers ARE the SDK values. */
constexpr uint64_t kStageFragment = 1ull << 1;
constexpr uint64_t kStageDispatch = 1ull << 27;
constexpr uint64_t kStageBlit = 1ull << 28;
constexpr uint64_t kStageAll = 0x7fffffffffffffffull;
constexpr uint64_t kVisibilityDevice = 1ull << 0;

/* MtlFormat / WMTPixelFormat -> MTLPixelFormat: values are identical modulo
 * the driver's custom-swizzle marker bits (all >= 0x20000; real formats are
 * < 0x10000) — mirror of ORIGINAL_FORMAT() in winemetal_unix.c. */
static inline MTLPixelFormat pixelFormat(uint32_t f) {
  return (MTLPixelFormat)(f & 0xFFFFu);
}

/* Port of attributeFormat() (mtl_context_transfer.cpp): MtlFormat values ->
 * MTLVertexFormat. Default Float4 (loud-but-safe, matches client). */
static MTLVertexFormat vertexFormat(uint32_t f) {
  switch (f) {
    case 55:  return MTLVertexFormatFloat;                  /* R32Float */
    case 105: return MTLVertexFormatFloat2;                 /* RG32Float */
    case 900: return MTLVertexFormatFloat3;                 /* RGB32Float (marker) */
    case 125: return MTLVertexFormatFloat4;                 /* RGBA32Float */
    case 70:  return MTLVertexFormatUChar4Normalized;       /* RGBA8Unorm */
    case 80:  return MTLVertexFormatUChar4Normalized_BGRA;  /* BGRA8Unorm */
    case 72:  return MTLVertexFormatChar4Normalized;        /* RGBA8Snorm */
    case 65:  return MTLVertexFormatHalf2;                  /* RG16Float */
    case 115: return MTLVertexFormatHalf4;                  /* RGBA16Float */
    case 25:  return MTLVertexFormatHalf;                   /* R16Float */
    case 112: return MTLVertexFormatShort4Normalized;       /* RGBA16Snorm */
    case 110: return MTLVertexFormatUShort4Normalized;      /* RGBA16Unorm */
    case 62:  return MTLVertexFormatShort2Normalized;       /* RG16Snorm */
    case 60:  return MTLVertexFormatUShort2Normalized;      /* RG16Unorm */
    case 90:  return MTLVertexFormatUInt1010102Normalized;  /* RGB10A2Unorm */
    default:  return MTLVertexFormatFloat4;
  }
}

/* MtlPrimitiveTopology (wire values) -> MTLPrimitiveType; TriangleFan (5) is
 * emulated via the shared fan index pattern (port of ensureFanIndices). */
static MTLPrimitiveType primitiveType(uint32_t topo) {
  switch (topo) {
    case 0:  return MTLPrimitiveTypePoint;
    case 1:  return MTLPrimitiveTypeLine;
    case 2:  return MTLPrimitiveTypeLineStrip;
    case 4:  return MTLPrimitiveTypeTriangleStrip;
    default: return MTLPrimitiveTypeTriangle;
  }
}

/* D3D9 write mask (R=1,G=2,B=4,A=8) -> MTLColorWriteMask (A=1,B=2,G=4,R=8) —
 * port of wmtWriteMask() in mtl_pso_cache.cpp. */
static MTLColorWriteMask writeMask(uint32_t v) {
  MTLColorWriteMask m = MTLColorWriteMaskNone;
  if (v & 0x1u) m |= MTLColorWriteMaskRed;
  if (v & 0x2u) m |= MTLColorWriteMaskGreen;
  if (v & 0x4u) m |= MTLColorWriteMaskBlue;
  if (v & 0x8u) m |= MTLColorWriteMaskAlpha;
  return m;
}

/* --- resources -------------------------------------------------------------- */

struct Shader {
  id<MTLLibrary> library = nil;
  std::string entry = "main";
  uint32_t stage = 0;
  bool ab = false;
  int8_t cbvMap[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  uint32_t abTableIndex = 21u;
  std::vector<std::pair<uint8_t, uint8_t>> abWords; /* {kind, reg} */
};

/* Texture-view cache key (bind_texture_by_id view bits). */
struct TexViewKey {
  uint32_t format = 0, viewType = 0;
  uint32_t baseMip = 0, mipCount = 0, baseLayer = 0, layerCount = 0;
  bool operator==(const TexViewKey& o) const {
    return std::memcmp(this, &o, sizeof(o)) == 0;
  }
};

struct Resource {
  enum Kind { KindBuffer, KindTexture, KindSampler, KindShader } kind;
  uint64_t wireId = 0;
  uint64_t clientHandle = 0;
  id<MTLBuffer> buffer = nil;
  id<MTLTexture> texture = nil;
  id<MTLSamplerState> sampler = nil;
  std::shared_ptr<Shader> shader;
  /* S3 v2: async create handshake (shader library creation runs on the
   * create worker so the consumer never blocks in newLibraryWithData).
   * Consumer acquire-loads before touching `shader`; the worker writes the
   * payload THEN release-stores. Synchronously-created resources are born
   * Ready. 0 = building, 1 = ready, 2 = failed. */
  std::atomic<uint32_t> ready = { 1 };
  /* S3: true once the server holds REAL bytes (heap upload landed) or the
   * texture was rendered to server-side. Content-less private textures stay
   * dummy-bound — their storage is undefined. */
  bool hasContent = false;
  /* S3: format/subresource views minted for bind_texture_by_id (small; a
   * texture rarely has more than a couple of live view keys). */
  std::vector<std::pair<TexViewKey, id<MTLTexture>>> views;
  /* S3 v7 (texture creation moved to the worker): consumer-only fields.
   * Uploads arriving while the texture is still building stash their bytes
   * here (heap region acked immediately) and drain when the consumer first
   * observes ready==1; residency is added at the same observation point
   * (MTLResidencySet mutation stays on the consumer thread). */
  struct StashedUpload {
    d9mt_wire_rec_upload_texture rec;
    std::vector<uint8_t> bytes;
  };
  std::vector<StashedUpload> stash;
  bool residencyAdded = false;
  /* S3 v13 parity: fnv64 of the LAST upload_buffer bytes consumed for this
   * resource — compared against the client's sum of the bytes it shipped
   * (wire-draw#N `up=` fields). */
  uint64_t uploadSum = 0;
  /* S3 v14: fnv64 of the FIRST <=64 bytes of the last upload_buffer region and
   * its dst_offset. logServerDraw's constant window sum used to read
   * buffer.contents, which for a per-frame ROTATING ring offset LAGS the xfer
   * blit (blits run at submitFrame; the log runs at record) and so reported a
   * FROZEN zero-fill hash even though the draw reads correct bytes — the false
   * "server constsum frozen" that must never mislead again. The window sum now
   * reads these shipped bytes when the bound offset matches. */
  uint64_t lastUploadWindow = 0;
  uint64_t lastUploadOffset = ~0ull;
  /* V15 upload-flood fix: CPU-staging discipline for shared-storage buffers
   * (mirror of the client's UploadRing rule: memory is never rewritten under
   * an encoded-but-incomplete draw). refMin/refMax summarize the byte ranges
   * of this buffer referenced by GPU work encoded in command buffers that
   * have NOT yet completed; refSerial is the latest such serial (watermark is
   * dead once completedSerial() >= refSerial). D3D9 constant rings advance
   * offsets monotonically per draw and rename on wrap, so a fresh upload
   * almost never overlaps the live watermark — those uploads memcpy straight
   * into the shared MTLBuffer contents: NO encoder, NO render-pass close. */
  uint64_t refMin = ~0ull, refMax = 0;
  uint64_t refSerial = 0;
  /* Overlapping/non-shared uploads stage into the slab immediately (heap ack
   * flows) and their slab->buffer blit is DEFERRED to the next natural pass
   * boundary; a draw that binds a buffer with pending blits promotes them to
   * a direct memcpy when the range has retired (FLOW-CONTROL) or forces the
   * flush (counted — the old always-close behavior, now the rare path). */
  uint32_t pendingXfers = 0;
  uint64_t xferMin = ~0ull, xferMax = 0;
  /* FLOW-CONTROL: texture-content blits parked for the next natural pass
   * boundary (upload_texture arriving inside an open render pass no longer
   * closes it); a draw that samples this texture before the boundary forces
   * the flush (counted, uplFlushUse). */
  uint32_t pendingTexXfers = 0;
};

/* S3 v13 checksum basis for the per-draw input parity logs — identical
 * constant/loop on the client side (mtl_wire_context.cpp fnv64). */
static uint64_t fnv64(const void* data, size_t n,
                      uint64_t h = 0xcbf29ce484222325ull) {
  const uint8_t* p = (const uint8_t*)data;
  for (size_t i = 0; i < n; i++)
    h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

/* v13.1 copy_buffer content-sum propagation — IDENTICAL mix on the client
 * (mtl_wire_context.cpp mixCopySum): wire-draw `up=` parity covers the
 * staging + copy_buffer transport (static VB/IB), not just upload_buffer. */
static uint64_t mixCopySum(uint64_t srcSum, uint64_t srcOff, uint64_t dstOff,
                           uint64_t bytes) {
  uint64_t h = 0xcbf29ce484222325ull;
  h = fnv64(&srcSum, 8, h);
  h = fnv64(&srcOff, 8, h);
  h = fnv64(&dstOff, 8, h);
  h = fnv64(&bytes, 8, h);
  return h;
}

struct PendingShader {
  struct d9mt_wire_rec_create_shader meta = {};
  std::vector<std::pair<uint8_t, uint8_t>> abWords;
  std::vector<uint8_t> bytes;
  uint64_t received = 0;
};

/* --- PSO key (port of RenderPsoKey, memcmp identity) ------------------------ */

struct PsoKey {
  const void* vsLib;    /* library pointer identity (device-lifetime) */
  const void* fsLib;
  uint64_t entryHash;   /* entry names folded (one library can hold several) */
  struct { uint32_t format, offset, bufferIndex; } attrs[20];
  struct { uint32_t stride, stepFunction, bufferIndex; } layouts[20];
  uint32_t attrCount, layoutCount;
  struct { uint32_t fmt, blend, wmask, csrc, cdst, cop, asrc, adst, aop; } colors[4];
  uint32_t colorCount;
  uint32_t sampleCount, a2c, pad0, pad1;
};

struct PsoKeyHash {
  size_t operator()(const PsoKey& k) const {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof(PsoKey); i++)
      h = (h ^ p[i]) * 0x100000001b3ull;
    return size_t(h);
  }
};
struct PsoKeyEq {
  bool operator()(const PsoKey& a, const PsoKey& b) const {
    return std::memcmp(&a, &b, sizeof(PsoKey)) == 0;
  }
};

/* Async PSO cache entry (S3). The consumer thread creates it in state
 * Building and kicks an MTL4Compiler completion-handler task; the handler
 * (compiler thread) writes `pso` THEN release-stores the state. The consumer
 * acquire-loads state before touching pso. Failed builds park at Failed
 * forever (no retry storm — same policy as the S2 nil cache). */
struct PsoEntry {
  id<MTLRenderPipelineState> pso = nil;
  std::atomic<uint32_t> state = { 0 }; /* 0 building, 1 ready, 2 failed */
};

/* --- folded state (mirrors MtlContextImpl's fold members) ------------------- */

struct Stream {
  id<MTLBuffer> buf = nil;
  uint64_t offset = 0, length = 0;
  uint32_t stride = 0;
  bool bound = false;
  std::shared_ptr<Resource> res; /* S3 v13: parity logging (wireId/uploadSum) */
};

struct CbvBinding {
  id<MTLBuffer> buf = nil;
  uint64_t offset = 0;
  uint64_t length = 0; /* V15: bound range length (draw-time ref marking) */
  bool bound = false;
  std::shared_ptr<Resource> res; /* S3 v13: parity logging (wireId/uploadSum) */
};

struct StageState {
  CbvBinding cbvs[8];
  bool texBound[kMaxTexTrack] = {};
  /* S3: resolved REAL texture (or view) per slot; nil = dummy-bound. */
  id<MTLTexture> textures[kMaxTexTrack] = {};
  /* FLOW-CONTROL: Resource backref per by-id-bound slot (the V15 honest
   * flag) — a draw sampling a texture with PARKED content blits
   * (pendingTexXfers) must force the flush; only scanned while parked
   * blits exist. */
  std::shared_ptr<Resource> texRes[kMaxTexTrack];
  /* V18 bind-once promotion: a bind that resolved to the dummy because the
   * texture was still building (or its content still in flight) keeps its
   * record here; draws re-resolve the slot once the resource turns
   * ready+content. Without this, an app that binds a texture ONCE and
   * never rebinds samples the dummy FOREVER (permanent black) whenever the
   * bind raced the async create/upload — "rebound next draw" only heals
   * apps that actually rebind. */
  d9mt_wire_rec_bind_texture_by_id texPendRec[kMaxTexTrack];
  bool texPendSlot[kMaxTexTrack] = {};
  uint32_t texPendCount = 0;
  id<MTLSamplerState> samplers[kMaxTexTrack] = {};
  uint8_t pushData[4096] = {};
  uint32_t pushExtent = 0;
  bool pushDirty = false;
  uint64_t pushGpuAddr = 0;
  /* V16: submission serial the pushGpuAddr slice was allocated under — a
   * slice from an OLDER serial lives in slab memory that ringAlloc rewinds
   * once that serial completes, while draws encoded NOW would still read it
   * through the argument-table snapshot. See flushBindings. */
  uint64_t pushSerial = 0;
};

struct RtAttachment {
  id<MTLTexture> tex = nil;
  MTLPixelFormat format = MTLPixelFormatInvalid;
  uint32_t baseMip = 0, baseLayer = 0;
  uint64_t wireId = 0;
  bool backbuffer = false;
  bool named = false; /* record referenced SOMETHING here (even if unresolved) */
};

struct Pass {
  RtAttachment color[4], ds;
  bool pending = false, active = false;
  bool clearColor[4] = {};
  MTLClearColor clearValues[4] = {};
  bool clearDepth = false, clearStencil = false;
  double depthClear = 1.0;
  uint32_t stencilClear = 0;
  id<MTL4RenderCommandEncoder> encoder = nil;
};

struct ArgTablePair {
  id<MTL4ArgumentTable> table[2] = {nil, nil};
  uint64_t serial = 0;
};

struct UploadSlab {
  id<MTLBuffer> buf = nil;
  uint64_t used = 0;
  uint64_t serial = 0;
};

/* V15: one slab->buffer blit parked for the next natural pass boundary
 * (materializePass / submitFrame / on-demand at a consuming draw). The slab
 * bytes are already copied (heap region acked); only the encode is deferred. */
struct DeferredXfer {
  std::shared_ptr<Resource> res;
  id<MTLBuffer> srcBuf = nil;
  uint64_t srcOff = 0, dstOff = 0, size = 0;
};

/* FLOW-CONTROL: one slab->texture blit parked for the next natural pass
 * boundary (every xfer-encoder open drains these — see ensureXfer). Same
 * discipline as DeferredXfer: bytes already in the slab, heap region acked,
 * only the encode is deferred, so a mid-pass texture stream no longer
 * shatters the pass. */
struct DeferredTexXfer {
  std::shared_ptr<Resource> res;
  d9mt_wire_rec_upload_texture rec;
  id<MTLBuffer> srcBuf = nil;
  uint64_t srcOff = 0;
};

struct Exec {
  struct d9mt_status* status = nullptr;

  id<MTLDevice> device = nil;
  id<MTL4CommandQueue> queue = nil;
  id<MTL4Compiler> compiler = nil;
  id<MTLSharedEvent> event = nil; /* completion timeline (decision 5 port) */
  MTLSharedEventListener* listener = nil; /* S3: completion-gated publication */
  uint64_t serial = 0;            /* serial of the CURRENT recording cmdbuf */
  uint64_t lastSubmitted = 0;

  /* S3 shared heap (mtl_wire.h d9mt_wire_heap): server-side mapping; upload
   * records memcpy regions into the slab ring and blit from there (see the
   * header comment for why zero-copy wrapping is unsound). */
  struct d9mt_wire_heap* heap = nullptr;

  /* S3 transfer encoder: open between passes for heap blits, closed (with a
   * producer barrier) before the next render pass / submit. */
  id<MTL4ComputeCommandEncoder> xfer = nil;

  id<MTL4CommandAllocator> allocators[3] = {};
  uint64_t allocSerial[3] = {};
  uint32_t frameIndex = 0;
  id<MTL4CommandBuffer> cmdbuf = nil;

  id<MTLResidencySet> residency = nil;
  bool residencyDirty = false;

  /* resource tables */
  std::unordered_map<uint64_t, std::shared_ptr<Resource>> byHandle;
  std::unordered_map<uint64_t, std::shared_ptr<Resource>> byWire;
  std::unordered_map<uint64_t, PendingShader> pendingShaders; /* by client_handle */
  /* GPU-lifetime keepalive for destroyed resources (deferred destruction:
   * the GPU may still read a buffer whose destroy record already arrived). */
  std::vector<std::pair<uint64_t, id>> retire;
  /* same, for objects that never joined the residency set (texture views —
   * their parent's allocation is the resident one). */
  std::vector<std::pair<uint64_t, id>> retireNoRes;

  /* dummies: defined sampling/fetch for unresolvable binds */
  id<MTLBuffer> dummyBuffer = nil;   /* 64 KiB zeroed */
  id<MTLTexture> dummyTexture = nil; /* 1x1 black */
  id<MTLSamplerState> dummySampler = nil;
  id<MTLBuffer> fanIndices = nil;

  /* S3 v2: create worker — shader library creation (newLibraryWithData,
   * milliseconds each; GTA IV boots with hundreds) off the consumer thread.
   * The consumer only decodes + dispatches + memcpys; jobs land here and
   * the worker publishes into Resource::ready/shader. */
  std::thread creatWorker;
  std::mutex creatMutex;
  std::condition_variable creatCv;
  struct CreateJob {
    enum Kind { KindShaderLib, KindTexture, KindSampler } kind = KindShaderLib;
    std::shared_ptr<Resource> res;
    std::vector<uint8_t> bytes;                    /* shader metallib */
    d9mt_wire_rec_create_shader meta;              /* shader */
    std::vector<std::pair<uint8_t, uint8_t>> abWords;
    d9mt_wire_rec_create_texture tex = {};         /* texture (v7) */
    d9mt_wire_rec_create_sampler smp = {};         /* sampler (v7) */
  };
  std::deque<CreateJob> creatQueue;
  bool creatStop = false;

  /* S3 v8 watchdog: pure diagnostics, no unblocking — names a wedged worker
   * job (an ObjC call that never returned) and a stuck consumer record.
   * workerJobDesc is written by the worker BEFORE the release-store of
   * workerJobStartMs and only overwritten after clearing it. */
  std::thread watchdog;
  std::atomic<bool> watchdogStop = { false };
  std::atomic<uint64_t> workerJobStartMs = { 0 }; /* 0 = idle */
  char workerJobDesc[160] = {};
  uint32_t wedgeTestMs = 0; /* D9MT_WEDGE_CREATE_MS — TEST HOOK ONLY */

  /* S3 v7: worker-built resources awaiting consumer-side observation
   * (residency add + stash drain). Consumer-only; swept at present and at
   * every reference point. */
  std::vector<std::shared_ptr<Resource>> asyncCreates;

  /* folded state */
  std::shared_ptr<Shader> vs, fs;
  /* S3 v2: last-bound shader whose library is still building; promoted into
   * vs/fs at draw time once ready (draws skip + count until then). */
  std::shared_ptr<Resource> vsPend, fsPend;
  std::vector<d9mt_wire_vertex_attr> attrs;
  std::vector<d9mt_wire_vertex_layout> layouts;
  uint32_t topology = 3; /* TriangleList */
  d9mt_wire_rec_set_blend_mode blend[4] = {};
  d9mt_wire_rec_set_multisample_state msaa = {};
  d9mt_wire_rec_set_rasterizer_state raster = {};
  d9mt_wire_rec_set_depth_stencil_state depthStencil = {};
  d9mt_wire_viewport viewport = {};
  bool viewportSet = false;
  float blendColor[4] = {};
  uint32_t stencilRef = 0;
  float depthBias[3] = {};
  StageState stages[2];
  Stream streams[kMaxStreams];
  struct {
    id<MTLBuffer> buf = nil;
    uint64_t offset = 0, length = 0;
    uint32_t type = 0; /* 0=u16 1=u32 */
    bool bound = false;
    std::shared_ptr<Resource> res; /* S3 v13: parity logging */
  } index;

  /* S3 v13: first-4-executed-draws input parity log counter */
  uint32_t dbgDrawLog = 0;

  Pass pass;
  bool dirtyPso = true, dirtyState = true, dirtyBindings = true;

  /* per-encoder trim shadows (port of m_lastEncodedPso / m_dynValid) */
  id<MTLRenderPipelineState> lastPso = nil;
  bool dynValid = false;
  d9mt_wire_viewport lastViewport = {};
  id<MTLDepthStencilState> lastDsso = nil;
  uint32_t lastStencilRef = 0;
  float lastBlendColor[4] = {};

  /* caches */
  std::unordered_map<PsoKey, std::shared_ptr<PsoEntry>, PsoKeyHash, PsoKeyEq> psoCache;
  std::unordered_map<std::string, id<MTLDepthStencilState>> dssoCache;

  /* argument tables (retire-by-serial ring, rotated once per pass) */
  std::vector<ArgTablePair> argTables;
  uint32_t argCursor = 0;
  uint32_t currentArgTable = 0;
  bool argTablesBound = false;

  /* upload ring (push data / AB words) */
  std::vector<UploadSlab> slabs;
  size_t slabCursor = 0;

  uint32_t presentSlot = 0;
  uint64_t backbufferWireId = 0;
  /* persistent-backbuffer present model: ONE render texture substitutes for
   * every backbuffer-routed bind — content survives presents (the client's
   * BLIT/COPY contract, d3d9_swapchain.cpp); presentFrame COPIES it into the
   * rotating pool slot at publish. */
  id<MTLTexture> persistentBB = nil;

  /* S3 v11 skip-reason buckets + periodic render-stats (the 'drawing-and-
   * black vs skipping-everything' discriminator). Logged every 256 presents
   * as deltas. */
  uint64_t skipNoRt = 0, skipPsoFail = 0, skipNoShader = 0, skipNoIndex = 0;
  /* V12 depth-chain buckets: every dropped/defaulted depth behavior must
   * count and name itself (first-8 logs at the drop sites). */
  uint64_t clearDropUnresolved = 0; /* clear_by_id target never resolved */
  uint64_t clearDsPartialDrop = 0;  /* clear_image_view partial-rect DS drop */
  /* V15 upload-flood buckets: direct = memcpy into shared contents (no
   * encoder); staged = slab copy + deferred blit; flushUse = a draw consumed
   * a buffer/texture with pending blits and forced the flush (the old
   * pass-shatter shape — FLOW-CONTROL: now the never-fire backstop behind
   * the promote path); promoted = parked blits converted to a direct memcpy
   * at a consuming draw because the range retired by then (the credit-
   * bounded common case); texParked = texture blits deferred to a natural
   * boundary instead of closing the pass. */
  uint64_t uplDirect = 0, uplStaged = 0, uplFlushUse = 0;
  uint64_t uplPromoted = 0, uplTexParked = 0;
  std::vector<DeferredXfer> deferredXfers;
  std::vector<DeferredTexXfer> deferredTexXfers;
  /* TEST HOOK (D9MT_TEST_FRAME_DELAY_US): sleep this long in handlePresent —
   * a throttled consumer for the sustained-overload flow-control gate. */
  uint32_t frameDelayUs = 0;
  uint64_t dsUnresolvedBinds = 0;   /* set_rt ds named but texture nil */
  uint64_t dssoDefaulted = 0;       /* draw w/ ds attachment before any
                                       set_depth_stencil_state record */
  bool depthStencilSeen = false;
  struct StatsSnap {
    uint64_t draws = 0, skipped = 0, pend = 0, noRt = 0, psoFail = 0,
             noShader = 0, noIndex = 0, encoders = 0, uploads = 0,
             clrDrop = 0, clrDsPart = 0, dsUnres = 0, dssoDef = 0,
             dsLate = 0, uplDirect = 0, uplStaged = 0, uplFlushUse = 0,
             uplPromoted = 0, uplTexParked = 0;
  } statsSnap;

  /* S3 v9 meta-blit: MSL source compiled once at init (MTL4Compiler),
   * per-{format,sampleCount} PSOs built ASYNC through PsoEntry; a blit
   * whose PSO is still compiling is STASHED and drained at present (a
   * one-shot record cannot retry like a draw). */
  id<MTLLibrary> blitLib = nil;
  id<MTLSamplerState> blitSamplers[2] = {nil, nil}; /* nearest, linear */
  std::unordered_map<uint64_t, std::shared_ptr<PsoEntry>> blitPsoCache;
  /* V15: partial-clear fill pipelines (scissored fullscreen-triangle draw
   * writing the clear color and/or clear depth) — the partial-rect
   * clear_image_view path that used to DROP. Keyed {colorFmt|0, samples,
   * hasColor}; async like blitPsoCache, stash+retry via pendingImgClears. */
  std::unordered_map<uint64_t, std::shared_ptr<PsoEntry>> fillPsoCache;
  std::vector<d9mt_wire_rec_blit_texture_view> pendingBlits;
  /* one-shot records whose texture is still BUILDING on the create worker
   * (the UpdateTexture-right-after-create shape): stashed + drained at
   * present, same discipline as pendingBlits. Cross-family stream order is
   * approximated at drain time (copies, clears, mipgens, then blits). */
  std::vector<d9mt_wire_rec_copy_texture> pendingCopies;
  std::vector<d9mt_wire_rec_clear_image_view> pendingImgClears;
  std::vector<d9mt_wire_rec_generate_mipmaps> pendingMipgens;

  /* --- occlusion-query transport (HANDOFF-QUERIES.md) ----------------------
   * begin/end records bracket draws in stream order; each encoder segment
   * inside a bracket gets its own visibility slot (Metal counters are
   * per-encoder — a pass split mid-query would otherwise lose or clobber
   * counts), summed on the CPU at publication. Results publish into the
   * shared d9mt_wire_query_ring (inside the heap file) ONLY once the
   * covering submission serial has signaled — never an early lie. */
  id<MTLBuffer> visBuf = nil;      /* kVisSlots x u64, shared storage */
  uint64_t* visCpu = nullptr;      /* visBuf.contents (stable) */
  struct d9mt_wire_query_ring* qring = nullptr; /* heap + fixed offset */
  uint64_t qCursor = 0;            /* consumer-only free-running slot alloc */
  uint64_t qRetired = 0;           /* under queryMu: slots whose results published */
  struct QActive {
    bool on = false;
    bool overflow = false;         /* slot pool exhausted: end -> all-passed */
    uint64_t qid = 0;
    std::vector<uint32_t> slots;
  } qActive;                       /* consumer-thread only */
  struct QPending {
    uint64_t qid = 0;              /* 0 = slot-retire only (session drain) */
    uint64_t serial = 0;           /* submission covering the LAST slot */
    std::vector<uint32_t> slots;
  };
  std::deque<QPending> qPending;   /* under queryMu (consumer + watchdog) */
  std::mutex queryMu;
  /* diagnostics (finish summary) */
  uint64_t qBegun = 0, qPublished = 0, qNested = 0, qOrphan = 0,
           qOverflow = 0, qSessionEnd = 0;
};

Exec* g = nullptr;

static inline uint64_t completedSerial() { return g->event.signaledValue; }

static void execError(const char* what) {
  static uint64_t count = 0;
  if (count++ < 16)
    fprintf(stderr, "[serverd] exec error: %s\n", what);
  atomic_fetch_add_explicit(&g->status->exec_errors, 1, memory_order_relaxed);
}

static bool waitSerial(uint64_t serial, uint64_t timeoutMs) {
  if (completedSerial() >= serial)
    return true;
  return [g->event waitUntilSignaledValue:serial timeoutMS:timeoutMs];
}

/* --- residency (one queue-attached set; add at create, remove deferred) ---- */

static void residencyAdd(id<MTLAllocation> alloc) {
  if (!alloc)
    return;
  [g->residency addAllocation:alloc];
  g->residencyDirty = true;
}

static void residencyCommitIfDirty() {
  if (g->residencyDirty) {
    [g->residency commit];
    g->residencyDirty = false;
  }
}

/* --- occlusion queries (HANDOFF-QUERIES.md) --------------------------------
 * Port of the client's OLD local implementation semantics
 * (mtl_context_transfer.cpp beginQuery/endQuery: per-context visibility
 * buffer, one 8-byte counter slot per bracket, CPU-zeroed at allocation,
 * counting mode armed on the live encoder), upgraded for pass splits: one
 * slot PER ENCODER SEGMENT, summed at publication (Metal visibility
 * counters are per-encoder). */

constexpr uint32_t kVisSlots = 8192u;          /* 64 KiB visibility buffer */
constexpr uint64_t kQueryAllPassed = 1u << 20; /* mirror of the client stub */

/* caller holds g->queryMu */
static void publishQueryLocked(uint64_t qid, uint64_t samples, uint64_t serial,
                               bool dropped) {
  if (!g->qring)
    return;
  auto* slot = &g->qring->slots[qid & (D9MT_WIRE_QUERY_SLOTS - 1u)];
  /* seqlock-lite (mtl_wire.h contract): pass through id = 0 so a reader
   * can never pair a stale id with a torn payload. */
  atomic_store_explicit(&slot->id, 0, memory_order_release);
  atomic_store_explicit(&slot->samples, samples, memory_order_relaxed);
  atomic_store_explicit(&slot->serial, serial, memory_order_relaxed);
  atomic_store_explicit(&slot->id, qid, memory_order_release);
  atomic_fetch_add_explicit(&g->qring->published, 1, memory_order_relaxed);
  if (dropped)
    atomic_fetch_add_explicit(&g->qring->dropped, 1, memory_order_relaxed);
  g->qPublished++;
}

/* Publish every pending bracket whose covering submission COMPLETED (the
 * no-early-lies gate: completedSerial is the queue-timeline signal). Runs on
 * the consumer (begin/end/present) AND on the watchdog tick — a game may
 * stop emitting records entirely while it polls GetData; the watchdog is
 * the guaranteed publisher (10 ms worst-case latency). */
static void sweepQueries() {
  std::lock_guard<std::mutex> lk(g->queryMu);
  const uint64_t done = completedSerial();
  while (!g->qPending.empty() && g->qPending.front().serial <= done) {
    Exec::QPending& p = g->qPending.front();
    if (p.qid) {
      uint64_t sum = 0;
      if (g->visCpu)
        for (uint32_t s : p.slots)
          sum += g->visCpu[s];
      publishQueryLocked(p.qid, sum, p.serial, false);
    }
    g->qRetired += p.slots.size();
    g->qPending.pop_front();
  }
}

/* Allocate + arm a visibility slot on the LIVE encoder. Called at begin
 * (pass already open) and from materializePass for every later encoder
 * segment the bracket spans. */
static void armQuerySlot() {
  if (!g->qActive.on || g->qActive.overflow || !g->pass.active)
    return;
  if (!g->visCpu) {
    /* no visibility buffer (alloc failed at boot) — a bracket without
     * counters must resolve all-passed, never a 0-sum "occluded" lie */
    g->qActive.overflow = true;
    g->qOverflow++;
    return;
  }
  uint64_t retired;
  {
    std::lock_guard<std::mutex> lk(g->queryMu);
    retired = g->qRetired;
  }
  if (g->qCursor - retired >= kVisSlots) {
    /* pool exhausted (kVisSlots un-published segments in flight) — the
     * bracket resolves all-passed at end, never a stale counter readback */
    g->qActive.overflow = true;
    g->qOverflow++;
    return;
  }
  const uint32_t slot = (uint32_t)(g->qCursor++ % kVisSlots);
  g->visCpu[slot] = 0u; /* retired => no in-flight GPU writer; a segment
                         * with zero draws must sum as 0 */
  [g->pass.encoder setVisibilityResultMode:MTLVisibilityResultModeCounting
                                    offset:(NSUInteger)slot * 8u];
  g->qActive.slots.push_back(slot);
}

/* Close the active bracket into the pending queue; publication happens at
 * GPU completion (sweepQueries). */
static void endActiveQuery() {
  if (!g->qActive.on)
    return;
  if (g->pass.active && !g->qActive.overflow && !g->qActive.slots.empty())
    [g->pass.encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled
                                      offset:0];
  {
    std::lock_guard<std::mutex> lk(g->queryMu);
    if (g->qActive.overflow) {
      publishQueryLocked(g->qActive.qid, kQueryAllPassed, completedSerial(),
                         true);
      g->qRetired += g->qActive.slots.size();
    } else {
      Exec::QPending p;
      p.qid = g->qActive.qid;
      /* serial covering the LAST slot: the still-recording cmdbuf if one is
       * open, else everything already submitted. Earlier segments live in
       * smaller serials — waiting for the max is sufficient. */
      p.serial = g->cmdbuf ? g->serial : g->lastSubmitted;
      p.slots = std::move(g->qActive.slots);
      g->qPending.push_back(std::move(p));
    }
  }
  g->qActive = {};
}

static void handleBeginQuery(const d9mt_wire_rec_query* r) {
  sweepQueries();
  if (g->qActive.on) {
    /* D3D9 occlusion queries cannot nest; a second begin force-ends the
     * previous bracket so the stream stays sane (counted). */
    g->qNested++;
    endActiveQuery();
  }
  g->qActive.on = true;
  g->qActive.overflow = false;
  g->qActive.qid = r->query_id;
  g->qActive.slots.clear();
  g->qBegun++;
  if (g->pass.active)
    armQuerySlot();
}

static void handleEndQuery(const d9mt_wire_rec_query* r) {
  if (!g->qActive.on || g->qActive.qid != r->query_id) {
    /* orphan end (protocol anomaly, e.g. bracket split across a session
     * reset): resolve all-passed — never falsely occlude on a transport
     * artifact. */
    g->qOrphan++;
    std::lock_guard<std::mutex> lk(g->queryMu);
    publishQueryLocked(r->query_id, kQueryAllPassed, completedSerial(), true);
    return;
  }
  endActiveQuery();
  sweepQueries(); /* zero-work brackets (serial already complete) publish now */
}

/* --- frame / submission spine (decision 5 port) ----------------------------- */

static void ensureCmdbuf() {
  if (g->cmdbuf)
    return;
  uint32_t slot = g->frameIndex % 3u;
  g->frameIndex++;
  if (!g->allocators[slot]) {
    g->allocators[slot] = [g->device newCommandAllocator];
  } else {
    /* MTL4CommandAllocator contract: prior command buffers must have
     * COMPLETED before reset (mtl_context_record.cpp beginRecording). */
    if (!waitSerial(g->allocSerial[slot], 5000))
      execError("allocator reset wait timed out");
    [g->allocators[slot] reset];
  }
  g->cmdbuf = [g->device newCommandBuffer];
  [g->cmdbuf beginCommandBufferWithAllocator:g->allocators[slot]];
  g->serial = g->lastSubmitted + 1;
  g->allocSerial[slot] = g->serial;
}

static void closePass();
static void closeXfer();
static void flushPendingClears();
static void flushDeferredXfers();

static void submitFrame() {
  /* v11: a clear-only frame (no draw ever materialized the pass) must not
   * lose its folded clears at the frame boundary — the client's local
   * execution flushes them at present too. */
  flushPendingClears();
  closePass();
  flushDeferredXfers(); /* V15: parked slab blits ride the frame's xfer close */
  closeXfer();
  if (!g->cmdbuf)
    return;
  [g->cmdbuf endCommandBuffer];
  residencyCommitIfDirty();
  id<MTL4CommandBuffer> cb = g->cmdbuf;
  [g->queue commit:&cb count:1];
  /* MTL4: the completion signal is a queue-timeline op AFTER commit. */
  [g->queue signalEvent:g->event value:g->serial];
  g->lastSubmitted = g->serial;
  g->cmdbuf = nil;
  g->dirtyPso = g->dirtyState = g->dirtyBindings = true;

  /* deferred destruction: release everything whose last-use submission
   * completed (the strong ref in `retire` was the keepalive). */
  uint64_t completed = completedSerial();
  size_t w = 0;
  for (size_t i = 0; i < g->retire.size(); i++) {
    if (g->retire[i].first <= completed) {
      [g->residency removeAllocation:(id<MTLAllocation>)g->retire[i].second];
      g->residencyDirty = true;
    } else {
      g->retire[w++] = g->retire[i];
    }
  }
  g->retire.resize(w);
  w = 0;
  for (size_t i = 0; i < g->retireNoRes.size(); i++) {
    if (g->retireNoRes[i].first > completed)
      g->retireNoRes[w++] = g->retireNoRes[i];
  }
  g->retireNoRes.resize(w);
}

/* --- S3 transfer encoder + heap acks ----------------------------------------- */

/* The slab copy detaches the GPU from the heap region, so a region is
 * reusable the moment its record executes: publish the generation into the
 * heap header (the producer's space check) and status v3 (observability).
 * Generations are monotonic in stream order. */
static void heapAck(uint64_t generation) {
  atomic_store_explicit(&g->heap->consumed, generation, memory_order_release);
  atomic_store_explicit(&g->status->heap_consumed, generation, memory_order_relaxed);
}

/* FLOW-CONTROL: parked texture blits (defined with encodeTextureUpload). */
static void drainDeferredTexXfers();

/* Open (or reuse) the between-passes MTL4ComputeCommandEncoder heap blits
 * encode into. An active render pass closes first — upload records sit at
 * exactly this stream position client-side too (prepareDirectEncode). */
static id<MTL4ComputeCommandEncoder> ensureXfer() {
  if (g->xfer)
    return g->xfer;
  if (g->pass.active)
    closePass();
  ensureCmdbuf();
  g->xfer = [g->cmdbuf computeCommandEncoder];
  if (!g->xfer)
    execError("computeCommandEncoder failed");
  /* FLOW-CONTROL: parked texture blits ride EVERY xfer-encoder open (a
   * natural boundary — the pass is already closed here). Texture blits can
   * only park while a pass is ACTIVE, so any copy-family record that opens
   * the encoder later in the stream drains them first — stream order
   * against the whole copy family is preserved with no per-handler hooks.
   * (g->xfer is set above, so the drain's own ensureXfer is a no-op.) */
  drainDeferredTexXfers();
  return g->xfer;
}

static void closeXfer() {
  if (!g->xfer)
    return;
  /* §4c producer barrier: order the blit writes ahead of every later queue
   * stage (mirror of the client's emitProducerBarrier on transfer encoders). */
  [g->xfer barrierAfterStages:(MTLStages)(kStageBlit | kStageDispatch)
            beforeQueueStages:(MTLStages)kStageAll
            visibilityOptions:(MTL4VisibilityOptions)kVisibilityDevice];
  [g->xfer endEncoding];
  g->xfer = nil;
}

/* --- upload ring (push data / AB word slices) -------------------------------- */

struct RingSlice {
  void* cpu = nullptr;
  uint64_t gpu = 0;
  id<MTLBuffer> buf = nil; /* S3: blit sources need buffer + offset */
  uint64_t off = 0;
};

static RingSlice ringAlloc(uint64_t size, uint64_t align) {
  size = (size + 15) & ~15ull;
  const uint64_t completed = completedSerial();
  for (size_t scan = 0; scan < g->slabs.size(); scan++) {
    UploadSlab& s = g->slabs[(g->slabCursor + scan) % g->slabs.size()];
    /* a slab last used by a RETIRED submission may be rewound; a slab used
     * by the CURRENT recording (serial == g->serial) may only be appended */
    if (s.serial < g->serial && s.serial <= completed)
      s.used = 0;
    else if (s.serial != g->serial && s.used != 0)
      continue; /* in flight */
    uint64_t off = (s.used + align - 1) & ~(align - 1);
    if (off + size > s.buf.length)
      continue;
    g->slabCursor = (g->slabCursor + scan) % g->slabs.size();
    s.used = off + size;
    s.serial = g->serial;
    return { (uint8_t*)s.buf.contents + off, s.buf.gpuAddress + off, s.buf, off };
  }
  uint64_t bytes = size > kUploadSlabBytes ? size : kUploadSlabBytes;
  id<MTLBuffer> buf = [g->device newBufferWithLength:bytes
      options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
  if (!buf) {
    execError("upload slab allocation failed");
    return {};
  }
  residencyAdd(buf);
  UploadSlab s;
  s.buf = buf;
  s.used = size;
  s.serial = g->serial;
  g->slabs.push_back(s);
  g->slabCursor = g->slabs.size() - 1;
  return { buf.contents, buf.gpuAddress, buf, 0 };
}

/* --- V15 upload staging (the pass-shatter fix) --------------------------------
 * Every wire buffer is SHARED storage (create_buffer), so an upload whose
 * destination range is not referenced by any encoded-but-incomplete GPU work
 * may write the bytes with a plain memcpy — visible to every LATER-encoded
 * draw, invisible to nothing (no encoded work reads the range). The D3D9
 * constant ring guarantees exactly this shape (fresh offset per draw, rename
 * on wrap), which is what turned per-draw constant re-ships into per-draw
 * render-pass closes (ensureXfer) — ~292 passes/frame on a ~60-pass scene. */

/* Record that encoded GPU work (a draw's binding, a copy) reads or writes
 * res->buffer bytes [off, off+len). Live until the current serial completes. */
static void markBufRef(const std::shared_ptr<Resource>& res, uint64_t off,
                       uint64_t len) {
  if (!res || !len)
    return;
  if (res->refSerial && completedSerial() >= res->refSerial) {
    res->refMin = ~0ull; /* watermark retired with the GPU work */
    res->refMax = 0;
  }
  if (off < res->refMin)
    res->refMin = off;
  if (off + len > res->refMax)
    res->refMax = off + len;
  res->refSerial = g->serial;
}

/* True when [off, off+len) may be observed by encoded-but-incomplete GPU
 * work (draw refs) or would reorder against a parked slab blit. */
static bool rangeLive(const std::shared_ptr<Resource>& res, uint64_t off,
                      uint64_t len) {
  if (res->refSerial && completedSerial() < res->refSerial &&
      off < res->refMax && off + len > res->refMin)
    return true;
  if (res->pendingXfers && off < res->xferMax && off + len > res->xferMin)
    return true;
  return false;
}

/* Encode every parked slab->buffer blit into the shared xfer encoder. Only
 * called at natural boundaries (materializePass, submitFrame) or on demand
 * when a draw consumes a buffer with parked blits — never per record. */
static void flushDeferredXfers() {
  if (g->deferredXfers.empty() && g->deferredTexXfers.empty())
    return;
  id<MTL4ComputeCommandEncoder> enc = ensureXfer(); /* drains parked tex blits */
  drainDeferredTexXfers(); /* idempotent belt (encoder was already open) */
  for (DeferredXfer& d : g->deferredXfers) {
    if (enc)
      [enc copyFromBuffer:d.srcBuf
             sourceOffset:(NSUInteger)d.srcOff
                 toBuffer:d.res->buffer
        destinationOffset:(NSUInteger)d.dstOff
                     size:(NSUInteger)d.size];
    /* the blit is now encoded GPU work WRITING the range — a later direct
     * memcpy into it would be overwritten when the GPU runs; keep it live */
    markBufRef(d.res, d.dstOff, d.size);
    if (d.res->pendingXfers && --d.res->pendingXfers == 0) {
      d.res->xferMin = ~0ull;
      d.res->xferMax = 0;
    }
  }
  g->deferredXfers.clear();
}

/* FLOW-CONTROL: promote parked slab->buffer blits into direct memcpys (the
 * V15 direct path applied LATE, at the consuming draw). A parked blit was
 * staged because its destination range was live at upload time; with frame
 * credits bounding the client's lead, completedSerial() tracks the present
 * rate, so by the time a draw CONSUMES the range the old readers have
 * usually retired — memcpy is legal and the pass-closing flush (uplFlushUse)
 * becomes the never-fire backstop. In-order walk; an entry may promote only
 * if no EARLIER kept entry writes an overlapping range of the same buffer
 * (that kept blit encodes LATER and would clobber the newer bytes). */
static void promoteParkedXfers() {
  if (g->deferredXfers.empty())
    return;
  const uint64_t completed = completedSerial();
  size_t w = 0;
  for (size_t i = 0; i < g->deferredXfers.size(); i++) {
    DeferredXfer& d = g->deferredXfers[i];
    /* draw-liveness only (rangeLive's pendingXfers half is THIS queue) */
    bool blocked = d.res->buffer.storageMode != MTLStorageModeShared ||
                   (d.res->refSerial && completed < d.res->refSerial &&
                    d.dstOff < d.res->refMax && d.dstOff + d.size > d.res->refMin);
    if (!blocked) {
      for (size_t j = 0; j < w; j++) {
        const DeferredXfer& k = g->deferredXfers[j];
        if (k.res == d.res && d.dstOff < k.dstOff + k.size &&
            d.dstOff + d.size > k.dstOff) {
          blocked = true;
          break;
        }
      }
    }
    if (blocked) {
      if (w != i)
        g->deferredXfers[w] = std::move(d);
      w++;
      continue;
    }
    std::memcpy((uint8_t*)d.res->buffer.contents + d.dstOff,
                (uint8_t*)d.srcBuf.contents + d.srcOff, (size_t)d.size);
    g->uplPromoted++;
    atomic_store_explicit(&g->status->upl_promoted, g->uplPromoted,
                          memory_order_relaxed);
    if (d.res->pendingXfers && --d.res->pendingXfers == 0) {
      d.res->xferMin = ~0ull;
      d.res->xferMax = 0;
    }
  }
  g->deferredXfers.resize(w);
}

/* --- argument tables (decision 3 port; rotate once per pass) ----------------- */

static ArgTablePair makeArgTablePair() {
  MTL4ArgumentTableDescriptor* desc = [[MTL4ArgumentTableDescriptor alloc] init];
  desc.maxBufferBindCount = 31;
  desc.maxTextureBindCount = 16;
  desc.maxSamplerStateBindCount = 16;
  desc.initializeBindings = YES;
  desc.supportAttributeStrides = YES;
  ArgTablePair pair;
  NSError* err = nil;
  pair.table[0] = [g->device newArgumentTableWithDescriptor:desc error:&err];
  pair.table[1] = [g->device newArgumentTableWithDescriptor:desc error:&err];
  if (!pair.table[0] || !pair.table[1])
    execError("newArgumentTable failed");
  return pair;
}

static void selectArgumentTablesForPass() {
  const uint64_t completed = completedSerial();
  const size_t n = g->argTables.size();
  for (size_t scan = 0; scan < n; scan++) {
    const size_t idx = (g->argCursor + scan) % n;
    if (g->argTables[idx].serial <= completed) {
      g->currentArgTable = (uint32_t)idx;
      g->argCursor = (uint32_t)(idx + 1);
      g->argTables[idx].serial = g->serial;
      return;
    }
  }
  g->argTables.push_back(makeArgTablePair());
  g->currentArgTable = (uint32_t)(g->argTables.size() - 1);
  g->argCursor = g->currentArgTable + 1;
  g->argTables[g->currentArgTable].serial = g->serial;
}

/* --- pass management (decision 1 port, immediate mode) ----------------------- */

static bool passHasValidAttachment() {
  for (auto& c : g->pass.color)
    if (c.tex)
      return true;
  return g->pass.ds.tex != nil;
}

/* V17: late re-resolve for named-but-nil attachments. set_rt resolves at
 * CONSUME time, but texture creation runs on the async create worker — the
 * first bind of a fresh texture (game-created DS surfaces during a boot
 * storm are the GTA shape) races the worker and the whole pass would run
 * without its depth attachment. Retry at materialize (first draw): by then
 * the worker has typically published. Defined after observeReady/lookupWire
 * via forward declarations. */
static std::shared_ptr<Resource> lookupWire(uint64_t id);
static bool observeReady(const std::shared_ptr<Resource>& res);
static uint64_t g_lateResolves = 0;

static void lateResolveAttachment(RtAttachment& a) {
  if (!a.named || a.tex || a.backbuffer || !a.wireId)
    return;
  auto res = lookupWire(a.wireId);
  if (res && observeReady(res) && res->texture) {
    a.tex = res->texture;
    res->hasContent = true; /* attachment write defines the storage —
                             * later depth-as-texture sampling depends on it */
    g_lateResolves++;
  }
}

static void materializePass() {
  if (g->pass.active)
    return;
  ensureCmdbuf();
  flushDeferredXfers(); /* V15: parked slab blits ride this natural boundary */
  closeXfer(); /* pending heap blits must order ahead of this pass */

  for (auto& c : g->pass.color)
    lateResolveAttachment(c);
  lateResolveAttachment(g->pass.ds);

  MTL4RenderPassDescriptor* desc = [[MTL4RenderPassDescriptor alloc] init];
  for (uint32_t i = 0; i < 4; i++) {
    RtAttachment& a = g->pass.color[i];
    if (!a.tex)
      continue;
    desc.colorAttachments[i].texture = a.tex;
    desc.colorAttachments[i].level = a.baseMip;
    desc.colorAttachments[i].slice = a.baseLayer;
    desc.colorAttachments[i].loadAction =
        g->pass.clearColor[i] ? MTLLoadActionClear : MTLLoadActionLoad;
    desc.colorAttachments[i].storeAction = MTLStoreActionStore;
    if (g->pass.clearColor[i])
      desc.colorAttachments[i].clearColor = g->pass.clearValues[i];
  }
  if (g->pass.ds.tex) {
    desc.depthAttachment.texture = g->pass.ds.tex;
    desc.depthAttachment.level = g->pass.ds.baseMip;
    desc.depthAttachment.slice = g->pass.ds.baseLayer;
    desc.depthAttachment.loadAction =
        g->pass.clearDepth ? MTLLoadActionClear : MTLLoadActionLoad;
    desc.depthAttachment.storeAction = MTLStoreActionStore;
    desc.depthAttachment.clearDepth = g->pass.depthClear;
    /* stencil aspect only for stencil-bearing formats (255 =
     * Depth24Unorm_Stencil8, deprecated in the macOS 27 SDK but still what
     * the wire carries for D3DFMT_D24S8) */
    MTLPixelFormat f = g->pass.ds.format;
    if ((NSUInteger)f == 255 ||
        f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatStencil8) {
      desc.stencilAttachment.texture = g->pass.ds.tex;
      desc.stencilAttachment.level = g->pass.ds.baseMip;
      desc.stencilAttachment.slice = g->pass.ds.baseLayer;
      desc.stencilAttachment.loadAction =
          g->pass.clearStencil ? MTLLoadActionClear : MTLLoadActionLoad;
      desc.stencilAttachment.storeAction = MTLStoreActionStore;
      desc.stencilAttachment.clearStencil = g->pass.stencilClear;
    }
  }

  /* occlusion transport: every pass can carry visibility counters (port of
   * the client's pass-creation binding, mtl_context_record.cpp). */
  desc.visibilityResultBuffer = g->visBuf;

  g->pass.encoder = [g->cmdbuf renderCommandEncoderWithDescriptor:desc];
  if (!g->pass.encoder) {
    execError("renderCommandEncoder failed");
    /* stay pending: draws will keep skipping */
    return;
  }
  atomic_fetch_add_explicit(&g->status->encoders, 1, memory_order_relaxed);
  g->pass.active = true;
  g->pass.pending = false;
  g->dirtyPso = g->dirtyState = g->dirtyBindings = true;
  g->argTablesBound = false;
  g->lastPso = nil;
  g->dynValid = false;

  /* a bracket spanning this pass split continues counting into a FRESH
   * slot on the new encoder (segments summed at publication) */
  if (g->qActive.on)
    armQuerySlot();
}

static void closePass() {
  if (!g->pass.active)
    return;
  /* §4c: producer barrier — order this pass's fragment writes ahead of every
   * later queue stage (mtl_context_transfer.cpp emitProducerBarrier). */
  [g->pass.encoder barrierAfterStages:(MTLStages)kStageFragment
                    beforeQueueStages:(MTLStages)kStageAll
                    visibilityOptions:(MTL4VisibilityOptions)kVisibilityDevice];
  [g->pass.encoder endEncoding];
  g->pass.encoder = nil;
  g->pass.active = false;
  g->pass.pending = true; /* same targets stay bound until rebound */
  g->argTablesBound = false;
  for (auto& c : g->pass.clearColor)
    c = false;
  g->pass.clearDepth = g->pass.clearStencil = false;
}

static void flushPendingClears() {
  if (g->pass.active || !g->pass.pending)
    return;
  bool any = g->pass.clearDepth || g->pass.clearStencil;
  for (bool c : g->pass.clearColor)
    any = any || c;
  if (!any || !passHasValidAttachment())
    return;
  materializePass();
  closePass();
}

/* --- PSO resolve (decision 9 port) -------------------------------------------- */

static uint64_t strHash(const std::string& s) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (char c : s)
    h = (h ^ (uint8_t)c) * 0x100000001b3ull;
  return h;
}

/* S3: NEVER blocks. Cache hit returns the pipeline (nil for a failed build);
 * a miss kicks an ASYNC MTL4Compiler task and reports pending — the caller
 * skips the draw (counted draws_skipped_pending_pso) and retries at the next
 * draw, keeping ring consumption flowing through compile storms. */
static id<MTLRenderPipelineState> resolvePso(bool* pending) {
  *pending = false;
  if (!g->vs || !g->vs->library)
    return nil;

  PsoKey key;
  std::memset(&key, 0, sizeof(key));
  key.vsLib = (__bridge const void*)g->vs->library;
  key.fsLib = g->fs ? (__bridge const void*)g->fs->library : nullptr;
  key.entryHash = strHash(g->vs->entry) * 31 + (g->fs ? strHash(g->fs->entry) : 0);

  uint32_t nAttr = (uint32_t)g->attrs.size();
  if (nAttr > 20)
    nAttr = 20;
  for (uint32_t i = 0; i < nAttr; i++) {
    key.attrs[i].format = (uint32_t)vertexFormat(g->attrs[i].format);
    key.attrs[i].offset = g->attrs[i].offset;
    key.attrs[i].bufferIndex = kVertexBufferBaseIndex + g->attrs[i].binding;
  }
  key.attrCount = nAttr;

  uint32_t nLayout = (uint32_t)g->layouts.size();
  if (nLayout > 20)
    nLayout = 20;
  for (uint32_t i = 0; i < nLayout; i++) {
    const d9mt_wire_vertex_layout& src = g->layouts[i];
    /* live SetStreamSource stride wins over a 0 layout stride (the GTA
     * "buffer has no stride" fix — mtl_context_transfer.cpp resolvePso). */
    uint32_t stride = src.stride;
    if (src.binding < kMaxStreams && g->streams[src.binding].stride != 0)
      stride = g->streams[src.binding].stride;
    key.layouts[i].stride = stride;
    key.layouts[i].stepFunction = src.per_instance ? 2u : 1u;
    key.layouts[i].bufferIndex = kVertexBufferBaseIndex + src.binding;
  }
  key.layoutCount = nLayout;

  uint32_t colorCount = 0;
  for (uint32_t i = 0; i < 4; i++) {
    if (!g->pass.color[i].tex)
      continue;
    key.colors[i].fmt = (uint32_t)g->pass.color[i].format;
    key.colors[i].blend = g->blend[i].enable;
    key.colors[i].wmask = g->blend[i].write_mask & 0xFu;
    key.colors[i].csrc = g->blend[i].color_src;
    key.colors[i].cdst = g->blend[i].color_dst;
    key.colors[i].cop = g->blend[i].color_op;
    key.colors[i].asrc = g->blend[i].alpha_src;
    key.colors[i].adst = g->blend[i].alpha_dst;
    key.colors[i].aop = g->blend[i].alpha_op;
    colorCount = i + 1;
  }
  key.colorCount = colorCount;
  key.sampleCount = g->msaa.sample_count ? g->msaa.sample_count : 1u;
  key.a2c = g->msaa.alpha_to_coverage ? 1u : 0u;

  auto it = g->psoCache.find(key);
  if (it != g->psoCache.end()) {
    const uint32_t state = it->second->state.load(std::memory_order_acquire);
    if (state == 0) {
      *pending = true;
      return nil; /* compile in flight — skip, retry next draw */
    }
    return it->second->pso; /* nil when failed: cached to avoid retry storms */
  }

  MTL4RenderPipelineDescriptor* desc = [[MTL4RenderPipelineDescriptor alloc] init];

  MTL4LibraryFunctionDescriptor* vfd = [[MTL4LibraryFunctionDescriptor alloc] init];
  vfd.library = g->vs->library;
  vfd.name = [NSString stringWithUTF8String:g->vs->entry.c_str()];
  desc.vertexFunctionDescriptor = vfd;

  if (g->fs && g->fs->library) {
    MTL4LibraryFunctionDescriptor* ffd = [[MTL4LibraryFunctionDescriptor alloc] init];
    ffd.library = g->fs->library;
    ffd.name = [NSString stringWithUTF8String:g->fs->entry.c_str()];
    desc.fragmentFunctionDescriptor = ffd;
  }

  for (uint32_t i = 0; i < key.colorCount; i++) {
    const auto& c = key.colors[i];
    desc.colorAttachments[i].pixelFormat = (MTLPixelFormat)c.fmt;
    desc.colorAttachments[i].blendingState =
        c.blend ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
    desc.colorAttachments[i].writeMask = writeMask(c.wmask);
    desc.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)c.cop;
    desc.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)c.aop;
    desc.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)c.csrc;
    desc.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)c.cdst;
    desc.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)c.asrc;
    desc.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)c.adst;
  }

  if (key.attrCount || key.layoutCount) {
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    for (uint32_t i = 0; i < key.attrCount; i++) {
      vd.attributes[i].format = (MTLVertexFormat)key.attrs[i].format;
      vd.attributes[i].offset = key.attrs[i].offset;
      vd.attributes[i].bufferIndex = key.attrs[i].bufferIndex;
    }
    /* layouts are indexed BY BUFFER INDEX (mtl_pso_cache.cpp build()) */
    for (uint32_t i = 0; i < key.layoutCount; i++) {
      uint32_t idx = key.layouts[i].bufferIndex;
      if (idx >= 31 || (key.layouts[i].stride == 0 && key.layouts[i].stepFunction == 1u))
        continue;
      vd.layouts[idx].stride = key.layouts[i].stride;
      vd.layouts[idx].stepFunction = key.layouts[i].stepFunction == 2u
          ? MTLVertexStepFunctionPerInstance
          : MTLVertexStepFunctionPerVertex;
      vd.layouts[idx].stepRate = 1;
    }
    desc.vertexDescriptor = vd;
  }

  desc.rasterSampleCount = key.sampleCount;
  desc.alphaToCoverageState =
      key.a2c ? MTL4AlphaToCoverageStateEnabled : MTL4AlphaToCoverageStateDisabled;
  desc.rasterizationEnabled = YES;
  /* Unspecified: D3D9 sm3 VS always declares [[point_size]] and Metal
   * rejects that against an explicit topology class (mtl_pso_cache.cpp). */
  desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassUnspecified;

  /* ASYNC build (S3): the completion handler runs on a compiler thread and
   * touches ONLY the entry + status atomics; the consumer thread observes
   * the result via the acquire/release state handshake. */
  auto entry = std::make_shared<PsoEntry>();
  g->psoCache.emplace(key, entry);
  atomic_fetch_add_explicit(&g->status->psos_pending, 1, memory_order_relaxed);
  struct d9mt_status* status = g->status;
  [g->compiler newRenderPipelineStateWithDescriptor:desc
                                compilerTaskOptions:nil
                                  completionHandler:^(id<MTLRenderPipelineState> pso,
                                                      NSError* err) {
    if (pso) {
      entry->pso = pso;
      entry->state.store(1, std::memory_order_release);
      atomic_fetch_add_explicit(&status->psos_built, 1, memory_order_relaxed);
    } else {
      entry->state.store(2, std::memory_order_release);
      atomic_fetch_add_explicit(&status->exec_errors, 1, memory_order_relaxed);
      static std::atomic<bool> warned = { false };
      bool expected = false;
      if (warned.compare_exchange_strong(expected, true))
        fprintf(stderr, "[serverd] first PSO failure: %s\n",
                err ? err.localizedDescription.UTF8String : "(no error)");
    }
    atomic_fetch_sub_explicit(&status->psos_pending, 1, memory_order_relaxed);
  }];
  *pending = true;
  return nil;
}

/* --- depth-stencil state cache (port of getDepthStencilState) ---------------- */

static id<MTLDepthStencilState> resolveDsso() {
  std::string key(reinterpret_cast<const char*>(&g->depthStencil.depth_compare_op),
                  sizeof(g->depthStencil) - sizeof(d9mt_wire_rec_hdr));
  auto it = g->dssoCache.find(key);
  if (it != g->dssoCache.end())
    return it->second;

  MTLDepthStencilDescriptor* desc = [[MTLDepthStencilDescriptor alloc] init];
  /* V12: honor depth_test. Metal has no test-enable bit — disabled test maps
   * to compare=Always + write=NO (a disabled depth test performs no depth
   * writes either, per the D3D9 Z-enable contract). MIRRORS the client's
   * getDepthStencilState (mtl_device_impl.cpp) exactly — both sides changed
   * together; the wire record has carried depth_test since S2. The cache key
   * spans depth_compare_op..end of record, so depth_test is already keyed. */
  const bool depthTest = g->depthStencil.depth_test != 0;
  desc.depthCompareFunction = depthTest
      ? (MTLCompareFunction)g->depthStencil.depth_compare_op
      : MTLCompareFunctionAlways;
  desc.depthWriteEnabled = depthTest && g->depthStencil.depth_write != 0;
  if (g->depthStencil.stencil_enable) {
    auto fill = [](const d9mt_wire_stencil_face& f) {
      MTLStencilDescriptor* s = [[MTLStencilDescriptor alloc] init];
      s.stencilCompareFunction = (MTLCompareFunction)f.compare_op;
      s.stencilFailureOperation = (MTLStencilOperation)f.fail_op;
      s.depthFailureOperation = (MTLStencilOperation)f.depth_fail_op;
      s.depthStencilPassOperation = (MTLStencilOperation)f.pass_op;
      s.readMask = f.read_mask;
      s.writeMask = f.write_mask;
      return s;
    };
    desc.frontFaceStencil = fill(g->depthStencil.front);
    desc.backFaceStencil = fill(g->depthStencil.back);
  }
  id<MTLDepthStencilState> dsso = [g->device newDepthStencilStateWithDescriptor:desc];
  g->dssoCache.emplace(std::move(key), dsso);
  return dsso;
}

/* --- draw-time flush (decisions 2+3 port) ------------------------------------- */

static bool promotePendingTexBinds(); /* V18: defined with the bind handler */

static void flushBindings() {
  ArgTablePair& pair = g->argTables[g->currentArgTable];

  /* vertex streams at fixed indices in the VERTEX table */
  for (uint32_t i = 0; i < kMaxStreams; i++) {
    Stream& s = g->streams[i];
    uint64_t addr = s.bound && s.buf ? s.buf.gpuAddress + s.offset
                                     : g->dummyBuffer.gpuAddress;
    [pair.table[0] setAddress:addr atIndex:kVertexBufferBaseIndex + i];
  }

  for (uint32_t st = 0; st < 2; st++) {
    StageState& b = g->stages[st];
    id<MTL4ArgumentTable> table = pair.table[st];
    const std::shared_ptr<Shader>& fn = st == 0 ? g->vs : g->fs;

    /* push data -> fresh ring slice (snapshot semantics). V16: ALSO re-slice
     * when the cached slice is from an OLDER submission — ringAlloc rewinds
     * a slab once its serial completes, so a stale pushGpuAddr points into
     * memory the rewound slab hands to a NEW allocation while draws encoded
     * NOW still read the old address through their table snapshot (garbage
     * push block = exploded geometry). The law: no mutation of GPU-visible
     * memory until completed_serial covers its last reader — enforced by
     * construction: every referenced slice carries the CURRENT serial. */
    if (b.pushExtent && (b.pushDirty || b.pushSerial != g->serial)) {
      RingSlice rs = ringAlloc(b.pushExtent, 256);
      if (rs.cpu) {
        std::memcpy(rs.cpu, b.pushData, b.pushExtent);
        b.pushGpuAddr = rs.gpu;
        b.pushSerial = g->serial;
      }
      b.pushDirty = false;
    }
    [table setAddress:(b.pushGpuAddr ? b.pushGpuAddr : g->dummyBuffer.gpuAddress)
              atIndex:kPushConstantIndex];

    if (!fn)
      continue;

    if (fn->ab) {
      /* argument-buffer model: one slice of 8-byte handle words (port of the
       * abWords path in flushResources). Values: CBV -> gpuAddress, texture
       * -> dummy (no view identity in S2), sampler -> resolved/default. */
      const size_t words = fn->abWords.size();
      RingSlice rs = ringAlloc(words * 8, 16);
      if (!rs.cpu)
        continue;
      uint64_t* w = (uint64_t*)rs.cpu;
      for (size_t i = 0; i < words; i++) {
        const auto& e = fn->abWords[i];
        uint64_t v = 0;
        switch (e.first) {
          case 0: /* Cbv */
            if (e.second < 8 && b.cbvs[e.second].bound && b.cbvs[e.second].buf)
              v = b.cbvs[e.second].buf.gpuAddress + b.cbvs[e.second].offset;
            else
              v = g->dummyBuffer.gpuAddress;
            break;
          case 1: /* Texture */
            v = b.textures[e.second] /* uint8 reg: kMaxTexTrack covers all */
                    ? b.textures[e.second].gpuResourceID._impl
                    : g->dummyTexture.gpuResourceID._impl;
            break;
          case 2: /* Sampler */
            v = b.samplers[e.second]
                    ? b.samplers[e.second].gpuResourceID._impl
                    : g->dummySampler.gpuResourceID._impl;
            break;
        }
        w[i] = v;
      }
      [table setAddress:rs.gpu atIndex:fn->abTableIndex];
      continue;
    }

    /* classic per-slot binding */
    for (uint32_t slot = 0; slot < 8; slot++) {
      int8_t airSlot = fn->cbvMap[slot];
      if (airSlot < 0)
        continue;
      CbvBinding& cbv = b.cbvs[slot];
      uint64_t addr = cbv.bound && cbv.buf ? cbv.buf.gpuAddress + cbv.offset
                                           : g->dummyBuffer.gpuAddress;
      [table setAddress:addr atIndex:(uint32_t)airSlot];
    }
    for (uint32_t slot = 0; slot < kMaxTexSlots; slot++) {
      /* every slot defined: real texture when bind_texture_by_id resolved
       * one (S3), dummy for unresolvable AND unbound */
      id<MTLTexture> tex = b.textures[slot] ? b.textures[slot] : g->dummyTexture;
      [table setTexture:tex.gpuResourceID atIndex:slot];
      id<MTLSamplerState> smp = b.samplers[slot] ? b.samplers[slot] : g->dummySampler;
      [table setSamplerState:smp.gpuResourceID atIndex:slot];
    }
  }
}

static void flushDynamicState() {
  id<MTL4RenderCommandEncoder> enc = g->pass.encoder;
  const bool first = !g->dynValid;
  g->dynValid = true;

  /* attachment dims for clamping (scissor must stay inside the RT) */
  uint32_t rtW = 0, rtH = 0;
  for (uint32_t i = 0; i < 4 && !rtW; i++) {
    if (g->pass.color[i].tex) {
      rtW = (uint32_t)g->pass.color[i].tex.width >> g->pass.color[i].baseMip;
      rtH = (uint32_t)g->pass.color[i].tex.height >> g->pass.color[i].baseMip;
    }
  }
  if (!rtW && g->pass.ds.tex) {
    rtW = (uint32_t)g->pass.ds.tex.width >> g->pass.ds.baseMip;
    rtH = (uint32_t)g->pass.ds.tex.height >> g->pass.ds.baseMip;
  }
  if (!rtW) { rtW = 1; rtH = 1; }

  if (g->viewportSet
      && (first || std::memcmp(&g->lastViewport, &g->viewport, sizeof(g->viewport)) != 0)) {
    g->lastViewport = g->viewport;
    MTLViewport vp;
    vp.originX = g->viewport.origin_x;
    vp.originY = g->viewport.origin_y;
    vp.width = g->viewport.width;
    vp.height = g->viewport.height;
    vp.znear = g->viewport.znear;
    vp.zfar = g->viewport.zfar;
    /* clamp: the server RT (IOSurface slot) may be larger/smaller than the
     * client's backbuffer until the resize path settles */
    if (vp.originX + vp.width > rtW) vp.width = rtW > vp.originX ? rtW - vp.originX : 0;
    if (vp.originY + vp.height > rtH) vp.height = rtH > vp.originY ? rtH - vp.originY : 0;
    [enc setViewport:vp];

    MTLScissorRect sc;
    if (g->viewport.scissor_w && g->viewport.scissor_h) {
      sc.x = g->viewport.scissor_x;
      sc.y = g->viewport.scissor_y;
      sc.width = g->viewport.scissor_w;
      sc.height = g->viewport.scissor_h;
    } else {
      sc = { 0, 0, rtW, rtH };
    }
    if (sc.x >= rtW) sc.x = 0;
    if (sc.y >= rtH) sc.y = 0;
    if (sc.x + sc.width > rtW) sc.width = rtW - sc.x;
    if (sc.y + sc.height > rtH) sc.height = rtH - sc.y;
    [enc setScissorRect:sc];
  } else if (first) {
    MTLScissorRect sc = { 0, 0, rtW, rtH };
    [enc setScissorRect:sc];
  }

  id<MTLDepthStencilState> dsso = g->pass.ds.tex ? resolveDsso() : nil;
  if (dsso && !g->depthStencilSeen) {
    /* V12: drawing against a depth attachment with the DEFAULT depth state
     * (no set_depth_stencil_state record arrived) — name it, it means the
     * server is guessing what the client depth-tests with. */
    g->dssoDefaulted++;
    static int n = 0;
    if (n++ < 8)
      fprintf(stderr, "[serverd] draw #%d: ds attachment bound but NO "
                      "set_depth_stencil_state record seen — default DSSO "
                      "(Always/no-write) in use\n", n);
  }
  if (dsso && (first || dsso != g->lastDsso)) {
    g->lastDsso = dsso;
    [enc setDepthStencilState:dsso];
  }
  if (first || g->stencilRef != g->lastStencilRef) {
    g->lastStencilRef = g->stencilRef;
    [enc setStencilReferenceValue:g->stencilRef];
  }
  if (first || std::memcmp(g->lastBlendColor, g->blendColor, sizeof(g->blendColor)) != 0) {
    std::memcpy(g->lastBlendColor, g->blendColor, sizeof(g->blendColor));
    [enc setBlendColorRed:g->blendColor[0] green:g->blendColor[1]
                     blue:g->blendColor[2] alpha:g->blendColor[3]];
  }
  [enc setCullMode:(MTLCullMode)g->raster.cull_mode];
  [enc setFrontFacingWinding:(MTLWinding)g->raster.front_face];
  [enc setDepthBias:g->depthBias[0] slopeScale:g->depthBias[1] clamp:g->depthBias[2]];
}

/* S3 v2: promote pending-create shader binds (the generalized pending-PSO
 * pattern). Returns true while a bound shader's library is still building —
 * the draw skips and counts, exactly like an in-flight PSO. */
static bool promotePendingShaders() {
  bool pending = false;
  auto step = [&](std::shared_ptr<Resource>& pend, std::shared_ptr<Shader>& slot) {
    if (!pend)
      return;
    const uint32_t state = pend->ready.load(std::memory_order_acquire);
    if (state == 0) {
      pending = true;
      return;
    }
    if (state == 1)
      slot = pend->shader;
    /* failed: slot stays null (no-shader skip path) */
    pend = nullptr;
    g->dirtyPso = true;
    g->dirtyBindings = true;
  };
  step(g->vsPend, g->vs);
  step(g->fsPend, g->fs);
  return pending;
}

/* V15: stamp the ranges this draw's bindings make the GPU read — the
 * upload direct-memcpy path must never rewrite bytes an encoded draw
 * observes (Resource watermark; see markBufRef). O(streams+cbvs+index). */
static void markDrawRefs() {
  for (uint32_t i = 0; i < kMaxStreams; i++) {
    Stream& s = g->streams[i];
    if (s.bound && s.res && s.buf)
      markBufRef(s.res, s.offset,
                 s.length ? s.length : (uint64_t)s.buf.length - s.offset);
  }
  if (g->index.bound && g->index.res && g->index.buf)
    markBufRef(g->index.res, g->index.offset,
               g->index.length ? g->index.length
                               : (uint64_t)g->index.buf.length - g->index.offset);
  for (uint32_t st = 0; st < 2; st++) {
    for (uint32_t slot = 0; slot < 8; slot++) {
      CbvBinding& cbv = g->stages[st].cbvs[slot];
      if (!cbv.bound || !cbv.res || !cbv.buf)
        continue;
      /* range length when the bind carried one, else the D3D9 constant-block
       * ceiling (256 float4 registers) clamped to the buffer end */
      uint64_t len = cbv.length ? cbv.length : 4096ull;
      if (cbv.offset < cbv.buf.length &&
          len > (uint64_t)cbv.buf.length - cbv.offset)
        len = (uint64_t)cbv.buf.length - cbv.offset;
      markBufRef(cbv.res, cbv.offset, len);
    }
  }
}

/* V15: does any buffer/texture this draw binds still have a PARKED slab
 * blit? Only called while parked blits exist (the queue-empty common case
 * never scans). */
static bool boundResHasPendingXfer() {
  for (uint32_t i = 0; i < kMaxStreams; i++)
    if (g->streams[i].bound && g->streams[i].res && g->streams[i].res->pendingXfers)
      return true;
  if (g->index.bound && g->index.res && g->index.res->pendingXfers)
    return true;
  for (uint32_t st = 0; st < 2; st++)
    for (uint32_t slot = 0; slot < 8; slot++)
      if (g->stages[st].cbvs[slot].bound && g->stages[st].cbvs[slot].res &&
          g->stages[st].cbvs[slot].res->pendingXfers)
        return true;
  /* FLOW-CONTROL: a sampled texture with parked content blits must flush
   * before this draw (checked BEFORE promotePendingTexBinds runs, so a
   * dummy-bound slot about to be promoted this draw is covered too —
   * texRes is stamped at bind time regardless of resolution). */
  if (!g->deferredTexXfers.empty())
    for (uint32_t st = 0; st < 2; st++)
      for (uint32_t slot = 0; slot < kMaxTexTrack; slot++)
        if (g->stages[st].texRes[slot] &&
            g->stages[st].texRes[slot]->pendingTexXfers)
          return true;
  return false;
}

static bool prepareDraw() {
  if (promotePendingShaders()) {
    /* library build in flight on the create worker — same accounting as an
     * in-flight PSO compile (the consumer NEVER waits) */
    atomic_fetch_add_explicit(&g->status->draws_skipped, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g->status->draws_skipped_pending_pso, 1,
                              memory_order_relaxed);
    return false;
  }
  if ((!g->deferredXfers.empty() || !g->deferredTexXfers.empty()) &&
      boundResHasPendingXfer()) {
    /* FLOW-CONTROL: a parked upload feeds THIS draw. Try the late direct-
     * memcpy promotion first — with frame credits bounding the backlog,
     * the staged range has usually retired by consumption time and the
     * pass survives. Only a range STILL observable by encoded-but-
     * incomplete GPU work (or a parked TEXTURE blit) forces the flush
     * ahead of the pass — the old always-close behavior, kept as a counted
     * never-fire backstop (uplFlushUse, target ~0). */
    promoteParkedXfers();
    if (boundResHasPendingXfer()) {
      if (g->pass.active)
        closePass();
      flushDeferredXfers();
      g->uplFlushUse++;
      atomic_store_explicit(&g->status->upl_flush_use, g->uplFlushUse,
                            memory_order_relaxed);
    }
  }
  if (!g->pass.pending && !g->pass.active) {
    atomic_fetch_add_explicit(&g->status->draws_skipped, 1, memory_order_relaxed);
    g->skipNoRt++;
    return false;
  }
  if (!g->pass.active) {
    if (!passHasValidAttachment()) {
      atomic_fetch_add_explicit(&g->status->draws_skipped, 1, memory_order_relaxed);
      g->skipNoRt++;
      return false;
    }
    materializePass();
  }
  if (!g->pass.active) { /* encoder creation failed */
    atomic_fetch_add_explicit(&g->status->draws_skipped, 1, memory_order_relaxed);
    g->skipNoRt++;
    return false;
  }

  id<MTLRenderPipelineState> pso = nil;
  if (g->dirtyPso || !g->lastPso) {
    bool pending = false;
    pso = resolvePso(&pending);
    if (!pso) {
      /* dirtyPso stays set: the next draw re-resolves (the pending compile
       * may have landed by then). The consumer NEVER waits here. */
      atomic_fetch_add_explicit(&g->status->draws_skipped, 1, memory_order_relaxed);
      if (pending)
        atomic_fetch_add_explicit(&g->status->draws_skipped_pending_pso, 1,
                                  memory_order_relaxed);
      else if (!g->vs || !g->vs->library)
        g->skipNoShader++;
      else
        g->skipPsoFail++;
      return false;
    }
    if (pso != g->lastPso) {
      [g->pass.encoder setRenderPipelineState:pso];
      g->lastPso = pso;
    }
    g->dirtyPso = false;
  }

  if (!g->argTablesBound) {
    selectArgumentTablesForPass();
    ArgTablePair& pair = g->argTables[g->currentArgTable];
    [g->pass.encoder setArgumentTable:pair.table[0] atStages:MTLRenderStageVertex];
    [g->pass.encoder setArgumentTable:pair.table[1] atStages:MTLRenderStageFragment];
    g->argTablesBound = true;
    g->dirtyBindings = true;
  }

  /* V18: dummy-bound slots whose texture turned ready+content since the
   * bind record re-resolve here (bind-once apps never rebind). */
  if (promotePendingTexBinds())
    g->dirtyBindings = true;

  if (g->dirtyBindings) {
    flushBindings();
    g->dirtyBindings = false;
  }
  if (g->dirtyState || !g->dynValid) {
    flushDynamicState();
    g->dirtyState = false;
  }
  markDrawRefs(); /* V15: the draw is now encoded work reading these ranges */
  return true;
}

/* --- fan emulation (port of ensureFanIndices) --------------------------------- */

static bool ensureFanIndices() {
  if (g->fanIndices)
    return true;
  uint64_t bytes = (uint64_t)3 * (kFanMaxVerts - 2) * 4;
  id<MTLBuffer> buf = [g->device newBufferWithLength:bytes
      options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
  if (!buf)
    return false;
  uint32_t* p = (uint32_t*)buf.contents;
  for (uint32_t t = 0; t + 2 < kFanMaxVerts; t++) {
    *p++ = 0;
    *p++ = t + 1;
    *p++ = t + 2;
  }
  residencyAdd(buf);
  g->fanIndices = buf;
  return true;
}

/* --- record handlers ----------------------------------------------------------- */

static std::shared_ptr<Resource> lookupHandle(uint64_t h) {
  auto it = g->byHandle.find(h);
  return it != g->byHandle.end() ? it->second : nullptr;
}

static std::shared_ptr<Resource> lookupWire(uint64_t id) {
  auto it = g->byWire.find(id);
  return it != g->byWire.end() ? it->second : nullptr;
}

static void registerResource(std::shared_ptr<Resource> res) {
  if (res->clientHandle)
    g->byHandle[res->clientHandle] = res;
  if (res->wireId)
    g->byWire[res->wireId] = res;
}

/* S3 v7 sanity clamps: a rejected create must NEVER hang the consumer. The
 * wire_id/handle stays unregistered — every later reference dummy-resolves
 * (defined), and the reject is counted + named. */
static bool rejectCreate(const char* what, uint64_t wireId, const char* why) {
  const uint64_t n = atomic_fetch_add_explicit(&g->status->creates_rejected, 1,
                                               memory_order_relaxed);
  if (n < 8)
    fprintf(stderr, "[serverd] REJECT %s wire=%llu: %s\n", what,
            (unsigned long long)wireId, why);
  return true;
}

static void handleCreateBuffer(const d9mt_wire_rec_create_buffer* r) {
  if (r->size == 0u) { /* v8: pseudo-resource signature */
    rejectCreate("create_buffer", r->wire_id, "zero size");
    return;
  }
  if (r->size > (1ull << 30)) { /* > 1 GiB: no D3D9 buffer is this big */
    rejectCreate("create_buffer", r->wire_id, "size > 1 GiB");
    return;
  }
  /* shared + CPU-zeroed: index/vertex fetch from a wire-created buffer must
   * be garbage-free (a private buffer's undefined indices could drive vertex
   * fetch out of bounds — GPU fault class). Content itself is S3. */
  id<MTLBuffer> buf = [g->device newBufferWithLength:(r->size ? r->size : 16)
      options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
  if (!buf) {
    execError("create_buffer failed");
    return;
  }
  std::memset(buf.contents, 0, (size_t)buf.length);
  residencyAdd(buf);
  auto res = std::make_shared<Resource>();
  res->kind = Resource::KindBuffer;
  res->wireId = r->wire_id;
  res->clientHandle = r->client_handle;
  res->buffer = buf;
  registerResource(std::move(res));
}

/* Descriptor construction from the wire record — pure, worker-callable. */
static MTLTextureDescriptor* textureDescriptorFor(const d9mt_wire_rec_create_texture* r) {
  MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
  uint32_t layers = r->array_layers ? r->array_layers : 1;
  bool cube = r->cube != 0;
  if (cube) {
    uint32_t cubes = layers / 6 ? layers / 6 : 1;
    td.textureType = cubes > 1 ? MTLTextureTypeCubeArray : MTLTextureTypeCube;
    td.arrayLength = cubes;
  } else if (r->type == 0) {
    td.textureType = layers > 1 ? MTLTextureType1DArray : MTLTextureType1D;
    td.arrayLength = layers;
  } else if (r->type == 2) {
    td.textureType = MTLTextureType3D;
    td.arrayLength = 1;
  } else {
    td.textureType = r->sample_count > 1 ? MTLTextureType2DMultisample
                     : layers > 1        ? MTLTextureType2DArray
                                         : MTLTextureType2D;
    td.arrayLength = r->sample_count > 1 ? 1 : layers;
  }
  td.pixelFormat = pixelFormat(r->format);
  td.width = r->width ? r->width : 1;
  td.height = r->height ? r->height : 1;
  td.depth = r->depth ? r->depth : 1;
  td.mipmapLevelCount = r->mip_levels ? r->mip_levels : 1;
  td.sampleCount = r->sample_count ? r->sample_count : 1;
  td.storageMode = MTLStorageModePrivate;
  td.hazardTrackingMode = MTLHazardTrackingModeUntracked;
  MTLTextureUsage usage = MTLTextureUsageShaderRead;
  if (r->usage & (0x20u | 0x40u)) /* MtlUsage::RenderTarget | DepthStencil */
    usage |= MTLTextureUsageRenderTarget;
  td.usage = usage;
  return td;
}

/* S3 v7: creation runs on the CREATE WORKER (a poisoned or memory-pressured
 * newTextureWithDescriptor must never freeze the consumer — GTA boot proved
 * the 'cheap' assumption wrong, execOp=27). The consumer only clamps,
 * registers the building Resource (stream-order lookups resolve instantly)
 * and dispatches. */
static void handleCreateTexture(const d9mt_wire_rec_create_texture* r) {
  const uint32_t fmt = r->format & 0xFFFFu;
  /* v8: ZERO dims are the pseudo-resource signature (run-8 killer was
   * "1x0x0 layers=0" — the v7 clamps only checked maxima). */
  if (!r->width || !r->height || !r->depth || !r->mip_levels || !r->array_layers)
    { rejectCreate("create_texture", r->wire_id, "zero dimension/mips/layers"); return; }
  if (r->width > 16384u || r->height > 16384u || r->depth > 2048u)
    { rejectCreate("create_texture", r->wire_id, "extent > 16384/2048"); return; }
  if (r->mip_levels > 15u)
    { rejectCreate("create_texture", r->wire_id, "mips > 15"); return; }
  if (r->array_layers > 2048u)
    { rejectCreate("create_texture", r->wire_id, "layers > 2048"); return; }
  if (fmt == 0u || fmt > 640u) /* MTLPixelFormat range (macOS SDK tops out ~600) */
    { rejectCreate("create_texture", r->wire_id, "format outside known enum"); return; }
  if (r->sample_count > 8u)
    { rejectCreate("create_texture", r->wire_id, "sampleCount > 8"); return; }

  auto res = std::make_shared<Resource>();
  res->kind = Resource::KindTexture;
  res->wireId = r->wire_id;
  res->clientHandle = r->client_handle;
  res->ready.store(0, std::memory_order_relaxed);
  registerResource(res);
  g->asyncCreates.push_back(res);

  Exec::CreateJob job;
  job.kind = Exec::CreateJob::KindTexture;
  job.res = std::move(res);
  job.tex = *r;
  {
    std::lock_guard<std::mutex> lock(g->creatMutex);
    g->creatQueue.push_back(std::move(job));
  }
  g->creatCv.notify_one();
}

static uint64_t monotonicMsSrv() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* S3 v8 watchdog thread — V19: it also OWNS the liveness heartbeat.
 *
 * V19 LIVENESS LAW: liveness is a property of the process, not of work
 * arriving. status->heartbeat is bumped HERE, every 10 ms, unconditionally,
 * for as long as the process runs. It used to be bumped by the consumer
 * thread between records, which silently made liveness depend on the
 * consumer loop cycling: any long record handler (or any park anomaly)
 * froze the heartbeat, the client latched "dead server" and stopped
 * producing forever, the server drained and idled forever — distributed
 * mutual starvation, with BOTH processes healthy. The watchdog is the one
 * thread that is always sleeping in a timer loop and touches no Metal or
 * ring state, so it can never be wedged by workload.
 *
 * Diagnostics (S3 v8, unchanged, every second): name (a) a create-worker
 * Metal call that has not returned in >5 s — with the exact values that
 * wedged it — and (b) a consumer stuck inside one wire opcode with NO
 * progress for >5 s. Diagnostics only (a stuck ObjC call cannot be
 * aborted); one line per incident. */
static void watchdogMain() {
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0); /* not frame-critical */
  uint64_t warnedJob = 0;
  uint32_t lastOp = 0;
  uint64_t lastProgress = 0, opSinceMs = 0;
  bool warnedOp = false;
  uint32_t tick = 0;
  while (!g->watchdogStop.load(std::memory_order_acquire)) {
    usleep(10000); /* 10 ms — the ~100 Hz heartbeat cadence */
    atomic_fetch_add_explicit(&g->status->heartbeat, 1, memory_order_relaxed);
    /* occlusion results: the guaranteed publisher. A game may stop sending
     * records entirely while it spins on GetData — completion-gated
     * publication must not depend on the consumer loop cycling. Touches
     * only the queryMu-guarded state + the shared-event value (both
     * thread-safe); worst-case poll-to-publish latency is one tick. */
    sweepQueries();
    if (++tick < 100)
      continue; /* diagnostics below stay on their 1 s cadence */
    tick = 0;
    const uint64_t now = monotonicMsSrv();
    const uint64_t js = g->workerJobStartMs.load(std::memory_order_acquire);
    if (js && now - js > 5000 && warnedJob != js) {
      warnedJob = js;
      fprintf(stderr, "[serverd] WEDGED: create worker stuck %llus in %s\n",
              (unsigned long long)((now - js) / 1000), g->workerJobDesc);
    }
    const uint32_t op =
        atomic_load_explicit(&g->status->exec_opcode, memory_order_relaxed);
    const uint64_t prog =
        atomic_load_explicit(&g->status->progress, memory_order_relaxed);
    if (op == 0 || op != lastOp || prog != lastProgress) {
      lastOp = op;
      lastProgress = prog;
      opSinceMs = now;
      warnedOp = false;
    } else if (op && now - opSinceMs > 5000 && !warnedOp) {
      warnedOp = true;
      fprintf(stderr, "[serverd] WEDGED: consumer stuck inside wire opcode %u "
                      "for %llus (progress frozen)\n",
              op - 1, (unsigned long long)((now - opSinceMs) / 1000));
    }
  }
}

/* S3 v2: the create worker. newLibraryWithData takes milliseconds per
 * library and boot storms carry hundreds — running it on the consumer
 * starved ring consumption (and heap acks) for >5 s and tripped the client
 * watchdog in a real game. The worker builds the library + Shader payload
 * and publishes through Resource::ready; the consumer registered the
 * (building) resource synchronously so stream-order lookups always resolve. */
static void createWorkerMain() {
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  for (;;) {
    Exec::CreateJob job;
    {
      std::unique_lock<std::mutex> lock(g->creatMutex);
      g->creatCv.wait(lock, [] { return g->creatStop || !g->creatQueue.empty(); });
      if (g->creatQueue.empty())
        return; /* stop requested and drained */
      job = std::move(g->creatQueue.front());
      g->creatQueue.pop_front();
    }
    @autoreleasepool {
      switch (job.kind) {
      case Exec::CreateJob::KindShaderLib: {
        dispatch_data_t data = dispatch_data_create(
            job.bytes.data(), job.bytes.size(), NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError* err = nil;
        snprintf(g->workerJobDesc, sizeof(g->workerJobDesc),
                 "newLibraryWithData wire=%llu (%zu B)",
                 (unsigned long long)job.meta.wire_id, job.bytes.size());
        g->workerJobStartMs.store(monotonicMsSrv(), std::memory_order_release);
        id<MTLLibrary> lib = [g->device newLibraryWithData:data error:&err];
        g->workerJobStartMs.store(0, std::memory_order_release);
        if (!lib) {
          job.res->ready.store(2, std::memory_order_release);
          atomic_fetch_add_explicit(&g->status->exec_errors, 1, memory_order_relaxed);
          static std::atomic<bool> warned = { false };
          bool expected = false;
          if (warned.compare_exchange_strong(expected, true))
            fprintf(stderr, "[serverd] first newLibraryWithData failure: %s\n",
                    err ? err.localizedDescription.UTF8String : "(no error)");
          break;
        }
        auto sh = std::make_shared<Shader>();
        sh->library = lib;
        char entry[33] = {};
        std::memcpy(entry, job.meta.entry, 32);
        sh->entry = entry[0] ? entry : "main";
        sh->stage = job.meta.stage;
        sh->ab = job.meta.ab != 0;
        std::memcpy(sh->cbvMap, job.meta.cbv_slot_map, 8);
        sh->abTableIndex = job.meta.ab_table_index ? job.meta.ab_table_index : 21u;
        sh->abWords = std::move(job.abWords);
        job.res->shader = std::move(sh);
        job.res->ready.store(1, std::memory_order_release);
        break;
      }
      case Exec::CreateJob::KindTexture: {
        /* v7 breadcrumb BEFORE the Metal call: the values a hang leaves in
         * the log ARE the poisoned values. First 64 creates + anything big,
         * plus a running memory estimate (dual-execution doubles texture
         * memory — pressure is a hang candidate of its own). */
        static std::atomic<uint64_t> count = { 0 };
        static std::atomic<uint64_t> bytesEst = { 0 };
        const d9mt_wire_rec_create_texture& t = job.tex;
        const uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
        const uint64_t est = (uint64_t)(t.width ? t.width : 1) *
                             (t.height ? t.height : 1) * (t.depth ? t.depth : 1) *
                             (t.array_layers ? t.array_layers : 1) * 4u;
        const uint64_t cum = bytesEst.fetch_add(est, std::memory_order_relaxed) + est;
        if (n < 64 || t.width >= 4096 || t.height >= 4096 || t.array_layers > 6)
          fprintf(stderr, "[serverd] create_texture #%llu wire=%llu fmt=%u "
                          "%ux%ux%u mips=%u layers=%u smp=%u usage=0x%x cube=%u "
                          "(~%llu MB cum)\n",
                  (unsigned long long)n, (unsigned long long)t.wire_id,
                  t.format & 0xFFFFu, t.width, t.height, t.depth, t.mip_levels,
                  t.array_layers, t.sample_count, t.usage, t.cube,
                  (unsigned long long)(cum >> 20));
        snprintf(g->workerJobDesc, sizeof(g->workerJobDesc),
                 "newTexture wire=%llu fmt=%u %ux%ux%u mips=%u layers=%u usage=0x%x",
                 (unsigned long long)t.wire_id, t.format & 0xFFFFu, t.width,
                 t.height, t.depth, t.mip_levels, t.array_layers, t.usage);
        g->workerJobStartMs.store(monotonicMsSrv(), std::memory_order_release);
        if (g->wedgeTestMs) { /* TEST HOOK: simulate a wedged create once */
          static std::atomic<bool> wedgedOnce = { false };
          bool expected = false;
          if (wedgedOnce.compare_exchange_strong(expected, true)) {
            fprintf(stderr, "[serverd] TEST: wedging create worker %u ms\n",
                    g->wedgeTestMs);
            usleep((useconds_t)g->wedgeTestMs * 1000u);
          }
        }
        id<MTLTexture> tex = [g->device newTextureWithDescriptor:textureDescriptorFor(&t)];
        g->workerJobStartMs.store(0, std::memory_order_release);
        if (!tex) {
          job.res->ready.store(2, std::memory_order_release);
          atomic_fetch_add_explicit(&g->status->exec_errors, 1, memory_order_relaxed);
          fprintf(stderr, "[serverd] create_texture wire=%llu FAILED (nil)\n",
                  (unsigned long long)t.wire_id);
          break;
        }
        job.res->texture = tex;
        job.res->ready.store(1, std::memory_order_release);
        break;
      }
      case Exec::CreateJob::KindSampler: {
        const d9mt_wire_rec_create_sampler& r = job.smp;
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = (MTLSamplerMinMagFilter)r.min_filter;
        sd.magFilter = (MTLSamplerMinMagFilter)r.mag_filter;
        sd.mipFilter = (MTLSamplerMipFilter)r.mip_mode;
        sd.sAddressMode = (MTLSamplerAddressMode)r.address_u;
        sd.tAddressMode = (MTLSamplerAddressMode)r.address_v;
        sd.rAddressMode = (MTLSamplerAddressMode)r.address_w;
        sd.borderColor = (MTLSamplerBorderColor)r.border_color;
        sd.compareFunction = r.compare_enable ? (MTLCompareFunction)r.compare_op
                                              : MTLCompareFunctionNever;
        sd.lodMinClamp = r.min_lod;
        sd.lodMaxClamp = r.max_lod;
        sd.maxAnisotropy = r.max_anisotropy ? r.max_anisotropy : 1;
        sd.normalizedCoordinates = YES;
        sd.supportArgumentBuffers = YES;
        snprintf(g->workerJobDesc, sizeof(g->workerJobDesc),
                 "newSamplerState wire=%llu", (unsigned long long)r.wire_id);
        g->workerJobStartMs.store(monotonicMsSrv(), std::memory_order_release);
        id<MTLSamplerState> smp = [g->device newSamplerStateWithDescriptor:sd];
        g->workerJobStartMs.store(0, std::memory_order_release);
        if (!smp) {
          job.res->ready.store(2, std::memory_order_release);
          atomic_fetch_add_explicit(&g->status->exec_errors, 1, memory_order_relaxed);
          break;
        }
        job.res->sampler = smp;
        job.res->ready.store(1, std::memory_order_release);
        break;
      }
      }
    }
  }
}

static void handleCreateShader(const d9mt_wire_rec_create_shader* r) {
  PendingShader& p = g->pendingShaders[r->client_handle];
  if (r->chunk_offset == 0) {
    p.meta = *r;
    p.bytes.assign((size_t)r->total_size, 0);
    p.abWords.clear();
    p.received = 0;
    const uint8_t* tail = (const uint8_t*)(r + 1);
    for (uint32_t i = 0; i < r->ab_word_count; i++)
      p.abWords.emplace_back(tail[i * 2], tail[i * 2 + 1]);
  }
  uint32_t abPad = r->chunk_offset == 0 ? ((r->ab_word_count * 2 + 7u) & ~7u) : 0;
  const uint8_t* payload = (const uint8_t*)(r + 1) + abPad;
  if (r->chunk_offset + r->chunk_size <= p.bytes.size()) {
    std::memcpy(p.bytes.data() + r->chunk_offset, payload, r->chunk_size);
    p.received += r->chunk_size;
  } else {
    execError("create_shader chunk out of range");
  }
  if (p.received < p.meta.total_size)
    return;

  /* all chunks in: register the resource NOW (building) so the very next
   * bind_shader resolves it, and hand the expensive library build to the
   * create worker. The consumer never blocks here. */
  auto res = std::make_shared<Resource>();
  res->kind = Resource::KindShader;
  res->wireId = p.meta.wire_id;
  res->clientHandle = p.meta.client_handle;
  res->ready.store(0, std::memory_order_relaxed);
  registerResource(res);

  Exec::CreateJob job;
  job.res = std::move(res);
  job.bytes = std::move(p.bytes);
  job.meta = p.meta;
  job.abWords = std::move(p.abWords);
  {
    std::lock_guard<std::mutex> lock(g->creatMutex);
    g->creatQueue.push_back(std::move(job));
  }
  g->creatCv.notify_one();
  g->pendingShaders.erase(r->client_handle);
}

static void handleCreateSampler(const d9mt_wire_rec_create_sampler* r) {
  /* S3 v7: sampler creation also runs on the worker (consumer stays
   * Metal-free: decode, dispatch, memcpy only). A building sampler binds
   * as nil -> dummy until ready; D3D9 games rebind per draw. */
  auto res = std::make_shared<Resource>();
  res->kind = Resource::KindSampler;
  res->wireId = r->wire_id;
  res->clientHandle = r->client_handle;
  res->ready.store(0, std::memory_order_relaxed);
  registerResource(res);

  Exec::CreateJob job;
  job.kind = Exec::CreateJob::KindSampler;
  job.res = std::move(res);
  job.smp = *r;
  {
    std::lock_guard<std::mutex> lock(g->creatMutex);
    g->creatQueue.push_back(std::move(job));
  }
  g->creatCv.notify_one();
}

static void handleDestroy(const d9mt_wire_rec_destroy_resource* r) {
  auto res = r->client_handle ? lookupHandle(r->client_handle) : lookupWire(r->wire_id);
  if (!res)
    return; /* create may have failed or never crossed — not an error */
  /* deferred destruction: the current/last submission may still reference it */
  uint64_t retireAfter = g->cmdbuf ? g->serial : g->lastSubmitted;
  if (res->buffer)
    g->retire.emplace_back(retireAfter, res->buffer);
  if (res->texture)
    g->retire.emplace_back(retireAfter, res->texture);
  if (res->sampler)
    g->retireNoRes.emplace_back(retireAfter, res->sampler);
  for (auto& v : res->views)
    if (v.second)
      g->retireNoRes.emplace_back(retireAfter, v.second);
  res->stash.clear();
  if (res->clientHandle)
    g->byHandle.erase(res->clientHandle);
  if (res->wireId)
    g->byWire.erase(res->wireId);
}

/* --- S3 v7: async-create observation (consumer thread only) -------------------
 * The worker publishes {texture/sampler, ready}; the consumer applies its own
 * bookkeeping — residency (MTLResidencySet mutation stays single-threaded)
 * and stashed uploads — the first time it OBSERVES ready==1. Returns true
 * only when the resource is ready and observed. */

static void drainStash(const std::shared_ptr<Resource>& res);

static bool observeReady(const std::shared_ptr<Resource>& res) {
  const uint32_t state = res->ready.load(std::memory_order_acquire);
  if (state != 1)
    return false;
  if (res->kind == Resource::KindTexture && res->texture && !res->residencyAdded) {
    residencyAdd(res->texture);
    res->residencyAdded = true;
  }
  if (!res->stash.empty())
    drainStash(res);
  return true;
}

/* Sweep point (once per present): resources built since the last sweep get
 * residency + stash drains even if nothing referenced them yet. */
static void sweepAsyncCreates() {
  size_t w = 0;
  for (size_t i = 0; i < g->asyncCreates.size(); i++) {
    std::shared_ptr<Resource>& res = g->asyncCreates[i];
    const uint32_t state = res->ready.load(std::memory_order_acquire);
    if (state == 0) {
      g->asyncCreates[w++] = std::move(res); /* still building — keep */
      continue;
    }
    /* destroyed before it finished building? drop without bookkeeping */
    const bool live =
        (res->clientHandle && lookupHandle(res->clientHandle) == res) ||
        (res->wireId && lookupWire(res->wireId) == res);
    if (live && state == 1)
      observeReady(res);
    else
      res->stash.clear();
  }
  g->asyncCreates.resize(w);
}

/* --- S3: shared-heap content uploads + real texture binds --------------------- */

/* Resolve a heap region to a CPU pointer in the server's mapping. */
static const uint8_t* heapRegion(uint64_t offset, uint64_t size) {
  if (!g->heap || !size)
    return nullptr;
  const uint64_t cap = g->heap->capacity;
  const uint64_t idx = offset & (cap - 1u);
  if (size > cap || idx + size > cap)
    return nullptr; /* regions never straddle the wrap by contract */
  return (const uint8_t*)g->heap + g->heap->data_offset + idx;
}

static void handleUploadBuffer(const d9mt_wire_rec_upload_buffer* r) {
  auto res = r->client_handle ? lookupHandle(r->client_handle) : lookupWire(r->wire_id);
  if (!res || !res->buffer)
    return; /* create failed / never crossed — same policy as binds */
  const uint8_t* src = heapRegion(r->heap_offset, r->size);
  if (!src) {
    execError("upload_buffer: bad heap region");
    return;
  }
  if (r->dst_offset + r->size > res->buffer.length) {
    execError("upload_buffer: out of range");
    return;
  }
  res->uploadSum = fnv64(src, (size_t)r->size); /* S3 v13 parity (client twin
    * sums the same bytes at emission — mtl_wire_context emitUploadBuffer) */
  /* S3 v14: capture the shipped-bytes window so logServerDraw reports the
   * constants the draw will ACTUALLY read, not the record-time (pre-blit)
   * buffer.contents that freezes at the fresh rotating offset. */
  res->lastUploadWindow = fnv64(src, r->size < 64u ? (size_t)r->size : 64u);
  res->lastUploadOffset = r->dst_offset;

  /* V15 THE PASS-SHATTER FIX: wire buffers are shared storage; a range no
   * encoded-but-incomplete GPU work references takes a plain memcpy — no
   * encoder, no ensureXfer, no render-pass close. The D3D9 constant-ring
   * shape (fresh offset per draw, rename on wrap) hits this path ~always. */
  if (res->buffer.storageMode == MTLStorageModeShared &&
      !rangeLive(res, r->dst_offset, r->size)) {
    std::memcpy((uint8_t*)res->buffer.contents + r->dst_offset, src,
                (size_t)r->size);
    heapAck(r->heap_generation);
    res->hasContent = true;
    g->uplDirect++;
    atomic_fetch_add_explicit(&g->status->uploads_executed, 1, memory_order_relaxed);
    return;
  }

  /* Overlapping (or non-shared) destination: stage into the slab NOW so the
   * heap ack flows, and PARK the blit for the next natural pass boundary —
   * earlier-encoded draws of the open pass execute before the blit (stream
   * semantics preserved), later consumers force the flush at prepareDraw. */
  ensureCmdbuf(); /* ringAlloc stamps the current recording serial */
  RingSlice rs = ringAlloc(r->size, 256);
  if (!rs.cpu) {
    execError("upload_buffer: slab alloc failed");
    return;
  }
  std::memcpy(rs.cpu, src, (size_t)r->size);
  heapAck(r->heap_generation); /* region free the moment the copy is taken */
  DeferredXfer d;
  d.res = res;
  d.srcBuf = rs.buf;
  d.srcOff = rs.off;
  d.dstOff = r->dst_offset;
  d.size = r->size;
  g->deferredXfers.push_back(std::move(d));
  res->pendingXfers++;
  if (r->dst_offset < res->xferMin)
    res->xferMin = r->dst_offset;
  if (r->dst_offset + r->size > res->xferMax)
    res->xferMax = r->dst_offset + r->size;
  res->hasContent = true;
  g->uplStaged++;
  atomic_store_explicit(&g->status->upl_staged, g->uplStaged,
                        memory_order_relaxed);
  atomic_fetch_add_explicit(&g->status->uploads_executed, 1, memory_order_relaxed);
}

/* Encode one slab->texture blit into the shared xfer encoder NOW (the
 * caller is at a natural boundary or explicitly flushing). */
static void encodeTextureUploadNow(const std::shared_ptr<Resource>& res,
                                   const d9mt_wire_rec_upload_texture* r,
                                   id<MTLBuffer> srcBuf, uint64_t srcOff) {
  id<MTL4ComputeCommandEncoder> enc = ensureXfer();
  if (!enc)
    return;
  MTLSize sz = MTLSizeMake(r->width ? r->width : 1, r->height ? r->height : 1,
                           r->depth ? r->depth : 1);
  MTLOrigin origin = MTLOriginMake(r->x, r->y, r->z);
  [enc copyFromBuffer:srcBuf
         sourceOffset:(NSUInteger)srcOff
    sourceBytesPerRow:(NSUInteger)r->row_pitch
  sourceBytesPerImage:(NSUInteger)r->slice_pitch
           sourceSize:sz
            toTexture:res->texture
     destinationSlice:(NSUInteger)r->layer
     destinationLevel:(NSUInteger)r->mip
    destinationOrigin:origin];
}

/* FLOW-CONTROL: drain every parked texture blit (stream order preserved —
 * one FIFO vector). Reentrancy-safe by swap: encodeTextureUploadNow calls
 * ensureXfer, whose open path calls back into this drain. */
static void drainDeferredTexXfers() {
  while (!g->deferredTexXfers.empty()) {
    std::vector<DeferredTexXfer> q;
    q.swap(g->deferredTexXfers);
    for (DeferredTexXfer& t : q) {
      encodeTextureUploadNow(t.res, &t.rec, t.srcBuf, t.srcOff);
      if (t.res->pendingTexXfers)
        t.res->pendingTexXfers--;
    }
  }
}

/* Take one texture-content upload from CPU bytes (heap region or stash):
 * slab-copy the bytes NOW (the heap region detaches immediately), then
 * encode the blit — or, when a render pass is OPEN, park it for the next
 * natural boundary (FLOW-CONTROL: mid-frame texture streaming must not
 * close render passes; a same-pass consumer forces the flush at
 * prepareDraw, counted uplFlushUse). */
static void encodeTextureUpload(const std::shared_ptr<Resource>& res,
                                const d9mt_wire_rec_upload_texture* r,
                                const uint8_t* src, bool allowPark) {
  id<MTLTexture> tex = res->texture;
  if (r->mip >= tex.mipmapLevelCount || r->layer >= tex.arrayLength * (tex.textureType == MTLTextureTypeCube || tex.textureType == MTLTextureTypeCubeArray ? 6 : 1)) {
    execError("upload_texture: subresource out of range");
    return;
  }
  ensureCmdbuf(); /* ringAlloc stamps the current recording serial */
  RingSlice rs = ringAlloc(r->data_size, 256);
  if (!rs.cpu) {
    execError("upload_texture: slab alloc failed");
    return;
  }
  std::memcpy(rs.cpu, src, (size_t)r->data_size);
  res->hasContent = true; /* bytes are owned; the blit is ordered before any
                           * later pass (boundary drain / consumer flush) */
  atomic_fetch_add_explicit(&g->status->uploads_executed, 1, memory_order_relaxed);
  { static int n = 0;
    if (n++ < 8)
      fprintf(stderr, "[serverd] upload_texture #%d: wire=%llu fmt=%u mip=%u "
                      "box %u,%u %ux%u\n", n, (unsigned long long)r->wire_id,
              r->format & 0xFFFFu, r->mip, r->x, r->y, r->width, r->height); }
  /* allowPark=false: the stash-drain path (observeReady) can run INSIDE
   * draw preparation, after the consumer-flush check already passed — a
   * parked blit there would let a just-promoted bind sample stale content.
   * That path keeps the immediate encode (pass close on a create-race boot
   * transient only — the streaming hot path is the allowPark one). */
  if (allowPark && g->pass.active) {
    DeferredTexXfer t;
    t.res = res;
    t.rec = *r;
    t.srcBuf = rs.buf;
    t.srcOff = rs.off;
    g->deferredTexXfers.push_back(std::move(t));
    res->pendingTexXfers++;
    g->uplTexParked++;
    return;
  }
  encodeTextureUploadNow(res, r, rs.buf, rs.off);
}

static void drainStash(const std::shared_ptr<Resource>& res) {
  for (auto& s : res->stash)
    encodeTextureUpload(res, &s.rec, s.bytes.data(), false /* see above */);
  res->stash.clear();
  res->stash.shrink_to_fit();
}

static void handleUploadTexture(const d9mt_wire_rec_upload_texture* r) {
  auto res = r->client_handle ? lookupHandle(r->client_handle) : lookupWire(r->wire_id);
  if (!res)
    return;
  const uint8_t* src = heapRegion(r->heap_offset, r->data_size);
  if (!src) {
    execError("upload_texture: bad heap region");
    return;
  }
  const uint32_t state = res->ready.load(std::memory_order_acquire);
  if (state == 0) {
    /* v7: texture still building on the worker — STASH the bytes (the heap
     * region is acked immediately; the blit encodes when the consumer first
     * observes readiness). Order within the stash is stream order. */
    Resource::StashedUpload s;
    s.rec = *r;
    s.bytes.assign(src, src + r->data_size);
    res->stash.push_back(std::move(s));
    heapAck(r->heap_generation);
    return;
  }
  if (state != 1 || !res->texture) {
    heapAck(r->heap_generation);
    return; /* failed create: content dropped, resource dummy-resolves */
  }
  observeReady(res); /* residency + any earlier stash first (stream order) */
  encodeTextureUpload(res, r, src, true); /* memcpys the region into the slab... */
  heapAck(r->heap_generation);      /* ...ack only after the bytes are taken */
}

/* Resolve the bound view: the base texture when the key is trivial, else a
 * cached newTextureViewWithPixelFormat view. Unmintable views fall back to
 * the base texture (defined, possibly wrong-format sampling — better than
 * the dummy for content). view_type reinterpretation (e.g. cube-as-2D) is
 * NOT modeled beyond level/slice ranges — S3 leftover. */
static id<MTLTexture> resolveTexView(const std::shared_ptr<Resource>& res,
                                     const d9mt_wire_rec_bind_texture_by_id* r) {
  id<MTLTexture> tex = res->texture;
  MTLPixelFormat fmt = pixelFormat(r->format);
  const NSUInteger levels = tex.mipmapLevelCount;
  const bool cube = tex.textureType == MTLTextureTypeCube ||
                    tex.textureType == MTLTextureTypeCubeArray;
  const NSUInteger slices = tex.arrayLength * (cube ? 6 : 1);
  const bool wholeMips =
      r->base_mip == 0 && (r->mip_count == 0 || r->mip_count >= levels);
  const bool wholeLayers =
      r->base_layer == 0 && (r->layer_count == 0 || r->layer_count >= slices);
  if ((fmt == tex.pixelFormat || fmt == MTLPixelFormatInvalid) && wholeMips && wholeLayers)
    return tex;
  if (r->base_mip >= levels || r->base_layer >= slices)
    return tex;

  TexViewKey key;
  key.format = r->format;
  key.viewType = r->view_type;
  key.baseMip = r->base_mip;
  key.mipCount = r->mip_count;
  key.baseLayer = r->base_layer;
  key.layerCount = r->layer_count;
  for (auto& v : res->views)
    if (v.first == key)
      return v.second ? v.second : tex;

  NSUInteger mipN = r->mip_count ? r->mip_count : (NSUInteger)(levels - r->base_mip);
  if (r->base_mip + mipN > levels)
    mipN = levels - r->base_mip;
  NSUInteger layerN = r->layer_count ? r->layer_count : (NSUInteger)(slices - r->base_layer);
  if (r->base_layer + layerN > slices)
    layerN = slices - r->base_layer;
  id<MTLTexture> view =
      [tex newTextureViewWithPixelFormat:(fmt == MTLPixelFormatInvalid ? tex.pixelFormat : fmt)
                             textureType:tex.textureType
                                  levels:NSMakeRange(r->base_mip, mipN)
                                  slices:NSMakeRange(r->base_layer, layerN)];
  res->views.emplace_back(key, view); /* nil cached too (no retry churn) */
  return view ? view : tex;
}

static void handleBindTextureById(const d9mt_wire_rec_bind_texture_by_id* r) {
  id<MTLTexture> tex = nil;
  bool pend = false;
  std::shared_ptr<Resource> res;
  if (r->image_wire_id) {
    res = lookupWire(r->image_wire_id);
    /* content gate: a wire texture with no uploads and no server-side render
     * has UNDEFINED private storage — the dummy stays (defined sampling).
     * v7: a still-building texture also stays dummy. V18: the slot REMEMBERS
     * the bind (texPend) and draws promote it once ready+content — apps
     * that bind once must not sample the dummy forever. */
    if (res && observeReady(res) && res->texture && res->hasContent)
      tex = resolveTexView(res, r);
    else if (res && res->ready.load(std::memory_order_acquire) != 2)
      pend = true; /* building, or created with content still in flight */
  }
  for (uint32_t st = 0; st < 2; st++) {
    if (r->stages & (1u << st) && r->slot < kMaxTexTrack) {
      StageState& b = g->stages[st];
      b.textures[r->slot] = tex;
      b.texRes[r->slot] = res; /* FLOW-CONTROL: parked-blit consumer check */
      b.texBound[r->slot] = r->image_wire_id != 0;
      if (pend && !b.texPendSlot[r->slot])
        b.texPendCount++;
      else if (!pend && b.texPendSlot[r->slot])
        b.texPendCount--;
      b.texPendSlot[r->slot] = pend;
      if (pend)
        b.texPendRec[r->slot] = *r;
    }
  }
  g->dirtyBindings = true;
}

/* V18 bind-once promotion (see StageState::texPendRec): re-resolve dummy-
 * bound slots whose texture has since turned ready+content. Runs on the
 * draw-prepare path; returns true when any slot changed so the argument
 * tables re-materialize. A FAILED create (ready==2) or a vanished wire id
 * clears the pend — the dummy stays by design (defined sampling). */
static bool promotePendingTexBinds() {
  bool changed = false;
  for (uint32_t st = 0; st < 2; st++) {
    StageState& b = g->stages[st];
    if (!b.texPendCount)
      continue;
    for (uint32_t slot = 0; slot < kMaxTexTrack && b.texPendCount; slot++) {
      if (!b.texPendSlot[slot])
        continue;
      const d9mt_wire_rec_bind_texture_by_id& r = b.texPendRec[slot];
      auto res = lookupWire(r.image_wire_id);
      if (!res || res->ready.load(std::memory_order_acquire) == 2) {
        b.texPendSlot[slot] = false;
        b.texPendCount--;
        continue;
      }
      if (observeReady(res) && res->texture && res->hasContent) {
        b.textures[slot] = resolveTexView(res, &r);
        b.texRes[slot] = res; /* FLOW-CONTROL: keep the backref exact */
        b.texPendSlot[slot] = false;
        b.texPendCount--;
        changed = true;
      }
    }
  }
  return changed;
}

/* --- S3 v9: the GPU-side copy/blit family --------------------------------------
 * Port of mtl_context_transfer.cpp: copies via the shared xfer compute
 * encoder (closes render passes, §4c producer barrier at closeXfer); the
 * resolve is a draw-less Load/MultisampleResolve pass; the meta-blit is a
 * sampled fullscreen-triangle draw through its own tiny library. */

static const char* kBlitMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct P { float4 uv; };
struct V { float4 pos [[position]]; float2 uv; };
vertex V d9mtBlitVs(uint vid [[vertex_id]], constant P& p [[buffer(0)]]) {
  float2 t = float2(vid == 1 ? 2.0 : 0.0, vid == 2 ? 2.0 : 0.0);
  V o;
  o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
  o.uv = mix(p.uv.xy, p.uv.zw, t);
  return o;
}
fragment float4 d9mtBlitFs(V in [[stage_in]],
                           texture2d<float> src [[texture(0)]],
                           sampler smp [[sampler(0)]]) {
  return src.sample(smp, in.uv);
}
/* V15 partial clears: fullscreen triangle at z = clear depth, scissored to
 * the clear rect. Depth/stencil aspects run WITHOUT a fragment function
 * (vertex-only pipeline: depth from rasterization, stencil from the Replace
 * DSSO + reference); the color aspect adds the constant-color fragment. */
struct FillP { float4 color; float depth; };
struct FillV { float4 pos [[position]]; };
vertex FillV d9mtFillVs(uint vid [[vertex_id]], constant FillP& p [[buffer(0)]]) {
  float2 t = float2(vid == 1 ? 2.0 : 0.0, vid == 2 ? 2.0 : 0.0);
  FillV o;
  o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, p.depth, 1.0);
  return o;
}
fragment float4 d9mtFillFs(constant FillP& p [[buffer(0)]]) {
  return p.color;
}
)MSL";

static std::shared_ptr<Resource> resolveCopyRes(uint64_t handle, uint64_t wireId) {
  auto res = handle ? lookupHandle(handle) : lookupWire(wireId);
  if (!res && wireId)
    res = lookupWire(wireId);
  return res;
}

/* false = a side is still building (stash + retry at present). */
static bool tryExecuteCopyTexture(const d9mt_wire_rec_copy_texture* r) {
  auto dres = resolveCopyRes(r->dst_handle, r->dst_wire_id);
  auto sres = resolveCopyRes(r->src_handle, r->src_wire_id);
  if (!dres || !sres)
    return true; /* unresolvable — drop */
  if (dres->ready.load(std::memory_order_acquire) == 0 ||
      sres->ready.load(std::memory_order_acquire) == 0)
    return false; /* building — retry */
  if (!observeReady(dres) || !observeReady(sres) ||
      !dres->texture || !sres->texture)
    return true; /* failed create — drop */
  id<MTL4ComputeCommandEncoder> enc = ensureXfer();
  if (!enc)
    return true;
  MTLSize sz = MTLSizeMake(r->width ? r->width : 1, r->height ? r->height : 1,
                           r->depth ? r->depth : 1);
  [enc copyFromTexture:sres->texture
           sourceSlice:r->src_layer
           sourceLevel:r->src_mip
          sourceOrigin:MTLOriginMake(r->src_x, r->src_y, r->src_z)
            sourceSize:sz
             toTexture:dres->texture
      destinationSlice:r->dst_layer
      destinationLevel:r->dst_mip
     destinationOrigin:MTLOriginMake(r->dst_x, r->dst_y, r->dst_z)];
  dres->hasContent = true;
  { static int n = 0;
    if (n++ < 8)
      fprintf(stderr, "[serverd] copy_texture #%d: %llu->%llu %ux%u @%u,%u\n", n,
              (unsigned long long)r->src_wire_id, (unsigned long long)r->dst_wire_id,
              r->width, r->height, r->dst_x, r->dst_y); }
  return true;
}

static void handleCopyTexture(const d9mt_wire_rec_copy_texture* r) {
  if (!tryExecuteCopyTexture(r) && g->pendingCopies.size() < 8192)
    g->pendingCopies.push_back(*r);
}

static void handleCopyBuffer(const d9mt_wire_rec_copy_buffer* r) {
  auto dres = resolveCopyRes(r->dst_handle, r->dst_wire_id);
  auto sres = resolveCopyRes(r->src_handle, r->src_wire_id);
  if (!dres || !sres || !dres->buffer || !sres->buffer || !r->bytes)
    return;
  if (r->dst_offset + r->bytes > dres->buffer.length ||
      r->src_offset + r->bytes > sres->buffer.length) {
    execError("copy_buffer: out of range");
    return;
  }
  /* V15: parked slab blits into either side must encode BEFORE this copy
   * (stream order); the copy itself is encoded GPU work on both ranges. */
  if (!g->deferredXfers.empty() &&
      (sres->pendingXfers || dres->pendingXfers))
    flushDeferredXfers();
  id<MTL4ComputeCommandEncoder> enc = ensureXfer();
  if (!enc)
    return;
  [enc copyFromBuffer:sres->buffer
         sourceOffset:(NSUInteger)r->src_offset
             toBuffer:dres->buffer
    destinationOffset:(NSUInteger)r->dst_offset
                 size:(NSUInteger)r->bytes];
  markBufRef(sres, r->src_offset, r->bytes);
  markBufRef(dres, r->dst_offset, r->bytes);
  dres->hasContent = true;
  /* v13.1 parity: propagate the shipped-content sum through the GPU copy
   * (client twin propagates the same mix at emission). */
  dres->uploadSum = mixCopySum(sres->uploadSum, r->src_offset, r->dst_offset,
                               r->bytes);
}

static void handleResolveTexture(const d9mt_wire_rec_resolve_texture* r) {
  auto dres = resolveCopyRes(r->dst_handle, r->dst_wire_id);
  auto sres = resolveCopyRes(r->src_handle, r->src_wire_id);
  if (!dres || !sres || !observeReady(dres) || !observeReady(sres) ||
      !dres->texture || !sres->texture)
    return;
  closePass();
  closeXfer();
  ensureCmdbuf();
  MTL4RenderPassDescriptor* desc = [[MTL4RenderPassDescriptor alloc] init];
  desc.colorAttachments[0].texture = sres->texture;
  desc.colorAttachments[0].loadAction = MTLLoadActionLoad;
  desc.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
  desc.colorAttachments[0].level = r->src_mip;
  desc.colorAttachments[0].slice = r->src_layer;
  desc.colorAttachments[0].resolveTexture = dres->texture;
  desc.colorAttachments[0].resolveLevel = r->dst_mip;
  desc.colorAttachments[0].resolveSlice = r->dst_layer;
  desc.renderTargetWidth = (NSUInteger)sres->texture.width >> r->src_mip;
  desc.renderTargetHeight = (NSUInteger)sres->texture.height >> r->src_mip;
  id<MTL4RenderCommandEncoder> enc = [g->cmdbuf renderCommandEncoderWithDescriptor:desc];
  if (!enc) {
    execError("resolve: encoder failed");
    return;
  }
  [enc barrierAfterStages:(MTLStages)kStageFragment
        beforeQueueStages:(MTLStages)kStageAll
        visibilityOptions:(MTL4VisibilityOptions)kVisibilityDevice];
  [enc endEncoding];
  atomic_fetch_add_explicit(&g->status->encoders, 1, memory_order_relaxed);
  dres->hasContent = true;
}

static bool tryExecuteMipgen(const d9mt_wire_rec_generate_mipmaps* r) {
  auto res = resolveCopyRes(r->client_handle, r->wire_id);
  if (!res)
    return true;
  if (res->ready.load(std::memory_order_acquire) == 0)
    return false; /* building — retry */
  if (!observeReady(res) || !res->texture)
    return true;
  id<MTL4ComputeCommandEncoder> enc = ensureXfer();
  if (!enc)
    return true;
  [enc generateMipmapsForTexture:res->texture];
  return true;
}

static void handleGenerateMipmaps(const d9mt_wire_rec_generate_mipmaps* r) {
  if (!tryExecuteMipgen(r) && g->pendingMipgens.size() < 4096)
    g->pendingMipgens.push_back(*r);
}

/* V15: partial-rect clears execute as a scissored fullscreen-triangle
 * mini-pass (the client's depthClearVsMain semantics ported): Load the
 * attachment, draw at z = clear depth with an Always/write DSSO (stencil
 * Replace + reference for the stencil aspect; constant-color fragment for
 * the color aspect), scissor = the clear rect. At GTA's 369 drops/window
 * this is no longer a rare residue — a dropped partial DEPTH clear leaves
 * garbage that can reject every later fragment.
 * Returns 0 while the fill PSO is still compiling (stash + retry at present,
 * same discipline as blits); 1 = executed; -1 = dropped for real. */
static int tryExecutePartialClear(const d9mt_wire_rec_clear_image_view* r,
                                  const std::shared_ptr<Resource>& res,
                                  uint32_t mipW, uint32_t mipH) {
  id<MTLTexture> tex = res->texture;
  if (!g->blitLib)
    return -1; /* no fill shaders — drop (counted by the caller) */
  const bool colorAspect = (r->aspects & 0x1u) != 0;
  const bool depthAspect = (r->aspects & 0x2u) != 0;
  const bool stencilAspect = (r->aspects & 0x4u) != 0;
  const MTLPixelFormat fmt = tex.pixelFormat;
  const bool fmtHasStencil =
      (NSUInteger)fmt == 255 || fmt == MTLPixelFormatDepth32Float_Stencil8 ||
      fmt == MTLPixelFormatStencil8;
  const uint64_t samples = tex.sampleCount ? tex.sampleCount : 1;

  /* fill PSO (async, cached): color variant keys the attachment format */
  const uint64_t key = (colorAspect ? (1ull << 63) | (uint64_t)fmt : 0ull) |
                       (samples << 40);
  auto it = g->fillPsoCache.find(key);
  std::shared_ptr<PsoEntry> entry;
  if (it == g->fillPsoCache.end()) {
    entry = std::make_shared<PsoEntry>();
    g->fillPsoCache.emplace(key, entry);
    MTL4RenderPipelineDescriptor* desc = [[MTL4RenderPipelineDescriptor alloc] init];
    MTL4LibraryFunctionDescriptor* vfd = [[MTL4LibraryFunctionDescriptor alloc] init];
    vfd.library = g->blitLib;
    vfd.name = @"d9mtFillVs";
    desc.vertexFunctionDescriptor = vfd;
    if (colorAspect) {
      MTL4LibraryFunctionDescriptor* ffd = [[MTL4LibraryFunctionDescriptor alloc] init];
      ffd.library = g->blitLib;
      ffd.name = @"d9mtFillFs";
      desc.fragmentFunctionDescriptor = ffd;
      desc.colorAttachments[0].pixelFormat = fmt;
    } /* depth/stencil aspects: vertex-only pipeline, no color attachments */
    desc.rasterSampleCount = (NSUInteger)samples;
    desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    struct d9mt_status* status = g->status;
    atomic_fetch_add_explicit(&status->psos_pending, 1, memory_order_relaxed);
    [g->compiler newRenderPipelineStateWithDescriptor:desc
                                  compilerTaskOptions:nil
                                    completionHandler:^(id<MTLRenderPipelineState> pso,
                                                        NSError* err) {
      if (pso) {
        entry->pso = pso;
        entry->state.store(1, std::memory_order_release);
        atomic_fetch_add_explicit(&status->psos_built, 1, memory_order_relaxed);
      } else {
        entry->state.store(2, std::memory_order_release);
        atomic_fetch_add_explicit(&status->exec_errors, 1, memory_order_relaxed);
        fprintf(stderr, "[serverd] fill PSO build failed: %s\n",
                err ? err.localizedDescription.UTF8String : "(no error)");
      }
      atomic_fetch_sub_explicit(&status->psos_pending, 1, memory_order_relaxed);
    }];
    return 0; /* just kicked — stash */
  }
  entry = it->second;
  const uint32_t state = entry->state.load(std::memory_order_acquire);
  if (state == 0)
    return 0; /* compiling — stash */
  if (state != 1 || !entry->pso)
    return -1; /* failed — drop (counted by the caller) */

  /* DSSO: Always + write for the cleared aspects only (cached) */
  id<MTLDepthStencilState> dsso = nil;
  if (depthAspect || stencilAspect) {
    std::string dkey = std::string("V15pclr:") + (depthAspect ? "D" : "d") +
                       (stencilAspect ? "S" : "s");
    auto dit = g->dssoCache.find(dkey);
    if (dit != g->dssoCache.end()) {
      dsso = dit->second;
    } else {
      MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];
      dd.depthCompareFunction = MTLCompareFunctionAlways;
      dd.depthWriteEnabled = depthAspect;
      if (stencilAspect) {
        MTLStencilDescriptor* sd = [[MTLStencilDescriptor alloc] init];
        sd.stencilCompareFunction = MTLCompareFunctionAlways;
        sd.stencilFailureOperation = MTLStencilOperationReplace;
        sd.depthFailureOperation = MTLStencilOperationReplace;
        sd.depthStencilPassOperation = MTLStencilOperationReplace;
        sd.readMask = 0xFFu;
        sd.writeMask = 0xFFu;
        dd.frontFaceStencil = sd;
        dd.backFaceStencil = sd;
      }
      dsso = [g->device newDepthStencilStateWithDescriptor:dd];
      g->dssoCache.emplace(std::move(dkey), dsso);
    }
    if (!dsso)
      return -1;
  }

  closePass();
  closeXfer();
  ensureCmdbuf();
  MTL4RenderPassDescriptor* desc = [[MTL4RenderPassDescriptor alloc] init];
  if (colorAspect) {
    desc.colorAttachments[0].texture = tex;
    desc.colorAttachments[0].level = r->mip;
    desc.colorAttachments[0].slice = r->layer;
    desc.colorAttachments[0].loadAction = MTLLoadActionLoad;
    desc.colorAttachments[0].storeAction = MTLStoreActionStore;
  } else {
    if (fmt != MTLPixelFormatStencil8) {
      desc.depthAttachment.texture = tex;
      desc.depthAttachment.level = r->mip;
      desc.depthAttachment.slice = r->layer;
      desc.depthAttachment.loadAction = MTLLoadActionLoad;
      desc.depthAttachment.storeAction = MTLStoreActionStore;
    }
    if (fmtHasStencil) {
      desc.stencilAttachment.texture = tex;
      desc.stencilAttachment.level = r->mip;
      desc.stencilAttachment.slice = r->layer;
      desc.stencilAttachment.loadAction = MTLLoadActionLoad;
      desc.stencilAttachment.storeAction = MTLStoreActionStore;
    }
  }
  desc.renderTargetWidth = mipW;
  desc.renderTargetHeight = mipH;
  id<MTL4RenderCommandEncoder> enc = [g->cmdbuf renderCommandEncoderWithDescriptor:desc];
  if (!enc) {
    execError("partial clear: encoder failed");
    return -1;
  }
  atomic_fetch_add_explicit(&g->status->encoders, 1, memory_order_relaxed);
  [enc setRenderPipelineState:entry->pso];
  MTLViewport vp = {0.0, 0.0, (double)mipW, (double)mipH, 0.0, 1.0};
  [enc setViewport:vp];
  MTLScissorRect sc;
  sc.x = r->x < mipW ? r->x : 0;
  sc.y = r->y < mipH ? r->y : 0;
  sc.width = r->width;
  sc.height = r->height;
  if (sc.x + sc.width > mipW)
    sc.width = mipW - sc.x;
  if (sc.y + sc.height > mipH)
    sc.height = mipH - sc.y;
  [enc setScissorRect:sc];
  if (dsso) {
    [enc setDepthStencilState:dsso];
    [enc setStencilReferenceValue:r->value.stencil];
  }
  struct { float color[4]; float depth; float pad[3]; } fill = {};
  std::memcpy(fill.color, r->value.color, sizeof(fill.color));
  fill.depth = r->value.depth;
  RingSlice rs = ringAlloc(sizeof(fill), 256);
  if (!rs.cpu) {
    [enc endEncoding];
    execError("partial clear: slab alloc failed");
    return -1;
  }
  std::memcpy(rs.cpu, &fill, sizeof(fill));
  selectArgumentTablesForPass();
  ArgTablePair& pair = g->argTables[g->currentArgTable];
  [pair.table[0] setAddress:rs.gpu atIndex:0];
  [pair.table[1] setAddress:rs.gpu atIndex:0];
  [enc setArgumentTable:pair.table[0] atStages:MTLRenderStageVertex];
  [enc setArgumentTable:pair.table[1] atStages:MTLRenderStageFragment];
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [enc barrierAfterStages:(MTLStages)kStageFragment
        beforeQueueStages:(MTLStages)kStageAll
        visibilityOptions:(MTL4VisibilityOptions)kVisibilityDevice];
  [enc endEncoding];
  res->hasContent = true;
  g->dirtyPso = g->dirtyState = g->dirtyBindings = true;
  g->argTablesBound = false;
  { static int n = 0;
    if (n++ < 8)
      fprintf(stderr, "[serverd] partial clear #%d: wire=%llu aspects=0x%x "
                      "rect=%u,%u %ux%u of %ux%u\n", n,
              (unsigned long long)r->wire_id, r->aspects,
              (unsigned)sc.x, (unsigned)sc.y, (unsigned)sc.width,
              (unsigned)sc.height, mipW, mipH); }
  return 1;
}

static bool tryExecuteClearImage(const d9mt_wire_rec_clear_image_view* r) {
  auto res = resolveCopyRes(r->client_handle, r->wire_id);
  if (!res)
    return true;
  if (res->ready.load(std::memory_order_acquire) == 0)
    return false; /* building — retry */
  if (!observeReady(res) || !res->texture)
    return true;
  id<MTLTexture> tex = res->texture;
  const uint32_t mipW = (uint32_t)tex.width >> r->mip ? (uint32_t)tex.width >> r->mip : 1u;
  const uint32_t mipH = (uint32_t)tex.height >> r->mip ? (uint32_t)tex.height >> r->mip : 1u;
  const bool fullRect = r->x == 0 && r->y == 0 && r->width >= mipW && r->height >= mipH;
  if (!fullRect) {
    /* V15: partial rects now execute as the scissored fill mini-pass. Only a
     * genuinely unbuildable fill PSO still drops — counted + named, because
     * a dropped partial DEPTH clear leaves garbage that can reject every
     * later fragment. */
    const int rc = tryExecutePartialClear(r, res, mipW, mipH);
    if (rc == 0)
      return false; /* fill PSO compiling — stash + retry at present */
    if (rc < 0) {
      g->clearDsPartialDrop++;
      static int warned = 0;
      if (warned++ < 8) {
        fprintf(stderr, "[serverd] clear_image_view #%d: partial clear DROPPED "
                        "(fill PSO unavailable; wire=%llu aspects=0x%x "
                        "rect=%u,%u %ux%u of %ux%u)\n",
                warned, (unsigned long long)r->wire_id,
                r->aspects, r->x, r->y, r->width, r->height, mipW, mipH);
      }
    }
    return true;
  }
  /* full-subresource: the standalone mini-pass shape (V12: DS aspects too —
   * a full DS clear is a plain load-action clear on the depth/stencil
   * attachment, exactly the client's clearRenderTarget mini-pass; only
   * PARTIAL rects need the depth-clear draw pipeline). */
  if (g->pass.active)
    closePass();
  Pass saved = g->pass;
  g->pass = Pass();
  if (r->aspects & 0x1u) {
    g->pass.color[0].tex = tex;
    g->pass.color[0].format = tex.pixelFormat;
    g->pass.color[0].baseMip = r->mip;
    g->pass.color[0].baseLayer = r->layer;
    g->pass.color[0].named = true;
    g->pass.clearColor[0] = true;
    g->pass.clearValues[0] = MTLClearColorMake(r->value.color[0], r->value.color[1],
                                               r->value.color[2], r->value.color[3]);
  } else {
    g->pass.ds.tex = tex;
    g->pass.ds.format = tex.pixelFormat;
    g->pass.ds.baseMip = r->mip;
    g->pass.ds.baseLayer = r->layer;
    g->pass.ds.named = true;
    if (r->aspects & 0x2u) {
      g->pass.clearDepth = true;
      g->pass.depthClear = r->value.depth;
    }
    if (r->aspects & 0x4u) {
      g->pass.clearStencil = true;
      g->pass.stencilClear = r->value.stencil;
    }
  }
  g->pass.pending = true;
  materializePass();
  closePass();
  g->pass = saved;
  g->dirtyPso = g->dirtyState = true;
  res->hasContent = true;
  return true;
}

static void handleClearImageView(const d9mt_wire_rec_clear_image_view* r) {
  if (!tryExecuteClearImage(r) && g->pendingImgClears.size() < 4096)
    g->pendingImgClears.push_back(*r);
}

/* Meta-blit. Returns false when the dst-format PSO is still compiling — the
 * caller stashes the record and the present sweep retries (a lost blit is
 * the black-menu class this slice exists to fix). */
static bool tryExecuteBlit(const d9mt_wire_rec_blit_texture_view* r) {
  auto dres = resolveCopyRes(r->dst_handle, r->dst_wire_id);
  auto sres = resolveCopyRes(r->src_handle, r->src_wire_id);
  if (!dres || !sres)
    return true; /* unresolvable — drop */
  if (dres->ready.load(std::memory_order_acquire) == 0 ||
      sres->ready.load(std::memory_order_acquire) == 0)
    return false; /* still building — stash + retry at present */
  if (!observeReady(dres) || !observeReady(sres) ||
      !dres->texture || !sres->texture)
    return true; /* failed create — drop */
  if (!g->blitLib)
    return true;

  /* PSO keyed on the DST TEXTURE's real format (a mismatching view format
   * would need a dst view — reinterpretation not modeled; base format is
   * correct for every observed caller). Async build, stash while pending. */
  const uint64_t key = (uint64_t)dres->texture.pixelFormat |
                       ((uint64_t)dres->texture.sampleCount << 32);
  auto it = g->blitPsoCache.find(key);
  std::shared_ptr<PsoEntry> entry;
  if (it == g->blitPsoCache.end()) {
    entry = std::make_shared<PsoEntry>();
    g->blitPsoCache.emplace(key, entry);
    MTL4RenderPipelineDescriptor* desc = [[MTL4RenderPipelineDescriptor alloc] init];
    MTL4LibraryFunctionDescriptor* vfd = [[MTL4LibraryFunctionDescriptor alloc] init];
    vfd.library = g->blitLib;
    vfd.name = @"d9mtBlitVs";
    desc.vertexFunctionDescriptor = vfd;
    MTL4LibraryFunctionDescriptor* ffd = [[MTL4LibraryFunctionDescriptor alloc] init];
    ffd.library = g->blitLib;
    ffd.name = @"d9mtBlitFs";
    desc.fragmentFunctionDescriptor = ffd;
    desc.colorAttachments[0].pixelFormat = dres->texture.pixelFormat;
    desc.rasterSampleCount = dres->texture.sampleCount ? dres->texture.sampleCount : 1;
    desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    struct d9mt_status* status = g->status;
    atomic_fetch_add_explicit(&status->psos_pending, 1, memory_order_relaxed);
    [g->compiler newRenderPipelineStateWithDescriptor:desc
                                  compilerTaskOptions:nil
                                    completionHandler:^(id<MTLRenderPipelineState> pso,
                                                        NSError* err) {
      if (pso) {
        entry->pso = pso;
        entry->state.store(1, std::memory_order_release);
        atomic_fetch_add_explicit(&status->psos_built, 1, memory_order_relaxed);
      } else {
        entry->state.store(2, std::memory_order_release);
        atomic_fetch_add_explicit(&status->exec_errors, 1, memory_order_relaxed);
        fprintf(stderr, "[serverd] blit PSO build failed: %s\n",
                err ? err.localizedDescription.UTF8String : "(no error)");
      }
      atomic_fetch_sub_explicit(&status->psos_pending, 1, memory_order_relaxed);
    }];
    return false; /* just kicked — stash */
  }
  entry = it->second;
  const uint32_t state = entry->state.load(std::memory_order_acquire);
  if (state == 0)
    return false; /* compiling — stash */
  if (state != 1 || !entry->pso)
    return true; /* failed — drop (logged once by the handler) */

  /* src sampled through a (cached) view for mip/layer/format selection */
  d9mt_wire_rec_bind_texture_by_id viewKey = {};
  viewKey.format = r->src_format;
  viewKey.view_type = 1;
  viewKey.base_mip = r->src_mip;
  viewKey.mip_count = 1;
  viewKey.base_layer = r->src_layer;
  viewKey.layer_count = 1;
  id<MTLTexture> srcTex = resolveTexView(sres, &viewKey);
  if (!srcTex)
    return true;

  closePass();
  closeXfer();
  ensureCmdbuf();
  const uint32_t mipW = (uint32_t)dres->texture.width >> r->dst_mip ? (uint32_t)dres->texture.width >> r->dst_mip : 1u;
  const uint32_t mipH = (uint32_t)dres->texture.height >> r->dst_mip ? (uint32_t)dres->texture.height >> r->dst_mip : 1u;

  MTL4RenderPassDescriptor* desc = [[MTL4RenderPassDescriptor alloc] init];
  desc.colorAttachments[0].texture = dres->texture;
  desc.colorAttachments[0].level = r->dst_mip;
  desc.colorAttachments[0].slice = r->dst_layer;
  desc.colorAttachments[0].loadAction = MTLLoadActionLoad;
  desc.colorAttachments[0].storeAction = MTLStoreActionStore;
  desc.renderTargetWidth = mipW;
  desc.renderTargetHeight = mipH;
  id<MTL4RenderCommandEncoder> enc = [g->cmdbuf renderCommandEncoderWithDescriptor:desc];
  if (!enc) {
    execError("blit: encoder failed");
    return true;
  }
  atomic_fetch_add_explicit(&g->status->encoders, 1, memory_order_relaxed);
  [enc setRenderPipelineState:entry->pso];

  MTLViewport vp;
  vp.originX = r->dst_x0;
  vp.originY = r->dst_y0;
  vp.width = double(r->dst_x1 - r->dst_x0);
  vp.height = double(r->dst_y1 - r->dst_y0);
  vp.znear = 0.0;
  vp.zfar = 1.0;
  [enc setViewport:vp];
  MTLScissorRect sc;
  sc.x = (NSUInteger)r->dst_x0;
  sc.y = (NSUInteger)r->dst_y0;
  sc.width = (NSUInteger)(r->dst_x1 - r->dst_x0);
  sc.height = (NSUInteger)(r->dst_y1 - r->dst_y0);
  if (sc.x + sc.width > mipW || sc.y + sc.height > mipH) { /* belt+braces */
    sc.x = 0; sc.y = 0; sc.width = mipW; sc.height = mipH;
  }
  [enc setScissorRect:sc];

  RingSlice rs = ringAlloc(sizeof(float) * 4, 256);
  if (!rs.cpu) {
    [enc endEncoding];
    execError("blit: slab alloc failed");
    return true;
  }
  std::memcpy(rs.cpu, r->uv, sizeof(float) * 4);
  selectArgumentTablesForPass();
  ArgTablePair& pair = g->argTables[g->currentArgTable];
  [pair.table[0] setAddress:rs.gpu atIndex:0];
  [pair.table[1] setAddress:rs.gpu atIndex:0];
  [pair.table[1] setTexture:srcTex.gpuResourceID atIndex:0];
  id<MTLSamplerState> smp = g->blitSamplers[r->filter == 1u ? 1 : 0];
  [pair.table[1] setSamplerState:(smp ? smp : g->dummySampler).gpuResourceID atIndex:0];
  [enc setArgumentTable:pair.table[0] atStages:MTLRenderStageVertex];
  [enc setArgumentTable:pair.table[1] atStages:MTLRenderStageFragment];
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [enc barrierAfterStages:(MTLStages)kStageFragment
        beforeQueueStages:(MTLStages)kStageAll
        visibilityOptions:(MTL4VisibilityOptions)kVisibilityDevice];
  [enc endEncoding];
  dres->hasContent = true;
  g->dirtyPso = g->dirtyState = g->dirtyBindings = true;
  g->argTablesBound = false;
  { static int n = 0;
    if (n++ < 8)
      fprintf(stderr, "[serverd] blit #%d: %llu->%llu rect %d,%d-%d,%d\n", n,
              (unsigned long long)r->src_wire_id, (unsigned long long)r->dst_wire_id,
              r->dst_x0, r->dst_y0, r->dst_x1, r->dst_y1); }
  return true;
}

static void drainPendingBlits() {
  size_t w = 0;
  for (size_t i = 0; i < g->pendingCopies.size(); i++) {
    if (!tryExecuteCopyTexture(&g->pendingCopies[i]))
      g->pendingCopies[w++] = g->pendingCopies[i];
  }
  g->pendingCopies.resize(w);
  w = 0;
  for (size_t i = 0; i < g->pendingImgClears.size(); i++) {
    if (!tryExecuteClearImage(&g->pendingImgClears[i]))
      g->pendingImgClears[w++] = g->pendingImgClears[i];
  }
  g->pendingImgClears.resize(w);
  w = 0;
  for (size_t i = 0; i < g->pendingMipgens.size(); i++) {
    if (!tryExecuteMipgen(&g->pendingMipgens[i]))
      g->pendingMipgens[w++] = g->pendingMipgens[i];
  }
  g->pendingMipgens.resize(w);
  w = 0;
  for (size_t i = 0; i < g->pendingBlits.size(); i++) {
    if (!tryExecuteBlit(&g->pendingBlits[i]))
      g->pendingBlits[w++] = g->pendingBlits[i];
  }
  g->pendingBlits.resize(w);
}

static void handleBlit(const d9mt_wire_rec_blit_texture_view* r) {
  if (!tryExecuteBlit(r)) {
    if (g->pendingBlits.size() < 4096)
      g->pendingBlits.push_back(*r);
  }
}


/* The client's present contract (d3d9_swapchain.cpp "BLIT/COPY present
 * model"): buffer 0 is the app's PERSISTENT back buffer — present COPIES it
 * to the drawable and the swap-chain buffers never rotate, because D3D9 apps
 * may compose a frame INCREMENTALLY (a few pretransformed tiles per present)
 * and rely on back-buffer content SURVIVING Present regardless of DISCARD.
 * The server must honor the same contract: all backbuffer-routed rendering
 * lands in this ONE persistent texture (passes Load existing content unless
 * the stream clears it); handlePresent COPIES it into the next IOSurface
 * pool slot before the completion-gated publication. The pool keeps rotating
 * for cross-process buffering — the RENDER TARGET does not. Rendering into
 * the rotating slot directly spreads an incremental composition across
 * D9MT_SERVER_SURFACES surfaces (one slice of the artwork per published
 * frame — the "CRT scan" failure). */
static id<MTLTexture> persistentBackbuffer() {
  const uint32_t w = d9mt_pool_width(), h = d9mt_pool_height();
  if (g->persistentBB && g->persistentBB.width == w &&
      g->persistentBB.height == h)
    return g->persistentBB;
  if (g->persistentBB) { /* pool resized: retire the old backing by serial */
    g->retire.emplace_back(g->lastSubmitted, g->persistentBB);
    g->persistentBB = nil;
  }
  MTLTextureDescriptor* d = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                   width:w
                                  height:h
                               mipmapped:NO];
  d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  d.storageMode = MTLStorageModePrivate;
  id<MTLTexture> t = [g->device newTextureWithDescriptor:d];
  if (!t) {
    execError("persistent backbuffer alloc failed");
    return nil;
  }
  residencyAdd(t);
  g->persistentBB = t;
  return t;
}

static void handleSetRenderTargets(const d9mt_wire_rec_set_render_targets_by_id* r) {
  if (g->pass.active)
    closePass();
  else
    flushPendingClears();

  /* backbuffer size negotiation: the first flagged RT carries the client's
   * real backbuffer size — resize the IOSurface pool if it disagrees
   * (spawn-time default). Requires a full GPU drain (old pool textures may
   * be referenced by in-flight frames). */
  for (uint32_t i = 0; i < 5; i++) {
    const d9mt_wire_rt_id_ref& ref = i < 4 ? r->color[i] : r->depth_stencil;
    if ((ref.flags & D9MT_WIRE_RT_FLAG_BACKBUFFER) && ref.width && ref.height &&
        (ref.width != d9mt_pool_width() || ref.height != d9mt_pool_height())) {
      submitFrame();
      if (!waitSerial(g->lastSubmitted, 5000))
        execError("resize drain timed out");
      /* swap pool-texture residency across the resize (publish-copy dst) */
      for (int s = 0; s < D9MT_SERVER_SURFACES; s++)
        if (id<MTLTexture> old = d9mt_pool_texture(s))
          [g->residency removeAllocation:old];
      if (d9mt_iosurface_pool_resize(g->status, ref.width, ref.height))
        execError("pool resize failed");
      for (int s = 0; s < D9MT_SERVER_SURFACES; s++)
        residencyAdd(d9mt_pool_texture(s));
      break;
    }
  }

  Pass fresh;
  auto resolve = [&](const d9mt_wire_rt_id_ref& ref, RtAttachment& out) {
    out = RtAttachment();
    if (!ref.image_wire_id && !(ref.flags & D9MT_WIRE_RT_FLAG_BACKBUFFER))
      return;
    out.named = true;
    out.wireId = ref.image_wire_id;
    out.format = pixelFormat(ref.format);
    out.baseMip = ref.base_mip;
    out.baseLayer = ref.base_layer;
    /* v11: substitute on the FLAG or on identity with the LAST PRESENTED
     * backbuffer (the present record teaches which wire id is live — the
     * multi-backbuffer rotation belt). */
    if ((ref.flags & D9MT_WIRE_RT_FLAG_BACKBUFFER) ||
        (ref.image_wire_id && ref.image_wire_id == g->backbufferWireId)) {
      out.backbuffer = true;
      out.tex = persistentBackbuffer();
      out.format = MTLPixelFormatBGRA8Unorm; /* IOSurface pool format */
      out.baseMip = out.baseLayer = 0;
      return;
    }
    auto res = lookupWire(ref.image_wire_id);
    if (res && observeReady(res) && res->texture) {
      out.tex = res->texture;
      res->hasContent = true; /* server-side render defines the storage */
    }
    /* unresolved: named-but-nil — draws needing it are skipped */
  };
  for (uint32_t i = 0; i < 4; i++)
    resolve(r->color[i], fresh.color[i]);
  resolve(r->depth_stencil, fresh.ds);
  if (fresh.ds.named && !fresh.ds.tex) {
    /* V12: a named-but-nil ds means this pass renders WITHOUT the depth
     * attachment the client renders WITH — silent divergence class.
     * V17: name the failure CLASS — the three shapes need three different
     * fixes (unknown id = client emission/session bug; building = async
     * create race, heals at materialize; FAILED = Metal refused the
     * descriptor, permanent). */
    g->dsUnresolvedBinds++;
    static int n = 0;
    if (n++ < 8) {
      auto res = lookupWire(r->depth_stencil.image_wire_id);
      const char* why = !res ? "UNKNOWN ID (no create this session)"
        : res->ready.load(std::memory_order_acquire) == 2
          ? "create FAILED server-side (permanent)"
        : res->ready.load(std::memory_order_acquire) == 0
          ? "still building (async create; late-resolves at draw)"
          : "ready but texture nil";
      fprintf(stderr, "[serverd] set_rt #%d: ds wire=%llu UNRESOLVED [%s] — "
                      "pass will run without depth attachment\n", n,
              (unsigned long long)r->depth_stencil.image_wire_id, why);
    }
  }
  { /* v11 substitution audit: name the first RT binds' routing */
    static int n = 0;
    if (n++ < 8)
      fprintf(stderr, "[serverd] set_rt #%d: color0 id=%llu flags=0x%x -> %s, "
                      "ds id=%llu -> %s (lastPresentBB=%llu slot=%u)\n", n,
              (unsigned long long)r->color[0].image_wire_id, r->color[0].flags,
              fresh.color[0].backbuffer ? "IOSURFACE"
              : (fresh.color[0].tex ? "texture" : (fresh.color[0].named ? "UNRESOLVED" : "none")),
              (unsigned long long)r->depth_stencil.image_wire_id,
              fresh.ds.backbuffer ? "IOSURFACE"
              : (fresh.ds.tex ? "texture" : (fresh.ds.named ? "UNRESOLVED" : "none")),
              (unsigned long long)g->backbufferWireId, g->presentSlot);
  }
  fresh.pending = true;
  g->pass = fresh;
  g->dirtyPso = g->dirtyState = true;
}

static void handleClearById(const d9mt_wire_rec_clear_render_target_by_id* r) {
  auto matches = [&](const RtAttachment& a) {
    if (!a.named)
      return false;
    if ((r->flags & D9MT_WIRE_RT_FLAG_BACKBUFFER) && a.backbuffer)
      return true;
    return r->image_wire_id != 0 && a.wireId == r->image_wire_id;
  };

  /* fold into the pending pass when the target is one of its attachments
   * (clear-then-draw = free on TBDR) */
  if (!g->pass.active && g->pass.pending) {
    if (r->clear_aspects & 0x1u) { /* color */
      for (uint32_t i = 0; i < 4; i++) {
        if (matches(g->pass.color[i])) {
          g->pass.clearColor[i] = true;
          g->pass.clearValues[i] = MTLClearColorMake(
              r->value.color[0], r->value.color[1], r->value.color[2], r->value.color[3]);
          return;
        }
      }
    }
    if ((r->clear_aspects & 0x6u) && matches(g->pass.ds)) {
      if (r->clear_aspects & 0x2u) {
        g->pass.clearDepth = true;
        g->pass.depthClear = r->value.depth;
      }
      if (r->clear_aspects & 0x4u) {
        g->pass.clearStencil = true;
        g->pass.stencilClear = r->value.stencil;
      }
      return;
    }
  }

  /* standalone clear: dedicated mini-pass (port of clearRenderTarget) */
  RtAttachment target;
  if (r->flags & D9MT_WIRE_RT_FLAG_BACKBUFFER) {
    target.tex = persistentBackbuffer();
    target.format = MTLPixelFormatBGRA8Unorm;
    target.named = true;
    target.backbuffer = true;
  } else {
    auto res = lookupWire(r->image_wire_id);
    if (!res || !observeReady(res) || !res->texture) {
      /* unresolvable/building clear target. V12: a dropped DEPTH clear is
       * the garbage-rejects-everything class — count + name first-8. */
      g->clearDropUnresolved++;
      static int n = 0;
      if (n++ < 8)
        fprintf(stderr, "[serverd] clear_by_id #%d DROPPED: wire=%llu "
                        "aspects=0x%x %s\n", n,
                (unsigned long long)r->image_wire_id, r->clear_aspects,
                !res ? "unresolved" : "still building");
      return;
    }
    target.tex = res->texture;
    target.format = res->texture.pixelFormat;
    target.named = true;
    target.wireId = r->image_wire_id;
    res->hasContent = true; /* the clear defines the storage */
  }

  if (g->pass.active)
    closePass();
  Pass saved = g->pass;
  g->pass = Pass();
  if (r->clear_aspects & 0x1u) {
    g->pass.color[0] = target;
    g->pass.clearColor[0] = true;
    g->pass.clearValues[0] = MTLClearColorMake(
        r->value.color[0], r->value.color[1], r->value.color[2], r->value.color[3]);
  } else {
    g->pass.ds = target;
    if (r->clear_aspects & 0x2u) {
      g->pass.clearDepth = true;
      g->pass.depthClear = r->value.depth;
    }
    if (r->clear_aspects & 0x4u) {
      g->pass.clearStencil = true;
      g->pass.stencilClear = r->value.stencil;
    }
  }
  g->pass.pending = true;
  materializePass();
  closePass();
  g->pass = saved;
  g->dirtyPso = g->dirtyState = true;
}

/* Folded-state defaults — MUST mirror the client's (mtl_types.h): zero-init
 * is NOT the D3D9 default (writeMask=0xF, depth compare Always, stencil
 * masks 0xFF). Used at init AND at every session reset (S3 v5). */
static void resetFoldedState() {
  for (auto& b : g->blend) {
    b = d9mt_wire_rec_set_blend_mode();
    b.write_mask = 0xFu;
    b.color_dst = b.alpha_dst = 0u; /* Zero */
    b.color_src = b.alpha_src = 1u; /* One */
  }
  g->depthStencilSeen = false; /* V12: default-DSSO observability */
  g->depthStencil = d9mt_wire_rec_set_depth_stencil_state();
  g->depthStencil.depth_compare_op = 7u; /* Always */
  g->depthStencil.front.compare_op = g->depthStencil.back.compare_op = 7u;
  g->depthStencil.front.read_mask = g->depthStencil.back.read_mask = 0xFFu;
  g->depthStencil.front.write_mask = g->depthStencil.back.write_mask = 0xFFu;
  g->msaa = d9mt_wire_rec_set_multisample_state();
  g->msaa.sample_count = 1u;
  g->msaa.sample_mask = ~0u;
  g->raster = d9mt_wire_rec_set_rasterizer_state();
  g->attrs.clear();
  g->layouts.clear();
  g->topology = 3; /* TriangleList */
  g->viewport = d9mt_wire_viewport();
  g->viewportSet = false;
  std::memset(g->blendColor, 0, sizeof(g->blendColor));
  g->stencilRef = 0;
  std::memset(g->depthBias, 0, sizeof(g->depthBias));
  for (auto& st : g->stages)
    st = StageState();
  for (auto& s : g->streams)
    s = Stream();
  g->index = {};
  g->dirtyPso = g->dirtyState = g->dirtyBindings = true;
}

/* S3 v5: session boundary (quiesce at context death / begin at connect).
 * The server is PROCESS-LIFETIME — drain the in-flight frame, retire every
 * table resource GPU-safely, drop the PSO cache (its keys embed dead library
 * POINTERS: a new library at a recycled address must never alias a stale
 * pipeline), reset the folded state to defaults, ack via an epoch bump and
 * KEEP RUNNING. Device/queue/event/argtables/slabs/heap/dummies persist.
 *
 * S3 v6 DEADLOCK-FREE BY CONSTRUCTION + self-naming stages: a real-game
 * quiesce hung the consumer with no log and no crash. This handler now
 * (a) prints a breadcrumb per stage so a hang names its stage in the next
 * log, and (b) contains NO wait that depends on another thread making
 * progress: the create worker's queue is ABANDONED (swapped out under the
 * queue mutex, destroyed outside it) rather than joined or acked — the
 * worker never touches executor state beyond its own job's Resource +
 * status atomics, so no ack is needed and no ordering can invert; the
 * "drain" is submitFrame (commit + timeline signal, NO GPU wait — retire-
 * by-serial owns GPU lifetime exactly as handleDestroy does); retirement
 * is pure list pushes. No lock is held across any Metal call. */
static void handleSession(const d9mt_wire_rec_session* r) {
  const char* what = r->kind ? "BEGIN" : "QUIESCE";
  fprintf(stderr, "[serverd] session %s: worker-abandon...\n", what);

  /* 1. abandon queued create jobs: stale libraries for a dead session must
   * not keep the worker churning for minutes; the job IN FLIGHT (possibly
   * deep inside newLibraryWithData) finishes into an orphaned Resource. */
  size_t abandoned = 0;
  {
    std::deque<Exec::CreateJob> stale;
    {
      std::lock_guard<std::mutex> lock(g->creatMutex);
      stale.swap(g->creatQueue);
    }
    abandoned = stale.size(); /* destroyed OUTSIDE the lock */
  }
  fprintf(stderr, "[serverd] session %s: drain-submit (abandoned %zu jobs)...\n",
          what, abandoned);

  /* 2. close + commit the in-flight frame; NO completion wait */
  submitFrame(); /* closes pass + xfer inside */
  const uint64_t after = g->lastSubmitted;
  fprintf(stderr, "[serverd] session %s: retire (serial=%llu)...\n", what,
          (unsigned long long)after);

  /* occlusion state: an in-flight bracket can never be answered for the new
   * session (the client resolves it all-passed by session mismatch), but
   * its slots must still retire on GPU completion — convert it to a normal
   * pending (publication is harmless: ids are process-monotonic, never
   * reused). qPending entries publish/retire normally. */
  if (g->qActive.on) {
    g->qSessionEnd++;
    endActiveQuery(); /* pass closed above -> serial = `after` */
  }

  auto retireRes = [&](const std::shared_ptr<Resource>& res) {
    if (res->buffer)
      g->retire.emplace_back(after, res->buffer);
    if (res->texture)
      g->retire.emplace_back(after, res->texture);
    if (res->sampler)
      g->retireNoRes.emplace_back(after, res->sampler);
    for (auto& v : res->views)
      if (v.second)
        g->retireNoRes.emplace_back(after, v.second);
    res->views.clear();
  };
  for (auto& kv : g->byHandle)
    retireRes(kv.second);
  for (auto& kv : g->byWire)
    if (!kv.second->clientHandle) /* byHandle already retired the rest */
      retireRes(kv.second);
  g->byHandle.clear();
  g->byWire.clear();
  g->pendingShaders.clear(); /* in-flight create jobs finish into orphans */
  for (auto& res : g->asyncCreates)
    res->stash.clear();
  g->asyncCreates.clear();
  fprintf(stderr, "[serverd] session %s: psocache-drop...\n", what);

  for (auto& kv : g->psoCache)
    if (kv.second && kv.second->pso)
      g->retireNoRes.emplace_back(after, kv.second->pso);
  g->psoCache.clear();
  g->pendingBlits.clear(); /* v9: their resources just died */
  g->pendingCopies.clear();
  g->pendingImgClears.clear();
  g->pendingMipgens.clear();
  /* blitPsoCache persists: keyed by pixel format (values), library owned by
   * the executor — no dead pointers involved. */
  fprintf(stderr, "[serverd] session %s: state-reset...\n", what);

  g->vs.reset();
  g->fs.reset();
  g->vsPend.reset();
  g->fsPend.reset();
  resetFoldedState();
  g->pass = Pass(); /* closed above; attachments drop */
  g->argTablesBound = false;
  g->lastPso = nil;
  g->dynValid = false;
  g->backbufferWireId = 0;
  if (g->persistentBB) { /* fresh session gets a fresh persistent backbuffer */
    g->retire.emplace_back(after, g->persistentBB);
    g->persistentBB = nil;
  }

  const uint64_t epoch =
      atomic_fetch_add_explicit(&g->status->epoch, 1, memory_order_release) + 1;
  fprintf(stderr, "[serverd] session %s (client session %u): state reset, epoch=%llu\n",
          r->kind ? "BEGIN" : "QUIESCE", r->session, (unsigned long long)epoch);
}

/* --- present-timeline instrumentation (status v4 tl ring + srv-timeline) ----
 * Machine-global clock: mach_absolute_time scaled to ns — the SAME domain the
 * client's QPC marks are calibrated into (WMTWireWaitTimeTap), so the hops
 * compose across the process boundary. Hops per frame serial N:
 *   t2 (tl_consume_ns) handlePresent entry — the frame marker is consumed,
 *   t3 (tl_gpu_ns)     the publish site observed GPU completion (listener
 *                      block entry; includes the listener dispatch latency),
 *   t4 (tl_publish_ns) immediately before the frame_serial release-store.
 * Written into the status v4 tl ring for the client line, and aggregated
 * below into the srv-timeline stderr line on the srv-stats cadence (one line
 * per 256 frames). Sample arrays are indexed (serial & 255): gap on the
 * consumer thread, gpu/publish on the serial listener queue — the printer
 * reads whatever completed (instrumentation-grade, not a snapshot). */
static uint64_t exec_now_ns(void) {
  static mach_timebase_info_data_t tb; /* {0,0} until first call */
  if (tb.denom == 0)
    mach_timebase_info(&tb);
  return mach_absolute_time() * tb.numer / tb.denom;
}

static uint32_t g_tlMarkGapUs[256]; /* t2[N] - t2[N-1]: marker arrival period */
static uint32_t g_tlSrvGpuUs[256];  /* t3 - t2 */
static uint32_t g_tlPublishUs[256]; /* t4 - t3 */
static uint64_t g_tlPrevConsumeNs;

static void tlAvgP95(const uint32_t* smp, uint32_t* avg, uint32_t* p95) {
  uint32_t tmp[256];
  memcpy(tmp, smp, sizeof(tmp));
  std::sort(tmp, tmp + 256);
  uint64_t sum = 0;
  for (int i = 0; i < 256; i++)
    sum += tmp[i];
  *avg = (uint32_t)(sum / 256u);
  *p95 = tmp[243]; /* floor(256 * 0.95) */
}

static void handlePresent(const d9mt_wire_rec_present_frame* r) {
  /* timeline t2: the frame marker is consumed HERE. Invalidate the tl slot
   * first — the publish site completes it and release-stores tl_serial LAST,
   * so a reader matching tl_serial == N sees fully-written hops. */
  const uint64_t tlConsumeNs = exec_now_ns();
  {
    const uint32_t ti = (uint32_t)(r->frame_serial & (D9MT_STATUS_TL_SLOTS - 1u));
    atomic_store_explicit(&g->status->tl_serial[ti], 0, memory_order_relaxed);
    atomic_store_explicit(&g->status->tl_consume_ns[ti], tlConsumeNs,
                          memory_order_relaxed);
    if (g_tlPrevConsumeNs)
      g_tlMarkGapUs[r->frame_serial & 255u] =
          (uint32_t)((tlConsumeNs - g_tlPrevConsumeNs) / 1000u);
    g_tlPrevConsumeNs = tlConsumeNs;
  }

  /* TEST HOOK (D9MT_TEST_FRAME_DELAY_US): throttled consumer for the
   * sustained-overload flow-control gate — simulates a server that executes
   * frames slower than the client produces them. */
  if (g->frameDelayUs)
    usleep(g->frameDelayUs);

  /* v7: drain worker-built resources (residency + stashed content) into
   * THIS frame before it closes; v9: retry blits whose PSO just landed */
  sweepAsyncCreates();
  drainPendingBlits();
  sweepQueries(); /* publish brackets whose submissions completed by now */
  const uint32_t slot = g->presentSlot;

  /* Persistent-backbuffer publish copy: the frame's rendering lives in the
   * ONE persistent texture (content survives presents — the client's
   * BLIT/COPY contract); ship a full-texture GPU copy into this present's
   * pool slot, ordered AFTER all frame rendering (folded clears materialize
   * first; closePass's producer barrier orders the fragment writes ahead of
   * this compute-encoder copy) and BEFORE the completion-gated publication
   * below — frame_serial == N still means "slot pixels done". */
  if (g->persistentBB) {
    if (g->pass.active)
      closePass();
    else
      flushPendingClears();
    id<MTLTexture> dst = d9mt_pool_texture((int)slot);
    id<MTL4ComputeCommandEncoder> enc = dst ? ensureXfer() : nil;
    if (enc) {
      /* CONSUMER barrier (§4c pairs with closePass's producer barrier): this
       * copy is the stream's one render->compute READ of an attachment —
       * without an incoming-encoder barrier the copy can be scheduled before
       * the pass's fragment writes flush (published slot = stale previous
       * frame; caught by the repeated quadrant gate). */
      [enc barrierAfterQueueStages:(MTLStages)kStageFragment
                      beforeStages:(MTLStages)(kStageBlit | kStageDispatch)
                 visibilityOptions:(MTL4VisibilityOptions)kVisibilityDevice];
      MTLSize sz = MTLSizeMake(
          g->persistentBB.width < dst.width ? g->persistentBB.width : dst.width,
          g->persistentBB.height < dst.height ? g->persistentBB.height : dst.height,
          1);
      [enc copyFromTexture:g->persistentBB
               sourceSlice:0
               sourceLevel:0
              sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:sz
                 toTexture:dst
          destinationSlice:0
          destinationLevel:0
         destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
  }
  submitFrame();
  g->backbufferWireId = r->backbuffer_wire_id;
  { static int n = 0;
    if (n++ < 8)
      fprintf(stderr, "[serverd] present #%d: serial=%llu bbWire=%llu slot=%u\n",
              n, (unsigned long long)r->frame_serial,
              (unsigned long long)r->backbuffer_wire_id, slot); }

  /* v11: per-window render stats — the drawing-vs-skipping discriminator */
  {
    const uint64_t frames =
        atomic_load_explicit(&g->status->frames, memory_order_relaxed);
    if ((frames & 255u) == 255u) {
      Exec::StatsSnap now;
      now.draws = atomic_load_explicit(&g->status->draws_executed, memory_order_relaxed);
      now.skipped = atomic_load_explicit(&g->status->draws_skipped, memory_order_relaxed);
      now.pend = atomic_load_explicit(&g->status->draws_skipped_pending_pso, memory_order_relaxed);
      now.noRt = g->skipNoRt;
      now.psoFail = g->skipPsoFail;
      now.noShader = g->skipNoShader;
      now.noIndex = g->skipNoIndex;
      now.encoders = atomic_load_explicit(&g->status->encoders, memory_order_relaxed);
      now.uploads = atomic_load_explicit(&g->status->uploads_executed, memory_order_relaxed);
      now.clrDrop = g->clearDropUnresolved;
      now.clrDsPart = g->clearDsPartialDrop;
      now.dsUnres = g->dsUnresolvedBinds;
      now.dssoDef = g->dssoDefaulted;
      now.dsLate = g_lateResolves;
      now.uplDirect = g->uplDirect;
      now.uplStaged = g->uplStaged;
      now.uplFlushUse = g->uplFlushUse;
      now.uplPromoted = g->uplPromoted;
      now.uplTexParked = g->uplTexParked;
      const Exec::StatsSnap& p = g->statsSnap;
      fprintf(stderr, "[serverd] srv-stats: draws=%llu skip=%llu{psoPend=%llu "
                      "psoFail=%llu noSh=%llu noRt=%llu noIdx=%llu} passes=%llu "
                      "upl=%llu{dir=%llu stg=%llu useFlush=%llu promote=%llu "
                      "texPark=%llu} "
                      "depth{clrDrop=%llu clrPart=%llu dsUnres=%llu "
                      "dssoDef=%llu lateRes=%llu} /window\n",
              (unsigned long long)(now.draws - p.draws),
              (unsigned long long)(now.skipped - p.skipped),
              (unsigned long long)(now.pend - p.pend),
              (unsigned long long)(now.psoFail - p.psoFail),
              (unsigned long long)(now.noShader - p.noShader),
              (unsigned long long)(now.noRt - p.noRt),
              (unsigned long long)(now.noIndex - p.noIndex),
              (unsigned long long)(now.encoders - p.encoders),
              (unsigned long long)(now.uploads - p.uploads),
              (unsigned long long)(now.uplDirect - p.uplDirect),
              (unsigned long long)(now.uplStaged - p.uplStaged),
              (unsigned long long)(now.uplFlushUse - p.uplFlushUse),
              (unsigned long long)(now.uplPromoted - p.uplPromoted),
              (unsigned long long)(now.uplTexParked - p.uplTexParked),
              (unsigned long long)(now.clrDrop - p.clrDrop),
              (unsigned long long)(now.clrDsPart - p.clrDsPart),
              (unsigned long long)(now.dsUnres - p.dsUnres),
              (unsigned long long)(now.dssoDef - p.dssoDef),
              (unsigned long long)(now.dsLate - p.dsLate));
      g->statsSnap = now;

      /* srv-timeline: server-side hops of the present pipeline over the last
       * 256 frames (us, avg/p95). markGap = frame-marker arrival period (the
       * pacing as the SERVER sees it); srvGpu = t3-t2 (final-copy encode +
       * commit + GPU + listener dispatch); publish = t4-t3 (~0 by
       * construction — a fat publish would mean store-side trouble). */
      uint32_t aGap, pGap, aGpu, pGpu, aPub, pPub;
      tlAvgP95(g_tlMarkGapUs, &aGap, &pGap);
      tlAvgP95(g_tlSrvGpuUs, &aGpu, &pGpu);
      tlAvgP95(g_tlPublishUs, &aPub, &pPub);
      fprintf(stderr, "[serverd] srv-timeline: markGap=%u/%u srvGpu=%u/%u "
                      "publish=%u/%u us (avg/p95, win=256)\n",
              aGap, pGap, aGpu, pGpu, aPub, pPub);
    }
  }

  /* S3: GPU-completion-gated publication (S2 flag 3). {present_slot,
   * frame_serial, frames} are stored ONLY once the frame's serial signals on
   * the shared-event timeline — a client reading frame_serial == N may
   * consume slot's IOSurface knowing the pixels are done. The listener queue
   * is serial, and serials are registered in increasing order, so the
   * publications stay monotonic. */
  const uint64_t submitted = g->lastSubmitted;
  const uint64_t frameSerial = r->frame_serial;
  struct d9mt_status* status = g->status;
  if (submitted && completedSerial() < submitted && g->listener) {
    [g->event notifyListener:g->listener
                     atValue:submitted
                       block:^(id<MTLSharedEvent> ev, uint64_t value) {
      (void)ev;
      /* timeline t3: GPU completion observed (block entry — includes the
       * listener dispatch latency, which is exactly what the client pays). */
      const uint64_t tlGpuNs = exec_now_ns();
      atomic_store_explicit(&status->present_slot, slot, memory_order_relaxed);
      atomic_store_explicit(&status->completed_serial, value, memory_order_relaxed);
      /* timeline t4 + tl-slot completion BEFORE the frame_serial store: a
       * client that observes frame_serial == N must find the finished slot. */
      const uint64_t tlPubNs = exec_now_ns();
      {
        const uint32_t ti = (uint32_t)(frameSerial & (D9MT_STATUS_TL_SLOTS - 1u));
        atomic_store_explicit(&status->tl_gpu_ns[ti], tlGpuNs, memory_order_relaxed);
        atomic_store_explicit(&status->tl_publish_ns[ti], tlPubNs, memory_order_relaxed);
        atomic_store_explicit(&status->tl_serial[ti], frameSerial, memory_order_release);
        g_tlSrvGpuUs[frameSerial & 255u]  = (uint32_t)((tlGpuNs - tlConsumeNs) / 1000u);
        g_tlPublishUs[frameSerial & 255u] = (uint32_t)((tlPubNs - tlGpuNs) / 1000u);
      }
      atomic_store_explicit(&status->frame_serial, frameSerial, memory_order_release);
      atomic_fetch_add_explicit(&status->frames, 1, memory_order_release);
      /* Publish notice: wake any client parked on frame_serial (unixcall
       * 217 op WMTWireWaitFrameSerial). Store (release) THEN wake — the
       * wake is per-frame, so unconditional (no waiter = cheap ENOENT). */
      d9mt_futex_wake_all(&status->frame_serial);
    }];
  } else {
    /* nothing in flight (empty frame or already completed): publish inline */
    atomic_store_explicit(&status->present_slot, slot, memory_order_relaxed);
    atomic_store_explicit(&status->completed_serial, completedSerial(),
                          memory_order_relaxed);
    /* timeline t3 == t4 (no GPU wait — nothing was in flight) */
    {
      const uint64_t tlNowNs = exec_now_ns();
      const uint32_t ti = (uint32_t)(frameSerial & (D9MT_STATUS_TL_SLOTS - 1u));
      atomic_store_explicit(&status->tl_gpu_ns[ti], tlNowNs, memory_order_relaxed);
      atomic_store_explicit(&status->tl_publish_ns[ti], tlNowNs, memory_order_relaxed);
      atomic_store_explicit(&status->tl_serial[ti], frameSerial, memory_order_release);
      g_tlSrvGpuUs[frameSerial & 255u]  = (uint32_t)((tlNowNs - tlConsumeNs) / 1000u);
      g_tlPublishUs[frameSerial & 255u] = 0u;
    }
    atomic_store_explicit(&status->frame_serial, frameSerial, memory_order_release);
    atomic_fetch_add_explicit(&status->frames, 1, memory_order_release);
    d9mt_futex_wake_all(&status->frame_serial); /* store (release) THEN wake */
  }

  g->presentSlot = (g->presentSlot + 1) % D9MT_SERVER_SURFACES;
  /* The render target is the PERSISTENT texture, not the rotated slot — the
   * pending pass's backbuffer attachments stay valid across the present
   * (content survives; the next pass Loads it unless the stream clears). */
}

/* --- draws ------------------------------------------------------------------- */

/* S3 v13 observability twin of MtlWireContext::logWireDraw — the identical
 * "wire-draw#N ..." / "wire-vl ..." line format from the SERVER's resolved
 * state, for eyeball diffing against the client emission. Window sums hash
 * the first 64 B at the bound offset of the buffer's CPU view; they LAG the
 * xfer-encoder blit (contents update when the GPU executes), so early frames
 * legitimately read zeros — the `up=` sums (exact bytes that crossed the
 * heap) are the transport-parity discriminator and must match immediately. */
static void logServerDraw() {
  if (g->dbgDrawLog >= 244)
    return;
  const uint32_t n = g->dbgDrawLog++;
  /* two bursts, same schedule as the client: #0-3, then #240-243 — by the
   * late burst the early xfer blits have executed, so the window sums show
   * the REAL fetched bytes (and prove per-frame constant updates land). */
  if (n >= 4 && n < 240)
    return;
  auto window = [](id<MTLBuffer> buf, uint64_t off) -> uint64_t {
    if (!buf || off >= buf.length)
      return 0;
    const uint64_t len = buf.length - off < 64 ? buf.length - off : 64;
    return fnv64((const uint8_t*)buf.contents + off, (size_t)len);
  };
  const Stream& s = g->streams[0];
  const uint64_t vbId = s.res ? s.res->wireId : 0;
  const uint64_t vbUp = s.res ? s.res->uploadSum : 0;
  const uint64_t ibId = g->index.res ? g->index.res->wireId : 0;
  const uint64_t ibUp = g->index.res ? g->index.res->uploadSum : 0;
  uint64_t cSum = 0xcbf29ce484222325ull, cUp = 0xcbf29ce484222325ull;
  uint32_t cMask = 0;
  for (uint32_t slot = 0; slot < 8; slot++) {
    const CbvBinding& cbv = g->stages[0].cbvs[slot];
    if (!cbv.buf)
      continue;
    cMask |= 1u << slot;
    /* S3 v14: prefer the shipped-bytes window at the bound offset — the draw
     * reads those (the xfer blit orders ahead of the pass). buffer.contents
     * lags the blit at record time and freezes at the fresh rotating offset. */
    const uint64_t wsum =
        (cbv.res && cbv.res->lastUploadOffset == cbv.offset)
            ? cbv.res->lastUploadWindow
            : window(cbv.buf, cbv.offset);
    const uint64_t upsum = cbv.res ? cbv.res->uploadSum : 0;
    const uint8_t s8 = (uint8_t)slot;
    cSum = fnv64(&s8, 1, cSum); cSum = fnv64(&wsum, 8, cSum);
    cUp  = fnv64(&s8, 1, cUp);  cUp  = fnv64(&upsum, 8, cUp);
  }
  const uint64_t pushSum = fnv64(g->stages[0].pushData, g->stages[0].pushExtent);
  fprintf(stderr,
          "[serverd] wire-draw#%u vb=%llu+%llu/%u vbsum=%016llx up=%016llx "
          "ib=%llu+%llu ibsum=%016llx up=%016llx "
          "cbv=%02x constsum=%016llx up=%016llx pushsum=%016llx pext=%u "
          "ups=%llu\n",
          n, (unsigned long long)vbId, (unsigned long long)s.offset, s.stride,
          (unsigned long long)window(s.buf, s.offset), (unsigned long long)vbUp,
          (unsigned long long)ibId, (unsigned long long)g->index.offset,
          (unsigned long long)window(g->index.buf, g->index.offset),
          (unsigned long long)ibUp, cMask,
          (unsigned long long)cSum, (unsigned long long)cUp,
          (unsigned long long)pushSum, g->stages[0].pushExtent,
          (unsigned long long)atomic_load_explicit(&g->status->uploads_executed,
                                                   memory_order_relaxed));
  if (n == 0) {
    fprintf(stderr, "[serverd] wire-vl n=%zu/%zu\n",
            g->attrs.size(), g->layouts.size());
    for (size_t i = 0; i < g->attrs.size(); i++)
      fprintf(stderr, "[serverd] wire-vl a%zu loc=%u bind=%u fmt=%u off=%u\n",
              i, g->attrs[i].location, g->attrs[i].binding,
              g->attrs[i].format, g->attrs[i].offset);
    for (size_t i = 0; i < g->layouts.size(); i++)
      fprintf(stderr, "[serverd] wire-vl l%zu bind=%u stride=%u pi=%u div=%u\n",
              i, g->layouts[i].binding, g->layouts[i].stride,
              g->layouts[i].per_instance, g->layouts[i].divisor);
  }
}

static void handleDraw(const d9mt_wire_rec_draw* r) {
  const d9mt_wire_draw_args* args = (const d9mt_wire_draw_args*)(r + 1);
  if (!prepareDraw()) {
    /* prepareDraw counted ONE skip; count the rest of the batch */
    if (r->count > 1)
      atomic_fetch_add_explicit(&g->status->draws_skipped, r->count - 1,
                                memory_order_relaxed);
    return;
  }
  logServerDraw();
  id<MTL4RenderCommandEncoder> enc = g->pass.encoder;

  if (g->topology == 5) { /* TriangleFan emulation */
    if (!ensureFanIndices())
      return;
    for (uint32_t i = 0; i < r->count; i++) {
      if (args[i].vertex_count < 3)
        continue;
      uint32_t verts = args[i].vertex_count < kFanMaxVerts ? args[i].vertex_count
                                                           : kFanMaxVerts;
      [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:3 * (verts - 2)
                       indexType:MTLIndexTypeUInt32
                     indexBuffer:g->fanIndices.gpuAddress
               indexBufferLength:(NSUInteger)g->fanIndices.length
                   instanceCount:args[i].instance_count ? args[i].instance_count : 1
                      baseVertex:(NSInteger)args[i].first_vertex
                    baseInstance:args[i].first_instance];
    }
    atomic_fetch_add_explicit(&g->status->draws_executed, r->count,
                              memory_order_relaxed);
    return;
  }

  MTLPrimitiveType prim = primitiveType(g->topology);
  for (uint32_t i = 0; i < r->count; i++) {
    [enc drawPrimitives:prim
            vertexStart:args[i].first_vertex
            vertexCount:args[i].vertex_count
          instanceCount:args[i].instance_count ? args[i].instance_count : 1
           baseInstance:args[i].first_instance];
  }
  atomic_fetch_add_explicit(&g->status->draws_executed, r->count,
                            memory_order_relaxed);
}

static void handleDrawIndexed(const d9mt_wire_rec_draw_indexed* r) {
  const d9mt_wire_draw_indexed_args* args =
      (const d9mt_wire_draw_indexed_args*)(r + 1);
  if (!g->index.bound || !g->index.buf) {
    atomic_fetch_add_explicit(&g->status->draws_skipped, r->count,
                              memory_order_relaxed);
    g->skipNoIndex += r->count;
    return;
  }
  if (!prepareDraw()) {
    if (r->count > 1)
      atomic_fetch_add_explicit(&g->status->draws_skipped, r->count - 1,
                                memory_order_relaxed);
    return;
  }
  logServerDraw();
  id<MTL4RenderCommandEncoder> enc = g->pass.encoder;
  const uint64_t idxSize = g->index.type == 0 ? 2 : 4;
  const MTLIndexType idxType =
      g->index.type == 0 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
  uint32_t executed = 0;

  for (uint32_t i = 0; i < r->count; i++) {
    uint64_t indexOffset = g->index.offset + (uint64_t)args[i].first_index * idxSize;
    if (indexOffset >= g->index.buf.length) {
      atomic_fetch_add_explicit(&g->status->draws_skipped, 1, memory_order_relaxed);
      continue;
    }
    /* MTL4 rule: indexBufferLength = accessible bytes from the address */
    uint64_t accessible = g->index.buf.length - indexOffset;
    [enc drawIndexedPrimitives:primitiveType(g->topology)
                    indexCount:args[i].index_count
                     indexType:idxType
                   indexBuffer:g->index.buf.gpuAddress + indexOffset
             indexBufferLength:(NSUInteger)accessible
                 instanceCount:args[i].instance_count ? args[i].instance_count : 1
                    baseVertex:(NSInteger)args[i].vertex_offset
                  baseInstance:args[i].first_instance];
    executed++;
  }
  atomic_fetch_add_explicit(&g->status->draws_executed, executed,
                            memory_order_relaxed);
}

/* --- IOSurface liveness clear (S1 gate, now METAL 4) --------------------------- */

static int livenessClear() {
  static const double colors[D9MT_SERVER_SURFACES][4] = {
      {1.0, 0.0, 0.0, 1.0}, /* slot 0: red   */
      {0.0, 1.0, 0.0, 1.0}, /* slot 1: green */
      {0.0, 0.0, 1.0, 1.0}, /* slot 2: blue  */
  };

  ensureCmdbuf();
  for (int i = 0; i < D9MT_SERVER_SURFACES; i++) {
    MTL4RenderPassDescriptor* rp = [[MTL4RenderPassDescriptor alloc] init];
    rp.colorAttachments[0].texture = d9mt_pool_texture(i);
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(
        colors[i][0], colors[i][1], colors[i][2], colors[i][3]);
    id<MTL4RenderCommandEncoder> enc =
        [g->cmdbuf renderCommandEncoderWithDescriptor:rp];
    [enc barrierAfterStages:(MTLStages)kStageFragment
          beforeQueueStages:(MTLStages)kStageAll
          visibilityOptions:(MTL4VisibilityOptions)kVisibilityDevice];
    [enc endEncoding];
  }
  submitFrame();
  if (!waitSerial(g->lastSubmitted, 5000)) {
    fprintf(stderr, "[serverd] liveness clear never completed\n");
    atomic_store_explicit(&g->status->error, d9mt_status_err_metal,
                          memory_order_release);
    return 1;
  }

  /* verify one pixel per surface via CPU readback (shared IOSurface texture) */
  for (int i = 0; i < D9MT_SERVER_SURFACES; i++) {
    uint8_t px[4] = {};
    [d9mt_pool_texture(i) getBytes:px
                       bytesPerRow:4
                        fromRegion:MTLRegionMake2D(0, 0, 1, 1)
                       mipmapLevel:0];
    uint8_t expect[4] = {(uint8_t)(colors[i][2] * 255.0),
                         (uint8_t)(colors[i][1] * 255.0),
                         (uint8_t)(colors[i][0] * 255.0),
                         (uint8_t)(colors[i][3] * 255.0)};
    if (std::memcmp(px, expect, 4) != 0) {
      fprintf(stderr, "[serverd] MTL4 clear verify FAILED (slot %d)\n", i);
      atomic_store_explicit(&g->status->error, d9mt_status_err_clear_verify,
                            memory_order_release);
      return 1;
    }
  }
  fprintf(stderr, "[serverd] MTL4 liveness clear verified (3 slots)\n");
  return 0;
}

} // namespace

/* --- C ABI (d9mt_server.h) ------------------------------------------------------ */

extern "C" int
d9mt_executor_init(struct d9mt_status* status, struct d9mt_wire_heap* heap,
                   uint64_t heap_bytes) {
  g = new Exec();
  g->status = status;
  resetFoldedState();
  g->device = d9mt_pool_device();
  if (!g->device) {
    atomic_store_explicit(&status->error, d9mt_status_err_metal,
                          memory_order_release);
    return 1;
  }
  /* METAL 4 ONLY: MTL4CommandQueue + MTL4Compiler + MTLSharedEvent timeline */
  g->queue = [g->device newMTL4CommandQueue];
  MTL4CompilerDescriptor* cdesc = [[MTL4CompilerDescriptor alloc] init];
  NSError* err = nil;
  g->compiler = [g->device newCompilerWithDescriptor:cdesc error:&err];
  g->event = [g->device newSharedEvent];
  if (!g->queue || !g->compiler || !g->event) {
    fprintf(stderr, "[serverd] MTL4 queue/compiler/event init failed\n");
    atomic_store_explicit(&status->error, d9mt_status_err_metal,
                          memory_order_release);
    return 1;
  }
  /* S3: frame-completion listener (GPU-gated status publication). The queue
   * is frame-critical: USER_INTERACTIVE like every server thread (main.m). */
  dispatch_queue_t lq = dispatch_queue_create(
      "d9mt.frame-complete",
      dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
                                              QOS_CLASS_USER_INTERACTIVE, 0));
  g->listener = [[MTLSharedEventListener alloc] initWithDispatchQueue:lq];

  /* S3 v9: the meta-blit library (tiny MSL, compiled once here — init runs
   * before epoch, sync is fine) + its two samplers. Failure degrades to
   * blit records being dropped (logged). */
  {
    MTL4LibraryDescriptor* ld = [[MTL4LibraryDescriptor alloc] init];
    ld.source = [NSString stringWithUTF8String:kBlitMsl];
    NSError* lerr = nil;
    g->blitLib = [g->compiler newLibraryWithDescriptor:ld error:&lerr];
    if (!g->blitLib)
      fprintf(stderr, "[serverd] blit library compile FAILED: %s\n",
              lerr ? lerr.localizedDescription.UTF8String : "(no error)");
    MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.normalizedCoordinates = YES;
    sd.supportArgumentBuffers = YES;
    sd.minFilter = MTLSamplerMinMagFilterNearest;
    sd.magFilter = MTLSamplerMinMagFilterNearest;
    g->blitSamplers[0] = [g->device newSamplerStateWithDescriptor:sd];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    g->blitSamplers[1] = [g->device newSamplerStateWithDescriptor:sd];
  }

  /* S3: content heap. CPU mapping only — upload records memcpy regions into
   * the slab ring (see the header comment for why a whole-file
   * newBufferWithBytesNoCopy wrap is unsound over a sparse shared file). */
  if (heap && heap_bytes >= heap->data_offset + heap->capacity)
    g->heap = heap;

  MTLResidencySetDescriptor* rdesc = [[MTLResidencySetDescriptor alloc] init];
  rdesc.initialCapacity = 256;
  g->residency = [g->device newResidencySetWithDescriptor:rdesc error:&err];
  if (!g->residency) {
    fprintf(stderr, "[serverd] newResidencySet failed\n");
    atomic_store_explicit(&status->error, d9mt_status_err_metal,
                          memory_order_release);
    return 1;
  }
  [g->queue addResidencySet:g->residency];

  /* dummies */
  g->dummyBuffer = [g->device newBufferWithLength:65536
      options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
  std::memset(g->dummyBuffer.contents, 0, 65536);
  MTLTextureDescriptor* td = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                   width:1
                                  height:1
                               mipmapped:NO];
  td.usage = MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModeShared;
  g->dummyTexture = [g->device newTextureWithDescriptor:td];
  uint32_t black = 0xFF000000u;
  [g->dummyTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                     mipmapLevel:0
                       withBytes:&black
                     bytesPerRow:4];
  MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
  sd.supportArgumentBuffers = YES;
  g->dummySampler = [g->device newSamplerStateWithDescriptor:sd];
  if (!g->dummyBuffer || !g->dummyTexture || !g->dummySampler) {
    fprintf(stderr, "[serverd] dummy resource init failed\n");
    atomic_store_explicit(&status->error, d9mt_status_err_metal,
                          memory_order_release);
    return 1;
  }
  residencyAdd(g->dummyBuffer);
  residencyAdd(g->dummyTexture);
  /* pool IOSurface textures: as render-pass ATTACHMENTS they are resident
   * via the pass descriptor, but the persistent-backbuffer publish COPY
   * writes them from a COMPUTE encoder — which requires explicit residency
   * (a non-resident copy dst is silently dropped: published slots freeze on
   * stale content while draws keep landing). */
  for (int i = 0; i < D9MT_SERVER_SURFACES; i++)
    residencyAdd(d9mt_pool_texture(i));

  /* occlusion-query transport: visibility buffer (GPU-written counters,
   * CPU-read at publication) + the shared result ring inside the heap file
   * (mtl_wire.h d9mt_wire_query_ring). The ring is zeroed ONCE here, before
   * the magic release-store — never on session boundaries (ids are
   * client-process-monotonic; a fresh server process means a fresh client
   * process, so stale file content can never alias a live id). */
  g->visBuf = [g->device newBufferWithLength:(kVisSlots * 8u)
      options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
  if (g->visBuf) {
    g->visCpu = (uint64_t*)g->visBuf.contents;
    std::memset(g->visCpu, 0, kVisSlots * 8u);
    residencyAdd(g->visBuf);
  } else {
    fprintf(stderr, "[serverd] visibility buffer alloc failed — occlusion "
                    "queries resolve all-passed\n");
  }
  if (g->heap) {
    auto* qr = (struct d9mt_wire_query_ring*)((uint8_t*)g->heap +
                                              D9MT_WIRE_QUERY_RING_OFFSET);
    for (uint32_t i = 0; i < D9MT_WIRE_QUERY_SLOTS; i++) {
      atomic_store_explicit(&qr->slots[i].id, 0, memory_order_relaxed);
      atomic_store_explicit(&qr->slots[i].samples, 0, memory_order_relaxed);
      atomic_store_explicit(&qr->slots[i].serial, 0, memory_order_relaxed);
    }
    atomic_store_explicit(&qr->published, 0, memory_order_relaxed);
    atomic_store_explicit(&qr->dropped, 0, memory_order_relaxed);
    qr->capacity = D9MT_WIRE_QUERY_SLOTS;
    atomic_store_explicit(&qr->magic, D9MT_WIRE_QUERY_RING_MAGIC,
                          memory_order_release);
    g->qring = qr;
  }

  g->argTables.push_back(makeArgTablePair());

  /* S3 v2: the create worker (QoS declared inside — main.m discipline) */
  g->creatWorker = std::thread(createWorkerMain);

  /* FLOW-CONTROL gate TEST hook: throttled per-frame execution */
  if (const char* fd = getenv("D9MT_TEST_FRAME_DELAY_US")) {
    g->frameDelayUs = (uint32_t)atoi(fd);
    if (g->frameDelayUs)
      fprintf(stderr, "[serverd] TEST HOOK armed: %u us frame delay\n",
              g->frameDelayUs);
  }

  /* S3 v8: watchdog (diagnostics) + the wedge-simulation TEST hook */
  if (const char* w = getenv("D9MT_WEDGE_CREATE_MS")) {
    g->wedgeTestMs = (uint32_t)atoi(w);
    if (g->wedgeTestMs)
      fprintf(stderr, "[serverd] TEST HOOK armed: first texture create wedges %u ms\n",
              g->wedgeTestMs);
  }
  g->watchdog = std::thread(watchdogMain);

  if (livenessClear())
    return 1;

  atomic_store_explicit(&status->version, D9MT_STATUS_VERSION,
                        memory_order_release);
  return 0;
}

extern "C" void
d9mt_executor_record(const struct d9mt_wire_rec_hdr* hdr) {
  if (!g)
    return;
  switch (hdr->opcode) {
    /* --- S2 cold records (executed) --- */
    case d9mt_wire_op_create_buffer:
      handleCreateBuffer((const d9mt_wire_rec_create_buffer*)hdr);
      break;
    case d9mt_wire_op_create_texture:
      handleCreateTexture((const d9mt_wire_rec_create_texture*)hdr);
      break;
    case d9mt_wire_op_create_shader:
      handleCreateShader((const d9mt_wire_rec_create_shader*)hdr);
      break;
    case d9mt_wire_op_create_sampler:
      handleCreateSampler((const d9mt_wire_rec_create_sampler*)hdr);
      break;
    case d9mt_wire_op_destroy_resource:
      handleDestroy((const d9mt_wire_rec_destroy_resource*)hdr);
      break;
    case d9mt_wire_op_set_render_targets_by_id:
      handleSetRenderTargets((const d9mt_wire_rec_set_render_targets_by_id*)hdr);
      break;
    case d9mt_wire_op_present_frame:
      handlePresent((const d9mt_wire_rec_present_frame*)hdr);
      break;
    case d9mt_wire_op_clear_render_target_by_id:
      handleClearById((const d9mt_wire_rec_clear_render_target_by_id*)hdr);
      break;

    /* --- S3 content / view records --- */
    case d9mt_wire_op_upload_buffer:
      handleUploadBuffer((const d9mt_wire_rec_upload_buffer*)hdr);
      break;
    case d9mt_wire_op_upload_texture:
      handleUploadTexture((const d9mt_wire_rec_upload_texture*)hdr);
      break;
    case d9mt_wire_op_bind_texture_by_id:
      handleBindTextureById((const d9mt_wire_rec_bind_texture_by_id*)hdr);
      break;

    /* --- S3 v5 session boundary (process-lifetime server) --- */
    case d9mt_wire_op_session:
      handleSession((const d9mt_wire_rec_session*)hdr);
      break;

    /* --- S3 v9 copy/blit family --- */
    case d9mt_wire_op_copy_texture:
      handleCopyTexture((const d9mt_wire_rec_copy_texture*)hdr);
      break;
    case d9mt_wire_op_copy_buffer:
      handleCopyBuffer((const d9mt_wire_rec_copy_buffer*)hdr);
      break;
    case d9mt_wire_op_blit_texture_view:
      handleBlit((const d9mt_wire_rec_blit_texture_view*)hdr);
      break;
    case d9mt_wire_op_resolve_texture:
      handleResolveTexture((const d9mt_wire_rec_resolve_texture*)hdr);
      break;
    case d9mt_wire_op_generate_mipmaps:
      handleGenerateMipmaps((const d9mt_wire_rec_generate_mipmaps*)hdr);
      break;
    case d9mt_wire_op_clear_image_view:
      handleClearImageView((const d9mt_wire_rec_clear_image_view*)hdr);
      break;

    /* --- occlusion-query transport (HANDOFF-QUERIES.md) --- */
    case d9mt_wire_op_begin_query:
      handleBeginQuery((const d9mt_wire_rec_query*)hdr);
      break;
    case d9mt_wire_op_end_query:
      handleEndQuery((const d9mt_wire_rec_query*)hdr);
      break;

    /* --- hot-loop state (folded) --- */
    case d9mt_wire_op_bind_shader: {
      const auto* r = (const d9mt_wire_rec_bind_shader*)hdr;
      auto res = r->library ? lookupHandle(r->library) : nullptr;
      if (res && res->kind != Resource::KindShader)
        res = nullptr;
      std::shared_ptr<Shader> sh;
      std::shared_ptr<Resource> pend;
      if (res) {
        /* S3 v2: the library may still be building on the create worker —
         * bind the RESOURCE as pending; draws promote it once ready. */
        const uint32_t state = res->ready.load(std::memory_order_acquire);
        if (state == 0)
          pend = res;
        else if (state == 1)
          sh = res->shader;
        /* failed: both null — draws skip as no-shader */
      }
      if (r->stage == 0) {
        g->vs = std::move(sh);
        g->vsPend = std::move(pend);
      } else {
        g->fs = std::move(sh);
        g->fsPend = std::move(pend);
      }
      g->dirtyPso = true;
      g->dirtyBindings = true; /* CBV slot map is shader-derived */
      break;
    }
    case d9mt_wire_op_set_vertex_layout: {
      const auto* r = (const d9mt_wire_rec_set_vertex_layout*)hdr;
      const auto* wa = (const d9mt_wire_vertex_attr*)(r + 1);
      const auto* wl = (const d9mt_wire_vertex_layout*)(wa + r->n_attr);
      g->attrs.assign(wa, wa + r->n_attr);
      g->layouts.assign(wl, wl + r->n_layout);
      g->dirtyPso = true;
      break;
    }
    case d9mt_wire_op_set_primitive_topology:
      g->topology = ((const d9mt_wire_rec_set_primitive_topology*)hdr)->topology;
      break;
    case d9mt_wire_op_set_viewports: {
      const auto* r = (const d9mt_wire_rec_set_viewports*)hdr;
      if (r->count) {
        g->viewport = *(const d9mt_wire_viewport*)(r + 1);
        g->viewportSet = true;
        g->dirtyState = true;
      }
      break;
    }
    case d9mt_wire_op_set_rasterizer_state:
      g->raster = *(const d9mt_wire_rec_set_rasterizer_state*)hdr;
      g->dirtyState = true;
      break;
    case d9mt_wire_op_set_multisample_state:
      g->msaa = *(const d9mt_wire_rec_set_multisample_state*)hdr;
      g->dirtyPso = true;
      break;
    case d9mt_wire_op_set_depth_stencil_state:
      g->depthStencil = *(const d9mt_wire_rec_set_depth_stencil_state*)hdr;
      g->depthStencilSeen = true; /* V12: default-DSSO observability */
      g->dirtyState = true;
      break;
    case d9mt_wire_op_set_blend_mode: {
      const auto* r = (const d9mt_wire_rec_set_blend_mode*)hdr;
      if (r->rt < 4) {
        g->blend[r->rt] = *r;
        g->dirtyPso = true;
      }
      break;
    }
    case d9mt_wire_op_set_blend_color: {
      const auto* r = (const d9mt_wire_rec_set_blend_color*)hdr;
      std::memcpy(g->blendColor, r->color, sizeof(g->blendColor));
      g->dirtyState = true;
      break;
    }
    case d9mt_wire_op_set_stencil_reference:
      g->stencilRef = ((const d9mt_wire_rec_set_stencil_reference*)hdr)->reference;
      g->dirtyState = true;
      break;
    case d9mt_wire_op_set_depth_bias: {
      const auto* r = (const d9mt_wire_rec_set_depth_bias*)hdr;
      g->depthBias[0] = r->constant_factor;
      g->depthBias[1] = r->slope_factor;
      g->depthBias[2] = r->clamp;
      g->dirtyState = true;
      break;
    }

    /* --- binds --- */
    case d9mt_wire_op_bind_vertex_buffer: {
      const auto* r = (const d9mt_wire_rec_bind_vertex_buffer*)hdr;
      if (r->binding < kMaxStreams) {
        Stream& s = g->streams[r->binding];
        auto res = r->buf.buffer ? lookupHandle(r->buf.buffer) : nullptr;
        s.buf = res ? res->buffer : nil;
        s.res = res;
        s.offset = r->buf.offset;
        s.length = r->buf.length;
        s.stride = r->stride;
        s.bound = r->buf.buffer != 0;
        g->dirtyBindings = true;
        g->dirtyPso = true; /* stride participates in the PSO key */
      }
      break;
    }
    case d9mt_wire_op_bind_vertex_buffer_range: {
      const auto* r = (const d9mt_wire_rec_bind_vertex_buffer_range*)hdr;
      if (r->binding < kMaxStreams && g->streams[r->binding].bound) {
        Stream& s = g->streams[r->binding];
        s.offset = r->offset;
        s.length = r->length;
        if (s.stride != r->stride)
          g->dirtyPso = true;
        s.stride = r->stride;
        g->dirtyBindings = true;
      }
      break;
    }
    case d9mt_wire_op_bind_index_buffer: {
      const auto* r = (const d9mt_wire_rec_bind_index_buffer*)hdr;
      auto res = r->buf.buffer ? lookupHandle(r->buf.buffer) : nullptr;
      g->index.buf = res ? res->buffer : nil;
      g->index.res = res;
      g->index.offset = r->buf.offset;
      g->index.length = r->buf.length;
      g->index.type = r->index_type;
      g->index.bound = g->index.buf != nil;
      break;
    }
    case d9mt_wire_op_bind_uniform_buffer: {
      const auto* r = (const d9mt_wire_rec_bind_uniform_buffer*)hdr;
      for (uint32_t st = 0; st < 2; st++) {
        if (r->stages & (1u << st) && r->slot < 8) {
          auto res = r->buf.buffer ? lookupHandle(r->buf.buffer) : nullptr;
          CbvBinding& cbv = g->stages[st].cbvs[r->slot];
          cbv.buf = res ? res->buffer : nil;
          cbv.res = res;
          cbv.offset = r->buf.offset;
          cbv.length = r->buf.length;
          cbv.bound = cbv.buf != nil;
        }
      }
      g->dirtyBindings = true;
      break;
    }
    case d9mt_wire_op_bind_uniform_buffer_range: {
      const auto* r = (const d9mt_wire_rec_bind_uniform_buffer_range*)hdr;
      for (uint32_t st = 0; st < 2; st++) {
        if (r->stages & (1u << st) && r->slot < 8) {
          g->stages[st].cbvs[r->slot].offset = r->offset;
          g->stages[st].cbvs[r->slot].length = r->length;
        }
      }
      g->dirtyBindings = true;
      break;
    }
    case d9mt_wire_op_bind_texture: {
      /* client VIEW handles have no wire identity in S2 — track bound-ness
       * only; the flush binds the dummy so sampling stays defined. */
      const auto* r = (const d9mt_wire_rec_bind_texture*)hdr;
      for (uint32_t st = 0; st < 2; st++) {
        if (r->stages & (1u << st) && r->slot < kMaxTexTrack)
          g->stages[st].texBound[r->slot] = r->view != 0;
      }
      g->dirtyBindings = true;
      break;
    }
    case d9mt_wire_op_bind_sampler: {
      const auto* r = (const d9mt_wire_rec_bind_sampler*)hdr;
      for (uint32_t st = 0; st < 2; st++) {
        if (r->stages & (1u << st) && r->slot < kMaxTexTrack) {
          auto res = r->sampler ? lookupHandle(r->sampler) : nullptr;
          g->stages[st].samplers[r->slot] =
              res && res->ready.load(std::memory_order_acquire) == 1
                  ? res->sampler : nil;
        }
      }
      g->dirtyBindings = true;
      break;
    }
    case d9mt_wire_op_push_data: {
      const auto* r = (const d9mt_wire_rec_push_data*)hdr;
      const uint8_t* payload = (const uint8_t*)(r + 1);
      /* Mirror MtlContextImpl::pushData exactly: the record carries the RAW
       * frontend offset; a single-stage block shifts past the 32-byte shared
       * D3D9SharedPushData reservation (kPushSharedBlockSize). */
      const bool isShared = (r->stages & (r->stages - 1u)) != 0u;
      const uint32_t baseOff = isShared ? 0u : 32u;
      for (uint32_t st = 0; st < 2; st++) {
        if (r->stages & (1u << st)) {
          StageState& b = g->stages[st];
          if ((uint64_t)baseOff + r->offset + r->size <= sizeof(b.pushData)) {
            std::memcpy(b.pushData + baseOff + r->offset, payload, r->size);
            if (baseOff + r->offset + r->size > b.pushExtent)
              b.pushExtent = baseOff + r->offset + r->size;
            b.pushDirty = true;
          }
        }
      }
      g->dirtyBindings = true;
      break;
    }

    /* --- draws --- */
    case d9mt_wire_op_draw:
      handleDraw((const d9mt_wire_rec_draw*)hdr);
      break;
    case d9mt_wire_op_draw_indexed:
      handleDrawIndexed((const d9mt_wire_rec_draw_indexed*)hdr);
      break;

    /* op 10 (bind_render_targets, handle-based) and op 22 (clear, view-
     * handle-based) are COUNTED by the consumer and superseded by their
     * by-id twins (op 30 / op 32) which the client emits from the same
     * calls. Detach/pad never reach here. */
    default:
      break;
  }
}

extern "C" void
d9mt_executor_finish(void) {
  if (!g)
    return;
  if (g->watchdog.joinable()) {
    g->watchdogStop.store(true, std::memory_order_release);
    g->watchdog.join();
  }
  /* stop the create worker (drains queued jobs first — cheap, bounded) */
  if (g->creatWorker.joinable()) {
    {
      std::lock_guard<std::mutex> lock(g->creatMutex);
      g->creatStop = true;
    }
    g->creatCv.notify_one();
    g->creatWorker.join();
  }
  submitFrame();
  if (g->lastSubmitted && !waitSerial(g->lastSubmitted, 2000))
    fprintf(stderr, "[serverd] finish: GPU drain timed out\n");
  if (g->qActive.on)
    endActiveQuery();
  sweepQueries(); /* final publication after the drain */
  atomic_store_explicit(&g->status->completed_serial, completedSerial(),
                        memory_order_relaxed);
  if (g->qBegun)
    fprintf(stderr,
            "[serverd] queries: begun=%llu published=%llu nested=%llu "
            "orphan=%llu overflow=%llu sessEnd=%llu\n",
            (unsigned long long)g->qBegun, (unsigned long long)g->qPublished,
            (unsigned long long)g->qNested, (unsigned long long)g->qOrphan,
            (unsigned long long)g->qOverflow,
            (unsigned long long)g->qSessionEnd);
  fprintf(stderr,
          "[serverd] executor: frames=%llu encoders=%llu draws=%llu skipped=%llu "
          "psos=%llu errors=%llu\n",
          (unsigned long long)atomic_load_explicit(&g->status->frames,
                                                   memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&g->status->encoders,
                                                   memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&g->status->draws_executed,
                                                   memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&g->status->draws_skipped,
                                                   memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&g->status->psos_built,
                                                   memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&g->status->exec_errors,
                                                   memory_order_relaxed));
}
