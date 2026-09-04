#!/usr/bin/env python3
"""Compare two cube_forge BMP dumps, ignoring the FPS counter.

Reads the real dimensions from the BMP header. A dump of an 800x600 window is
1600x1200 on a 2x Retina display, and hard-coding 800x600 silently compares a
mis-sliced sub-region rather than the frame.

Run the program with CF_NOMOUSE=1. Without it the camera yaw depends on where
the window manager placed the window relative to the pointer, so two runs render
different views and any comparison is meaningless.
"""
import struct, sys

def load(p):
    d = open(p, 'rb').read()
    w, h = struct.unpack('<ii', d[18:26])
    off = struct.unpack('<I', d[10:14])[0]
    return d, w, h, off, (w * 3 + 3) // 4 * 4

a, w, h, off, stride = load(sys.argv[1])
b, w2, h2, _, _ = load(sys.argv[2])
if (w, h) != (w2, h2):
    print(f"size mismatch: {w}x{h} vs {w2}x{h2}")
    sys.exit(2)

# FPS counter sits in the top-right; scale the mask with the framebuffer.
mx0, my1 = int(w * 0.90), int(h * 0.10)

bad, first = 0, None
for r in range(h):
    top = h - 1 - r                      # BMP rows are bottom-up
    base = off + r * stride
    if a[base:base + w * 3] == b[base:base + w * 3]:
        continue
    for c in range(w):
        if top <= my1 and c >= mx0:
            continue
        i = base + c * 3
        if a[i:i + 3] != b[i:i + 3]:
            bad += 1
            if first is None:
                first = (top, c)
print(f"differing pixels outside the FPS HUD: {bad}  ({w}x{h})" +
      (f"  first at row={first[0]} col={first[1]}" if first else ""))
sys.exit(1 if bad else 0)
