"""Launcher icons and launcher art for the generated app, in pure Python.

The XBE's title image (decoded by xbe_info, usually 128x128) becomes the
launcher icon at every density and the launcher's artwork; a title without
one gets a plain generated icon. Resampling is bilinear; corners are rounded
with an anti-aliased mask.
"""

import math

from tools.android import xbe_info

DENSITIES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}


def resize(w, h, rgba, nw, nh):
    """Bilinear resample of an RGBA byte buffer (premultiplied for the blend)."""
    src = rgba
    out = bytearray(nw * nh * 4)
    sx = w / nw
    sy = h / nh
    for y in range(nh):
        fy = (y + 0.5) * sy - 0.5
        y0 = max(0, min(h - 1, int(math.floor(fy))))
        y1 = min(h - 1, y0 + 1)
        ty = min(1.0, max(0.0, fy - y0))
        for x in range(nw):
            fx = (x + 0.5) * sx - 0.5
            x0 = max(0, min(w - 1, int(math.floor(fx))))
            x1 = min(w - 1, x0 + 1)
            tx = min(1.0, max(0.0, fx - x0))
            acc = [0.0, 0.0, 0.0, 0.0]
            for (xx, yy, wt) in ((x0, y0, (1 - tx) * (1 - ty)), (x1, y0, tx * (1 - ty)),
                                 (x0, y1, (1 - tx) * ty), (x1, y1, tx * ty)):
                o = (yy * w + xx) * 4
                a = src[o + 3] / 255.0
                acc[0] += src[o] * a * wt
                acc[1] += src[o + 1] * a * wt
                acc[2] += src[o + 2] * a * wt
                acc[3] += a * wt
            o = (y * nw + x) * 4
            if acc[3] > 0:
                out[o] = int(min(255, acc[0] / acc[3] + 0.5))
                out[o + 1] = int(min(255, acc[1] / acc[3] + 0.5))
                out[o + 2] = int(min(255, acc[2] / acc[3] + 0.5))
            out[o + 3] = int(min(255, acc[3] * 255 + 0.5))
    return bytes(out)


def round_corners(n, rgba, radius_frac=0.18, background=(16, 20, 24)):
    """Composite onto a dark square with rounded, anti-aliased corners."""
    r = n * radius_frac
    out = bytearray(rgba)
    for y in range(n):
        for x in range(n):
            o = (y * n + x) * 4
            a = out[o + 3] / 255.0
            for c in range(3):
                out[o + c] = int(out[o + c] * a + background[c] * (1 - a) + 0.5)
            # distance outside the rounded rectangle
            cx = min(max(x + 0.5, r), n - r)
            cy = min(max(y + 0.5, r), n - r)
            d = math.hypot(x + 0.5 - cx, y + 0.5 - cy) - r
            cover = min(1.0, max(0.0, 0.5 - d))
            out[o + 3] = int(255 * cover + 0.5)
    return bytes(out)


def default_icon(n):
    """A neutral icon: dark tile, green ring, white X."""
    out = bytearray(n * n * 4)
    c = n / 2.0
    ring_r, ring_w = n * 0.36, n * 0.07
    arm = n * 0.055
    for y in range(n):
        for x in range(n):
            px, py = x + 0.5 - c, y + 0.5 - c
            o = (y * n + x) * 4
            col = [16, 20, 24]
            d_ring = abs(math.hypot(px, py) - ring_r) - ring_w / 2
            if d_ring < 0.5:
                t = min(1.0, 0.5 - d_ring)
                col = [int(col[i] * (1 - t) + v * t) for i, v in enumerate((108, 194, 58))]
            if math.hypot(px, py) < ring_r - ring_w:
                d_x = min(abs(px - py), abs(px + py)) / math.sqrt(2) - arm
                if d_x < 0.5:
                    t = min(1.0, 0.5 - d_x)
                    col = [int(col[i] * (1 - t) + 236 * t) for i in range(3)]
            out[o:o + 4] = bytes((col[0], col[1], col[2], 255))
    return bytes(out)


def write_icons(res_dir, image=None, log=print):
    """mipmap-<density>/ic_launcher.png from the title image (w, h, rgba) or the default."""
    import os
    for dens, n in DENSITIES.items():
        d = os.path.join(res_dir, "mipmap-" + dens)
        os.makedirs(d, exist_ok=True)
        if image:
            w, h, rgba = image
            px = round_corners(n, resize(w, h, rgba, n, n))
        else:
            px = round_corners(n, default_icon(n))
        xbe_info.write_png(os.path.join(d, "ic_launcher.png"), n, n, px)
    log("icons: %s" % ("from the XBE title image" if image else "generated default"))


def write_art(path, image, size=512):
    w, h, rgba = image
    xbe_info.write_png(path, size, size, resize(w, h, rgba, size, size))
