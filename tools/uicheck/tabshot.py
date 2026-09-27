#!/usr/bin/env python3
"""把某个页面的所有页签(Tab)挨个点开截图。

性能监控页的 CPU/内存/GPU/磁盘 藏在 QTabWidget 里,drive.py 只点侧边栏导航,
进不去。这个脚本先按 nav 序号切到目标页,再横向扫描页签栏的文字亮带,
逐个点开截图。

用法:
    python tools/uicheck/tabshot.py <pid> <outdir> --nav 3 [--names cpu,mem,gpu,disk]

坐标换算与 drive.py 一致:图像坐标 + (ox,oy) = 客户区坐标。
"""

import ctypes
import os
import sys
import time
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from drive import (  # noqa: E402
    Shot,
    bands_in_column,
    click_client,
    find_tabs,
    find_window,
    force_foreground,
    scroll_nav_top,
    user32,
    window_title,
)

SW_MAXIMIZE = 3
SWP_NOMOVE = 0x0002
SWP_NOSIZE = 0x0001
SWP_SHOWWINDOW = 0x0040
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2


def main():
    pid = int(sys.argv[1])
    outdir = sys.argv[2]
    nav_index = 3
    names = None
    if "--nav" in sys.argv:
        nav_index = int(sys.argv[sys.argv.index("--nav") + 1])
    if "--names" in sys.argv:
        names = sys.argv[sys.argv.index("--names") + 1].split(",")

    os.makedirs(outdir, exist_ok=True)

    wins = find_window(pid)
    if not wins:
        print("ERROR: 没找到窗口")
        return 1
    hwnd = wins[0]

    print("窗口:", hex(hwnd), repr(window_title(hwnd)))
    user32.ShowWindow(hwnd, SW_MAXIMIZE)
    flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
    user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags)
    user32.SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags)
    force_foreground(hwnd)
    time.sleep(1.5)

    origin = wintypes.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(origin))

    scroll_nav_top(hwnd, origin.x + 100, origin.y + 200)

    shot = Shot(hwnd)
    ox = shot.left - origin.x
    oy = shot.top - origin.y

    # ---- 先切到目标页
    bands = bands_in_column(shot, 20, 200, threshold=95.0, min_rows=8, y_start=130)
    items = bands[:8]
    if len(items) < 8:
        print(f"WARN: 只识别出 {len(items)} 个导航项")
        return 1
    y0, y1 = items[nav_index - 1]
    click_client(hwnd, 70 + ox, (y0 + y1) // 2 + oy)
    time.sleep(1.5)
    force_foreground(hwnd)
    time.sleep(0.4)

    shot = Shot(hwnd)
    found = find_tabs(shot)
    if not found:
        print("ERROR: 没找到页签行")
        return 1
    tab_top, tab_bottom, tabs = found
    print(f"页签行 y {tab_top}..{tab_bottom}")
    print(f"识别到 {len(tabs)} 个页签: {tabs}")

    for i, (x0, x1) in enumerate(tabs):
        label = names[i] if names and i < len(names) else f"tab{i}"
        click_client(hwnd, (x0 + x1) // 2 + ox, (tab_top + tab_bottom) // 2 + oy)
        time.sleep(1.4)
        force_foreground(hwnd)
        time.sleep(0.4)
        shot = Shot(hwnd)
        path = os.path.join(outdir, f"{i + 1:02d}_{label}.png")
        shot.save(path)
        print(f"已截图 {path}  点击客户区 ({(x0 + x1) // 2 + ox},{(tab_top + tab_bottom) // 2 + oy})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
