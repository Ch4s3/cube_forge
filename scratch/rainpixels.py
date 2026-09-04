#!/usr/bin/env python3
"""Count rain-coloured pixels in a cube_forge BMP dump.

Rain is tinted (0.72, 0.80, 0.92) and drawn unlit, so it lands in a pale-blue
band no terrain, water or HUD element occupies. Used for the skylight gate:
a dump taken underground must report 0.
"""
import sys
W, H = 800, 600
d = open(sys.argv[1], 'rb').read()
hdr = len(d) - W * H * 3
n = 0
for r in range(H):
    for c in range(W):
        i = hdr + (r * W + c) * 3
        b, g, rr = d[i], d[i+1], d[i+2]
        if b > 170 and g > 150 and rr > 130 and b > rr + 12:
            n += 1
print(n)
