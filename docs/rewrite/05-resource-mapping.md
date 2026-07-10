# 05 — Resource-creation semantic mapping (Vulkan → Metal-first)

Status: NORMATIVE for the device-graph port. The d3d9 frontend's resource
creation sites are the one place the port is a semantic rewrite, not a token
swap. This is the complete decision table; do not re-derive it.

## Principle

Vulkan splits intent across usage bits + stage/access masks + memory
properties. Metal needs only: (a) which slots it can bind to (usage), (b) who
writes/reads it from the CPU (storage mode). Everything stage/access-shaped
DIES at this boundary — hazards are the backend's job (untracked resources +
explicit barriers, doc 00 §3).

## Buffers — D3D9CommonBuffer patterns

D3D9 buffers reduce to exactly three creation patterns:

| D3D9 case | DXVK creates | Ours |
|---|---|---|
| DEFAULT-pool VB/IB (GPU-read, Lock via staging or direct) | `VERTEX/INDEX_BUFFER \| TRANSFER_DST/SRC`, DEVICE_LOCAL (+ HOST_VISIBLE when mappable) | usage `VertexBuffer\|IndexBuffer` as declared; cpuAccess `Write` if D3D9 usage is DYNAMIC or the mapping path is direct, else `None` |
| SYSTEMMEM / MANAGED backing (CPU copy the GPU never binds) | HOST_VISIBLE \| HOST_COHERENT staging | usage `Staging`; cpuAccess `Write` (readback paths: `Read`) |
| Constant-buffer ring (`d3d9_constant_buffer`) | UNIFORM_BUFFER, HOST_VISIBLE, per-draw offset | usage `UniformBuffer`; cpuAccess `Write` |

Rules:
- `VK_BUFFER_USAGE_TRANSFER_SRC/DST` → drop; every MtlBuffer is blit-capable.
- Stage masks (`VK_PIPELINE_STAGE_*`) and access masks → drop entirely.
- `VK_MEMORY_PROPERTY_DEVICE_LOCAL` alone → `MtlCpuAccess::None`;
  `HOST_VISIBLE|HOST_COHERENT` (+`HOST_CACHED`) → `Write` (`Read` iff the
  site exists for readback, i.e. GetRenderTargetData staging).
- `D3DUSAGE_DYNAMIC` / `D3DLOCK_DISCARD`-reachable buffers must be
  `MtlCpuAccess::Write` — the rename ring (allocateStorage) requires shared
  storage with a CPU pointer.

## Images — D3D9CommonTexture patterns

| D3D9 case | DXVK creates | Ours |
|---|---|---|
| Sampled texture (any pool) | `SAMPLED \| TRANSFER_*`, OPTIMAL, DEVICE_LOCAL | usage `Sampled`; cpuAccess `None`; uploads go through staging + copyBufferToTexture (never linear-mapped GPU textures) |
| Render target / D3DUSAGE_RENDERTARGET | `COLOR_ATTACHMENT \| SAMPLED` | usage `RenderTarget\|Sampled` |
| Depth-stencil / D3DUSAGE_DEPTHSTENCIL | `DEPTH_STENCIL_ATTACHMENT (\| SAMPLED)` | usage `DepthStencil` (+`Sampled` iff shadow-sampled — the INTZ/shadow path) |
| SYSTEMMEM/SCRATCH textures (CPU-only) | LINEAR host-visible image or buffer-backed | do NOT create a Metal texture: buffer-backed (`Staging` buffer + layout math, doc 02 §4 keeps DXVK's mapping-mode split) |
| Backbuffer/swap image | swapchain-owned | comes from MtlPresenter, never createImage |

Rules:
- `VK_IMAGE_TILING_LINEAR` → does not exist; any linear-image path maps to
  the buffer-backed mapping mode.
- `VK_IMAGE_LAYOUT_*` / layout transitions → delete (transformImage no-ops;
  barriers are backend-inserted).
- CUBE flag → `cube=true, arrayLayers=6`; 3D → `MtlImageType::e3D`;
  MSAA → sampleCount, and resolve targets get their own image (the
  MtlTexture::resolveTexture slot).
- `VK_IMAGE_USAGE_TRANSFER_*` → drop (always copyable).
- Format: through `D3D9_MTL_FORMAT_MAPPING` (d3d9_format.cpp, done); typeless
  / depth-read variants pick the view-key format, not a second image.

## Queries (contract 3d)

- OCCLUSION → backend visibility-buffer counter (doc 01); EVENT → submission
  serial compare (`getCompletedSerial() >= serialAtIssue`); TIMESTAMP →
  serial-based fake like DXVK's (D3D9 games only gate on pass/fail).

## The `D3D9DeviceEx` compile-coupling rule

Port headers first, bodies second: d3d9_device.h + common_buffer.h +
common_texture.h + resource headers form the type graph every .cpp needs.
Stage 1 = all headers retyped (gate: a single probe TU that includes
d3d9_device.h compiles). Stage 2 = .cpp bodies in dependency order. Never
attempt a .cpp before its header set passes the probe.
