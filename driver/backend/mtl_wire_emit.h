#pragma once
/*
 * mtl_wire_emit.h — cross-file surface of the S2 wire producer
 * (implemented in mtl_wire_context.cpp). Three seams outside the wire
 * context need to talk to the transport:
 *
 *  - resource destructors (mtl_device_impl.{h,cpp}) queue destroy_resource
 *    records from ARBITRARY threads; the CS-thread producer drains the
 *    queue into the ring before its next record (so a destroy can never
 *    overtake the ring position of the resource's last use).
 *  - the swapchain (d3d9_swapchain.cpp) names its current backbuffer image
 *    (app thread) so the CS producer can flag backbuffer render targets and
 *    stamp presentFrame records.
 *  - the swapchain's present path reports each local present so the PROF
 *    line can compare the server's produced-frame serial against the local
 *    one (S2 gate: the server produces frames AT RATE).
 *
 * Header is PE-safe (mingw x86_64 C++17): no unix/ObjC types.
 */

#include <atomic>
#include <chrono>
#include <cstdint>

#include "../util/rc/util_rc_ptr.h"

namespace dxvk { class MtlTexture; class MtlTextureView; class MtlDevice; }

namespace dxvk::backend::wire {

  /* App thread (PresentImage): the local present serial this frame will use
   * (matches the counter the server echoes as status.frame_serial). Captured
   * into the present closure so the CS-thread server-frame gate compares like
   * for like. */
  uint64_t nextPresentSerial();

  /* CS thread (inside the present closure): THE present source, server-only
   * (ALL-IN cutover). When the wire is live and the server has EVER published
   * a completed frame, wraps the freshest published IOSurface (unix-cached per
   * {id,generation}; no staleness/pace gate — a repeat of the last frame is
   * the correct behavior when the server is behind) as a borrowed MtlTexture +
   * full-image view and returns true — the caller blits THOSE into the
   * drawable. Returns false ONLY when nothing was ever published (boot), the
   * wire is dead, or the lookup/wrap failed; the caller then SKIPS the
   * present (screen keeps the last frame / stays black). There is no local
   * fallback image — the client does not render. */
  bool acquireServerFrame(const Rc<MtlDevice>& device, uint64_t localSerial,
                          Rc<MtlTexture>& outImage, Rc<MtlTextureView>& outView);

  /* Any thread. No-op when the resource never got a wire identity. */
  void queueDestroy(uint64_t wireId, uint64_t clientHandle);

  /* App thread (PresentImage): the image the present blit reads — D3D9
   * back buffer 0. Held as an Rc; the CS producer compares pointers and
   * assigns/reads the wire id on its own thread. */
  void noteBackbuffer(const Rc<MtlTexture>& image);

  /* App thread (CreateBackBuffers, init + Reset): register EVERY swapchain
   * backbuffer image. S3 v11: with BackBufferCount > 1 the rotation means
   * frame N draws into a different image than the one noteBackbuffer saw at
   * present N-1 — a single-pointer compare never matches, no RT bind ever
   * carries the BACKBUFFER flag, and the server renders into plain textures
   * while its published IOSurface stays black. Membership in THIS set is
   * what flags render targets. Replaces the previous set (Reset). */
  void noteSwapchainImages(const Rc<MtlTexture>* images, uint32_t count);

  /* App thread (PresentImage, after the present was queued): bumps the
   * local present counter and samples the server's produced-frame serial;
   * feeds the PROF `srvFrames`/`srvPace` fields. Cheap (one unixcall). */
  void presentObserved();

  /* --- present-pipeline timeline (instrumentation ONLY — never gate
   * behavior on it) ---------------------------------------------------------
   * Per-frame hop timestamps across client -> server -> client, decoded into
   * ONE line every 256 completed presents (D9MT_PROFILE gate, same cadence
   * as the PROF line). Client marks are QPC, converted into the machine-
   * global mach-ns domain via the WMTWireWaitTimeTap calibration; the server
   * hops arrive through WMTServerFrame (status v4 tl ring). All four marks
   * run on the CS thread inside the present closure (d3d9_swapchain.cpp);
   * the t1/t5 hops are recorded internally by the wire producer. When
   * D9MT_PROFILE is unset each call is a single branch. Reading the line:
   * HANDOFF-TIMELINE.md. */
  void timelinePresentStart(uint64_t localSerial); /* t0: present closure entry */
  void timelineDrawableAcquired();                 /* t6: acquireNextImage returned */
  void timelineCommitDone();                       /* t7: synchronizeWsi returned (blit encoded + committed + presented) */
  void timelinePresentEnd();                       /* t8: presenter->present returned; may flush the window */

