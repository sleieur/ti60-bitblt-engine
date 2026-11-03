# -*- coding: utf-8 -*-
"""
Cut a clip out of a video / GIF and turn it into a frame strip for scene 8.

The IMAGES scene plays whatever assets.bin holds, one COPY per picture.  Feed it
24..170 consecutive frames and the display becomes a short looping video: the
engine streams a full 640x720 half-frame out of DDR in 1.49 ms, so the switching
rate is set by img_fps (keys +/- in the scene), not by the engine.

Frame budget - the real limit is the debugger, not DDR:

    measured at 16000 kHz:  406 KiB/s  ->  2.2 s per 640x720 frame
    IMG_MAX_PICS = 168     ->  about 6 minutes to load a full strip

So pick --fps with the length in mind: 88 frames at 24 fps = 3.7 s, at 12 fps =
7.3 s, at 6 fps = 14 s.  Anything slower than about 10 fps looks like a
slideshow, so 24 fps with fewer frames usually reads better as motion.

Usage
    # 2 seconds starting at 0:07 out of an mp4
    python vid2frames.py clip.mp4 --start 7 --duration 2 --fps 24

    # an animated GIF, whole thing, 12 fps
    python vid2frames.py anim.gif --fps 12

    # then pack + load
    python img2assets.py
    .\\load_assets.ps1 -SkipRebuild

Video decoding needs imageio-ffmpeg (bundles its own ffmpeg, no system install):

    pip install imageio-ffmpeg

GIF decoding needs nothing extra (Pillow handles it).
"""
import argparse
import os
import shutil
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(HERE, "pics")


