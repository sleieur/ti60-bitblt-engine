# -*- coding: utf-8 -*-
"""
Decode assets.bin exactly the way the board reads it and write PNGs out.

This is the sanity check for the picture pipeline: it parses the header the
same way scenes.c does (magic 'SFA1', count, then IMG_PIC_STRIDE apart), does
the same RGB565 -> RGB expansion, and dumps what the screen will show.  If a
picture looks right here it will look right on the HDMI output.

    python verify_assets.py                 # -> out/*.png
    python verify_assets.py --only 1 5      # just those (1-based)
"""
import argparse
import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "assets.bin")
OUT = os.path.join(HERE, "verify_out")

W, H = 640, 720
PIC_STRIDE = 0xE1000
HDR = 0x1000
MAGIC = 0x53464131


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", default=SRC)
    ap.add_argument("--out", dest="out", default=OUT)
    ap.add_argument("--only", nargs="*", type=int, default=None)
    a = ap.parse_args()

    if not os.path.isfile(a.src):
        print("missing", a.src)
        return 1

    blob = open(a.src, "rb").read()
    magic, count, w, h = struct.unpack("<IIII", blob[:16])

    print("file      : %s  (%.2f MiB)" % (os.path.basename(a.src), len(blob) / 1048576.0))
    print("magic     : 0x%08X %s" % (magic, "(OK)" if magic == MAGIC else "(WRONG)"))
    print("pictures  : %d" % count)
    print("geometry  : %d x %d" % (w, h))

    if magic != MAGIC:
        print("header is not valid - the program will say NONE LOADED")
        return 1
    if w != W or h != H:
        print("geometry mismatch, expected %dx%d" % (W, H))
        return 1

    need = HDR + count * PIC_STRIDE
    if len(blob) < need:
        print("file is %d bytes but %d pictures need %d" % (len(blob), count, need))
        return 1
    print("footprint : 0x06000000 .. 0x%08X" % (0x06000000 + need - 1))
    print()

    os.makedirs(a.out, exist_ok=True)
    want = set(a.only) if a.only else None

    for idx in range(count):
        if want and (idx + 1) not in want:
            continue
        off = HDR + idx * PIC_STRIDE
        img = Image.new("RGB", (W, H))
        px = img.load()
        for y in range(H):
            base = off + y * W * 2
            row = blob[base:base + W * 2]
            for x in range(W):
                v = row[x * 2] | (row[x * 2 + 1] << 8)
                px[x, y] = (((v >> 11) & 31) * 255 // 31,
                            ((v >> 5) & 63) * 255 // 63,
                            (v & 31) * 255 // 31)
        f = os.path.join(a.out, "pic%02d.png" % (idx + 1))
        img.save(f)
        print("  %-2d  0x%08X  -> %s" % (idx + 1, 0x06000000 + off, os.path.basename(f)))

    print()
    print("done - open %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
