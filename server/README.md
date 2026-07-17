# d9mt-serverd — native ARM64 render server (Phase S1)

The Rosetta escape (docs/RENDER_SERVER_SPEC.md): a standalone arm64 helper
process that consumes the d9mt wire ring (driver/mtl/mtl_wire.h) from shared
memory. In S1 it validates + counts records (echo semantics, content
discarded), proves GPU liveness by clearing 3 cross-process-visible
IOSurfaces, and publishes progress/heartbeat/IOSurfaceIDs through the
`status` block (server/d9mt_status.h). The client still executes everything
locally — S1 proves spawn/shm/ring/status across the process + ISA boundary.

## Build + test gate

```sh
scripts/build-server.sh            # builds arm64 server + x86_64 test client,
                                   # then RUNS the test gate (exits nonzero on fail)
scripts/build-server.sh --no-test  # build only
```

Outputs: `build/server/d9mt-serverd` (arm64), `build/server/test_feed`
(x86_64 — deliberately, so every test run crosses the same Rosetta<->native
boundary as the real wine client).

build-driver.sh tail-calls build-server.sh at its end (non-fatal while
experimental), so a normal driver build also builds + gates the server.

## Running standalone against a synthetic ring

`test_feed` IS the synthetic client — it does the whole dance:

```sh
build/server/test_feed build/server/d9mt-serverd
```

1. `mkdtemp` a shm dir; creates `ring` (192 B d9mt_wire_ring header + 1 MiB
   data) and `status` (136 B d9mt_status), both `mmap(MAP_SHARED)`.
2. Produces 1000 valid wire records (5 opcode kinds, incl. trailing-payload
   records: draw args, push data), release-publishing `head` per record.
3. `posix_spawn`s the server with `argv = {<shm-dir>, "640", "480"}`.
4. Asserts: `epoch==1`, `progress` reaches produced bytes, `records==1000`,
   `decode_errors==0`, 3 distinct nonzero IOSurfaceIDs (serial 1), heartbeat
   advances over 200 ms.
5. Emits the Detach record; asserts the server exits 0 with
   `consumer_exited==1` and `records==1001`.

Prints `PASS` / exits 0 on success.

To poke at the server manually: create the two files with the layouts above
(ring header: `magic=0x30315257544D3944`, power-of-two `capacity`,
`data_offset=192`), then:

```sh
build/server/d9mt-serverd <shm-dir> [width height]   # default 2992x1934
```

It maps `<dir>/ring` + `<dir>/status`, publishes IOSurfaces, stores
`epoch=1`, then drains. Exit paths: Detach opcode, `ring->shutdown` flag, or
unlinking `<dir>/ring` (client-death fallback, checked ~10 Hz).

## `status` block (server -> client, 136 bytes; authoritative:
## server/d9mt_status.h)

| offset | type   | field            | meaning                                   |
|-------:|--------|------------------|-------------------------------------------|
|      0 | u64    | epoch            | 0 booting, 1 ready (stored last)           |
|      8 | u64    | heartbeat        | ~100 Hz liveness counter                   |
|     16 | u64    | progress         | consumed ring bytes (mirror of ring->tail) |
|     24 | u64    | completed_serial | S1: always 0                               |
|     32 | u32    | error            | enum d9mt_status_error                     |
|     36 | u32    | _pad0            |                                            |
|     40 | u32[8] | iosurface_id     | per swapchain slot, 0 = empty              |
|     72 | u64[8] | iosurface_serial | generation/frame serial per slot           |

## S1 caveats (deliberate)

- Empty-ring park is a 1 ms poll: no cross-process kick channel yet
  (TODO(S2): pipe/kqueue EVFILT_USER or futex).
- iosurface_pool.m uses Metal 3 API (plain MTLCommandQueue) for the liveness
  clear only — TODO(METAL4): the S2 backend port must be MTL4-only per
  docs/SOURCE_OF_TRUTH.md §2.
- IOSurface sharing rides deprecated `kIOSurfaceIsGlobal` (Chromium
  precedent, PoC-verified); fallback is IOSurfaceCreateMachPort + bootstrap
  port.
