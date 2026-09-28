#!/usr/bin/env python3
"""核对托盘图标和窗口图标。

小窗是 Qt::Tool(WS_EX_TOOLWINDOW),不占任务栏按钮,进出全靠右下角的托盘图标。
这个脚本把相关的事情一次查完:

    status   小窗的扩展样式对不对;进程有没有登记托盘图标;登记条目是不是可见
    icons    主窗口和小窗的图标逐像素比对(顺手也查 WS_EX_TOOLWINDOW)
    click    模拟「左键单击托盘图标」(走 Qt 的托盘回调消息,不动真实鼠标)
    menu     模拟「右键托盘图标」,等菜单弹出来后截图

为什么不用真鼠标:会抢用户的鼠标。Qt 的托盘图标是 NOTIFYICON_VERSION_4,
外壳把事件投到 `Qt*TrayIconMessageWindowClass` 这个隐藏窗口上:

    消息号  = WM_APP + 101
    lParam  = MAKELONG(事件, 图标ID)      事件: NIN_SELECT / WM_CONTEXTMENU ...
    wParam  = MAKELONG(x, y)              屏幕坐标,只有 WM_CONTEXTMENU 用

所以直接 PostMessage 就能触发,等价于用户点了图标。

用法:
    python tools/uicheck/traycheck.py <pid> status
    python tools/uicheck/traycheck.py <pid> icons
    python tools/uicheck/traycheck.py <pid> click
    python tools/uicheck/traycheck.py <pid> menu [输出png]
"""

import ctypes
import os
import sys
import time
import winreg
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from drive import Shot, user32  # noqa: E402

gdi32 = ctypes.windll.gdi32

WM_APP = 0x8000
MYWM_NOTIFYICON = WM_APP + 101   # Qt 的托盘回调消息号
NIN_SELECT = 0x0400              # WM_USER + 0,单击
WM_CONTEXTMENU = 0x007B          # 右键
GWL_EXSTYLE = -20
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_TOPMOST = 0x00000008

# 64 位句柄必须显式声明 argtypes,否则 ctypes 按 32 位传参直接溢出
user32.GetDC.restype = ctypes.c_void_p
user32.GetDC.argtypes = [wintypes.HWND]
user32.ReleaseDC.argtypes = [wintypes.HWND, ctypes.c_void_p]
user32.GetIconInfo.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
user32.GetIconInfo.restype = wintypes.BOOL
gdi32.CreateCompatibleDC.restype = ctypes.c_void_p
gdi32.CreateCompatibleDC.argtypes = [ctypes.c_void_p]
gdi32.DeleteDC.argtypes = [ctypes.c_void_p]
gdi32.SelectObject.restype = ctypes.c_void_p
gdi32.SelectObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
gdi32.DeleteObject.argtypes = [ctypes.c_void_p]
gdi32.GetObjectW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
gdi32.GetObjectW.restype = ctypes.c_int
gdi32.GetDIBits.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint,
                            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
gdi32.GetDIBits.restype = ctypes.c_int


class ICONINFO(ctypes.Structure):
    _fields_ = [('fIcon', wintypes.BOOL), ('xHotspot', wintypes.DWORD), ('yHotspot', wintypes.DWORD),
                ('hbmMask', ctypes.c_void_p), ('hbmColor', ctypes.c_void_p)]


class BITMAP(ctypes.Structure):
    _fields_ = [('bmType', wintypes.LONG), ('bmWidth', wintypes.LONG), ('bmHeight', wintypes.LONG),
                ('bmWidthBytes', wintypes.LONG), ('bmPlanes', wintypes.WORD),
                ('bmBitsPixel', wintypes.WORD), ('bmBits', ctypes.c_void_p)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('biSize', wintypes.DWORD), ('biWidth', wintypes.LONG), ('biHeight', wintypes.LONG),
                ('biPlanes', wintypes.WORD), ('biBitCount', wintypes.WORD),
                ('biCompression', wintypes.DWORD), ('biSizeImage', wintypes.DWORD),
                ('biXPelsPerMeter', wintypes.LONG), ('biYPelsPerMeter', wintypes.LONG),
                ('biClrUsed', wintypes.DWORD), ('biClrImportant', wintypes.DWORD)]


