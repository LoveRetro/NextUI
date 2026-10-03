#!/usr/bin/env python3
"""Grab a screenshot from a device's framebuffer over adb.

usage: screenshot.py out.png [page]

Needs Pillow. The geometry is read from the device (sysfs), the pixel format is
assumed to be 32bpp BGRA, which is what the TrimUI devices use. The framebuffer is
multi-page (double buffered); page 0 is usually fine, pass 1 if you get a stale frame.
"""
import re
import subprocess
import sys
import tempfile

from PIL import Image


def adb(*args):
    return subprocess.run(["adb", *args], check=True, capture_output=True, text=True).stdout.strip()


def main():
    out = sys.argv[1]
    page = int(sys.argv[2]) if len(sys.argv) > 2 else 0

    fb = "/sys/class/graphics/fb0/"
    w, h = map(int, re.search(r"(\d+)x(\d+)", adb("shell", f"cat {fb}modes")).groups())
    stride = int(adb("shell", f"cat {fb}stride"))
    size = stride * h

    # dd on the device and pull: `adb exec-out cat /dev/fb0` is unreliable here
    adb("shell", f"dd if=/dev/fb0 of=/tmp/fb.raw bs={stride} skip={page * h} count={h} 2>/dev/null")
    with tempfile.NamedTemporaryFile() as tmp:
        adb("pull", "/tmp/fb.raw", tmp.name)
        raw = open(tmp.name, "rb").read()[:size]
    Image.frombuffer("RGBA", (stride // 4, h), raw, "raw", "BGRA", 0, 1).crop((0, 0, w, h)).convert("RGB").save(out)


main()
