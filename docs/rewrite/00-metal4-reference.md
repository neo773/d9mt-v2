# Metal 4 API Reference Notes (research extract)

Source: research-agent extract from Apple documentation (2026-07). Basis for the
Metal-first MtlContext design in 01-seam-mtlcontext.md. Driver ships first on
the Metal-3-era winemetal ABI; concepts below shape the interface so the
Metal 4 bridge (upstream DXMT `winemetal4/`) is a swap, not a redesign.

## Raw extract

- Apple Silicon only (A14/M1+), macOS 26+ / iOS 26+ family.
- **MTL4CommandAllocator**: per-thread command memory; lock-free parallel
  encoding; reusable after `endCommandBuffer()`.
- **MTL4CommandBuffer**: created from device (not queue); explicit
  `beginCommandBuffer(allocator:)` / `endCommandBuffer()`; no implicit commit.
- **MTL4CommandQueue**: batched `commit([MTL4CommandBuffer])`.
- **MTL4ComputeCommandEncoder**: unified compute+blit+accel-structure encoder.
- **MTL4RenderCommandEncoder**: replaces ParallelRenderCommandEncoder;
  explicit render-pass suspend/resume across command buffers.
- **MTL4ArgumentTable**: THE binding mechanism (replaces setBuffer/setTexture
  per stage); driver captures a zero-overhead snapshot of bindings at dispatch
  time. MTLTextureViewPool gives lightweight views with unique MTLResourceIDs.
- **Untracked by default**: no automatic hazard tracking; explicit
  producer/consumer barriers via MTL4VisibilityOptions injected in the command
  sequence. Residency via MTLResidencySet attached to queue or command buffer.
- **MTL4RenderPipelineDescriptor**: takes MTL4LibraryFunctionDescriptor (not
  MTLFunction); depth/stencil attachment pixel formats NOT required at PSO
  creation (deferred to runtime); blending booleans became typed enums.
- **MTL4Compiler**: explicit compiler object, background QoS queues, async
  makeRenderPipelineState, MTL4CompilerTaskOptions;
  MTL4PipelineDataSetSerializer harvests compiled pipelines to binary archives.
- MSL 4 = C++17 base; MTLTensor / ML encoder / cooperative tensors (not
  relevant to D3D9 driver).
- MetalFX: neural temporal upscaler (M5-class), dynamic subrectangle
  resolution, HW frame interpolation, integrated AI denoiser.

## Implications for this driver (delta vs our v1/v2 experience)

1. **PSO keying shrinks**: depth/stencil formats out of the PSO descriptor →
   fewer pipeline variants than the Vulkan-shaped keying v1 carried. Design
   MtlContext PSO key WITHOUT depth format from day one.
2. **Argument-table snapshot-at-dispatch** is exactly the per-draw semantics
   we hand-built rings for (the shared-AB-overwrite black-screen class).
   Design binding API around slot tables with snapshot semantics; Metal 3
   backend emulates via per-draw ring, Metal 4 backend gets it native.
3. **Untracked resources + explicit barriers** fit a D3D9 CS-thread driver:
   the state tracker knows every RT→texture transition; auto-tracking CPU cost
   disappears. MtlContext exposes explicit barrier points; Metal 3 impl may
   no-op some (Metal 3 tracks automatically).
4. **Batched commit + per-thread allocators**: our watchdog-motivated
   command-buffer splits get cheaper; single CS thread first, allocator model
   allows parallel encode later.
5. **MTL4Compiler + DataSetSerializer** replaces the v1 metallib disk cache
   entirely (kills the poisoned-toolchain-cache bug class); our AIR metallibs
   feed MTL4LibraryFunctionDescriptor directly.
6. **Render-pass suspend/resume** helps the pass-per-offscreen-target pattern
   deferred renderers force on us.
7. Bridge reality: all of this must flow through winemetal under Wine —
   Metal 3 winemetal now, DXMT upstream `winemetal4/` when we swap.
