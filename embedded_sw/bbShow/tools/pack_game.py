# -*- coding: utf-8 -*-
"""Pack the arcade sprite set into game_assets.bin.

Source art (all CC0):
  RUOK Space Construction Set  - enemies, boss, bullets, planet
  (player ship and effects stay procedural, see art.c)

The engine only understands RGB565 sprites with a 1-bit colour key, so every
PNG is flattened onto the key colour here.  Frames inside a sheet are found by
scanning for fully transparent columns instead of assuming an even split: the
RUOK boss sheet is 227 px wide for 4 frames, which does not divide.

Output layout (little endian), loaded at GAME_ASSET_ADDR:

    +0x000  'GPAK'          magic
    +0x004  u32 version     = 1
    +0x008  u32 count
    +0x00C  u32 data_off    offset of pixel data from the start of the blob
    +0x010  record[count]   { u32 off; u16 w; u16 h; u16 stride; u16 pad; }

Pixel data is always at +0x1000 so the header never has to be repacked.
"""
import os, struct, sys
from PIL import Image

RUOK = r"D:\FPGA\art\ruok"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "game_assets.bin")
MAGIC = 0x4B415047           # 'GPAK'
BASE = 0x07400000            # DDR, above the picture strip (ends ~0x06C2F000)
DATA_OFF = 0x1000
KEY = (0xF8, 0x1F)           # 0xF81F: r=31 g=0 b=31

# (name, file, frames in the sheet, which frame to use)
# The frame count comes from the file name, not from guessing: these sheets have
# no transparent gutter between frames, so splitting on empty columns merged
# them.  Even split + per-frame trim of the transparent border is exact.
SPRITES = [
    ("foe_a",    r"Enemies\GreenSpaceShip2Frames.png",       2, 0),
    ("foe_a2",   r"Enemies\GreenSpaceShip2Frames.png",       2, 1),
    ("foe_b",    r"Enemies\RailCannon2Frames.png",           2, 0),
    ("foe_c",    r"Enemies\SpaceTankBodyYellow2Frames.png",  2, 0),
    # the alien head is only 48x63: pack it doubled so it reads as a boss, and
    # keep a mirrored rail cannon to hang on each side as arms
    ("boss_0",   r"Enemies\AlienBoss4Frames.png",            4, 0, 3),
    ("boss_1",   r"Enemies\AlienBoss4Frames.png",            4, 2, 3),
    ("arm_l",    r"Enemies\RailCannon2Frames.png",           2, 0),
    ("arm_r",    r"Enemies\RailCannon2Frames.png",           2, 0, 1, True),
    # One sheet, three lengths: the small bullets are the bright tip of the same
    # bolt.  Without this every bullet on screen was 50 px tall, which buries the
    # playfield under stripes instead of building a pattern.
    ("bul_s_r",  r"Projectiles\BulletRed3Frames.png",        3, 0, 1, False, 18),
    ("bul_s_b",  r"Projectiles\BulletBlue3Frames.png",       3, 0, 1, False, 18),
    ("bul_s_p",  r"Projectiles\BulletPurple3Frames.png",     3, 0, 1, False, 18),
    ("bul_m_r",  r"Projectiles\BulletRed3Frames.png",        3, 1, 1, False, 30),
    ("bul_m_b",  r"Projectiles\BulletBlue3Frames.png",       3, 1, 1, False, 30),
    ("bul_m_p",  r"Projectiles\BulletPurple3Frames.png",     3, 1, 1, False, 30),
    ("bul_l_r",  r"Projectiles\BulletRed3Frames.png",        3, 2),
    ("bul_l_b",  r"Projectiles\BulletBlue3Frames.png",       3, 2),
    ("bul_l_p",  r"Projectiles\BulletPurple3Frames.png",     3, 2),
    ("neon_0",   r"Projectiles\NeonShot5Frames.png",         5, 0),
    ("neon_1",   r"Projectiles\NeonShot5Frames.png",         5, 1),
    ("neon_2",   r"Projectiles\NeonShot5Frames.png",         5, 2),
    ("planet",   r"Planets\Planet2Stars.png",                1, 0, 3),
]


def frames(img, n):
    """Even split into n cells, then trim each cell to its ink."""
    w, h = img.size
    a = img.split()[-1]
    boxes = []
    for i in range(n):
        x0 = round(i * w / n)
        x1 = round((i + 1) * w / n)
        sub = a.crop((x0, 0, x1, h))
        bb = sub.getbbox()
        if not bb:
            continue
        boxes.append((x0 + bb[0], x0 + bb[2]))
    return boxes


KEY565 = (31 << 11) | (0 << 5) | 31        # 0xF81F


def to565(img, box, scale=1, flip=False, crop_h=0):
    x0, x1 = box
    full = img.crop((x0, 0, x1, img.height)).convert("RGBA")
    bb = full.split()[-1].getbbox() or (0, 0, full.width, full.height)
    im = full.crop(bb)
    if crop_h:
        im = im.crop((0, 0, im.width, min(crop_h, im.height)))
    if flip:
        im = im.transpose(Image.FLIP_LEFT_RIGHT)
    if scale > 1:
        im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
    w, h = im.size
    px = im.load()
    out = bytearray(w * h * 2)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 96:
                v = KEY565
            else:
                # composite the stroke over black: compositing over the key colour
                # would leave every anti-aliased edge one code away from magenta,
                # which the key test then misses and draws as a pink fringe
                k = a / 255.0
                v = ((int(r * k) >> 3) << 11) | ((int(g * k) >> 2) << 5) | (int(b * k) >> 3)
            out[i] = v & 0xFF
            out[i + 1] = v >> 8
            i += 2
    return bytes(out), w, h


def main():
    blobs, recs, off = [], [], DATA_OFF
    for spec in SPRITES:
        name, rel, n, idx = spec[:4]
        scale = spec[4] if len(spec) > 4 else 1
        flip = spec[5] if len(spec) > 5 else False
        crop_h = spec[6] if len(spec) > 6 else 0
        path = os.path.join(RUOK, rel)
        if not os.path.exists(path):
            sys.exit("missing " + path)
        img = Image.open(path).convert("RGBA")
        runs = frames(img, n)
        if not runs:
            sys.exit("no ink in " + rel)
        fi = min(idx, len(runs) - 1)
        data, w, h = to565(img, runs[fi], scale, flip, crop_h)
        stride = w * 2
        blobs.append((off, data))
        recs.append((off, w, h, stride))
        off += len(data)
        off = (off + 3) & ~3
        print(f"{name:10s} {os.path.basename(rel)[:30]:30s} frame {fi}/{len(runs)}  "
              f"{w}x{h}  {len(data) / 1024:.1f} KiB")

    size = off
    blob = bytearray(size)
    head = struct.pack("<4sIIII", b"GPAK", 1, len(recs), DATA_OFF, 0)
    blob[0:len(head)] = head
    for i, (o, w, h, st) in enumerate(recs):
        struct.pack_into("<IHHHH", blob, 0x10 + i * 12, o, w, h, st, 0)
    for o, data in blobs:
        blob[o:o + len(data)] = data

    open(OUT, "wb").write(blob)
    print(f"\n{OUT}  {size / 1024:.1f} KiB  base 0x{BASE:08X}  "
          f"end 0x{BASE + size:08X}")


main()
