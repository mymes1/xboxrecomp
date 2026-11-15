"""Is the test title's picture on the device's screen? [android-port]

The self-test title (tools/android/testtitle/xbrtest.c) animates its
framebuffer once its checks are done: the top two thirds are eight vertical
colour bars, 80 pixels each, whose order turns by one every 30 frames; the
bottom third is a dark green band (dark red when a check failed) with a
moving white box and a frame counter. That is enough to recognise the
picture on a screenshot at any moment of the animation:

  - the band's bounding box gives the picture's position and size (the band
    is the full width of the picture and its bottom third);
  - the eight bar centres along a row near the top must be the eight bar
    colours in the title's cyclic order -- white, yellow, cyan, green,
    magenta, red, blue, black -- starting anywhere. A red/blue swap, a
    mirrored or rotated picture, a stale or black screen all fail;
  - outside the picture, the presenter's letterbox is black.

Input: what `adb exec-out screencap` writes without -p: width, height,
pixel format (1 RGBA_8888, 2 RGBX_8888, 5 BGRA_8888) as little-endian
uint32s, on Android 10 and later a fourth (the dataspace), then the pixels.

`check(raw, avoid)` returns a dict: ok, reason, and what was measured.
`avoid`: circles (x, y, r) drawn over the screen -- the on-screen controller,
from its logged layout -- where the letterbox is not expected to be black.
"""

import struct

BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
        (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
BAND_OK = (0, 48, 0)        # 0xFF003000
BAND_FAIL = (64, 0, 0)      # 0xFF400000
TOL = 40


def parse_raw(raw):
    """(width, height, rows of (r, g, b) accessor) from screencap's raw output."""
    if len(raw) < 12:
        raise ValueError("screencap output too short (%d bytes)" % len(raw))
    w, h, fmt = struct.unpack_from("<III", raw, 0)
    if not (0 < w <= 16384 and 0 < h <= 16384):
        raise ValueError("implausible screencap size %dx%d" % (w, h))
    header = len(raw) - w * h * 4
    if header not in (12, 16):
        raise ValueError("screencap output is %d bytes; %dx%d RGBA needs %d plus a 12- or "
                         "16-byte header" % (len(raw), w, h, w * h * 4))
    if fmt in (1, 2, 3):
        order = (0, 1, 2)
    elif fmt == 5:
        order = (2, 1, 0)
    else:
        raise ValueError("screencap pixel format %d is not RGBA/RGBX/BGRA 8888" % fmt)
    return w, h, Screen(raw, header, w, h, order)


class Screen:
    def __init__(self, raw, off, w, h, order):
        self.raw, self.off, self.w, self.h, self.order = raw, off, w, h, order

    def px(self, x, y):
        i = self.off + (y * self.w + x) * 4
        r = self.raw
        a, b, c = self.order
        return (r[i + a], r[i + b], r[i + c])


def _near(c, want, tol=TOL):
    return all(abs(int(c[k]) - want[k]) <= tol for k in range(3))


def _band_box(scr, want):
    """Bounding box of pixels near `want`, on a coarse grid then refined."""
    step = max(1, min(scr.w, scr.h) // 270)
    xs, ys = [], []
    for y in range(0, scr.h, step):
        for x in range(0, scr.w, step):
            if _near(scr.px(x, y), want, 16):
                xs.append(x)
                ys.append(y)
    if len(xs) < 50:
        return None
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)

    def refine(lo, hi, fixed, horizontal):
        # Walk outwards from the coarse edge to the last matching pixel.
        def hit(v):
            return _near(scr.px(v, fixed) if horizontal else scr.px(fixed, v), want, 16)
        while lo > 0 and hit(lo - 1):
            lo -= 1
        lim = (scr.w if horizontal else scr.h) - 1
        while hi < lim and hit(hi + 1):
            hi += 1
        return lo, hi

    # Rows/columns through the band that avoid the box and the digits.
    ymid = y1 - max(1, (y1 - y0) // 16)
    x0, x1 = refine(x0, x1, ymid, True)
    xq = x0 + max(1, (x1 - x0) // 40)
    y0, y1 = refine(y0, y1, xq, False)
    return x0, y0, x1, y1


def _covered(x, y, avoid):
    return any((x - cx) ** 2 + (y - cy) ** 2 <= (r * 1.3 + 4) ** 2 for (cx, cy, r) in avoid)


def check(raw, avoid=()):
    try:
        w, h, scr = parse_raw(raw)
    except ValueError as e:
        return {"ok": False, "reason": str(e)}
    out = {"screen": [w, h]}
    band, failed = _band_box(scr, BAND_OK), False
    if band is None:
        band, failed = _band_box(scr, BAND_FAIL), True
    if band is None:
        out.update(ok=False, reason="the title's bottom band (dark green or dark red) is not on"
                                    " the screen")
        return out
    bx0, by0, bx1, by1 = band
    pw = bx1 - bx0 + 1
    band_h = by1 - by0 + 1
    ph = band_h * 3                     # the band is rows 320..479 of 480
    py0 = by1 + 1 - ph
    out["picture"] = [bx0, py0, pw, ph]
    out["band"] = "fail" if failed else "pass"
    if py0 < -2 or pw < 64 or ph < 48:
        out.update(ok=False, reason="the band does not sit under a picture (%dx%d at %d,%d)"
                                    % (pw, ph, bx0, py0))
        return out
    aspect = pw / float(ph)
    out["aspect"] = round(aspect, 3)
    # Eight bar centres along a row a quarter of the way down the bars.
    row = max(0, py0 + ph * 80 // 480)
    seen = []
    for k in range(8):
        x = bx0 + (2 * k + 1) * pw // 16
        c = scr.px(min(max(x, 0), w - 1), min(row, h - 1))
        idx = [i for i, b in enumerate(BARS) if _near(c, b)]
        seen.append(idx[0] if idx else None)
    out["bars"] = seen
    if None in seen:
        out.update(ok=False, reason="bar %d is not one of the title's colours (row %d)"
                                    % (seen.index(None), row))
        return out
    start = seen[0]
    if seen != [(start + k) % 8 for k in range(8)]:
        out.update(ok=False, reason="the bars are not in the title's order (%s): mirrored,"
                                    " rotated or colour-swapped" % seen)
        return out
    # Letterbox: black beyond the picture, where there is room for it --
    # down the middle of each bar, except under the on-screen controls.
    edges = []
    xs = []
    if bx0 >= 8:
        xs.append(bx0 // 2)
    if w - 1 - bx1 >= 8:
        xs.append((bx1 + w) // 2)
    for x in xs:
        for k in range(1, 8):
            y = min(h - 1, max(0, py0 + ph * k // 8))
            if not _covered(x, y, avoid):
                edges.append((x, y))
    out["letterbox_skipped"] = len(xs) * 7 - len(edges)
    for (x, y) in edges:
        if not _near(scr.px(x, y), (0, 0, 0), 24):
            out.update(ok=False, reason="the screen at (%d,%d) beside the picture is %s, not"
                                        " black" % (x, y, scr.px(x, y)))
            return out
    out["letterbox_samples"] = len(edges)
    out.update(ok=True, reason="the title's picture is on the screen: %dx%d at %d,%d"
                               " (aspect %.3f), 8/8 bars in order%s"
                               % (pw, ph, bx0, py0, aspect,
                                  ", black beside it" if edges else ""))
    return out


def synth(w, h, phase=0, fmt=1, header=16, rect=None, swap_rb=False, fail_band=False):
    """A screencap-format screen with the title's picture (for tests)."""
    if rect is None:
        ph = h
        pw = h * 4 // 3
        if pw > w:
            pw, ph = w, w * 3 // 4
        rect = ((w - pw) // 2, (h - ph) // 2, pw, ph)
    x0, y0, pw, ph = rect
    buf = bytearray(header + w * h * 4)
    struct.pack_into("<III", buf, 0, w, h, fmt)
    band = BAND_FAIL if fail_band else BAND_OK
    for y in range(h):
        for x in range(w):
            c = (0, 0, 0)
            if x0 <= x < x0 + pw and y0 <= y < y0 + ph:
                fx = (x - x0) * 640 // pw
                fy = (y - y0) * 480 // ph
                c = BARS[(fx // 80 + phase) & 7] if fy < 320 else band
                if 340 <= fy < 404 and 100 <= fx < 164:
                    c = (255, 255, 255)
            if swap_rb:
                c = (c[2], c[1], c[0])
            i = header + (y * w + x) * 4
            if fmt == 5:
                buf[i:i + 4] = bytes((c[2], c[1], c[0], 255))
            else:
                buf[i:i + 4] = bytes((c[0], c[1], c[2], 255))
    return bytes(buf)
