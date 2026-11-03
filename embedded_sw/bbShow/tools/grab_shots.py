"""Pull still frames out of the demo recording for the design report.

The report embeds screenshots of each scene.  They have to match the build that
was actually recorded, so the simplest reliable source is the recording itself:
pick a timestamp inside a steady part of each scene and export that frame at the
capture resolution, then rebuild the docx.

    python grab_shots.py demo.mp4 --screen docs ^
        --at 0:06:shot_intro.png ^
        --at 0:20:shot_split.png ^
        --at 1:05:shot_images.png ^
        --at 1:55:shot_key.png ^
        --at 2:10:shot_alpha.png ^
        --at 3:05:shot_game.png ^
        --at 3:50:shot_limit.png

`--at` accepts SS, MM:SS or HH:MM:SS, the same format edit_video.py uses, so the
timestamps can be copied straight from the section list.
"""
import argparse
import os
import re
import subprocess
import sys

try:
    import imageio_ffmpeg
    FFMPEG = imageio_ffmpeg.get_ffmpeg_exe()
except Exception:                                      # pragma: no cover
    FFMPEG = "ffmpeg"


def parse_time(text):
    total = 0.0
    for part in str(text).split(":"):
        total = total * 60.0 + float(part)
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--screen", default=".", help="output directory")
    ap.add_argument("--at", action="append", default=[],
                    metavar="TIME:FILE", help="timestamp and output png, repeatable")
    ap.add_argument("--width", type=int, default=0,
                    help="scale to this width (0 keeps the capture size)")
    args = ap.parse_args()

    if not args.at:
        raise SystemExit("give at least one --at TIME:FILE")

    if not os.path.isdir(args.screen):
        os.makedirs(args.screen)

    for spec in args.at:
        m = re.match(r"^(.+?):([^:]+\.png)$", spec)
        if not m:
            raise SystemExit("bad --at value (expected TIME:file.png): %s" % spec)
        t = parse_time(m.group(1))
        dst = os.path.join(args.screen, m.group(2))

        vf = ["scale=%d:-2:flags=lanczos" % args.width] if args.width else []

        cmd = [FFMPEG, "-hide_banner", "-y", "-ss", "%.3f" % t, "-i", args.video,
               "-frames:v", "1"]
        if vf:
            cmd += ["-vf", ",".join(vf)]
        cmd += [dst]

        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0:
            sys.stderr.write(proc.stderr.decode("utf-8", "replace")[-2000:])
            raise SystemExit("ffmpeg failed at %s" % spec)
        print("%-8.2fs -> %s (%d bytes)" % (t, dst, os.path.getsize(dst)))

    print("\nnow rebuild the report:\n  python docs/gen_doc.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
