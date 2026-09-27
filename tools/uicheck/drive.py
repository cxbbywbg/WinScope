"""驱动 WinScope 逐页截图。

不移动鼠标:直接给顶层窗口发 WM_LBUTTONDOWN/UP,Qt 会自己派发给对应控件。
导航项的位置靠扫描侧边栏里图标像素的「亮带」自动定位,不写死坐标。

用法:
    python drive.py <pid> <输出目录> [--scan]
"""

import ctypes
import os
import struct
import sys
import time
import zlib
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

SRCCOPY = 0x00CC0020
SW_MAXIMIZE = 3
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_SHOWWINDOW = 0x0040
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_MOUSEWHEEL = 0x020A

user32.SetWindowPos.argtypes = [
    wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
    ctypes.c_int, ctypes.c_int, ctypes.c_uint,
]
user32.SetWindowPos.restype = wintypes.BOOL
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.GetWindowRect.restype = wintypes.BOOL
user32.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
user32.ClientToScreen.restype = wintypes.BOOL

PAGES = [
    ("01_dashboard", "系统概览"),
    ("02_processes", "进程管理"),
    ("03_performance", "性能监控"),
    ("04_network", "网络监控"),
    ("05_startup", "启动项"),
    ("06_services", "服务"),
    ("07_system", "系统信息"),
    ("08_tools", "工具箱"),
]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long), ("biHeight", ctypes.c_long),
        ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
        ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


def find_window(pid):
    found = []
    enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def callback(hwnd, _lparam):
        wpid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value != pid or not user32.IsWindowVisible(hwnd):
            return True
        rect = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        if rect.right - rect.left > 300 and rect.bottom - rect.top > 200:
            n = user32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            if "WinScope" in buf.value:
                found.append(hwnd)
        return True

    user32.EnumWindows(enum_proc(callback), 0)
    return found


