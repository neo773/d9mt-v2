#!/usr/bin/env python3
"""
compare_frames.py — S3 cutover autonomous correctness comparator.

Loads two raw frame dumps produced by the driver's present-path verifier
(d3d9_swapchain.cpp DumpTextureToFile): a little-endian header of
{u32 width, u32 height} followed by width*height*4 bytes of BGRA8 pixels.

The two dumps are the SAME logical frame rendered two ways — the client's
local Metal backbuffer and the render server's Metal IOSurface — captured for
the same present serial. They should be near-identical modulo the documented
S3 differences (gamma/sRGB handling, MSAA resolve mode, and the srvErr=4
dropped draws). This tool computes per-pixel diff statistics over the RGB
channels (alpha ignored — X8 formats leave it undefined) and prints PASS/FAIL.

Usage:
  compare_frames.py LOCAL.bin SERVER.bin [--tol N] [--min-within PCT] [--json]

Exit code 0 = PASS, 1 = FAIL, 2 = usage/load error.

How the coordinator reads the result:
  - PASS: mean delta small (a few LSBs) and the within-tolerance fraction is
    at/above --min-within (default 99.0%). The cutover is presenting the
    server's pixels and they match the client's render. Ship the flip.
  - FAIL with a UNIFORM small bias (mean ~ a constant, max small): almost
    certainly gamma/sRGB or resolve-mode — a KNOWN S3 diff source, not a
    cutover bug. Inspect max/mean; if max is also small, treat as expected and
    re-run with a looser --tol.
  - FAIL with LOCALIZED large deltas (low within-tolerance %, high max, modest
    mean): real content divergence — the srvErr=4 dropped draws or a missing
    server-side resource. This is the signal that the server render is wrong,
    NOT the present plumbing.
  - "dimension mismatch": the server IOSurface and local backbuffer disagree on
    size — a pool/resize generation bug; investigate before trusting any pixel
    stats.
"""

import struct
import sys
import argparse


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 8:
        raise ValueError(f"{path}: too short ({len(data)} bytes)")
    w, h = struct.unpack_from("<II", data, 0)
    need = 8 + w * h * 4
    if w == 0 or h == 0:
        raise ValueError(f"{path}: zero dimensions {w}x{h}")
    if len(data) < need:
        raise ValueError(f"{path}: truncated ({len(data)} < {need} for {w}x{h})")
    return w, h, memoryview(data)[8:8 + w * h * 4]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("local")
    ap.add_argument("server")
    ap.add_argument("--tol", type=int, default=8,
                    help="per-channel abs delta counted as within tolerance (default 8)")
    ap.add_argument("--min-within", type=float, default=99.0,
                    help="min %% of pixels within tolerance to PASS (default 99.0)")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    try:
        lw, lh, lpix = load(args.local)
        sw, sh, spix = load(args.server)
    except (OSError, ValueError) as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return 2

    if (lw, lh) != (sw, sh):
        print(f"FAIL: dimension mismatch local={lw}x{lh} server={sw}x{sh}",
              file=sys.stderr)
        return 1

    npix = lw * lh
    max_delta = 0
    sum_delta = 0
    within = 0
    tol = args.tol
    # BGRA8: compare B,G,R (bytes 0,1,2), skip A (byte 3).
    for i in range(0, npix * 4, 4):
        pd = 0
        for c in range(3):
            d = abs(lpix[i + c] - spix[i + c])
            sum_delta += d
            if d > pd:
                pd = d
        if pd > max_delta:
            max_delta = pd
        if pd <= tol:
            within += 1

    mean_delta = sum_delta / (npix * 3)
    within_pct = 100.0 * within / npix
    ok = within_pct >= args.min_within

    if args.json:
        import json
        print(json.dumps({
            "result": "PASS" if ok else "FAIL",
            "width": lw, "height": lh, "pixels": npix,
            "max_delta": max_delta, "mean_delta": round(mean_delta, 4),
            "within_pct": round(within_pct, 4), "tol": tol,
            "min_within": args.min_within,
        }))
    else:
        print(f"{'PASS' if ok else 'FAIL'}: {lw}x{lh} "
              f"max_delta={max_delta} mean_delta={mean_delta:.4f} "
              f"within_tol({tol})={within_pct:.4f}% (need >= {args.min_within}%)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
