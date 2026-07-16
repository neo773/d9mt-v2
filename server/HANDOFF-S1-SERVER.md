# HANDOFF — S1 server (d9mt-serverd), agent A

What exists, what it promises, what successors need. Commits 60f8657, 4e94880, 5e76bf4.

## Files
- `server/d9mt_status.h` — THE status-block ABI (136 B POD, static_asserts both compilers). AUTHORITATIVE.
- `server/main.m` — entry. argv[1] = shm dir containing `ring` + `status` files. Maps both, validates ring header, boots IOSurface pool, sets epoch=1 LAST.
- `server/consumer.m` — ring drain loop (port of the in-process consumer from winemetal_unix.c, wine-free). 1ms poll park (no kick channel in S1), ~100Hz heartbeat, exits on Detach opcode / shutdown flag / ring file unlinked.
- `server/iosurface_pool.m` — 3 BGRA8 IOSurfaces (default 2992x1934, overridable argv), kIOSurfaceIsGlobal=YES (deprecated; mach-port fallback documented in comment), each wrapped as MTLTexture and render-pass cleared red/green/blue + CPU-verified. TODO(METAL4): S2 backend must be MTL4-only.
- `server/test_feed.c` — x86_64 synthetic client (deliberately crosses the Rosetta↔native boundary): creates shm files, writes 1000 records + Detach, spawns server, asserts progress/records/heartbeat/iosurface ids. THE test gate; run by build-server.sh.
- `scripts/build-server.sh` — clang -arch arm64 -O2; verifies output is arm64; builds + RUNS test_feed.

## Status block (d9mt_status, 136 bytes) — offsets for clients
| off | type | field | notes |
|--:|--|--|--|
| 0 | u64 | epoch | 0 booting, 1 ready (stored last, acquire on read) |
| 8 | u64 | heartbeat | ~100Hz while alive |
| 16 | u64 | progress | consumed ring bytes (mirror of ring->tail) |
| 24 | u64 | completed_serial | S1: always 0 |
| 32 | u32 | error | 0 ok, 1 ring_map, 2 metal, 3 iosurface, 4 decode, 5 clear_verify |
| 36 | u32 | _pad0 | |
| 40 | u32[8] | iosurface_id | 0 = empty slot; DO NOT consume in S1 |
| 72 | u64[8] | iosurface_serial | 1 at boot |

## Verified (test_feed output)
1000 records / 33600 bytes consumed, decode_errors=0, epoch=1, 3 distinct
IOSurface IDs, heartbeat ~100Hz, Detach → exit 0. Binary is arm64 Mach-O.

## Gotchas for successors
- Detach COUNTS into ring->records (matches in-process consumer): expect N+1.
- Ring-unlink and shutdown-flag exit paths are code-reviewed but NOT exercised
  by test_feed (only the Detach path is).
- Poll park is S1-only; kick channel is an S2 item.
- Server never touches wine anything; spawn it with argv[1]=shm dir.