class Shot:
    """一次抓屏,同时保留像素缓冲,方便顺手做点图像分析。"""

    def __init__(self, hwnd, printwindow=True):
        rect = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        self.left, self.top = rect.left, rect.top
        self.width = rect.right - rect.left
        self.height = rect.bottom - rect.top

        hdc = user32.GetDC(0)
        memdc = gdi32.CreateCompatibleDC(hdc)
        hbmp = gdi32.CreateCompatibleBitmap(hdc, self.width, self.height)
        old = gdi32.SelectObject(memdc, hbmp)

        drawn = False
        if printwindow:
            # PrintWindow 直接让窗口自己画到 DC 上,窗口被别的程序盖住也不影响
            user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, ctypes.c_uint]
            user32.PrintWindow.restype = wintypes.BOOL
            drawn = bool(user32.PrintWindow(hwnd, memdc, 0x00000002))  # PW_RENDERFULLCONTENT
        if not drawn:
            gdi32.BitBlt(memdc, 0, 0, self.width, self.height, hdc, self.left, self.top, SRCCOPY)

        bih = BITMAPINFOHEADER()
        bih.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bih.biWidth = self.width
        bih.biHeight = -self.height
        bih.biPlanes = 1
        bih.biBitCount = 32
        bih.biCompression = 0

        size = self.width * self.height * 4
        buf = ctypes.create_string_buffer(size)
        gdi32.GetDIBits(memdc, hbmp, 0, self.height, buf, ctypes.byref(bih), 0)
        self.px = buf.raw[:size]

        gdi32.SelectObject(memdc, old)
        gdi32.DeleteObject(hbmp)
        gdi32.DeleteDC(memdc)
        user32.ReleaseDC(0, hdc)

    def rgb(self, x, y):
        i = (y * self.width + x) * 4
        return self.px[i + 2], self.px[i + 1], self.px[i]

    def lum(self, x, y):
        r, g, b = self.rgb(x, y)
        return 0.299 * r + 0.587 * g + 0.114 * b

    def save(self, path):
        raw = bytearray()
        stride = self.width * 4
        for y in range(self.height):
            raw.append(0)
            row = self.px[y * stride:(y + 1) * stride]
            for i in range(0, stride, 4):
                raw.append(row[i + 2])
                raw.append(row[i + 1])
                raw.append(row[i])

        def chunk(tag, data):
            return (struct.pack(">I", len(data)) + tag + data
                    + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

        png = (b"\x89PNG\r\n\x1a\n"
               + chunk(b"IHDR", struct.pack(">IIBBBBB", self.width, self.height, 8, 2, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
               + chunk(b"IEND", b""))
        with open(path, "wb") as fh:
            fh.write(png)


def bands_in_column(shot, x0, x1, threshold=70.0, min_rows=6, y_start=0):
    """在 x∈[x0,x1] 这一竖条里找「有亮像素」的连续行段。"""
    rows = []
    for y in range(y_start, shot.height):
        hit = False
        for x in range(x0, x1):
            if shot.lum(x, y) > threshold:
                hit = True
                break
        rows.append(hit)

    out = []
    start = None
    for i, hit in enumerate(rows):
        y = i + y_start
        if hit and start is None:
            start = y
        elif not hit and start is not None:
            if y - start >= min_rows:
                out.append((start, y - 1))
            start = None
    if start is not None:
        last = len(rows) - 1 + y_start
        if last - start + 1 >= min_rows:
            out.append((start, last))
    return out


def bands_in_row(shot, y0, y1, x_start, x_end, threshold=95.0, min_cols=8, gap=14):
    """在 y∈[y0,y1] 这一横条里找「有亮像素」的连续列段,用来定位页签文字。"""
    cols = []
    for x in range(x_start, x_end):
        hit = False
        for y in range(y0, y1):
            if shot.lum(x, y) > threshold:
                hit = True
                break
        cols.append(hit)

    out = []
    start = None
    last_end = None
    for i, hit in enumerate(cols):
        x = i + x_start
        if hit:
            if start is None:
                start = x
            last_end = x
        elif start is not None and (x - last_end) > gap:
            if last_end - start + 1 >= min_cols:
                out.append((start, last_end))
            start = None
    if start is not None and last_end is not None and last_end - start + 1 >= min_cols:
        out.append((start, last_end))
    return out


def find_tabs(shot):
    """在内容区顶部找 QTabWidget 的页签行。

    返回 (y0, y1, [(x0,x1), ...]);找不到返回 None。
    侧边栏宽约 205 图像像素,所以从 x=300 起扫,不会把导航项算进来。
    """
    rows = bands_in_column(shot, 300, 1000, threshold=95.0, min_rows=6, y_start=120)
    if not rows:
        return None
    top, bottom = rows[0]
    tabs = bands_in_row(shot, top, bottom + 1, 300, 1100)
    if not tabs:
        return None
    return top, bottom, tabs


def click_tab(hwnd, shot, ox, oy, index):
    """点开第 index 个页签(0 起)。返回 True 表示点到了。"""
    found = find_tabs(shot)
    if not found:
        return False
    top, bottom, tabs = found
    if index >= len(tabs):
        return False
    x0, x1 = tabs[index]
    click_client(hwnd, (x0 + x1) // 2 + ox, (top + bottom) // 2 + oy)
    return True


def find_highlight(shot, x0, x1):
    """找侧边栏里 accent-dim (#2a4c86) 的选中高亮,返回 (y0,y1)。"""
    target = (0x2A, 0x4C, 0x86)
    rows = []
    for y in range(shot.height):
        count = 0
        for x in range(x0, x1):
            r, g, b = shot.rgb(x, y)
            if abs(r - target[0]) < 18 and abs(g - target[1]) < 18 and abs(b - target[2]) < 18:
                count += 1
        rows.append(count > 20)
    start = None
    for y, hit in enumerate(rows):
        if hit and start is None:
            start = y
        elif not hit and start is not None:
            return start, y - 1
    return None


def force_foreground(hwnd):
    """把窗口真正顶到最前。

    SetForegroundWindow 对非前台进程基本会被静默忽略,标准解法是先把自己
    的输入队列挂到当前前台线程上,借它的权限调一次,再摘掉。
    """
    fg = user32.GetForegroundWindow()
    if fg == hwnd:
        return True
    fg_thread = user32.GetWindowThreadProcessId(fg, None) if fg else 0
    cur_thread = kernel32.GetCurrentThreadId()
    attached = False
    if fg_thread and fg_thread != cur_thread:
        attached = bool(user32.AttachThreadInput(cur_thread, fg_thread, True))
    try:
        user32.BringWindowToTop(hwnd)
        user32.SetForegroundWindow(hwnd)
    finally:
        if attached:
            user32.AttachThreadInput(cur_thread, fg_thread, False)
    return user32.GetForegroundWindow() == hwnd


def window_title(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(hwnd, buf, n + 1)
    return buf.value


def click_client(hwnd, cx, cy):
    lparam = (cy << 16) | (cx & 0xFFFF)
    user32.SendMessageW(hwnd, WM_LBUTTONDOWN, 1, lparam)
    user32.SendMessageW(hwnd, WM_LBUTTONUP, 0, lparam)


def scroll_nav_top(hwnd, screen_x, screen_y):
    """把侧边栏导航列表滚回顶部 —— 用户可能手动滚过,不重置的话点击会落错项。"""
    for _ in range(20):
        wparam = (120 << 16) & 0xFFFF0000   # 正值 = 向上滚
        lparam = ((screen_y & 0xFFFF) << 16) | (screen_x & 0xFFFF)
        user32.SendMessageW(hwnd, WM_MOUSEWHEEL, wparam, lparam)
        time.sleep(0.02)
    time.sleep(0.4)


def main():
    pid = int(sys.argv[1])
    outdir = sys.argv[2]
    scan_only = "--scan" in sys.argv
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
    print("置顶结果:", force_foreground(hwnd))
    time.sleep(1.5)

    origin = wintypes.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(origin))

    # 先把导航滚到顶,否则后面按位置点击会全错位
    scroll_nav_top(hwnd, origin.x + 100, origin.y + 200)

    shot = Shot(hwnd)
    # 换算:图像坐标 -> 客户区坐标。
    # 图像原点 = 窗口矩形左上角,客户区原点由 ClientToScreen 给出,所以是「加」偏移。
    ox = shot.left - origin.x
    oy = shot.top - origin.y

    hl = find_highlight(shot, 10, 205)
    # 跳过品牌区(y<130),扫整条侧边栏宽度,这样图标和文字都能命中
    bands = bands_in_column(shot, 20, 200, threshold=95.0, min_rows=8, y_start=130)
    print(f"窗口 {shot.width}x{shot.height} 客户区偏移 ({ox},{oy})")
    print(f"高亮带: {hl}")
    print("图标亮带:")
    for b in bands:
        print(f"   y {b[0]}..{b[1]}  中心 {(b[0]+b[1])//2}")

    if scan_only:
        shot.save(os.path.join(outdir, "_scan.png"))
        return 0

    # 已经跳过品牌区,每一条亮带就是一个导航项
    items = bands
    if len(items) < 8:
        print(f"WARN: 只识别出 {len(items)} 个导航项,按几何推算补齐")
        # 用前两项推间距,再向后铺开
        if len(items) >= 2:
            step = items[1][0] - items[0][0]
            y = items[0][0]
            items = [(y + i * step, y + i * step + (items[0][1] - items[0][0])) for i in range(8)]
    items = items[:8]

    for i, (name, label) in enumerate(PAGES):
        y0, y1 = items[i]
        cy = (y0 + y1) // 2
        cx = 70
        click_client(hwnd, cx + ox, cy + oy)
        # 服务/启动项/系统信息首次进入要现场枚举,多等一会
        time.sleep(2.2 if i in (4, 5, 6) else 1.2)
        force_foreground(hwnd)
        time.sleep(0.4)
        shot = Shot(hwnd)
        path = os.path.join(outdir, f"{name}.png")
        shot.save(path)
        print(f"已截图 {name} ({label})  点击客户区 ({cx + ox},{cy + oy})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