def win_title(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(hwnd, buf, n + 1)
    return buf.value


def win_class(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def enum_windows(pid, visible_only=False, title_filter=None):
    found = []

    def callback(hwnd, _):
        wpid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value != pid:
            return True
        if visible_only and not user32.IsWindowVisible(hwnd):
            return True
        title = win_title(hwnd)
        if title_filter and title_filter not in title:
            return True
        found.append(hwnd)
        return True

    user32.EnumWindows(ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)(callback), 0)
    return found


def tray_message_window(pid):
    for hwnd in enum_windows(pid):
        if 'TrayIconMessageWindow' in win_class(hwnd):
            return hwnd
    return None


def icon_pixels(hwnd, which=1):
    """which: 1 = ICON_BIG,0 = ICON_SMALL。返回 (w, h, bytes) 或 None。"""
    h = user32.SendMessageW(hwnd, 0x007F, which, 0)   # WM_GETICON
    if not h:
        h = gdi32.GetClassLongPtrW(hwnd, -14 if which else -34)
    if not h:
        return None

    ii = ICONINFO()
    if not user32.GetIconInfo(ctypes.c_void_p(h), ctypes.byref(ii)):
        return None

    bm = BITMAP()
    gdi32.GetObjectW(ii.hbmColor, ctypes.sizeof(bm), ctypes.byref(bm))
    w, hh = bm.bmWidth, bm.bmHeight

    bih = BITMAPINFOHEADER()
    bih.biSize = ctypes.sizeof(bih)
    bih.biWidth = w
    bih.biHeight = -hh
    bih.biPlanes = 1
    bih.biBitCount = 32

    size = w * hh * 4
    buf = ctypes.create_string_buffer(size)
    hdc = user32.GetDC(0)
    mem = gdi32.CreateCompatibleDC(hdc)
    gdi32.SelectObject(mem, ii.hbmColor)
    gdi32.GetDIBits(mem, ii.hbmColor, 0, hh, buf, ctypes.byref(bih), 0)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(0, hdc)
    gdi32.DeleteObject(ii.hbmColor)
    gdi32.DeleteObject(ii.hbmMask)
    return w, hh, buf.raw[:size]


def exe_path(pid):
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    buf = ctypes.create_unicode_buffer(1024)
    size = ctypes.c_ulong(1024)
    ok = k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size))
    k32.CloseHandle(h)
    return buf.value if ok else None


def find_notify_entry(path):
    """在外壳的托盘图标登记表里找这个 exe。返回 (子键名, {值...})。"""
    if not path:
        return None, {}
    try:
        root = winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Control Panel\NotifyIconSettings')
    except FileNotFoundError:
        return None, {}
    count = winreg.QueryInfoKey(root)[0]
    for i in range(count):
        sub = winreg.EnumKey(root, i)
        with winreg.OpenKey(root, sub) as sk:
            values = {}
            j = 0
            while True:
                try:
                    name, val, _ = winreg.EnumValue(sk, j)
                    values[name] = val
                    j += 1
                except OSError:
                    break
        if str(values.get('ExecutablePath', '')).lower() == path.lower():
            return sub, values
    return None, {}


def cmd_status(pid):
    wins = enum_windows(pid, visible_only=True, title_filter='WinScope')
    print(f'可见窗口 {len(wins)} 个:')
    for hwnd in wins:
        ex = user32.GetWindowLongW(hwnd, GWL_EXSTYLE) & 0xFFFFFFFF
        tool = 'TOOLWINDOW' if ex & WS_EX_TOOLWINDOW else '-'
        top = 'TOPMOST' if ex & WS_EX_TOPMOST else '-'
        print(f'  {hex(hwnd)} {win_title(hwnd)!r}  {tool} {top}')
        print(f'      任务栏会不会显示按钮: {"不会" if ex & WS_EX_TOOLWINDOW else "会"}')

    path = exe_path(pid)
    print(f'\n进程 exe: {path}')
    sub, values = find_notify_entry(path)
    if not sub:
        print('托盘登记: 没有 —— 图标还没被显示过')
        return
    promoted = values.get('IsPromoted')
    print(f'托盘登记: 子键 {sub}')
    print(f'  IsPromoted = {promoted}  ->  '
          f'{"在右下角可见区" if promoted else "被 Win11 收进了「隐藏的图标」浮窗"}')
    print('  (可见性存在 HKCU\\Control Panel\\NotifyIconSettings,没有公开 API;'
          '系统设置里「其他系统托盘图标」改的就是它)')


