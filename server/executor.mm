/*
 * executor.mm — d9mt-serverd Phase S2 record executor.
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
 * Threading: single-threaded, called only from the consumer thread (which
 * runs at QOS_CLASS_USER_INTERACTIVE — set in main.m).
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

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "d9mt_server.h"

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
constexpr uint32_t kMaxTexSlots = 16u;
constexpr uint32_t kFanMaxVerts = 16386u;
constexpr uint32_t kUploadSlabBytes = 2u << 20;

/* Bridge-numeric MTL4 stage/visibility values (winemetal.h WMTStage*): the
 * bridge casts these straight into MTLStages/MTL4VisibilityOptions, so the
 * numbers ARE the SDK values. */
constexpr uint64_t kStageFragment = 1ull << 1;
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

struct Resource {
  enum Kind { KindBuffer, KindTexture, KindSampler, KindShader } kind;
  uint64_t wireId = 0;
  uint64_t clientHandle = 0;
  id<MTLBuffer> buffer = nil;
  id<MTLTexture> texture = nil;
  id<MTLSamplerState> sampler = nil;
  std::shared_ptr<Shader> shader;
};

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

/* --- folded state (mirrors MtlContextImpl's fold members) ------------------- */

struct Stream {
  id<MTLBuffer> buf = nil;
  uint64_t offset = 0, length = 0;
  uint32_t stride = 0;
  bool bound = false;
};

struct CbvBinding {
  id<MTLBuffer> buf = nil;
  uint64_t offset = 0;
  bool bound = false;
};

struct StageState {
  CbvBinding cbvs[8];
  bool texBound[kMaxTexSlots] = {};
  id<MTLSamplerState> samplers[kMaxTexSlots] = {};
  uint8_t pushData[4096] = {};
  uint32_t pushExtent = 0;
  bool pushDirty = false;
  uint64_t pushGpuAddr = 0;
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

struct Exec {
  struct d9mt_status* status = nullptr;

  id<MTLDevice> device = nil;
  id<MTL4CommandQueue> queue = nil;
  id<MTL4Compiler> compiler = nil;
  id<MTLSharedEvent> event = nil; /* completion timeline (decision 5 port) */
  uint64_t serial = 0;            /* serial of the CURRENT recording cmdbuf */
  uint64_t lastSubmitted = 0;

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

  /* dummies: defined sampling/fetch for unresolvable binds */
  id<MTLBuffer> dummyBuffer = nil;   /* 64 KiB zeroed */
  id<MTLTexture> dummyTexture = nil; /* 1x1 black */
  id<MTLSamplerState> dummySampler = nil;
  id<MTLBuffer> fanIndices = nil;

  /* folded state */
  std::shared_ptr<Shader> vs, fs;
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
  } index;

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
  std::unordered_map<PsoKey, id<MTLRenderPipelineState>, PsoKeyHash, PsoKeyEq> psoCache;
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

