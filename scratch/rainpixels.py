#!/usr/bin/env python3
"""Count rain-coloured pixels in a cube_forge BMP dump.

ONLY VALID ON STORM FRAMES (CF_WEATHER high). Rain is tinted (0.72, 0.80, 0.92)
and drawn unlit, which is distinguishable against the grey of an overcast sky
but NOT against a clear blue one -- a clear-weather frame scores ~479,000 here,
which is the sky, not rain. Use it for the skylight gate, where the comparison
is storm-above-ground (~39,000) against storm-underground (0).
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
