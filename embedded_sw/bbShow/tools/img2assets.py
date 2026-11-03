# -*- coding: utf-8 -*-
"""
Pack pictures into the blob the debugger pushes into DDR.

Why a blob instead of a C array: one 640x720 RGB565 frame is 900 KiB and
the program is linked into 124 KiB of RAM, so pictures cannot live in the
program image at all.  The launcher restores this file to 0x06000000 before the
program starts (see tools/load_assets.ps1) and the IMAGES scene just COPYs
screenfuls out of DDR.

Layout produced here (little endian):

    0x0000  u32 magic 'SFA1'
    0x0004  u32 picture count
    0x0008  u32 width  (640)
    0x000C  u32 height (720)
    0x0010  u32 playback rate in pictures/s (0 = not recorded)
    0x1000  picture 0   (640*720*2 bytes)

The rate is optional.  0 (the default here) leaves the scene on its own 60 fps,
which is what the side-by-side demo wants: the strip holds only the sampled
frames, so replaying them at the display rate is what makes the engine half
look flat out.  Use --fps N to record a different rate when the clip should play
at the speed it was cut at (a 14-frame 70 ms GIF is 14.29 fps).
    0x1000 + 0xE1000  picture 1
    ...

Usage
    # pictures from tools/pics  (jpg / png / bmp / webp, any size)
    python img2assets.py

    # straight out of a PDF - vector rendered, much sharper than a screenshot
    python img2assets.py --pdf "comic.pdf" --pages 1-6
    python img2assets.py --pdf "comic.pdf" --pages all --dpi 200

    # explicit files
    python img2assets.py a.jpg b.png

Aspect ratio (--fit)
    auto      default.  close to 16:9 -> crop to fill; far from it -> fit whole
              picture and pad with black, so nothing important is cut off.
    cover     always crop to fill (may cut a lot on a portrait page)
    contain   always fit the whole picture, pad with black bars
    stretch   stretch to fill (distorts)

A portrait comic page in contain mode is 405 px wide on a 640x720 half - use
--pair to put two pages side by side, which fills the screen much better.
"""
import argparse
import io
import os
import struct
import sys

from PIL import Image

W, H = 640, 720                # HALF screen: scene 8 is a CPU vs engine split
PIC_STRIDE = 0xE1000           # exactly one 640x720 frame, matches IMG_PIC_STRIDE
MAX_PICS = 168                 # matches IMG_MAX_PICS
HDR = 0x1000
MAGIC = 0x53464131             # 'SFA1'

HERE = os.path.dirname(os.path.abspath(__file__))
PIC_DIR = os.path.join(HERE, "pics")
OUT = os.path.join(HERE, "assets.bin")
EXTS = (".png", ".jpg", ".jpeg", ".bmp", ".webp", ".tif", ".tiff")


