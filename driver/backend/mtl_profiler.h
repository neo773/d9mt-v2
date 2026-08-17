#pragma once

// Buckets nest and are not mutually exclusive — interpret a sub-bucket as a
// breakdown of its parent, not as additive across the whole frame.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk::backend::prof {

  enum Bucket : uint32_t {
    BeginRecording = 0,
    PrepareDraw,
    ResolvePso,
    FlushVertexBuffers,
    FlushResources,
    FlushDynamicState,
    EncodeDraw,
    FlushCmdChunk,
    Submit,
    SynchronizeWsi,
    EndFrame,
    BucketCount
  };

  enum Counter : uint32_t {
    CntDraws = 0,
    CntChunkFlush,
    CntPsoMiss,
    CntArgRotate,
    CntAbRebuild,
    CntPsoKeyBuild,
    CntPassCloseSubmit,
    CntPassCloseRt,
    CntPassCloseClear,
    CntPassCloseXfer,
    CntPassCloseBarrier,
    CntPassCloseBlit,
    CntFramePasses,
    CntStoreDontCare,
    CounterCount
  };

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