  bool warnedPsoFail = false;
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

static void submitFrame() {
  closePass();
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
}

/* --- upload ring (push data / AB word slices) -------------------------------- */

struct RingSlice {
  void* cpu = nullptr;
  uint64_t gpu = 0;
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
    return { (uint8_t*)s.buf.contents + off, s.buf.gpuAddress + off };
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
  return { buf.contents, buf.gpuAddress };
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

static void materializePass() {
  if (g->pass.active)
    return;
  ensureCmdbuf();

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

static id<MTLRenderPipelineState> resolvePso() {
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
  if (it != g->psoCache.end())
    return it->second; /* may be nil: failed build cached to avoid retry storms */

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

  NSError* err = nil;
  id<MTLRenderPipelineState> pso =
      [g->compiler newRenderPipelineStateWithDescriptor:desc
                                    compilerTaskOptions:nil
                                                  error:&err];
  if (pso) {
    atomic_fetch_add_explicit(&g->status->psos_built, 1, memory_order_relaxed);
  } else {
    execError("pipeline build failed");
    if (!g->warnedPsoFail) {
      g->warnedPsoFail = true;
      fprintf(stderr, "[serverd] first PSO failure: %s\n",
              err ? err.localizedDescription.UTF8String : "(no error)");
    }
  }
  g->psoCache.emplace(key, pso); /* nil cached too */
  return pso;
}

/* --- depth-stencil state cache (port of getDepthStencilState) ---------------- */

static id<MTLDepthStencilState> resolveDsso() {
  std::string key(reinterpret_cast<const char*>(&g->depthStencil.depth_compare_op),
                  sizeof(g->depthStencil) - sizeof(d9mt_wire_rec_hdr));
  auto it = g->dssoCache.find(key);
  if (it != g->dssoCache.end())
    return it->second;

  MTLDepthStencilDescriptor* desc = [[MTLDepthStencilDescriptor alloc] init];
  desc.depthCompareFunction = (MTLCompareFunction)g->depthStencil.depth_compare_op;
  desc.depthWriteEnabled = g->depthStencil.depth_write != 0;
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

    /* push data -> fresh ring slice (snapshot semantics) */
    if (b.pushDirty && b.pushExtent) {
      RingSlice rs = ringAlloc(b.pushExtent, 256);
      if (rs.cpu) {
        std::memcpy(rs.cpu, b.pushData, b.pushExtent);
        b.pushGpuAddr = rs.gpu;
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
            v = g->dummyTexture.gpuResourceID._impl;
            break;
          case 2: /* Sampler */
            v = (e.second < kMaxTexSlots && b.samplers[e.second])
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
      /* every slot defined: dummy for bound-but-unresolvable AND unbound */
      [table setTexture:g->dummyTexture.gpuResourceID atIndex:slot];
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

static bool prepareDraw() {
  if (!g->pass.pending && !g->pass.active) {
    atomic_fetch_add_explicit(&g->status->draws_skipped, 1, memory_order_relaxed);
    return false;
  }
  if (!g->pass.active) {
    if (!passHasValidAttachment()) {
      atomic_fetch_add_explicit(&g->status->draws_skipped, 1, memory_order_relaxed);
      return false;
    }
    materializePass();
  }
  if (!g->pass.active) { /* encoder creation failed */
    atomic_fetch_add_explicit(&g->status->draws_skipped, 1, memory_order_relaxed);
    return false;
  }

  id<MTLRenderPipelineState> pso = nil;
  if (g->dirtyPso || !g->lastPso) {
    pso = resolvePso();
    if (!pso) {
      atomic_fetch_add_explicit(&g->status->draws_skipped, 1, memory_order_relaxed);
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

  if (g->dirtyBindings) {
    flushBindings();
    g->dirtyBindings = false;
  }
  if (g->dirtyState || !g->dynValid) {
    flushDynamicState();
    g->dirtyState = false;
  }
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

static void handleCreateBuffer(const d9mt_wire_rec_create_buffer* r) {
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

static void handleCreateTexture(const d9mt_wire_rec_create_texture* r) {
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

  id<MTLTexture> tex = [g->device newTextureWithDescriptor:td];
  if (!tex) {
    execError("create_texture failed");
    return;
  }
  residencyAdd(tex);
  auto res = std::make_shared<Resource>();
  res->kind = Resource::KindTexture;
  res->wireId = r->wire_id;
  res->clientHandle = r->client_handle;
  res->texture = tex;
  registerResource(std::move(res));
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

  dispatch_data_t data = dispatch_data_create(
      p.bytes.data(), p.bytes.size(), NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError* err = nil;
  id<MTLLibrary> lib = [g->device newLibraryWithData:data error:&err];
  if (!lib) {
    execError("newLibraryWithData failed");
    g->pendingShaders.erase(r->client_handle);
    return;
  }
  auto sh = std::make_shared<Shader>();
  sh->library = lib;
  char entry[33] = {};
  std::memcpy(entry, p.meta.entry, 32);
  sh->entry = entry[0] ? entry : "main";
  sh->stage = p.meta.stage;
  sh->ab = p.meta.ab != 0;
  std::memcpy(sh->cbvMap, p.meta.cbv_slot_map, 8);
  sh->abTableIndex = p.meta.ab_table_index ? p.meta.ab_table_index : 21u;
  sh->abWords = std::move(p.abWords);

  auto res = std::make_shared<Resource>();
  res->kind = Resource::KindShader;
  res->wireId = p.meta.wire_id;
  res->clientHandle = p.meta.client_handle;
  res->shader = std::move(sh);
  registerResource(std::move(res));
  g->pendingShaders.erase(r->client_handle);
}

static void handleCreateSampler(const d9mt_wire_rec_create_sampler* r) {
  MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
  sd.minFilter = (MTLSamplerMinMagFilter)r->min_filter;
  sd.magFilter = (MTLSamplerMinMagFilter)r->mag_filter;
  sd.mipFilter = (MTLSamplerMipFilter)r->mip_mode;
  sd.sAddressMode = (MTLSamplerAddressMode)r->address_u;
  sd.tAddressMode = (MTLSamplerAddressMode)r->address_v;
  sd.rAddressMode = (MTLSamplerAddressMode)r->address_w;
  sd.borderColor = (MTLSamplerBorderColor)r->border_color;
  sd.compareFunction = r->compare_enable ? (MTLCompareFunction)r->compare_op
                                         : MTLCompareFunctionNever;
  sd.lodMinClamp = r->min_lod;
  sd.lodMaxClamp = r->max_lod;
  sd.maxAnisotropy = r->max_anisotropy ? r->max_anisotropy : 1;
  sd.normalizedCoordinates = YES;
  sd.supportArgumentBuffers = YES;
  id<MTLSamplerState> smp = [g->device newSamplerStateWithDescriptor:sd];
  if (!smp) {
    execError("create_sampler failed");
    return;
  }
  auto res = std::make_shared<Resource>();
  res->kind = Resource::KindSampler;
  res->wireId = r->wire_id;
  res->clientHandle = r->client_handle;
  res->sampler = smp;
  registerResource(std::move(res));
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
  if (res->clientHandle)
    g->byHandle.erase(res->clientHandle);
  if (res->wireId)
    g->byWire.erase(res->wireId);
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
      if (d9mt_iosurface_pool_resize(g->status, ref.width, ref.height))
        execError("pool resize failed");
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
    if (ref.flags & D9MT_WIRE_RT_FLAG_BACKBUFFER) {
      out.backbuffer = true;
      out.tex = d9mt_pool_texture((int)g->presentSlot);
      out.format = MTLPixelFormatBGRA8Unorm; /* IOSurface pool format */
      out.baseMip = out.baseLayer = 0;
      return;
    }
    auto res = lookupWire(ref.image_wire_id);
    if (res && res->texture)
      out.tex = res->texture;
    /* unresolved: named-but-nil — draws needing it are skipped */
  };
  for (uint32_t i = 0; i < 4; i++)
    resolve(r->color[i], fresh.color[i]);
  resolve(r->depth_stencil, fresh.ds);
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
    target.tex = d9mt_pool_texture((int)g->presentSlot);
    target.format = MTLPixelFormatBGRA8Unorm;
    target.named = true;
    target.backbuffer = true;
  } else {
    auto res = lookupWire(r->image_wire_id);
    if (!res || !res->texture)
      return; /* unresolvable clear target — drop silently (S2 subset) */
    target.tex = res->texture;
    target.format = res->texture.pixelFormat;
    target.named = true;
    target.wireId = r->image_wire_id;
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

static void handlePresent(const d9mt_wire_rec_present_frame* r) {
  uint32_t slot = g->presentSlot;
  submitFrame();
  g->backbufferWireId = r->backbuffer_wire_id;

  atomic_store_explicit(&g->status->present_slot, slot, memory_order_relaxed);
  atomic_store_explicit(&g->status->frame_serial, r->frame_serial,
                        memory_order_release);
  atomic_fetch_add_explicit(&g->status->frames, 1, memory_order_relaxed);
  atomic_store_explicit(&g->status->completed_serial, completedSerial(),
                        memory_order_relaxed);

  g->presentSlot = (g->presentSlot + 1) % D9MT_SERVER_SURFACES;
  /* fresh frame: the pending pass carries the OLD slot's backbuffer texture;
   * re-point flagged attachments at the new slot on the next bind (the client
   * re-binds render targets every frame — until then, drop backbuffer draws) */
  for (auto& c : g->pass.color)
    if (c.backbuffer)
      c.tex = nil;
  if (g->pass.ds.backbuffer)
    g->pass.ds.tex = nil;
}

/* --- draws ------------------------------------------------------------------- */

static void handleDraw(const d9mt_wire_rec_draw* r) {
  const d9mt_wire_draw_args* args = (const d9mt_wire_draw_args*)(r + 1);
  if (!prepareDraw()) {
    /* prepareDraw counted ONE skip; count the rest of the batch */
    if (r->count > 1)
      atomic_fetch_add_explicit(&g->status->draws_skipped, r->count - 1,
                                memory_order_relaxed);
    return;
  }
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
    return;
  }
  if (!prepareDraw()) {
    if (r->count > 1)
      atomic_fetch_add_explicit(&g->status->draws_skipped, r->count - 1,
                                memory_order_relaxed);
    return;
  }
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
d9mt_executor_init(struct d9mt_status* status) {
  g = new Exec();
  g->status = status;
  /* fold-state defaults MUST mirror the client's (mtl_types.h defaults) —
   * zero-init is NOT the D3D9 default: writeMask=0xF (a zero mask silently
   * kills all color writes), depth compare Always, stencil masks 0xFF. */
  for (auto& b : g->blend) {
    b.write_mask = 0xFu;
    b.color_dst = b.alpha_dst = 0u; /* Zero */
    b.color_src = b.alpha_src = 1u; /* One */
  }
  g->depthStencil.depth_compare_op = 7u; /* Always */
  g->depthStencil.front.compare_op = g->depthStencil.back.compare_op = 7u;
  g->depthStencil.front.read_mask = g->depthStencil.back.read_mask = 0xFFu;
  g->depthStencil.front.write_mask = g->depthStencil.back.write_mask = 0xFFu;
  g->msaa.sample_count = 1u;
  g->msaa.sample_mask = ~0u;
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

  g->argTables.push_back(makeArgTablePair());

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

    /* --- hot-loop state (folded) --- */
    case d9mt_wire_op_bind_shader: {
      const auto* r = (const d9mt_wire_rec_bind_shader*)hdr;
      auto res = r->library ? lookupHandle(r->library) : nullptr;
      auto sh = res && res->kind == Resource::KindShader ? res->shader : nullptr;
      if (r->stage == 0)
        g->vs = sh;
      else
        g->fs = sh;
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
          cbv.offset = r->buf.offset;
          cbv.bound = cbv.buf != nil;
        }
      }
      g->dirtyBindings = true;
      break;
    }
    case d9mt_wire_op_bind_uniform_buffer_range: {
      const auto* r = (const d9mt_wire_rec_bind_uniform_buffer_range*)hdr;
      for (uint32_t st = 0; st < 2; st++) {
        if (r->stages & (1u << st) && r->slot < 8)
          g->stages[st].cbvs[r->slot].offset = r->offset;
      }
      g->dirtyBindings = true;
      break;
    }
    case d9mt_wire_op_bind_texture: {
      /* client VIEW handles have no wire identity in S2 — track bound-ness
       * only; the flush binds the dummy so sampling stays defined. */
      const auto* r = (const d9mt_wire_rec_bind_texture*)hdr;
      for (uint32_t st = 0; st < 2; st++) {
        if (r->stages & (1u << st) && r->slot < kMaxTexSlots)
          g->stages[st].texBound[r->slot] = r->view != 0;
      }
      g->dirtyBindings = true;
      break;
    }
    case d9mt_wire_op_bind_sampler: {
      const auto* r = (const d9mt_wire_rec_bind_sampler*)hdr;
      for (uint32_t st = 0; st < 2; st++) {
        if (r->stages & (1u << st) && r->slot < kMaxTexSlots) {
          auto res = r->sampler ? lookupHandle(r->sampler) : nullptr;
          g->stages[st].samplers[r->slot] = res ? res->sampler : nil;
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
  submitFrame();
  if (g->lastSubmitted && !waitSerial(g->lastSubmitted, 2000))
    fprintf(stderr, "[serverd] finish: GPU drain timed out\n");
  atomic_store_explicit(&g->status->completed_serial, completedSerial(),
                        memory_order_relaxed);
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
