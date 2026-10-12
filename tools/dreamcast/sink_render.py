#!/usr/bin/env python3
"""Draw a Real3D sink dump (M3_SINK_DUMP) the way an exact rasteriser would:
flat colour, depth by w (larger is nearer), opaque first, then translucent
blended. Lists polygons whose screen coordinates run far off the screen.

    sink_render.py dump.txt out.png [--big N] [--only i,j,...] [--clip]
"""
import sys
import numpy as np
from PIL import Image

W, H = 496, 384
SHARED = '--shared-depth' in sys.argv
# --pvr: one depth buffer, each viewport's polygons cut to its rectangle
# and their w put in its priority's layer as pvr_r3d.c does, and depth as
# Flycast stores it: log2(1 + 100000 w) / 34 in 24 bits.
PVR = '--pvr' in sys.argv
layers = {}     # viewport -> its priority ("V layer x0 y0 x1 y1")
rects = {}
W_FLOOR, W_TOP = 2.0 ** -16, 2.0 ** 16
LAYER_LO, LAYER_HI = 2.0 ** -11, 2.0 ** 5
if '--stack' in sys.argv:   # log2 of floor, top: another stack to try
    W_FLOOR, W_TOP = (2.0 ** float(x) for x in
                      sys.argv[sys.argv.index('--stack') + 1].split(','))


def stack(polys):
    """pvr_r3d.c's begin_viewport: the first viewport of a priority starts
    a layer above the one before; the others share it."""
    nxt, sc, cur = W_FLOOR, 1.0, None
    scale = {}
    for vp in sorted(set(p[4] for p in polys) | set(layers)):
        if layers.get(vp) != cur:
            cur = layers.get(vp)
            sc = nxt / LAYER_LO
            if LAYER_HI * sc > W_TOP:
                sc = W_TOP / LAYER_HI
            nxt = LAYER_HI * sc
        scale[vp] = (LAYER_LO, LAYER_HI, sc)
    return scale


def load(path):
    polys = []
    vp = 0
    layers.clear()
    for line in open(path):
        f = line.split()
        if f[0] == 'V':
            vp += 1
            layers[vp] = int(f[1])
            if len(f) == 6:
                rects[vp] = tuple(float(x) for x in f[2:6])
            continue
        n, argb, tr, tx, pt = (int(f[0]), int(f[1], 16), int(f[2]),
                               int(f[3]), int(f[4]))
        v = [tuple(float(x) for x in f[5 + 5 * k: 10 + 5 * k]) for k in range(n)]
        polys.append((argb, 2 if tr else pt, tx, v, vp))
    return polys


def cut(v, axis, lim, sgn):
    """pvr_r3d.c's cut(): keep where sgn * (coord - lim) <= 0; w is
    affine on the screen. Only x, y, w: the colour is flat."""
    out = []
    for i in range(len(v)):
        a, b = v[i], v[(i + 1) % len(v)]
        da, db = sgn * (a[axis] - lim), sgn * (b[axis] - lim)
        if da <= 0:
            out.append(a)
        if (da <= 0) != (db <= 0):
            t = da / (da - db)
            out.append(tuple(a[k] + t * (b[k] - a[k]) for k in range(3)))
    return out


