/*
 * iosurface_pool.m — d9mt-serverd swapchain IOSurface pool (Phase S1).
 *
 * Creates D9MT_SERVER_SURFACES (3) BGRA8 IOSurfaces at the requested size,
 * wraps each as an MTLTexture and GPU-clears each to a distinct solid color
 * via an empty render pass (LoadActionClear + StoreActionStore) — proving
 * the native process owns a live Metal device whose work lands in
 * cross-process-visible surfaces. IDs are published into
 * status->iosurface_id[0..2] with iosurface_serial=1 (generation 1).
 *
 * The clear is verified by CPU readback of one pixel per surface: the S1
 * gate is "GPU work demonstrably landed", not "encoder ran without error".
 *
 * TODO(METAL4): this file uses the Metal 3 API surface (MTLCommandQueue /
 * MTLCommandBuffer) purely for the S1 skeleton's liveness clear. The real
 * S2 backend port must be MTL4-only per docs/SOURCE_OF_TRUTH.md §2
 * (MTL4CommandQueue + MTL4CommandBuffer + residency sets) — rip this out
 * when the backend state machine moves in.
 */

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <stdio.h>

#include "d9mt_server.h"

/* Keep the surfaces alive for the process lifetime: the client resolves
 * them by ID via IOSurfaceLookup, which fails once the last reference in
 * any process is gone. IOSurfaceRefs are CF objects — not ARC-managed. */
static IOSurfaceRef g_surfaces[D9MT_SERVER_SURFACES];
static id<MTLTexture> g_textures[D9MT_SERVER_SURFACES];
static id<MTLDevice> g_device;

int
d9mt_iosurface_pool_init(struct d9mt_status *status, uint32_t width,
                         uint32_t height) {
  /* distinct solid colors per slot (BGRA readback bytes below) */
  static const double colors[D9MT_SERVER_SURFACES][4] = {
      {1.0, 0.0, 0.0, 1.0}, /* slot 0: red   */
      {0.0, 1.0, 0.0, 1.0}, /* slot 1: green */
      {0.0, 0.0, 1.0, 1.0}, /* slot 2: blue  */
  };

  g_device = MTLCreateSystemDefaultDevice();
  if (!g_device) {
    fprintf(stderr, "[serverd] no Metal device\n");
    atomic_store_explicit(&status->error, d9mt_status_err_metal,
                          memory_order_release);
    return 1;
  }
  id<MTLCommandQueue> queue = [g_device newCommandQueue];

  for (int i = 0; i < D9MT_SERVER_SURFACES; i++) {
    /* kIOSurfaceIsGlobal is deprecated but makes the surface visible to
     * IOSurfaceLookup cross-process without a mach-port transfer (Chromium
     * shipped on it for years; PoC P2/P4 verified it against the real wine
     * process). If this stops working on a future macOS the fallback is
     * IOSurfaceCreateMachPort + a bootstrap-registered port. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSDictionary *props = @{
      (id)kIOSurfaceWidth : @(width),
      (id)kIOSurfaceHeight : @(height),
      (id)kIOSurfaceBytesPerElement : @4,
      (id)kIOSurfacePixelFormat : @(0x42475241) /* 'BGRA' */,
      (id)kIOSurfaceIsGlobal : @YES,
    };
#pragma clang diagnostic pop
    IOSurfaceRef surf = IOSurfaceCreate((CFDictionaryRef)props);
    if (!surf) {
      fprintf(stderr, "[serverd] IOSurfaceCreate %ux%u failed (slot %d)\n",
              width, height, i);
      atomic_store_explicit(&status->error, d9mt_status_err_iosurface,
                            memory_order_release);
      return 1;
    }
    g_surfaces[i] = surf;

    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:width
                                    height:height
                                 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [g_device newTextureWithDescriptor:td
                                                  iosurface:surf
                                                      plane:0];
    if (!tex) {
      fprintf(stderr, "[serverd] newTexture(iosurface) failed (slot %d)\n", i);
      atomic_store_explicit(&status->error, d9mt_status_err_metal,
                            memory_order_release);
      return 1;
    }
    g_textures[i] = tex;

    /* GPU liveness: clear the whole surface to the slot color. */
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(
        colors[i][0], colors[i][1], colors[i][2], colors[i][3]);
    id<MTLRenderCommandEncoder> enc =
        [cb renderCommandEncoderWithDescriptor:rp];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    /* verify the clear actually landed (BGRA byte order) */
    IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *px = IOSurfaceGetBaseAddress(surf);
    uint8_t expect[4] = {(uint8_t)(colors[i][2] * 255.0),
                         (uint8_t)(colors[i][1] * 255.0),
                         (uint8_t)(colors[i][0] * 255.0),
                         (uint8_t)(colors[i][3] * 255.0)};
    int ok = px[0] == expect[0] && px[1] == expect[1] && px[2] == expect[2] &&
             px[3] == expect[3];
    IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
    if (!ok) {
      fprintf(stderr, "[serverd] clear verify FAILED (slot %d)\n", i);
      atomic_store_explicit(&status->error, d9mt_status_err_clear_verify,
                            memory_order_release);
      return 1;
    }

    atomic_store_explicit(&status->iosurface_serial[i], 1,
                          memory_order_relaxed);
    atomic_store_explicit(&status->iosurface_id[i], IOSurfaceGetID(surf),
                          memory_order_release);
    fprintf(stderr, "[serverd] surface %d: id=%u %ux%u cleared+verified\n", i,
            IOSurfaceGetID(surf), width, height);
  }
  return 0;
}
