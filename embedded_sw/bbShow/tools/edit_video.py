"""Cut a screen recording into a chaptered demo reel.

Reference videos for this kind of contest are usually silent: a white title card
naming the section, then the clip, repeat - with music over the top.  This does
exactly that, so the only input needed is the raw recording and a list of
timestamps.

Usage
-----
    python edit_video.py sections.json -o demo.mp4 [--music bgm.mp3]

sections.json:

    {
      "source": "D:/FPGA/raw_capture.mp4",
      "size":   "1920x1080",        // optional, defaults to the source
      "card":   1.8,                // seconds per title card
      "gap":    0.15,               // black gap after each clip
      "music":  "D:/FPGA/bgm.mp3",  // optional
      "sections": [
        { "from": "00:00:12.4", "to": "00:00:24.0",
          "title": "SPLIT SCREEN", "sub": "CPU rasteriser vs hardware blit" }
      ]
    }

Timestamps accept SS, MM:SS, HH:MM:SS and any of those with .mmm.

Everything is re-encoded, so cuts land on the requested frame rather than the
nearest keyframe, and all pieces are produced with identical parameters before
being concatenated - that is what keeps the concat demuxer from drifting.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow is required: pip install pillow")

try:
    import imageio_ffmpeg
    FFMPEG = imageio_ffmpeg.get_ffmpeg_exe()
except Exception:
    FFMPEG = "ffmpeg"

FONT_BOLD = r"C:\Windows\Fonts\msyhbd.ttc"
FONT_MONO = r"C:\Windows\Fonts\consola.ttf"

WHITE = (255, 255, 255)
INK   = (18, 18, 20)
ACCENT = (0, 122, 204)
GREY  = (110, 112, 118)


def run(cmd, quiet=True):
    """Run ffmpeg and raise with its stderr if it fails."""
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode("utf-8", "replace")[-4000:])
        raise SystemExit("ffmpeg failed (%d): %s" % (proc.returncode, " ".join(cmd[:6])))
    return proc.stderr.decode("utf-8", "replace")


def parse_time(text):
    """'00:01:23.5' / '1:23' / '83.5' -> seconds."""
    if isinstance(text, (int, float)):
        return float(text)
    parts = str(text).strip().split(":")
    if len(parts) > 3:
        raise SystemExit("bad timestamp: %s" % text)
    total = 0.0
    for p in parts:
        total = total * 60.0 + float(p)
    return total


def probe(path):
    """Return (width, height, fps) using ffmpeg -i, since only ffmpeg ships here.

    `ffmpeg -i file` with no output argument always exits 1, so this must not go
    through run() - the stream details are on stderr either way.
    """
    proc = subprocess.run([FFMPEG, "-hide_banner", "-i", path],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    err = proc.stderr.decode("utf-8", "replace")
    size = fps = None

    m = re.search(r"Video:.*?(\d{2,5})x(\d{2,5})", err)
    if m:
        size = (int(m.group(1)), int(m.group(2)))

    m = re.search(r"(\d+(?:\.\d+)?)\s+fps", err)
    if m:
        fps = float(m.group(1))

    if size is None:
        raise SystemExit("could not read the resolution of %s" % path)
    return size[0], size[1], (fps or 30.0)


def make_card(path, w, h, title, sub, index, total):
    """White title card: big black title, optional grey subtitle, small index."""
    img = Image.new("RGB", (w, h), WHITE)
    d = ImageDraw.Draw(img)

    # thin accent bar so the card is not a blank white field
    d.rectangle([0, 0, w, max(6, h // 120)], fill=ACCENT)

    title_px = int(h * 0.085)
    sub_px = int(h * 0.036)
    idx_px = int(h * 0.026)

    try:
        f_title = ImageFont.truetype(FONT_BOLD, title_px)
        f_sub = ImageFont.truetype(FONT_MONO, sub_px)
        f_idx = ImageFont.truetype(FONT_MONO, idx_px)
    except Exception:
        f_title = f_sub = f_idx = ImageFont.load_default()

    def measure(text, font):
        box = d.textbbox((0, 0), text, font=font)
        return box[2] - box[0], box[3] - box[1]

    tw, th = measure(title, f_title)
    ty = h // 2 - th // 2 - (sub_px if sub else 0)
    d.text(((w - tw) // 2, ty), title, font=f_title, fill=INK)

    if sub:
        sw, sh = measure(sub, f_sub)
        d.text(((w - sw) // 2, ty + th + int(h * 0.045)), sub, font=f_sub, fill=GREY)

    if index:
        d.text((int(w * 0.04), int(h * 0.06)), index, font=f_idx, fill=GREY)

    img.save(path)


def build_piece(src, dst, start, dur, w, h, fps, vbitrate):
    """Re-encode one range with the common parameter set used by every piece."""
    cmd = [
        FFMPEG, "-hide_banner", "-y",
        "-ss", "%.3f" % start, "-t", "%.3f" % dur, "-i", src,
        "-vf", "scale=%d:%d:flags=lanczos,fps=%s,setsar=1,format=yuv420p"
               % (w, h, fps),
        "-an",
        "-c:v", "libx264", "-preset", "medium", "-crf", "18",
        "-maxrate", vbitrate, "-bufsize", vbitrate,
        "-g", str(max(1, int(float(fps) * 2))),
        dst,
    ]
    run(cmd)


def build_card_clip(png, dst, dur, w, h, fps, vbitrate, fade):
    fd = min(fade, dur / 3.0)
    vf = ("scale=%d:%d,setsar=1,format=yuv420p,"
          "fade=t=in:st=0:d=%.2f,fade=t=out:st=%.2f:d=%.2f"
          % (w, h, fd, dur - fd, fd))
    cmd = [
        FFMPEG, "-hide_banner", "-y",
        "-loop", "1", "-t", "%.3f" % dur, "-i", png,
        "-vf", vf + ",fps=%s" % fps,
        "-an",
        "-c:v", "libx264", "-preset", "medium", "-crf", "18",
        "-maxrate", vbitrate, "-bufsize", vbitrate,
        "-g", str(max(1, int(float(fps) * 2))),
        dst,
    ]
    run(cmd)


def build_gap(dst, dur, w, h, fps, vbitrate):
    cmd = [
        FFMPEG, "-hide_banner", "-y",
        "-f", "lavfi", "-i", "color=c=white:s=%dx%d:r=%s:d=%.3f" % (w, h, fps, dur),
        "-vf", "format=yuv420p",
        "-an",
        "-c:v", "libx264", "-preset", "medium", "-crf", "18",
        "-maxrate", vbitrate, "-bufsize", vbitrate,
        "-g", str(max(1, int(float(fps) * 2))),
        dst,
    ]
    run(cmd)


def concat(pieces, dst):
    listfile = dst + ".txt"
    with open(listfile, "w", encoding="utf-8") as fh:
        for p in pieces:
            fh.write("file '%s'\n" % p.replace("\\", "/"))
    run([FFMPEG, "-hide_banner", "-y", "-f", "concat", "-safe", "0",
         "-i", listfile, "-c", "copy", dst])
    os.remove(listfile)


def add_music(video, music, dst, total, fade_out=2.0):
    cmd = [
        FFMPEG, "-hide_banner", "-y",
        "-i", video, "-stream_loop", "-1", "-i", music,
        "-filter_complex",
        "[1:a]afade=t=in:st=0:d=1.2,afade=t=out:st=%.2f:d=%.2f,"
        "atrim=0:%.2f,volume=0.75[a]" % (max(0.0, total - fade_out), fade_out, total),
        "-map", "0:v", "-map", "[a]",
        "-c:v", "copy", "-c:a", "aac", "-b:a", "192k", "-shortest",
        dst,
    ]
    run(cmd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("config")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--size", help="override output size, WxH")
    ap.add_argument("--fps", type=float, default=0, help="override output fps")
    ap.add_argument("--music", help="background music file")
    ap.add_argument("--card", type=float, default=0, help="override card seconds")
    ap.add_argument("--no-cards", action="store_true")
    ap.add_argument("--vbitrate", default="24M")
    args = ap.parse_args()

    with open(args.config, encoding="utf-8") as fh:
        cfg = json.load(fh)

    src = cfg["source"]
    if not os.path.isfile(src):
        raise SystemExit("source not found: %s" % src)

    sw, sh, sfps = probe(src)
    if args.size:
        w, h = [int(v) for v in args.size.lower().split("x")]
    elif cfg.get("size"):
        w, h = [int(v) for v in cfg["size"].lower().split("x")]
    else:
        w, h = sw, sh
    fps = args.fps or cfg.get("fps") or sfps

    card = args.card or cfg.get("card", 1.8)
    gap = cfg.get("gap", 0.15)
    fade = cfg.get("fade", 0.25)
    music = args.music or cfg.get("music")

    secs = cfg["sections"]
    if not secs:
        raise SystemExit("no sections")

    print("source %dx%d @ %.3f fps  ->  output %dx%d @ %.3f fps"
          % (sw, sh, sfps, w, h, fps))
    print("%d sections, card %.2fs, gap %.2fs" % (len(secs), 0 if args.no_cards else card, gap))

    tmp = tempfile.mkdtemp(prefix="reel_")
    pieces = []
    total = 0.0

    for i, s in enumerate(secs, 1):
        start = parse_time(s["from"])
        end = parse_time(s["to"])
        dur = end - start
        if dur <= 0.05:
            raise SystemExit("section %d: 'to' must be after 'from'" % i)

        if not args.no_cards:
            png = os.path.join(tmp, "card%02d.png" % i)
            make_card(png, w, h, s.get("title", "SECTION %d" % i), s.get("sub", ""),
                      "%02d / %02d" % (i, len(secs)), len(secs))
            clip = os.path.join(tmp, "card%02d.mp4" % i)
            build_card_clip(png, clip, card, w, h, fps, args.vbitrate, fade)
            pieces.append(clip)
            total += card

        clip = os.path.join(tmp, "sec%02d.mp4" % i)
        build_piece(src, clip, start, dur, w, h, fps, args.vbitrate)
        pieces.append(clip)
        total += dur
        print("  %02d %-28s %7.2f -> %7.2f  (%.2fs)" % (i, s.get("title", ""), start, end, dur))

        if gap > 0:
            gp = os.path.join(tmp, "gap%02d.mp4" % i)
            build_gap(gp, gap, w, h, fps, args.vbitrate)
            pieces.append(gp)
            total += gap

    silent = os.path.join(tmp, "silent.mp4")
    concat(pieces, silent)
    print("silent cut %.2f s -> %s" % (total, silent))

    if music:
        if not os.path.isfile(music):
            raise SystemExit("music not found: %s" % music)
        add_music(silent, music, args.out, total)
    else:
        run([FFMPEG, "-hide_banner", "-y", "-i", silent, "-c", "copy", args.out])

    print("done -> %s" % os.path.abspath(args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