def place(fr, width, height, mode):
    """Fit a source frame into width x height."""
    w, h = fr.size
    if mode == "stretch":
        return fr.resize((width, height), Image.LANCZOS)
    if mode == "cover":
        want = width / float(height)
        have = w / float(h)
        if have > want:
            nw = int(round(h * want))
            fr = fr.crop(((w - nw) // 2, 0, (w - nw) // 2 + nw, h))
        elif have < want:
            nh = int(round(w / want))
            fr = fr.crop((0, (h - nh) // 2, w, (h - nh) // 2 + nh))
        return fr.resize((width, height), Image.LANCZOS)
    # contain
    scale = min(width / float(w), height / float(h))
    nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
    canvas = Image.new("RGB", (width, height), (0, 0, 0))
    canvas.paste(fr.resize((nw, nh), Image.LANCZOS), ((width - nw) // 2, (height - nh) // 2))
    return canvas


def find_ffmpeg():
    for name in ("ffmpeg", "ffmpeg.exe"):
        p = shutil.which(name)
        if p:
            return p
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        pass
    for c in (r"D:\ffmpeg\bin\ffmpeg.exe", r"C:\ffmpeg\bin\ffmpeg.exe"):
        if os.path.isfile(c):
            return c
    return None


def grab_video(path, start, duration, fps, width, height, out_dir, quality, fit, repeat):
    ff = find_ffmpeg()
    if not ff:
        print("no ffmpeg found.")
        print("  pip install imageio-ffmpeg      (recommended, self contained)")
        print("  or put ffmpeg.exe on PATH")
        return []

    if fit == "cover":
        vf = ("fps=%g,scale=%d:%d:force_original_aspect_ratio=increase,"
              "crop=%d:%d" % (fps, width, height, width, height))
    elif fit == "stretch":
        vf = "fps=%g,scale=%d:%d" % (fps, width, height)
    else:
        vf = ("fps=%g,scale=%d:%d:force_original_aspect_ratio=decrease,"
              "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=black"
              % (fps, width, height, width, height))

    tmp = os.path.join(out_dir, "_frames")
    if os.path.isdir(tmp):
        shutil.rmtree(tmp)
    os.makedirs(tmp)

    cmd = [ff, "-v", "error", "-ss", str(start), "-i", path]
    if duration and duration > 0:
        cmd += ["-t", str(duration)]
    cmd += ["-vf", vf, "-q:v", str(quality), os.path.join(tmp, "f%05d.jpg")]
    print("decoding with", os.path.basename(ff))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("ffmpeg failed:", (r.stderr or "").strip()[:400])
        return []

    files = sorted(os.listdir(tmp))
    print("  %d frames decoded" % len(files))

    out = []
    for i, f in enumerate(files):
        src = os.path.join(tmp, f)
        if repeat > 1:
            # re-save so each copy is an independent file, then repeat the name
            pass
        for _ in range(max(1, repeat)):
            dst = os.path.join(out_dir, "clip_%04d.jpg" % len(out))
            shutil.copyfile(src, dst)
            out.append(dst)
    shutil.rmtree(tmp, ignore_errors=True)
    print("  %d decoded x %d repeats = %d pictures" % (len(files), max(1, repeat), len(out)))
    return out


def grab_gif(path, start, duration, fps, width, height, out_dir, fit, repeat):
    im = Image.open(path)
    total = getattr(im, "n_frames", 1)
    dur = im.info.get("duration", 100) or 100
    gif_fps = 1000.0 / dur
    print("gif: %d frames, source %.1f fps" % (total, gif_fps))

    first = max(0, int(start * gif_fps))
    last = total if not duration else min(total, first + int(duration * gif_fps))
    if last <= first:
        last = total

    # Walk the source frames the source way round, then repeat each one
    # `repeat` times.  Playing the result at --fps reproduces the original
    # timing while using repeat x more pictures, which is what makes the
    # "many big pictures switched quickly" demo look like motion instead of
    # a 0.2 s flicker.
    step = max(1.0, gif_fps / float(fps))
    src = []
    t = float(first)
    while t < last:
        src.append(min(int(t), total - 1))
        t += step
    if not src:
        src = list(range(total))

    out = []
    i = 0
    for idx in src:
        im.seek(idx)
        canvas = place(im.convert("RGB"), width, height, fit)
        for _ in range(max(1, repeat)):
            dst = os.path.join(out_dir, "clip_%04d.jpg" % i)
            canvas.save(dst, quality=92)
            out.append(dst)
            i += 1

    print("  %d distinct source frames x %d repeats = %d pictures"
          % (len(src), max(1, repeat), len(out)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", help="video file or .gif")
    ap.add_argument("--start", type=float, default=0, help="start time in seconds")
    ap.add_argument("--duration", type=float, default=0, help="length in seconds (0 = to the end)")
    ap.add_argument("--fps", type=float, default=12, help="frames per second to keep (default 12)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output folder (default tools/pics)")
    ap.add_argument("--width", type=int, default=640, help="half screen, see scene 8")
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--quality", type=int, default=3, help="jpeg quality 2..31, lower is better")
    ap.add_argument("--clean", action="store_true", help="empty the output folder first")
    ap.add_argument("--fit", default="cover", choices=("cover", "contain", "stretch"),
                    help="how a source frame fills the 640x720 half (default cover: crop to fill)")
    ap.add_argument("--repeat", type=int, default=1,
                    help="repeat every source frame N times: same timing, N x more pictures "
                         "(use this to turn a 14 frame gif into 84 pictures played at 60 fps)")
    a = ap.parse_args()

    if not os.path.isfile(a.source):
        print("no such file:", a.source)
        return 1
    if not os.path.isdir(a.out):
        os.makedirs(a.out)
    if a.clean:
        for f in os.listdir(a.out):
            if f.lower().endswith((".jpg", ".jpeg", ".png", ".bmp", ".webp")):
                os.remove(os.path.join(a.out, f))
        print("cleared", a.out)

    ext = os.path.splitext(a.source)[1].lower()
    if ext == ".gif":
        files = grab_gif(a.source, a.start, a.duration, a.fps, a.width, a.height,
                         a.out, a.fit, a.repeat)
    else:
        files = grab_video(a.source, a.start, a.duration, a.fps,
                           a.width, a.height, a.out, a.quality, a.fit, a.repeat)

    if not files:
        return 1

    print()
    print("%d frames in %s" % (len(files), a.out))
    secs = len(files) / float(a.fps)
    kbytes = len(files) * 900
    print("plays for %.1f s at %g fps" % (secs, a.fps))
    print("DDR: %d MiB, JTAG load at 406 KiB/s: about %.1f min"
          % (kbytes // 1024, kbytes / 416.0 / 60))
    if len(files) > 168:
        print("WARNING: more than IMG_MAX_PICS (168) - cut a shorter clip "
              "or use a lower --fps")
    print()
    print("next:")
    print("  python img2assets.py")
    print("  .\\load_assets.ps1 -SkipRebuild -Speed 16000")
    return 0


if __name__ == "__main__":
    sys.exit(main())
