"""截取 WinScope 主窗口。

纯标准库实现:ctypes 调 GDI 抓位图,再用 zlib 手写 PNG。
不依赖 Pillow,也不需要在目标机上装任何东西。
"""

import ctypes
import struct
import sys
import time
import zlib
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

# 显式声明,免得 64 位下句柄/指针被当成 int 截断
user32.SetWindowPos.argtypes = [
    wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
    ctypes.c_int, ctypes.c_int, ctypes.c_uint,
]
user32.SetWindowPos.restype = wintypes.BOOL
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.GetWindowRect.restype = wintypes.BOOL

SRCCOPY = 0x00CC0020
SW_MAXIMIZE = 3
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_SHOWWINDOW = 0x0040


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", ctypes.c_long),
        ("biHeight", ctypes.c_long),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", ctypes.c_long),
        ("biYPelsPerMeter", ctypes.c_long),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


def find_window(pid):
    """按进程号找可见的顶层窗口,返回 (hwnd, 标题)。"""
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
            length = user32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(hwnd, buf, length + 1)
            found.append((hwnd, buf.value, rect))
        return True

    user32.EnumWindows(enum_proc(callback), 0)
    return found


def write_png(path, width, height, bgra):
    """bgra 是自上而下的 BGRA 字节串,直接写成 RGB PNG。"""
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # 每行的过滤器类型:None
        row = bgra[y * stride:(y + 1) * stride]
        # BGRA -> RGB,必须逐像素交错,不能按通道平面拼
        for i in range(0, stride, 4):
            raw.append(row[i + 2])
            raw.append(row[i + 1])
            raw.append(row[i])

    def chunk(tag, data):
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as fh:
        fh.write(png)


def main():
    pid = int(sys.argv[1])
    out = sys.argv[2]

    windows = find_window(pid)
    if not windows:
        print("ERROR: 没有找到 WinScope 窗口")
        return 1

    hwnd, title, _rect = windows[0]
    print("窗口:", hex(hwnd), repr(title))

    user32.ShowWindow(hwnd, SW_MAXIMIZE)
    user32.SetForegroundWindow(hwnd)
    # SetForegroundWindow 常被系统静默拒绝,用 topmost 开关把窗口硬顶到最前
    flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
    user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags)
    user32.SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags)
    user32.BringWindowToTop(hwnd)
    time.sleep(2.0)

    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    left, top = rect.left, rect.top
    width = rect.right - rect.left
    height = rect.bottom - rect.top
    print(f"尺寸: {width}x{height} @ ({left},{top})")

    hdc = user32.GetDC(0)
    memdc = gdi32.CreateCompatibleDC(hdc)
    hbmp = gdi32.CreateCompatibleBitmap(hdc, width, height)
    old = gdi32.SelectObject(memdc, hbmp)

    ok = gdi32.BitBlt(memdc, 0, 0, width, height, hdc, left, top, SRCCOPY)
    if not ok:
        print("ERROR: BitBlt 失败")
        return 1

    bih = BITMAPINFOHEADER()
    bih.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bih.biWidth = width
    bih.biHeight = -height  # 负数 = 自上而下
    bih.biPlanes = 1
    bih.biBitCount = 32
    bih.biCompression = 0  # BI_RGB

    size = width * height * 4
    buf = ctypes.create_string_buffer(size)
    got = gdi32.GetDIBits(memdc, hbmp, 0, height, buf, ctypes.byref(bih), 0)
    print("GetDIBits 扫描行:", got)

    gdi32.SelectObject(memdc, old)
    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(memdc)
    user32.ReleaseDC(0, hdc)

    write_png(out, width, height, buf.raw[:size])
    print("已保存:", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