def cmd_icons(pid):
    wins = enum_windows(pid, title_filter='WinScope')
    if len(wins) < 2:
        print(f'只找到 {len(wins)} 个 WinScope 窗口,需要主窗口和小窗同时在(哪怕藏着)')
        for hwnd in wins:
            print('  ', hex(hwnd), repr(win_title(hwnd)))
        return 1
    wins.sort(key=lambda h: (win_title(h) != 'WinScope 小窗'))
    mini, main = wins[0], wins[1]
    print(f'主窗口 {hex(main)}  小窗 {hex(mini)}')
    ok = True
    for which, label in [(1, '大图标'), (0, '小图标')]:
        a = icon_pixels(main, which)
        b = icon_pixels(mini, which)
        if not a or not b:
            print(f'  {label}: 有一边没有图标')
            ok = False
            continue
        same = a[:2] == b[:2] and a[2] == b[2]
        print(f'  {label}: 主窗口 {a[0]}x{a[1]} / 小窗 {b[0]}x{b[1]}  一致={same}')
        ok = ok and same
    print('图标一致' if ok else '图标不一致 —— 小窗多半漏了 setWindowIcon,'
                              '或者没走 QApplication::setWindowIcon 那一份')
    return 0 if ok else 1


def cmd_click(pid):
    hwnd = tray_message_window(pid)
    if not hwnd:
        print('没找到 Qt 的托盘消息窗口 —— 托盘图标还没 show 出来?')
        return 1
    before = [win_title(h) for h in enum_windows(pid, visible_only=True) if win_title(h)]
    user32.PostMessageW(hwnd, MYWM_NOTIFYICON, 0, NIN_SELECT)
    time.sleep(1.5)
    after = [win_title(h) for h in enum_windows(pid, visible_only=True) if win_title(h)]
    print(f'投递 NIN_SELECT 到 {hex(hwnd)}')
    print(f'  之前可见: {before}')
    print(f'  之后可见: {after}')
    return 0 if before != after else 1


def cmd_menu(pid, out):
    hwnd = tray_message_window(pid)
    if not hwnd:
        print('没找到 Qt 的托盘消息窗口')
        return 1
    # 菜单从托盘图标位置弹出来,给个屏幕坐标(wParam = MAKELONG(x, y))
    tray = user32.FindWindowW('Shell_TrayWnd', None)
    notify = user32.FindWindowExW(tray, 0, 'TrayNotifyWnd', None)
    rect = wintypes.RECT()
    user32.GetWindowRect(notify, ctypes.byref(rect))
    x, y = rect.left + 20, rect.top + rect.bottom - rect.top - 10
    user32.PostMessageW(hwnd, MYWM_NOTIFYICON, (y << 16) | (x & 0xFFFF), WM_CONTEXTMENU)
    time.sleep(1.5)

    # 注意:这个菜单是 Qt 自绘的弹出窗(类名 Qt*QWindowPopupDropShadowSaveBits),
    # 不是原生 #32768,所以别按原生菜单的类名去找
    popup = None
    for h in enum_windows(pid, visible_only=True):
        if 'Popup' in win_class(h):
            popup = h
            break
    if not popup:
        print('没等到弹出菜单')
        return 1
    r = wintypes.RECT()
    user32.GetWindowRect(popup, ctypes.byref(r))
    print(f'菜单窗口 {hex(popup)} {win_class(popup)} {(r.left, r.top, r.right, r.bottom)}')
    shot = Shot(popup)
    shot.save(out)
    print('已截图', out)

    # 收尾:把菜单关掉,别留在屏幕上
    user32.PostMessageW(popup, 0x001F, 0, 0)      # WM_CANCELMODE
    time.sleep(0.3)
    user32.PostMessageW(popup, 0x0100, 0x1B, 0)   # WM_KEYDOWN VK_ESCAPE
    user32.PostMessageW(popup, 0x0101, 0x1B, 0)
    time.sleep(0.5)
    return 0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    pid = int(sys.argv[1])
    action = sys.argv[2]
    if action == 'status':
        cmd_status(pid)
        return 0
    if action == 'icons':
        return cmd_icons(pid)
    if action == 'click':
        return cmd_click(pid)
    if action == 'menu':
        out = sys.argv[3] if len(sys.argv) > 3 else 'tray_menu.png'
        return cmd_menu(pid, out)
    print('未知动作:', action)
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