  /* --- P1 blob substrate (VENUS_CONTRACT_DIFF.md P1; mtl_blob_pool.h) -----
   * CPU-visible buffer backings are carved from the shared heap's blob
   * region; the slice's pages are the buffer on both sides, so content for
   * these backings never crosses the wire again. */

  /* App or CS thread (createBuffer / renameBuffer). On success fills the
   * slice's CPU pointer (into the shared mapping), its data-area-relative
   * heap offset (the create record's heap_offset) and the pool reservation
   * (pass back to blobRetire). false = pool unavailable/exhausted — caller
   * falls back to a private host backing + the legacy upload path. */
  bool blobAlloc(uint64_t size, void** mapPtr, uint64_t* heapOffset,
                 uint64_t* reserved);

  /* Any thread (~MtlAllocationImpl). Returns the slice to the freelist,
   * stamped with the current in-flight frame marker — the credit law gates
   * its reuse on the server's completed frame serial. */
  void blobRetire(uint64_t heapOffset, uint64_t reserved);

  /* Recycle-pool gate (mtl_device_impl.h): blobStamp() = the conservative
   * last-referencing-frame stamp for a backing entering the recycle pool;
   * blobReusable(stamp) = the server's completed serial covers it (or the
   * wire is down and no server read can ever happen). */
  uint64_t blobStamp();
  bool blobReusable(uint64_t stamp);

  /* --- Round 3: APP-THREAD PER-CALL COST instrumentation ------------------
   * Live GTA showed the calm-frame wall on the APP thread (timeline notice
   * = the whole inter-present gap while client CS totalled 0.7 ms): the
   * game makes thousands of D3D9 calls/frame, so anything per-call is a
   * frame-rate term. These buckets time the hot D3D9 entry seams
   * (d3d9_device.cpp) + the blob allocator, reported per PROF window as
   * appUs= with a breakdown — this layer can never hide again. Enabled only
   * under D9MT_PROFILE (g_appProfOn; ~2 atomics + 2 clock reads per call
   * when on, one relaxed load when off). */
  enum AppProfBucket : uint32_t {
    AppProfLock = 0,   /* Lock/LockBuffer/LockImage */
    AppProfUnlock,     /* Unlock twins */
    AppProfDraw,       /* Draw{,Indexed}Primitive{,UP} */
    AppProfSetConst,   /* SetVertexShaderConstantF */
    AppProfSetTex,     /* SetTexture */
    AppProfAlloc,      /* blobAlloc + blobRetire (inside Lock, decomposes it) */
    AppProfCount
  };
  extern std::atomic<uint64_t> g_appProfNs[AppProfCount];
  extern std::atomic<uint64_t> g_appProfCalls[AppProfCount];
  extern std::atomic<bool> g_appProfOn;

  struct AppProfScope {
    uint32_t bucket;
    bool on;
    std::chrono::steady_clock::time_point t0;
    explicit AppProfScope(uint32_t b)
      : bucket(b), on(g_appProfOn.load(std::memory_order_relaxed)) {
      if (on)
        t0 = std::chrono::steady_clock::now();
    }
    ~AppProfScope() {
      if (on) {
        const auto d = std::chrono::steady_clock::now() - t0;
        g_appProfNs[bucket].fetch_add(
          (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(d).count(),
          std::memory_order_relaxed);
        g_appProfCalls[bucket].fetch_add(1u, std::memory_order_relaxed);
      }
    }
  };

  /* Any thread (D3D9Query::GetData path — MtlBackendQuery::getData in
   * mtl_context_transfer.cpp). Polls the shared occlusion result ring
   * (mtl_wire.h d9mt_wire_query_ring, HANDOFF-QUERIES.md) for the query
   * emitted under {queryId, session}. NEVER blocks (D3D9 queries are
   * async-poll BY DESIGN — the game expects S_FALSE while pending):
   *   false -> pending (result not GPU-complete server-side yet)
   *   true  -> *samplesOut is the answer. Session bounces, wire death,
   *            ring eviction and pre-query servers all resolve here as
   *            all-passed with their own PROF counters (qSess=/qEvict=) —
   *            a game is NEVER wedged waiting on a result that cannot
   *            arrive. */
  bool queryPoll(uint64_t queryId, uint32_t session, uint64_t* samplesOut);

}