def draw(polys, out, keep=None):
    col = np.zeros((H, W, 3))
    dep = np.full((H, W), -np.inf)
    ys, xs = np.mgrid[0:H, 0:W] + 0.5
    # Viewport by viewport, a fresh depth buffer per priority as the
    # software path draws them (--shared-depth: one buffer for all).
    # p[1]: 0 opaque, 1 punch-through, 2 translucent. The PVR takes its
    # three lists in that order over the whole scene.
    order = []
    if PVR:
        for lst in (0, 1, 2):
            order += [i for i, p in enumerate(polys) if p[1] == lst]
    else:
        # Supermodel's order: a priority's opaque polygons, all its
        # viewports, then its translucent ones. A car's shadow is in one
        # viewport and the road it lies on in a later one.
        vps = sorted(set(p[4] for p in polys))
        for layer in sorted(set(layers.get(vp) for vp in vps), key=lambda x: (x is None, x)):
            mine = set(vp for vp in vps if layers.get(vp) == layer)
            for vp in sorted(mine):
                order += [i for i, p in enumerate(polys) if p[4] == vp and p[1] < 2]
            for vp in sorted(mine):
                order += [i for i, p in enumerate(polys) if p[4] == vp and p[1] == 2]
    cur = None
    sc = stack(polys) if PVR else {}
    for i in order:
        if layers.get(polys[i][4]) != cur and not SHARED and not PVR:
            dep[:] = -np.inf
        cur = layers.get(polys[i][4])
        if keep is not None and i not in keep:
            continue
        argb, tr, _, v, vp = polys[i]
        if vp in rects:   # both paths draw only inside the viewport
            rx0, ry0, rx1, ry1 = rects[vp]
            v = [(p[0], p[1], p[2]) for p in v]
            for axis, lim, sgn in ((0, rx0, -1), (0, rx1, 1), (1, ry0, -1), (1, ry1, 1)):
                if len(v) >= 3:
                    v = cut(v, axis, lim, sgn)
            if len(v) < 3:
                continue
        if PVR and vp in sc:
            lo, hi, k = sc[vp]
            v = [(p[0], p[1], min(max(p[2], lo), hi) * k) for p in v]
        rgb = np.array([(argb >> 16) & 255, (argb >> 8) & 255, argb & 255], float)
        a = (argb >> 24) / 255.0
        for k in range(1, len(v) - 1):
            (x0, y0, w0), (x1, y1, w1), (x2, y2, w2) = \
                [(p[0], p[1], p[2]) for p in (v[0], v[k], v[k + 1])]
            d = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
            if d == 0:
                continue
            l1 = ((xs - x0) * (y2 - y0) - (x2 - x0) * (ys - y0)) / d
            l2 = ((x1 - x0) * (ys - y0) - (xs - x0) * (y1 - y0)) / d
            l0 = 1 - l1 - l2
            inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
            z = l0 * w0 + l1 * w1 + l2 * w2
            if PVR:
                z = np.floor(np.minimum(np.log2(1 + 1e5 * np.maximum(z, 0)) / 34, 1) * 2 ** 24)
            # The PVR's compare, as KOS compiles it: strictly greater.
            m = inside & ((z > dep) if PVR else (z >= dep))
            if tr < 2:
                dep[m] = z[m]
                col[m] = rgb
            else:
                col[m] = col[m] * (1 - a) + rgb * a
    if '--near' in sys.argv:
        d = dep[np.isfinite(dep)]
        print(f"nearest drawn pixel: w {d.max():.4g} (z {1 / d.max():.4g}); "
              f"w over 2: {(d > 2).sum()} px, over 0.5: {(d > 0.5).sum()} px")
    Image.fromarray(col.clip(0, 255).astype(np.uint8)).save(out)


def main():
    path, out = sys.argv[1], sys.argv[2]
    big = 4096.0
    keep = None
    a = sys.argv[3:]
    if '--big' in a:
        big = float(a[a.index('--big') + 1])
    if '--only' in a:
        keep = set(int(x) for x in a[a.index('--only') + 1].split(','))
    polys = load(path)
    if '--pick' in a:
        px, py = (float(x) + 0.5 for x in a[a.index('--pick') + 1].split(','))
        for i, (argb, tr, tx, v, vp) in enumerate(polys):
            for k in range(1, len(v) - 1):
                (x0, y0, w0), (x1, y1, w1), (x2, y2, w2) = \
                    [(q[0], q[1], q[2]) for q in (v[0], v[k], v[k + 1])]
                d = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
                if d == 0:
                    continue
                l1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) / d
                l2 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) / d
                l0 = 1 - l1 - l2
                if min(l0, l1, l2) >= 0:
                    w = l0 * w0 + l1 * w1 + l2 * w2
                    zl = l0 / w0 + l1 / w1 + l2 / w2
                    print(f"pick #{i} vp {vp} argb {argb:08X} tr {tr} w {w:.5g} "
                          f"(z {1 / w:.4g}; screen-linear z {zl:.4g}) "
                          f"verts w {[round(q[2], 4) for q in v]}")
        return
    for i, (argb, tr, tx, v, vp) in enumerate(polys):
        m = max(max(abs(p[0]), abs(p[1])) for p in v)
        if m > big:
            print(f"#{i} argb {argb:08X} tr {tr} tex {tx} max|xy| {m:.0f} "
                  f"w {min(p[2] for p in v):.4g}..{max(p[2] for p in v):.4g}")
    draw(polys, out, keep)


main()
