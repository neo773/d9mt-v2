#!/usr/bin/env python3
# verify-pixels.py — autonomous pixel check on a captured window PNG.
#
#   verify-pixels.py <png> [expect]
#
# expect (optional): "nonblack" (default) | "r,g,b" center target within tol.
# Prints a JSON-ish summary + exits 0 PASS / 1 FAIL, so run-test.sh can gate.
import sys, struct, zlib

def load_png(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', "not a PNG"
    pos = 8; width = height = bitd = colort = 0; idat = b''
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos+4])[0]
        typ = data[pos+4:pos+8]
        chunk = data[pos+8:pos+8+ln]
        if typ == b'IHDR':
            width, height, bitd, colort = struct.unpack('>IIBB', chunk[:10])
        elif typ == b'IDAT':
            idat += chunk
        elif typ == b'IEND':
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    ch = {0:1,2:3,3:1,4:2,6:4}[colort]
    stride = width*ch
    out = bytearray(); prev = bytearray(stride)
    p = 0
    for y in range(height):
        ft = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        for x in range(stride):
            a = line[x-ch] if x >= ch else 0
            b = prev[x]
            c = prev[x-ch] if x >= ch else 0
            if ft == 1: line[x] = (line[x]+a) & 255
            elif ft == 2: line[x] = (line[x]+b) & 255
            elif ft == 3: line[x] = (line[x]+((a+b)>>1)) & 255
            elif ft == 4:
                pp=a+b-c; pa=abs(pp-a); pb=abs(pp-b); pc=abs(pp-c)
                pr=a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
                line[x]=(line[x]+pr)&255
        out += line; prev = line
    return width, height, ch, bytes(out)

def px(buf, w, ch, x, y):
    o = (y*w + x)*ch
    r,g,b = buf[o], buf[o+1], buf[o+2]
    return (r,g,b)

def main():
    path = sys.argv[1]
    expect = sys.argv[2] if len(sys.argv) > 2 else "nonblack"
    w,h,ch,buf = load_png(path)
    cx,cy = w//2, h//2
    # sample a small cross around center, take max luma to dodge thin geometry
    pts = [(cx,cy),(cx-10,cy),(cx+10,cy),(cx,cy-10),(cx,cy+10),
           (cx-30,cy+30),(cx+30,cy-30)]
    samples = [px(buf,w,ch,x,y) for (x,y) in pts if 0<=x<w and 0<=y<h]
    best = max(samples, key=lambda c: c[0]+c[1]+c[2])
    print(f"size={w}x{h} center={px(buf,w,ch,cx,cy)} brightest={best} "
          f"samples={samples}")
    if expect == "nonblack":
        ok = (best[0]+best[1]+best[2]) > 60
    else:
        tr,tg,tb = map(int, expect.split(','))
        ok = any(abs(s[0]-tr)<40 and abs(s[1]-tg)<40 and abs(s[2]-tb)<40
                 for s in samples)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()
