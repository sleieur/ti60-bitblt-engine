"""Search the demo-board schematic for the push buttons / DIP switches.

The bbShow demo wants a hardware "next scene" button.  Reading the SoC GPIO
port only helps if the switches are actually wired to those pins, and start-up
noise on a floating pad is what made the demo jump straight to scene 3.  This
walks every PDF under the workspace root, extracts the text layer and prints the
lines that mention a switch, key or button together with the pin/net they end up
on.
"""
import glob
import os
import re
import sys

try:
    from pypdf import PdfReader
except ImportError:                                    # pragma: no cover
    from PyPDF2 import PdfReader

ROOT = os.environ.get("SCH_ROOT", r"D:\易灵思")
PATTERNS = ("SW0", "SW1", "SW2", "SW3", "SW_", "KEY", "BUTTON", "推键", "按键", "开关")


def main():
    pdfs = [p for p in glob.glob(os.path.join(ROOT, "**", "*.pdf"), recursive=True)]
    for p in pdfs:
        print("[pdf]", p)
    print()

    for p in pdfs:
        try:
            reader = PdfReader(p)
        except Exception as exc:
            print("[skip]", os.path.basename(p), exc)
            continue

        hits = 0
        for i, page in enumerate(reader.pages):
            try:
                text = page.extract_text() or ""
            except Exception:
                continue
            lines = [ln.strip() for ln in text.splitlines()]
            for ln in lines:
                if any(pat in ln.upper() or pat in ln for pat in PATTERNS):
                    print("%s p%d: %s" % (os.path.basename(p), i + 1, ln[:160]))
                    hits += 1
                    if hits > 60:
                        print("  ... more")
                        break
            if hits > 60:
                break
        if hits:
            print()


if __name__ == "__main__":
    sys.exit(main())
