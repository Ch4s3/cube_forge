#!/usr/bin/env python3
"""Compare two cube_forge BMP dumps, ignoring the FPS counter.

The FPS readout in the top-right varies run to run, so a bare `cmp` never
matches even between two runs of the same build. Everything else is
deterministic for a fixed CF_SEED and CF_SUN.
"""
import sys
W, H = 800, 600
def load(p):
    d = open(p, 'rb').read()
    return d, len(d) - W * H * 3
a, hdr = load(sys.argv[1])
b, _ = load(sys.argv[2])
bad, first = 0, None
for r in range(H):
    top = H - 1 - r
    if 40 <= top <= 100:
        for c in range(W):
            if 740 <= c <= 790:
                continue
            i = hdr + (r * W + c) * 3
            if a[i:i+3] != b[i:i+3]:
                bad += 1
                first = first or (top, c)
    else:
        i = hdr + r * W * 3
        if a[i:i+W*3] != b[i:i+W*3]:
            for c in range(W):
                j = hdr + (r * W + c) * 3
                if a[j:j+3] != b[j:j+3]:
                    bad += 1
                    first = first or (top, c)
print(f"differing pixels outside the FPS HUD: {bad}" + (f"  first at row={first[0]} col={first[1]}" if first else ""))
sys.exit(1 if bad else 0)