def paper_colour(img):
    """Average of the four borders - a scan's paper tone, so the padding blends."""
    im = img.convert("RGB")
    w, h = im.size
    px = im.load()
    n = 0
    r = g = b = 0
    for x in range(0, w, max(1, w // 40)):
        for y in (0, 1, h - 2, h - 1):
            c = px[x, y]
            r += c[0]; g += c[1]; b += c[2]; n += 1
    for y in range(0, h, max(1, h // 40)):
        for x in (0, 1, w - 2, w - 1):
            c = px[x, y]
            r += c[0]; g += c[1]; b += c[2]; n += 1
    return (r // n, g // n, b // n)


def fit_image(img, mode, pad="auto"):
    """Return a 640x720 RGB image according to `mode`."""
    img = img.convert("RGB")
    w, h = img.size
    want = W / float(H)
    have = w / float(h)

    if mode == "auto":
        mode = "cover" if abs(have - want) < want * 0.18 else "contain"

    if mode == "stretch":
        return img.resize((W, H), Image.LANCZOS)

    if mode == "cover":
        if have > want:                       # too wide -> crop the sides
            nw = int(round(h * want))
            x0 = (w - nw) // 2
            img = img.crop((x0, 0, x0 + nw, h))
        elif have < want:                     # too tall -> crop top and bottom
            nh = int(round(w / want))
            y0 = (h - nh) // 2
            img = img.crop((0, y0, w, y0 + nh))
        return img.resize((W, H), Image.LANCZOS)

    # contain: scale so the whole picture fits, centre it on padding
    if pad == "auto":
        bg = paper_colour(img)
    elif pad == "white":
        bg = (255, 255, 255)
    else:
        bg = (0, 0, 0)
    scale = min(W / float(w), H / float(h))
    nw, nh = max(1, int(round(w * scale))), max(1, int(round(h * scale)))
    canvas = Image.new("RGB", (W, H), bg)
    canvas.paste(img.resize((nw, nh), Image.LANCZOS), ((W - nw) // 2, (H - nh) // 2))
    return canvas


def to_rgb565(img):
    """little endian RGB565 for a 640x720 RGB image"""
    px = img.load()
    out = bytearray(W * H * 2)
    i = 0
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[i] = v & 0xFF
            out[i + 1] = v >> 8
            i += 2
    return bytes(out)


def pair_up(pages):
    """Put two portrait pages side by side, in reading order."""
    out = []
    for i in range(0, len(pages), 2):
        if i + 1 < len(pages):
            a, b = pages[i], pages[i + 1]
            ph = max(a.size[1], b.size[1])
            pw = a.size[0] + b.size[0]
            canvas = Image.new("RGB", (pw, ph), (0, 0, 0))
            canvas.paste(a, (0, 0))
            canvas.paste(b, (a.size[0], 0))
            out.append(canvas)
        else:
            out.append(pages[i])
    return out


def parse_pages(spec, count):
    if spec is None:
        return list(range(count))
    if spec.strip().lower() == "all":
        return list(range(count))
    out = []
    for part in spec.split(","):
        part = part.strip()
        if "-" in part:
            a, b = part.split("-", 1)
            out.extend(range(int(a) - 1, int(b)))
        elif part:
            out.append(int(part) - 1)
    return [i for i in out if 0 <= i < count]


def from_pdf(path, spec, dpi, pair):
    try:
        import pymupdf
    except ImportError:
        try:
            import fitz as pymupdf
        except ImportError:
            print("pymupdf is required for --pdf:  pip install pymupdf")
            return []

    doc = pymupdf.open(path)
    idx = parse_pages(spec, doc.page_count)
    print("pdf: %s  %d pages, taking %d" % (os.path.basename(path), doc.page_count, len(idx)))

    pages = []
    for n in idx:
        page = doc[n]

        # A scanned page holds one big JPEG: take it as is.  Re-rendering the
        # page would only resample that same bitmap (a 3160x2300 scan at 200 dpi
        # becomes 8778 px of pure interpolation, slower and no sharper).
        best = None
        for im in page.get_images(full=True):
            xref = im[0]
            try:
                info = doc.extract_image(xref)
            except Exception:
                continue
            if not info or not info.get("image"):
                continue
            if best is None or info["width"] * info["height"] > best[0]:
                best = (info["width"] * info["height"], info)

        if best is not None:
            img = Image.open(io.BytesIO(best[1]["image"])).convert("RGB")
            print("  page %-3d %dx%d  (embedded scan)" % (n + 1, img.size[0], img.size[1]))
        else:
            pix = page.get_pixmap(dpi=dpi)
            img = Image.frombytes("RGB", (pix.width, pix.height), pix.samples)
            print("  page %-3d %dx%d  (rendered at %d dpi)" % (n + 1, img.size[0], img.size[1], dpi))
        pages.append(img)

    return pair_up(pages) if pair else pages


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("files", nargs="*", help="picture files (default: tools/pics/*)")
    ap.add_argument("--pdf", help="extract pages from a PDF instead of using files")
    ap.add_argument("--pages", default=None, help="PDF pages, e.g. 1-6 or all (default 1-6)")
    ap.add_argument("--dpi", type=int, default=200, help="PDF render resolution (default 200)")
    ap.add_argument("--pair", action="store_true",
                    help="put two pages side by side (portrait comics fill the screen better)")
    ap.add_argument("--fit", default="auto",
                    choices=("auto", "cover", "contain", "stretch"),
                    help="how to fill 640x720 (default auto)")
    ap.add_argument("--pad", default="auto", choices=("auto", "black", "white"),
                    help="colour of the bars in contain mode (default: the scan's paper tone)")
    ap.add_argument("--fps", type=int, default=0,
                    help="playback rate to record in the header, pictures/s "
                         "(use the --fps the clip was cut at; 0 = let the scene default)")
    ap.add_argument("--out", default=OUT, help="output blob (default tools/assets.bin)")
    ap.add_argument("--list", action="store_true", help="only show what would be packed")
    ap.add_argument("--dir", dest="src_dir",
                    help="pack every picture in this folder instead of tools/pics")
    a = ap.parse_args()

    if a.pdf:
        pages = from_pdf(a.pdf, a.pages or "1-6", a.dpi, a.pair)
        # label with the real page number, not the index inside the selection
        nums = [i + 1 for i in parse_pages(a.pages or "1-6", 10 ** 6)]
        if a.pair:
            items = [("pair %s" % "/".join(str(n + 1) for n in nums[i:i + 2]), img)
                     for i, img in enumerate(pages)]
        else:
            items = [("page %d" % nums[i], img) for i, img in enumerate(pages)]
    else:
        if a.files:
            files = [os.path.abspath(p) for p in a.files]
        else:
            src = a.src_dir or PIC_DIR
            if not os.path.isdir(src):
                os.makedirs(src)
                print("created", src)
                print("put pictures in there and run this again")
                return 0
            files = sorted(os.path.join(src, f) for f in os.listdir(src)
                           if f.lower().endswith(EXTS))
        if not files:
            print("no pictures found")
            return 1
        imgs, names = [], []
        for f in files:
            try:
                imgs.append(Image.open(f).convert("RGB"))
                names.append(os.path.basename(f))
            except Exception as e:
                print("  cannot open %s: %s" % (f, e))
        if a.pair and len(imgs) > 1:
            paired = pair_up(imgs)
            items = [("pair %d + %d" % (2 * i + 1, 2 * i + 2), im) for i, im in enumerate(paired)]
            items[-1] = (items[-1][0], paired[-1])
        else:
            items = list(zip(names, imgs))

    if not items:
        print("nothing to pack")
        return 1

    if len(items) > MAX_PICS:
        print("warning: only the first %d pictures are used (IMG_MAX_PICS)" % MAX_PICS)
        items = items[:MAX_PICS]

    if a.list:
        for i, (name, img) in enumerate(items):
            print("  [%d] %-40s %dx%d" % (i, name[:40], img.size[0], img.size[1]))
        return 0

    total = HDR + len(items) * PIC_STRIDE
    blob = bytearray(total)
    blob[0:20] = struct.pack("<IIIII", MAGIC, len(items), W, H, max(0, min(60, a.fps)))

    print()
    for idx, (name, img) in enumerate(items):
        w, h = img.size
        fitted = fit_image(img, a.fit, a.pad)
        data = to_rgb565(fitted)
        off = HDR + idx * PIC_STRIDE
        blob[off:off + len(data)] = data

        # how much of the original survived, so a bad --fit shows up immediately
        keep = "whole picture"
        if a.fit in ("cover",) or (a.fit == "auto" and abs(w / float(h) - W / float(H)) < (W / float(H)) * 0.18):
            keep = "%s" % ("filled, edges cropped")
        print("  [%d] %-28s %dx%d -> %s" % (idx, name[:28], w, h, keep))
        print("       at 0x%08X" % (0x06000000 + off))

    with open(a.out, "wb") as fp:
        fp.write(blob)

    print()
    print("wrote %s  (%d bytes, %d pictures)" % (a.out, len(blob), len(items)))
    print("DDR footprint: 0x06000000 .. 0x%08X" % (0x06000000 + total - 1))
    secs = total / 416000.0          # measured, 16000 kHz
    print("JTAG load time at 16000 kHz: about %.0f s (%.1f min)" % (secs, secs / 60))
    print()
    print("now run:  load_assets.ps1 -SkipRebuild")
    return 0


if __name__ == "__main__":
    sys.exit(main())
