"""Catch overlay text that runs off its panel, off the screen, or overlaps.

The overlays are drawn with literal coordinates, so a longer string or a panel
that is two pixels too short is silently allowed to spill - which is how the HUD
ended up with characters floating outside their black plates, the alpha legend
hanging below its bar, and the picture-index chip sitting on top of the frame
rate chip.

Three checks, all static:

  1. off screen       - text ends past the usable width of its half
  2. out of its panel - text vertically inside a panel must be horizontally
                        inside it too (and inside it vertically)
  3. panels overlap   - two plates sharing a row must not collide

Panels and text are paired by declaration order inside one function, which is how
the code actually draws: plate first, then its text.

    python check_layout.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")

SCREEN_W, SCREEN_H = 1280, 720
CPU_W = 640
MARGIN = 8
PAD = 12
FPS_W = 240
TOP_Y, TOP_H = 8, 48
BOT_H = 48
BOT_Y = SCREEN_H - MARGIN - BOT_H

SYMS = {
    "SCREEN_W": SCREEN_W, "SCREEN_H": SCREEN_H, "CPU_REG_W": CPU_W,
    "UI_MARGIN": MARGIN, "UI_PAD": PAD, "UI_FPS_W": FPS_W,
    "UI_TOP_Y": TOP_Y, "UI_TOP_H": TOP_H, "UI_BOT_Y": BOT_Y, "UI_BOT_H": BOT_H,
}

ADV = {"hwt_text": 24, "hwt_text_right": 24, "hwt_text_big": 44, "swr_text": 18}
TEXT_H = {"hwt_text": 32, "hwt_text_right": 32, "hwt_text_big": 56, "swr_text": 24}


def ev(expr):
    """Evaluate a small integer expression, or None if it is not static."""
    e = expr.strip()
    if not e:
        return None
    for k, v in SYMS.items():
        e = re.sub(r"\b%s\b" % k, str(v), e)
    if not re.fullmatch(r"[-+*/() 0-9]+", e):
        return None
    try:
        return int(eval(e.replace("/", "//")))
    except Exception:
        return None


def functions(text):
    """Split the file into top-level function bodies: (name, start, body)."""
    out = []
    for m in re.finditer(r"^(?:static\s+)?(?:void|int|uint32_t|const char \*)\s*"
                         r"(\w+)\s*\([^;]*?\)\s*\{", text, re.M):
        name = m.group(1)
        i = m.end()
        depth = 1
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        out.append((name, m.start(), text[m.start():i]))
    return out


PANEL = re.compile(r"hw_panel\s*\(\s*([^,]+?)\s*,\s*([^,]+?)\s*,\s*([^,]+?)\s*,\s*([^)]+?)\s*\)")
TEXT = re.compile(r"\b(hwt_text_right|hwt_text_big|hwt_text|swr_text)\s*\(\s*"
                  r"([^,]+?)\s*,\s*([^,]+?)\s*,\s*\"([^\"]*)\"")
FULLBAR = re.compile(r"\b(hw_bottom|hw_top)\s*\(\s*\"([^\"]*)\"")


def check_function(fname, body, fpath):
    problems = []
    panels = []        # (x, y, w, h, pos)

    # walk the body in source order so text can be matched to the panel above it
    events = []
    for m in PANEL.finditer(body):
        vals = [ev(m.group(i)) for i in range(1, 5)]
        if None in vals:
            continue
        events.append((m.start(), "panel", vals))
    for m in TEXT.finditer(body):
        x, y = ev(m.group(2)), ev(m.group(3))
        if x is None or y is None:
            continue
        events.append((m.start(), "text", (m.group(1), x, y, m.group(4))))
    for m in FULLBAR.finditer(body):
        events.append((m.start(), "bar", (m.group(1), m.group(2))))

    events.sort(key=lambda e: e[0])

    for pos, kind, val in events:
        if kind == "panel":
            panels.append((val[0], val[1], val[2], val[3], pos))
            continue

        if kind == "bar":
            which, s = val
            if which == "hw_bottom":
                x, limit = MARGIN + PAD, SCREEN_W - MARGIN - PAD
            else:
                x, limit = MARGIN + FPS_W + 8 + PAD, SCREEN_W - MARGIN - PAD
            end = x + len(s) * ADV["hwt_text"]
            if end > limit:
                problems.append("%s: %s(%r) ends at %d, past %d"
                                % (fname, which, s, end, limit))
            continue

        fn, x, y, s = val
        adv, th = ADV[fn], TEXT_H[fn]
        width = CPU_W if fn == "swr_text" else SCREEN_W

        if fn == "hwt_text_right":
            start = x - len(s) * adv
            if start < MARGIN or x > width - MARGIN:
                problems.append("%s: %s spans %d..%d outside the screen"
                                % (fname, s, start, x))
            continue

        end = x + len(s) * adv
        if end > width - MARGIN:
            problems.append("%s: %s(%r) ends at %d, past %d"
                            % (fname, fn, s, end, width - MARGIN))

        # must sit inside the most recent panel that covers its rows
        for (px, py, pw, ph, _p) in reversed(panels):
            if py <= y and y + th <= py + ph:      # vertically inside this panel
                if x < px or end > px + pw:
                    problems.append("%s: %r spans x %d..%d but its panel is %d..%d"
                                    % (fname, s, x, end, px, px + pw))
                break

    # panels that share a row must not collide
    for i in range(len(panels)):
        ax, ay, aw, ah, _ = panels[i]
        for j in range(i + 1, len(panels)):
            bx, by, bw, bh, _ = panels[j]
            if ay < by + bh and by < ay + ah:
                if ax < bx + bw and bx < ax + aw:
                    problems.append("%s: panels (%d,%d %dx%d) and (%d,%d %dx%d) overlap"
                                    % (fname, ax, ay, aw, ah, bx, by, bw, bh))

    return problems


def main():
    total = 0
    for name in ("scenes.c", "game.c", "main.c"):
        path = os.path.join(SRC, name)
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for fname, _start, body in functions(text):
            for msg in check_function(fname, body, path):
                print("%s: %s" % (name, msg))
                total += 1

    if total == 0:
        print("layout OK: text fits its panels, panels do not collide")
        return 0
    print("\n%d layout problem(s)" % total)
    return 1


if __name__ == "__main__":
    sys.exit(main())
