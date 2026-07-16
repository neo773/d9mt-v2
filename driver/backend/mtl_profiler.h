#pragma once
/*
 * mtl_profiler.h — per-frame CPU profiler for the METAL 4 backend hot path.
 *
 * Enabled ONLY when D9MT_PROFILE is set in the environment (checked once at
 * load into an inline const bool). When disabled every entry point is an
 * `if (!g_profEnabled) return;` fast path — no clock reads, no stores — so
 * the facility can stay compiled-in permanently (dormant).
 *
 * BUCKET CONTRACT
 *  - A bucket is a named per-frame nanosecond accumulator. prof::Scope is an
 *    RAII timer: steady_clock at ctor, elapsed added to the bucket at dtor.
 *  - Buckets NEST and are NOT mutually exclusive:
 *      PrepareDraw contains ResolvePso / FlushVertexBuffers / FlushResources /
 *      FlushDynamicState and (at the arena cap) FlushCmdChunk;
 *      SynchronizeWsi contains one Submit (the present commit);
 *      Submit does NOT contain FlushCmdChunk (closePass flushes it first,
 *      but that flush runs inside Submit's scope via closePass -> counted in
 *      both FlushCmdChunk and Submit — read sub-buckets as "of which").
 *    Interpret sub-buckets as a breakdown of their parent, not additive
 *    across the whole frame.
 *  - Granularity is FUNCTION-LEVEL by design: the driver runs under Rosetta
 *    and steady_clock reads are not free — a handful of clock reads per draw
 *    is acceptable (~4-12), per-instruction timing is not. Do not add Scopes
 *    inside per-slot loops.
 *  - Counters are plain per-window event counts (draws, chunk flushes, PSO
 *    cache misses = synchronous compiles on the hot path, arg-table
 *    rotations).
 *  - Everything is accumulated on the CS thread (the only thread that runs
 *    MtlContextImpl); no atomics needed. mtl_pso_cache.cpp bumps PsoMiss on
 *    the same thread (resolvePso is CS-side).
 *  - Every 256 frames (mirrors the DIAG cadence in endFrame) one Logger line
 *    reports each bucket's window total in ms + per-frame average, plus the
 *    counters, then the window resets.
 */

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk::backend::prof {

  enum Bucket : uint32_t {
    BeginRecording = 0, // allocator rotation + waitFor slot serial (stall here = GPU-bound / pacing)
    PrepareDraw,        // whole per-draw resolve (parent of the next four)
    ResolvePso,         //   PSO key build + cache get (miss = synchronous compile)
    FlushVertexBuffers, //   vertex-stream table writes (diffed)
    FlushResources,     //   CBV/texture/sampler/push table writes + push ring copy
    FlushDynamicState,  //   viewport/scissor/dsso/blend/raster wmtcmd appends
    EncodeDraw,         // draw()/drawIndexed() CmdChunk append loop (after prepareDraw)
    FlushCmdChunk,      // MTL4RenderCommandEncoder_encodeCommands PE->unix crossing
    Submit,             // endCommandBuffer + residency beforeSubmit + queue commit + retire bookkeeping
    SynchronizeWsi,     // waitForDrawable + present commit + signalDrawable + present
    EndFrame,           // processRetirements + residency serialRetired + ring onComplete
    BucketCount
  };

  enum Counter : uint32_t {
    CntDraws = 0,       // prepareDraw successes (draw calls encoded)
    CntChunkFlush,      // non-empty flushCmdChunk crossings
    CntPsoMiss,         // RenderPsoCache misses (hot-path pipeline compiles)
    CntArgRotate,       // selectArgumentTablesForDraw calls (binding-dirty draws)
    CntAbRebuild,       // AB word-slice rebuilds (ring alloc + full word rewrite)
    CntPsoKeyBuild,     // resolvePso invocations that rebuilt+hashed the key (DirtyPso)
    /* Why did an ACTIVE render pass close? (TBDR: every close = full tile
     * store, every reopen = full tile load — the pass-count levers.) */
    CntPassCloseSubmit, //   frame end (submit / present)
    CntPassCloseRt,     //   bindRenderTargets switch
    CntPassCloseClear,  //   standalone clear pass (folded clears, ColorFill, DS rect)
    CntPassCloseXfer,   //   transfer compute encoder (uploads/copies mid-frame)
    CntPassCloseBarrier,//   emitGraphicsBarrier (feedback-loop hazard)
    CntPassCloseBlit,   //   meta-blit / mipgen
    /* Frame-deferred pass list (decision 10): */
    CntFramePasses,     //   render encoders actually created per flush (after merge/cull)
    CntStoreDontCare,   //   attachments whose store was optimized to DontCare (Rules B/C)
    CounterCount
  };

  /* Read the env exactly once at image load; branch on a plain bool after. */
  inline const bool g_profEnabled = std::getenv("D9MT_PROFILE") != nullptr;

  inline uint64_t g_ns[BucketCount]     = {};
  inline uint64_t g_counts[CounterCount] = {};
  inline uint64_t g_frames = 0u;

  struct Scope {
    std::chrono::steady_clock::time_point t0;
    uint32_t bucket;
    explicit Scope(Bucket b) : bucket(b) {
      if (!g_profEnabled) return;
      t0 = std::chrono::steady_clock::now();
    }
    ~Scope() {
      if (!g_profEnabled) return;
      g_ns[bucket] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - t0).count());
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  };

  inline void bump(Counter c, uint64_t n = 1u) {
    if (!g_profEnabled) return;
    g_counts[c] += n;
  }

  /* Call once per presented frame (endFrame). Logs + resets every 256. */
  inline void onFrameEnd() {
    if (!g_profEnabled) return;
    if ((++g_frames & 0xFFu) != 0u)
      return;

    static constexpr const char* kNames[BucketCount] = {
      "beginRecording", "prepareDraw", "resolvePso", "flushVB",
      "flushRes", "flushDyn", "encodeDraw", "flushChunk",
      "submit", "syncWsi", "endFrame"
    };
    constexpr double kFrames = 256.0;

    std::string line = str::format("PROF frame=", g_frames, " win=256 ms(avg/f):");
    for (uint32_t i = 0u; i < BucketCount; i++) {
      double totalMs = double(g_ns[i]) * 1e-6;
      line += str::format(" ", kNames[i], "=",
        totalMs, "(", totalMs / kFrames, ")");
      g_ns[i] = 0u;
    }
    line += str::format(" | draws=", g_counts[CntDraws],
      "(", double(g_counts[CntDraws]) / kFrames, "/f)",
      " chunkFlush=", g_counts[CntChunkFlush],
      "(", double(g_counts[CntChunkFlush]) / kFrames, "/f)",
      " psoMiss=", g_counts[CntPsoMiss],
      " argRotate=", g_counts[CntArgRotate],
      "(", double(g_counts[CntArgRotate]) / kFrames, "/f)",
      " abRebuild=", g_counts[CntAbRebuild],
      "(", double(g_counts[CntAbRebuild]) / kFrames, "/f)",
      " psoKeyBuild=", g_counts[CntPsoKeyBuild],
      "(", double(g_counts[CntPsoKeyBuild]) / kFrames, "/f)",
      " passClose/f: submit=", double(g_counts[CntPassCloseSubmit]) / kFrames,
      " rt=", double(g_counts[CntPassCloseRt]) / kFrames,
      " clear=", double(g_counts[CntPassCloseClear]) / kFrames,
      " xfer=", double(g_counts[CntPassCloseXfer]) / kFrames,
      " barrier=", double(g_counts[CntPassCloseBarrier]) / kFrames,
      " blit=", double(g_counts[CntPassCloseBlit]) / kFrames,
      " | framePasses=", double(g_counts[CntFramePasses]) / kFrames, "/f",
      " storeDontCare=", double(g_counts[CntStoreDontCare]) / kFrames, "/f");
    for (uint32_t i = 0u; i < CounterCount; i++)
      g_counts[i] = 0u;

    Logger::info(line);
  }

}
