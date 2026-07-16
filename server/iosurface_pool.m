/*
 * iosurface_pool.m — d9mt-serverd swapchain IOSurface pool (Phase S2).
 *
 * Creates D9MT_SERVER_SURFACES (3) BGRA8 IOSurfaces at the requested size and
 * wraps each as an MTLTexture (RenderTarget | ShaderRead). IDs are published
 * into status->iosurface_id[0..2] with iosurface_serial = the pool
 * generation (1 at boot, bumped on every resize).
 *
 * S2 refactor: the S1 GPU liveness clear (which used the Metal 3
 * MTLCommandQueue API) moved into executor.mm and is METAL 4 ONLY per
 * docs/SOURCE_OF_TRUTH.md §2 — this file owns NO command queue. It also
 * gained resize (the client's real backbuffer size arrives with the first
 * backbuffer-flagged setRenderTargetsById record, after spawn).
 */

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <stdio.h>

#include "d9mt_server.h"

/* Keep the surfaces alive for the process lifetime: the client resolves
 * them by ID via IOSurfaceLookup, which fails once the last reference in
 * any process is gone. IOSurfaceRefs are CF objects — not ARC-managed.
 * Old-generation surfaces after a resize are deliberately LEAKED until
 * process exit: the client may still hold lookups into the previous
 * generation for a frame or two (bounded: resizes are rare, one generation
 * per display-mode change). */
static IOSurfaceRef g_surfaces[D9MT_SERVER_SURFACES];
static id<MTLTexture> g_textures[D9MT_SERVER_SURFACES];
static id<MTLDevice> g_device;
static uint32_t g_width, g_height;
static uint64_t g_generation;

id<MTLDevice>
d9mt_pool_device(void) {
  return g_device;
}

id<MTLTexture>
d9mt_pool_texture(int slot) {
  return (slot >= 0 && slot < D9MT_SERVER_SURFACES) ? g_textures[slot] : nil;
}

uint32_t
d9mt_pool_width(void) {
  return g_width;
}

uint32_t
d9mt_pool_height(void) {
  return g_height;
}

static int
pool_create(struct d9mt_status *status, uint32_t width, uint32_t height) {
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
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
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

    atomic_store_explicit(&status->iosurface_serial[i], g_generation,
                          memory_order_relaxed);
    atomic_store_explicit(&status->iosurface_id[i], IOSurfaceGetID(surf),
                          memory_order_release);
    fprintf(stderr, "[serverd] surface %d: id=%u %ux%u gen=%llu\n", i,
            IOSurfaceGetID(surf), width, height,
            (unsigned long long)g_generation);
  }
  g_width = width;
  g_height = height;
  return 0;
}

int
d9mt_iosurface_pool_init(struct d9mt_status *status, uint32_t width,
                         uint32_t height) {
  g_device = MTLCreateSystemDefaultDevice();
  if (!g_device) {
    fprintf(stderr, "[serverd] no Metal device\n");
    atomic_store_explicit(&status->error, d9mt_status_err_metal,
                          memory_order_release);
    return 1;
  }
  g_generation = 1u;
  return pool_create(status, width, height);
}

int
d9mt_iosurface_pool_resize(struct d9mt_status *status, uint32_t width,
                           uint32_t height) {
  if (width == g_width && height == g_height)
    return 0;
  /* Previous generation leaks by design (header comment). Textures are
   * released (ARC nil-out); the IOSurfaceRefs stay alive. */
  for (int i = 0; i < D9MT_SERVER_SURFACES; i++)
    g_textures[i] = nil;
  g_generation++;
  fprintf(stderr, "[serverd] pool resize %ux%u -> %ux%u (gen %llu)\n", g_width,
          g_height, width, height, (unsigned long long)g_generation);
  return pool_create(status, width, height);
}
