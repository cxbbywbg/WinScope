"""把窗口的某个区域放大截出来,用来核对小字号文本。

用法:
    python zoom.py <pid> <x> <y> <w> <h> <放大倍数> <输出png>
        [--click N]   先点第 N 个侧边栏导航项(0 起)
        [--tab M]     再点第 M 个页签(0 起,如性能监控页的 CPU/内存/GPU/磁盘)

坐标是相对窗口的,和 drive.py 输出的 PNG 一致。
"""

import ctypes
import struct
import sys
import time
import zlib
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

SRCCOPY = 0x00CC0020
HALFTONE = 4


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

    def callback(hwnd, _l):
        wp = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wp))
        if wp.value != pid or not user32.IsWindowVisible(hwnd):
            return True
        r = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        if r.right - r.left > 300:
            n = user32.GetWindowTextLengthW(hwnd)
            b = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, b, n + 1)
            if "WinScope" in b.value:
                found.append(hwnd)
        return True

    user32.EnumWindows(enum_proc(callback), 0)
    return found


def write_png(path, w, h, bgra):
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        row = bgra[y * stride:(y + 1) * stride]
        for i in range(0, stride, 4):
            raw.append(row[i + 2])
            raw.append(row[i + 1])
            raw.append(row[i])

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(png)


def main():
    pid, x, y, w, h, scale, out = (int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]),
                                   int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]), sys.argv[7])
    hwnd = find_window(pid)[0]

    # 可选:先切页面。--click N 点侧边栏第 N 项,--tab M 再点第 M 个页签
    nav = None
    tab = None
    if "--click" in sys.argv:
        nav = int(sys.argv[sys.argv.index("--click") + 1])
    if "--tab" in sys.argv:
        tab = int(sys.argv[sys.argv.index("--tab") + 1])

    if nav is not None or tab is not None:
        import drive
        drive.force_foreground(hwnd)
        origin = wintypes.POINT(0, 0)
        user32.ClientToScreen(hwnd, ctypes.byref(origin))
        shot = drive.Shot(hwnd)
        ox = shot.left - origin.x
        oy = shot.top - origin.y

        if nav is not None:
            bands = drive.bands_in_column(shot, 20, 200, threshold=95.0, min_rows=8, y_start=130)
            y0, y1 = bands[nav]
            drive.click_client(hwnd, 70 + ox, (y0 + y1) // 2 + oy)
            time.sleep(2.5)
            shot = drive.Shot(hwnd)

        if tab is not None:
            if not drive.click_tab(hwnd, shot, ox, oy, tab):
                print(f"WARN: 没找到第 {tab} 个页签")
            time.sleep(1.5)

    r = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    ww, wh = r.right - r.left, r.bottom - r.top

    hdc = user32.GetDC(0)
    src = gdi32.CreateCompatibleDC(hdc)
    srcbmp = gdi32.CreateCompatibleBitmap(hdc, ww, wh)
    gdi32.SelectObject(src, srcbmp)
    user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, ctypes.c_uint]
    user32.PrintWindow(hwnd, src, 0x00000002)

    dw, dh = w * scale, h * scale
    dst = gdi32.CreateCompatibleDC(hdc)
    dstbmp = gdi32.CreateCompatibleBitmap(hdc, dw, dh)
    gdi32.SelectObject(dst, dstbmp)
    gdi32.SetStretchBltMode(dst, HALFTONE)
    gdi32.StretchBlt(dst, 0, 0, dw, dh, src, x, y, w, h, SRCCOPY)

    bih = BITMAPINFOHEADER()
    bih.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bih.biWidth = dw
    bih.biHeight = -dh
    bih.biPlanes = 1
    bih.biBitCount = 32
    size = dw * dh * 4
    buf = ctypes.create_string_buffer(size)
    gdi32.GetDIBits(dst, dstbmp, 0, dh, buf, ctypes.byref(bih), 0)

    write_png(out, dw, dh, buf.raw[:size])
    print(f"已放大 {w}x{h} @({x},{y}) -> {dw}x{dh}  {out}")


if __name__ == "__main__":
    main()
