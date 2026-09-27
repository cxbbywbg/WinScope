#!/usr/bin/env python3
"""量一条横条到底填了多少。

肉眼看缩略图估比例太不可靠(我就在这上面判断错过)。这个脚本直接读窗口像素,
把某一行的颜色按「连续同色段」列出来,填充色占了多长一目了然。

用法:
    python tools/uicheck/measure.py <pid> <y> [x0] [x1] [--click N] [--tab M]

输出形如:
    y=1005
      x    497.. 1720  rgb(167,139,250)   1224px
      x   1720.. 2461  rgb( 20, 28, 36)    741px
"""

import ctypes
import os
import sys
import time
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from drive import Shot, bands_in_column, click_client, find_window, force_foreground, user32  # noqa: E402


def scan_line(shot, y, x0, x1, quantum=12):
    """把这一行切成「连续同色段」。quantum 是颜色容差(每通道)。"""
    runs = []
    for x in range(x0, x1):
        r, g, b = shot.rgb(x, y)
        key = (r // quantum, g // quantum, b // quantum)
        if runs and runs[-1][0] == key:
            runs[-1][2] = x
        else:
            runs.append([key, x, x, (r, g, b)])
    return runs


def main():
    pid = int(sys.argv[1])
    y = int(sys.argv[2])
    rest = [a for a in sys.argv[3:] if not a.startswith("--")]
    x0 = int(rest[0]) if len(rest) > 0 else 0
    x1 = int(rest[1]) if len(rest) > 1 else 0

    hwnd = find_window(pid)[0]

    nav = int(sys.argv[sys.argv.index("--click") + 1]) if "--click" in sys.argv else None
    tab = int(sys.argv[sys.argv.index("--tab") + 1]) if "--tab" in sys.argv else None

    if nav is not None or tab is not None:
        import drive
        drive.force_foreground(hwnd)
        origin = wintypes.POINT(0, 0)
        user32.ClientToScreen(hwnd, ctypes.byref(origin))
        shot = Shot(hwnd)
        ox, oy = shot.left - origin.x, shot.top - origin.y
        if nav is not None:
            bands = bands_in_column(shot, 20, 200, threshold=95.0, min_rows=8, y_start=130)
            y0, y1 = bands[nav]
            click_client(hwnd, 70 + ox, (y0 + y1) // 2 + oy)
            time.sleep(2.5)
            shot = Shot(hwnd)
        if tab is not None:
            drive.click_tab(hwnd, shot, ox, oy, tab)
            time.sleep(1.5)

    shot = Shot(hwnd)
    if x1 <= x0:
        x0, x1 = 0, shot.width

    # 抗锯齿和文字会把一行切成几百个碎片,只报足够长的色段
    min_px = 16
    if "--min" in sys.argv:
        min_px = int(sys.argv[sys.argv.index("--min") + 1])

    print(f"y={y}  x∈[{x0},{x1})  窗口 {shot.width}x{shot.height}  (只列 ≥{min_px}px 的色段)")
    skipped = 0
    for _key, a, b, color in scan_line(shot, y, x0, x1):
        if b - a + 1 < min_px:
            skipped += b - a + 1
            continue
        print(f"  x {a:5d}..{b:5d}  rgb({color[0]:3d},{color[1]:3d},{color[2]:3d})  {b - a + 1:5d}px")
    print(f"  (另有 {skipped}px 碎片,主要是文字和抗锯齿)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
