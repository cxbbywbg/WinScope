"""端到端验证进程页右键菜单 —— 重点是「打开文件所在位置」。

要验的 bug(修之前):
  ProcessPage::currentProcess() 读的是第 0 列的 Qt::UserRole,而列表行把
  「排序用的数值」写在那一格(名称列是 0.0),PID 实际写在 ColPid 列。于是永远
  拿到 pid=0 → 详情面板永远显示「系统空闲进程」,右键菜单里每个操作(结束/挂起/
  优先级/打开文件所在位置…)都在 `info.pid == 0` 那关被拦下,一律弹
  「请先在列表中选中一个进程」。修法是给 PID 单独开一个 role(PidRole)。

判据全部用**程序外部**的客观量(不读小字、不依赖窗口绝对位置、不依赖程序日志):
  1. 右键不同行 → 「复制 PID」→ 读 Windows 剪贴板,必须拿到 5 个以上
     **不同且非 0** 的 pid。(修之前每一行都复制出 0 —— 这一条就足以判定。)
  2. 同一行「复制 PID → 复制路径 → 复制 PID」,两次 pid 一致才算这一行没换人;
     路径必须与 Python 用同一套 API(QueryFullProcessImageNameW)独立查到的一致。
  3. 右键 → 「打开文件所在位置」→ 必须新开一个资源管理器窗口,标题含该行目录名。

用法:
    python tools/uicheck/revealcheck.py
    WINSCOPE_EXE=dist\\WinScope\\WinScope.exe python tools/uicheck/revealcheck.py

必须在**同一个进程**里跑完 —— 沙箱给每条命令套作业对象,命令一结束子进程就被收走。
截图落在 .tools/reveal/(该目录在 .gitignore 里)。

## 踩过的坑(每一条都让这脚本白跑过一整轮)

* **合成右键能不能弹出菜单,取决于窗口是不是前台。** `DefWindowProc` 收到
  `WM_RBUTTONUP` 时只在「没有窗口抓着鼠标」且「窗口是前台」的情况下才补发
  `WM_CONTEXTMENU`。窗口一旦失去前台焦点,菜单就一次都弹不出来,现象看起来像
  "右键整个坏了"。实测元凶是点错菜单项弹出来的模态框(见下一条)抢走了焦点。
  脚本现在每次开菜单前先 `dismiss_dialogs()`,失败时也会把 `GetCapture()` /
  前台窗口打出来自证 —— 光看"菜单没弹出"永远查不到这里。
* **别用「截图里横带的下标」当菜单项序号。** 菜单是 10 项 + 3 条分隔线,分隔线
  同样是"有亮像素的横带",被一起数进来时下标就整体错位 —— 点「复制 PID」(第 8 项)
  会点到「设置 CPU 亲和性」,弹出模态框「无法读取」,焦点被抢走,后面整轮全废。
  现在按"明显矮于中位数"滤掉分隔线,并且**项数不等于 10 就绝不点**,宁可重来。
* **列表是活的,每秒重排一次。** 「读一次路径 + 读一次 PID」要 8~10 秒,期间那一行
  早换人了 → 判据2 永远卡在"期间换行了"。正解是**点头部「暂停」把列表冻住**
  (MainWindow::togglePause → ProcessPage::onProcessSnapshot 直接 return),
  并且用"隔 2.5 秒抓两张图比数据行区域是否逐像素一致"来核对真的冻住了。
* **排序方向决定能不能核对。** 按 PID **降序**:升序会把 pid 最小的 SYSTEM 服务
  排在最前面,而非提权进程读不到这些服务的映像路径 —— 实测全表 245 个进程里
  137 个读不到(System/Registry/smss/csrss/winlogon/services 以及一大堆 svchost)。
  可见区只有 27 行,升序时整屏都是"读得到 PID、读不到路径"的行,判据2/3 无从核对。
  降序把用户自己的进程(pid 大)顶到可见区。**读不到不是 bug,是权限模型** ——
  Python 用同一套 API 同样读不到。
* **窗口落在哪块屏不影响判据,拿不到前台焦点才致命。** Qt 有时把主窗口摆到副屏
  (x 为负,实测 -2164)。坐标一律从当前矩形现算、截图走 `PrintWindow`(与位置无关)
  即可;但 `PrintWindow` 失败会退到屏幕 `BitBlt`,而 BitBlt 是从主屏 DC 上抠的,
  负坐标下内容不可信 —— 所以菜单截图先试 PrintWindow,切不出 10 项再退 BitBlt。
* **`drive.Shot()` 别拿来读窗口矩形。** 它走 `PrintWindow`(WM_PRINT),对被测窗口
  有副作用,而且为了两个整数把整张 1778x1162 抓一遍。读矩形用 `GetWindowRect`。
* **点菜单项必须用真实光标**(`SetCursorPos` + `mouse_event`):QMenu 靠 MouseMove
  设 currentAction,而且点到的往往是 Qt 的阴影窗
  (`Qt6112QWindowPopupDropShadowSaveBits`),只发合成消息石沉大海。
  另外**一步把光标跳进窗口只会产生 Enter、不产生 MouseMove**,要分小步挪进去;
  光标本来就在目标点上时 `SetCursorPos` 也不产生任何移动消息,得先弹开。
* **tooltip 也是可见顶层窗**(类名 `Qt*QWindowToolSaveBits`,不含 "Popup"),
  不排掉的话每次开菜单都会被当成模态框关一遍,白等一秒。
"""
import ctypes
import ctypes.wintypes as wt
import hashlib
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # tools/uicheck -> 仓库根
EXE = os.environ.get('WINSCOPE_EXE') or os.path.join(ROOT, 'build', 'WinScope.exe')
OUT = os.path.join(ROOT, '.tools', 'reveal')

sys.path.insert(0, HERE)
import drive  # noqa: E402

user32, kernel32 = drive.user32, drive.kernel32
# 只补 drive.py 没声明过的(句柄是 64 位,不声明会 OverflowError)。
# 别去覆盖 EnumWindows / GetWindowRect —— drive.py 已用另一套 WINFUNCTYPE 声明过,
# 类型对不上会报 "expected WinFunctionType instance instead of WinFunctionType"。
user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(wt.DWORD)]
user32.GetWindowTextLengthW.argtypes = [ctypes.c_void_p]
user32.GetWindowTextW.argtypes = [ctypes.c_void_p, wt.LPWSTR, ctypes.c_int]
user32.GetClassNameW.argtypes = [ctypes.c_void_p, wt.LPWSTR, ctypes.c_int]
user32.IsWindowVisible.argtypes = [ctypes.c_void_p]
user32.WindowFromPoint.argtypes = [wt.POINT]
user32.WindowFromPoint.restype = ctypes.c_void_p
user32.ScreenToClient.argtypes = [ctypes.c_void_p, ctypes.POINTER(wt.POINT)]
user32.GetCapture.restype = ctypes.c_void_p
user32.GetForegroundWindow.restype = ctypes.c_void_p
user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
user32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
user32.mouse_event.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_void_p]
user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_size_t]
user32.OpenClipboard.argtypes = [ctypes.c_void_p]
user32.GetClipboardData.argtypes = [ctypes.c_uint]
user32.GetClipboardData.restype = ctypes.c_void_p
user32.SetClipboardData.argtypes = [ctypes.c_uint, ctypes.c_void_p]
user32.SetClipboardData.restype = ctypes.c_void_p
kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
kernel32.GlobalLock.restype = ctypes.c_void_p
kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
kernel32.GlobalAlloc.argtypes = [ctypes.c_uint, ctypes.c_size_t]
kernel32.GlobalAlloc.restype = ctypes.c_void_p
kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.OpenProcess.restype = ctypes.c_void_p
kernel32.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, wt.DWORD, wt.LPWSTR,
                                                ctypes.POINTER(wt.DWORD)]
kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
WM_RBUTTONDOWN, WM_RBUTTONUP = 0x0204, 0x0205
WM_KEYDOWN, WM_KEYUP, WM_CANCELMODE, WM_CLOSE = 0x0100, 0x0101, 0x001F, 0x0010
VK_ESCAPE = 0x1B
MK_RBUTTON = 0x0002
HWND_TOP, HWND_TOPMOST, SWP_NOSIZE, SWP_SHOWWINDOW = 0, -1, 0x0001, 0x0040
ME_F = {'leftdown': 0x0002, 'leftup': 0x0004, 'rightdown': 0x0008, 'rightup': 0x0010}
CF_UNICODETEXT = 13
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

WIN_W, WIN_H = 1800, 1100
# 菜单项序号(0 起):0结束/1强制结束/2重启/3挂起/4恢复/5设置优先级/6设置CPU亲和性/
#                    7打开文件所在位置/8复制 PID/9复制路径
ITEM_REVEAL, ITEM_COPY_PID, ITEM_COPY_PATH = 7, 8, 9
MENU_ITEM_COUNT = 10            # 与 ProcessPage::showContextMenu 保持一致(另有 3 条分隔线)

os.makedirs(OUT, exist_ok=True)
proc = None
fails = []


# ---------------------------------------------------------------- 剪贴板

def _open_clipboard():
    for _ in range(30):
        if user32.OpenClipboard(None):
            return True
        time.sleep(0.1)
    return False


def get_clipboard_text():
    if not _open_clipboard():
        return None
    try:
        h = user32.GetClipboardData(CF_UNICODETEXT)
        if not h:
            return None
        p = kernel32.GlobalLock(h)
        if not p:
            return None
        try:
            return ctypes.wstring_at(p)
        finally:
            kernel32.GlobalUnlock(h)
    finally:
        user32.CloseClipboard()


def set_clipboard_text(text):
    """写剪贴板。**空串也要真的写空** —— 早先写成 `if not text: return`,
    于是"清空"变成了"不操作",后面读到的全是上一轮的陈旧值,把判据全带偏了。"""
    data = (text or '').encode('utf-16-le') + b'\x00\x00'
    h = kernel32.GlobalAlloc(0x0002, len(data))          # GMEM_MOVEABLE
    if not h:
        return False
    p = kernel32.GlobalLock(h)
    ctypes.memmove(p, data, len(data))
    kernel32.GlobalUnlock(h)
    if not _open_clipboard():
        return False
    try:
        user32.EmptyClipboard()
        user32.SetClipboardData(CF_UNICODETEXT, h)       # 所有权交给系统,别再 free
    finally:
        user32.CloseClipboard()
    return True


def clipboard_pid():
    t = get_clipboard_text()
    if not t or not t.strip().isdigit():
        return None
    return int(t.strip())


# ---------------------------------------------------------------- 窗口 / 进程

def top_windows(pid):
    out = []

    def cb(h, _):
        wp = wt.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(wp))
        if wp.value != pid or not user32.IsWindowVisible(h):
            return True
        cls = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(h, cls, 256)
        r = wt.RECT()
        user32.GetWindowRect(h, ctypes.byref(r))
        out.append((h, drive.window_title(h), cls.value, r))
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return out


def popups_of(pid):
    return [w for w in top_windows(pid) if 'Popup' in w[2]]


def explorer_windows():
    out = {}

    def cb(h, _):
        cls = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(h, cls, 256)
        if cls.value in ('CabinetWClass', 'ExploreWClass'):
            out[h] = drive.window_title(h)
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return out


def class_of(hwnd):
    cls = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, cls, 256)
    return cls.value


def query_process_path(pid):
    """自己查一遍这个 pid 的可执行文件路径 —— 当作**独立第二来源**,
    程序里 queryProcessPath() 用的就是同一套 API(QueryFullProcessImageNameW)。"""
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return None
    try:
        buf = ctypes.create_unicode_buffer(32768)
        size = wt.DWORD(32768)
        if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            return buf.value
        return None
    finally:
        kernel32.CloseHandle(h)


# ---------------------------------------------------------------- 点击

def ensure_on_primary(hwnd):
    """把主窗口挪到主屏左上角并**置顶**,返回是否拿到了前台焦点。

    两个坑叠在一起,不处理的话点击会全部落空:
      * 之前几轮测试用 ShellExecute 打开的资源管理器窗口还留在屏幕上,
        正好盖在要点的位置上 —— WindowFromPoint 返回的是它们(实测 0x2004a0、0x3a0a34),
        而不是被测窗口;
      * Qt 有时会把主窗口摆到副屏(x 为负,实测 -2164),跟它较劲没有意义。

    所以:**每次都要置顶**,不能"看着位置对了就跳过" —— 只要不是 topmost,
    任何别的窗口(资源管理器、tooltip)都能盖在要点的位置上。
    而位置本身不需要强求:屏幕坐标一律从当前矩形现算,截图走 PrintWindow(与位置无关)。
    """
    # 只挪位置 + 置顶,**不要改尺寸** —— 尺寸一变,之前按截图量出来的行 y 坐标就全废了
    user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW)
    drive.force_foreground(hwnd)
    time.sleep(0.45)
    return user32.GetForegroundWindow() == hwnd


def park_cursor():
    """把光标挪到窗口外。

    列表行带 tooltip,光标在列表上停一会儿 Qt 就弹一个 —— 那是个独立顶层窗,
    类名不含 "Popup" 所以 close_stray_popups 管不到它,结果 WindowFromPoint
    一直返回 tooltip(实测句柄每次都不同、永远不是被测窗口),点击全部落空。
    挪走光标 tooltip 立刻消失。
    """
    user32.SetCursorPos(user32.GetSystemMetrics(0) - 3, 5)
    time.sleep(0.2)


def _synth_click(hwnd, img_x, img_y, down, up, wparam, what):
    """在窗口图像坐标 (img_x,img_y) 处发合成点击。

    **每次都先确认窗口在主屏、把光标挪开、再重新抓图,并且断言算出来的屏幕点
    确实落在 hwnd 上。** 这几步不是洁癖:窗口被挪到副屏、被别的窗口盖住、
    被 tooltip 顶住,任意一条都会让点击石沉大海(实测命中过 0x2004a0 / 0xa509e8
    这种完全不相干的句柄)。
    """
    for attempt in range(3):
        park_cursor()
        ensure_on_primary(hwnd)
        # 这里只需要窗口左上角两个整数,别用 drive.Shot —— 它走 PrintWindow(WM_PRINT),
        # 对被测窗口有副作用,而且为了读两个整数把整张 1778x1162 的图抓一遍,白白慢一截。
        r = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        sx, sy = r.left + img_x, r.top + img_y
        under = user32.WindowFromPoint(wt.POINT(sx, sy))
        if under == hwnd:
            pt = wt.POINT(sx, sy)
            user32.ScreenToClient(hwnd, ctypes.byref(pt))
            lp = ((pt.y & 0xFFFF) << 16) | (pt.x & 0xFFFF)
            user32.SendMessageW(hwnd, down, wparam, lp)
            time.sleep(0.08)
            user32.SendMessageW(hwnd, up, 0, lp)
            return hwnd
        print('       %s:屏幕 (%d,%d) 下是 %#x (%s) 而非主窗口 %#x,重抓再试'
              % (what, sx, sy, under, class_of(under), hwnd))
        close_stray_popups()
        drive.force_foreground(hwnd)
        time.sleep(0.5)
    return None


def click_at(hwnd, x, y):
    return _synth_click(hwnd, x, y, WM_LBUTTONDOWN, WM_LBUTTONUP, 1, '左键')


def right_click_at(hwnd, x, y):
    """合成右键。开菜单这一步合成消息反而比真实光标稳:
    真实右键靠 OS 发 WM_CONTEXTMENU,本机实测菜单根本弹不出来。"""
    return _synth_click(hwnd, x, y, WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON, '右键')


# 头部「暂停」按钮在图像坐标里的位置(实测扫亮度游程量出来的:
# 按钮 x 1378..1446、y 65..97,窗口宽 1778)。头部是右对齐布局
# (MainWindow 里 addStretch(1) 在按钮左边),所以离右边缘的距离是固定的 ——
# 用宽度算,而不是写死 x,窗口宽度变了也不会点偏。
PAUSE_BTN_FROM_RIGHT = 366
PAUSE_BTN_Y = 81


def rows_fingerprint(shot):
    """数据行区域的一个指纹,用来判断列表还在不在动。"""
    buf = bytearray()
    for y in range(300, 1000, 7):
        for x in range(320, 1180, 11):
            buf += bytes(shot.rgb(x, y))
    return hashlib.md5(bytes(buf)).hexdigest()


def pause_sampling():
    """按头部「暂停」按钮把列表冻住,并**用结果核对**真的冻住了。

    为什么必须冻:列表每秒刷新一次(updateList 里 sortItems 会把行取出来重排),
    而「读一次路径 + 读一次 PID」要 8~10 秒,期间那一行早换人了 ——
    判据2/3 会永远卡在「期间换行了」,重试多少次都一样(实测)。

    按钮走 MainWindow::togglePause() → page->onSamplingPaused(true) →
    ProcessPage::onProcessSnapshot 直接 return,列表就不再动。

    核对方式不是看按钮文字变成「继续」,而是隔 2.5 秒抓两张图比数据行区域
    是否逐像素一致 —— 点偏了列表就还在动,一眼能看出来。
    """
    x = drive.Shot(hwnd).width - PAUSE_BTN_FROM_RIGHT
    click_at(hwnd, x, PAUSE_BTN_Y)
    time.sleep(1.2)
    a = rows_fingerprint(drive.Shot(hwnd))
    time.sleep(2.5)
    b = rows_fingerprint(drive.Shot(hwnd))
    ok = a == b
    print('   点头部「暂停」按钮 (x=%d,y=%d) -> 列表%s'
          % (x, PAUSE_BTN_Y, '已冻住' if ok else '**还在动**(没点中?)'))
    return ok


def glide_to(x, y, steps=10):
    """分小步把真实光标挪到 (x,y)。

    两个必须:**不能一步跳进去**(一步跳进窗口只会产生 Enter、不产生 MouseMove,
    Qt 认为"进入窗口"这件事已经由 Enter 表达了,窗口内收不到任何 move);
    **光标本来就在目标点附近时要先弹开**——否则 SetCursorPos 到同一个点不会产生
    任何移动消息,QMenu 就不会把那一项设成 currentAction,后面的点击等于点了个寂寞
    (实测:命中菜单窗=True 但动作没触发,剪贴板纹丝不动)。
    """
    pt = wt.POINT()
    user32.GetCursorPos(ctypes.byref(pt))
    if abs(pt.x - x) <= 4 and abs(pt.y - y) <= 4:
        user32.SetCursorPos(x + 60, y + 60)
        time.sleep(0.06)
    for i in range(1, steps + 1):
        user32.SetCursorPos(int(pt.x + (x - pt.x) * i / steps), int(pt.y + (y - pt.y) * i / steps))
        time.sleep(0.03)
    user32.SetCursorPos(x, y)
    time.sleep(0.05)


def real_click(x, y, button='left', hover=0.3):
    glide_to(x, y)
    time.sleep(hover)
    under = user32.WindowFromPoint(wt.POINT(x, y))
    user32.mouse_event(ME_F[button + 'down'], 0, 0, 0, 0)
    time.sleep(0.09)
    user32.mouse_event(ME_F[button + 'up'], 0, 0, 0, 0)
    return under


# ---------------------------------------------------------------- 菜单

def close_stray_popups():
    """菜单没点中时它会留在那儿,下一次右键只是把它关掉 —— 于是"菜单没弹出"。
    而且残留的菜单还占着 Qt 的 popupWidgets,后面的鼠标消息会被它截走。

    只发 WM_CANCELMODE 不够干净(实测菜单窗没了但 Qt 那边还认着),
    再补一发 Escape —— QMenu 自己处理 Escape 关菜单,这条路是干净的。
    """
    n = 0
    for h, _t, _c, _r in popups_of(proc.pid):
        user32.PostMessageW(h, WM_KEYDOWN, VK_ESCAPE, 0)
        user32.PostMessageW(h, WM_KEYUP, VK_ESCAPE, 0)
        time.sleep(0.15)
        user32.PostMessageW(h, WM_CANCELMODE, 0, 0)
        n += 1
    if n:
        time.sleep(0.5)
    return n


def dismiss_dialogs():
    """关掉意外弹出来的模态框,并把前台焦点还给主窗口。

    **模态框一旦出现就会抢走前台焦点**,之后所有合成右键都不再换来
    WM_CONTEXTMENU,菜单一次都弹不出来 —— 现象上看起来像"右键整个坏了",
    极难查(实测被这个坑了整整一轮:点错菜单项弹出「无法读取」,之后
    判据1 的 4 行 × 4 次重试全部报"菜单没弹出")。
    每次开菜单前先清一遍,测试就能自愈。
    """
    n = 0
    for h, title, cls, _r in top_windows(proc.pid):
        # tooltip 也是「可见的顶层窗」,类名是 Qt6112QWindowToolSaveBits(不含 Popup),
        # 标题为空 —— 不排掉的话每次开菜单都会把它当成模态框关一遍,白等一秒。
        if h == hwnd or 'Popup' in cls or 'Tool' in cls or not title:
            continue
        user32.PostMessageW(h, WM_KEYDOWN, VK_ESCAPE, 0)
        user32.PostMessageW(h, WM_KEYUP, VK_ESCAPE, 0)
        time.sleep(0.15)
        user32.PostMessageW(h, WM_CLOSE, 0, 0)
        n += 1
    if n:
        print('       (关掉 %d 个意外弹出的模态框 —— 它们会抢走焦点,让右键全失效)' % n)
        time.sleep(0.6)
        drive.force_foreground(hwnd)
        time.sleep(0.4)
    return n


def wait_popups(timeout=5.0, want=True):
    """轮询等菜单弹出/消失。

    别用固定 sleep:应用忙的时候(27 个进程在采样,还要应付我们的 PrintWindow)
    菜单要好几秒才出来,固定等 1.2s 会误判成"没弹出",然后重试时又把
    真正弹出来的那个菜单关掉 —— 越重试越乱。
    """
    end = time.time() + timeout
    while True:
        pops = popups_of(proc.pid)
        if bool(pops) == want:
            return pops
        if time.time() > end:
            return pops
        time.sleep(0.25)


def open_menu(cy, tries=4):
    """在图像 y=cy 处右键开菜单,返回 (菜单窗句柄, 该点下的窗口)。"""
    under = None
    for i in range(tries):
        dismiss_dialogs()
        park_cursor()
        ensure_on_primary(hwnd)
        close_stray_popups()
        under = right_click_at(hwnd, 500, cy)
        pops = wait_popups(5.0, want=True)
        if pops:
            return pops[0][0], under
        # 失败时把「合成右键为什么没换来菜单」的两个前提打出来。
        # DefWindowProc 在 WM_RBUTTONUP 时只在「没有窗口抓着鼠标」且「窗口是前台」
        # 的情况下才补发 WM_CONTEXTMENU,少任何一条菜单都不会出现。
        # 另外 _synth_click 会单独报「点下窗口不是主窗口」,三种原因就齐了。
        cap, fg = user32.GetCapture(), user32.GetForegroundWindow()
        print('       (菜单没弹出:GetCapture=%s 前台=%s%s) 第 %d/%d 次'
              % (cap if cap else 'NULL', hex(fg) if fg else 'NULL',
                 '' if fg == hwnd else ' ←不是被测窗口', i + 1, tries))
        if i < tries - 1:
            time.sleep(0.5)
    return None, under


def menu_items(ms):
    """把菜单截图切成「菜单项」横带,并**滤掉分隔线**。

    菜单是 10 项 + 3 条分隔线(见 ProcessPage::showContextMenu)。分隔线同样是
    「有亮像素的横带」,会被一起数进来 —— 一旦数进来,按下标点第 N 项就整体错位:
    点「复制 PID」(第 8 项)实际点到「设置 CPU 亲和性」,于是弹出模态框「无法读取」,
    焦点被抢走后**后面所有右键都换不来 WM_CONTEXTMENU**,菜单一次都弹不出来。
    这个错位会随菜单高度/位置变化而时有时无,非常难查(实测踩过)。

    分隔线只有几像素高,菜单项有 ~40 像素,按「明显矮于中位数」滤掉即可。
    """
    bands = drive.bands_in_column(ms, 8, ms.width - 8, threshold=70.0, min_rows=4, y_start=2)
    if len(bands) <= MENU_ITEM_COUNT:
        return bands
    heights = sorted(b - a + 1 for a, b in bands)
    med = heights[len(heights) // 2]
    return [b for b in bands if (b[1] - b[0] + 1) * 2 >= med]


def menu_shot(menu_hwnd):
    """菜单截图 + 切好的菜单项横带。返回 (截图, 横带列表)。

    先试 PrintWindow(让窗口自己画,和它落在哪块屏上无关),切不出 10 项再退回
    屏幕 BitBlt。窗口有时会被 Qt 摆到副屏(x 为负),而 BitBlt 是从主屏 DC 上抠的,
    负坐标下抠出来的内容不可信 —— 以前只用 BitBlt,于是同一台机器上时灵时不灵。
    """
    best = None
    for pw in (True, False):
        ms = drive.Shot(menu_hwnd, printwindow=pw)
        items = menu_items(ms)
        if len(items) == MENU_ITEM_COUNT:
            return ms, items
        if best is None:
            best = (ms, items)
    return best


def pick_menu_item(menu_hwnd, index):
    """用真实光标点菜单第 index 项。返回 (点到的窗口, 菜单截图, 项数)。

    点到的窗口必须就是菜单窗 —— 否则动作根本不会触发(常见是点在 Qt 的阴影窗上)。
    """
    ms, items = menu_shot(menu_hwnd)
    # 数目对不上就**绝不按下标点** —— 宁可整段重来。
    # 点错项的代价不只是这一次没中:它会弹出模态框把焦点抢走,后面整轮测试全废。
    if len(items) != MENU_ITEM_COUNT:
        return None, ms, items
    a, b = items[index]
    under = real_click(ms.left + ms.width // 2, ms.top + (a + b) // 2, 'left')
    wait_popups(2.0, want=False)          # 点完等菜单收干净,不然下一次右键只是把它关掉
    return under, ms, items


def menu_action(cy, index, tries=4):
    """右键开菜单 → 点第 index 项。整段按**结果**重试:中途任何一步不灵就重来。

    菜单这套交互在这台机器上时灵时不灵(窗口被挪到副屏 / 点到 Qt 阴影窗 /
    上一轮菜单没关干净 / tooltip 顶在光标下),所以别去断言中间环节,
    直接看最后有没有达到目的。
    """
    last = None
    for i in range(tries):
        menu, under = open_menu(cy)
        if not menu:
            last = '菜单没弹出(右键命中 %s)' % (hex(under) if under else 'None')
            continue
        got, _ms, items = pick_menu_item(menu, index)
        if got == menu:
            return True, menu, len(items)
        if len(items) != MENU_ITEM_COUNT:
            last = '菜单项识别到 %d 条(应为 %d),不敢按序号点' % (len(items), MENU_ITEM_COUNT)
        else:
            last = '没点中菜单窗(%s vs %s)' % (hex(got) if got else 'None', hex(menu))
        close_stray_popups()
        time.sleep(0.4)
    return False, last, 0


def copy_pid_at(cy):
    """右键 y=cy → 「复制 PID」,返回剪贴板里的 pid(失败返回 None)。"""
    set_clipboard_text('')
    okc, _info, _n = menu_action(cy, ITEM_COPY_PID)
    if not okc:
        return None
    time.sleep(0.5)
    return clipboard_pid()


def copy_path_at(cy):
    """右键 y=cy → 「复制路径」,返回剪贴板里的路径(失败返回 None)。"""
    set_clipboard_text('')
    okc, _info, _n = menu_action(cy, ITEM_COPY_PATH)
    if not okc:
        return None
    time.sleep(0.5)
    t = get_clipboard_text()
    return t or None


# ---------------------------------------------------------------- 主流程

def bail(msg):
    print(msg)
    if proc:
        subprocess.call(['cmd', '/c', 'taskkill', '/F', '/PID', str(proc.pid)],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sys.exit(1)


def close_window(hwnd):
    user32.PostMessageW(hwnd, 0x0010, 0, 0)              # WM_CLOSE


saved_cursor = wt.POINT()
user32.GetCursorPos(ctypes.byref(saved_cursor))
saved_clip = get_clipboard_text()

print('=== 启动 ===')
proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), creationflags=0x00000008)
time.sleep(5.0)
wins = drive.find_window(proc.pid)
if not wins:
    bail('!! 没找到主窗口')
hwnd = wins[0]
print('   窗口 %#x %r' % (hwnd, drive.window_title(hwnd)))

fg_ok = ensure_on_primary(hwnd)
time.sleep(0.5)
r = wt.RECT()
user32.GetWindowRect(hwnd, ctypes.byref(r))
# 窗口落在哪块屏不影响判据(坐标都是从当前矩形现算、截图走 PrintWindow),
# 但**拿不到前台焦点就是致命的** —— DefWindowProc 不会为非前台窗口补发 WM_CONTEXTMENU。
print('   摆位后窗口矩形 (%d,%d)-(%d,%d)  前台焦点=%s'
      % (r.left, r.top, r.right, r.bottom, '有' if fg_ok else '**没有**(右键会全部失效)'))

origin = wt.POINT(0, 0)
user32.ClientToScreen(hwnd, ctypes.byref(origin))
drive.scroll_nav_top(hwnd, origin.x + 100, origin.y + 200)

shot = drive.Shot(hwnd)
bands = drive.bands_in_column(shot, 20, 200, threshold=95.0, min_rows=8, y_start=130)
print('   图像 %dx%d  窗口左上 (%d,%d)  导航亮带 %d 条'
      % (shot.width, shot.height, shot.left, shot.top, len(bands)))
if len(bands) < 2:
    bail('!! 导航项识别不出来')

y0, y1 = bands[1]                      # 第 2 项 = 进程管理
nav_under = click_at(hwnd, 70, (y0 + y1) // 2)
print('   点导航第 2 项 -> 命中窗口 %#x (主窗口 %#x)' % (nav_under, hwnd))
time.sleep(3.0)
drive.force_foreground(hwnd)
time.sleep(0.6)

shot = drive.Shot(hwnd)
shot.save(os.path.join(OUT, '1-processes.png'))
rows = drive.bands_in_column(shot, 330, 700, threshold=70.0, min_rows=5, y_start=260)
print('   进程页已打开,名称列识别到 %d 条横带' % len(rows))
if len(rows) < 8:
    bail('!! 行识别太少')

# ---- 先点「PID」列头改成按 PID **降序**:默认按 CPU 降序,列表每秒重排,
#      同一个 y 对应的进程一直在变,后面的断言会被自己搞乱。
#
#      方向必须是降序,不能是升序:升序把 pid 最小的 SYSTEM 服务排在最前面,
#      而非提权进程**读不到这些服务的映像路径**(实测全表 245 个进程里 137 个读不到:
#      System/Registry/smss/csrss/winlogon/services 以及一大堆 svchost —— Python 和
#      程序用的是同一套 API,读不到就是读不到,这是权限模型不是 bug)。
#      可见区只有 27 行,升序时整屏都是"读得到 PID、读不到路径"的行,判据2/3 无从核对。
#      降序则把用户自己的进程(pid 大、刚启动的)顶到可见区,路径一定读得到。
head = rows[0]
cols = drive.bands_in_row(shot, head[0], head[1] + 1, 300, 1400, threshold=95.0, min_cols=4, gap=16)
print('   列头带 y %d..%d,识别到 %d 个列标题' % (head[0], head[1], len(cols)))
if len(cols) >= 2:
    cx = (cols[1][0] + cols[1][1]) // 2
    hy = (head[0] + head[1]) // 2
    click_at(hwnd, cx, hy)          # 第 1 次:升序
    time.sleep(0.9)
    click_at(hwnd, cx, hy)          # 第 2 次:降序
    print('   点两次第 2 个列标题(PID)改成降序 -> 客户区 x=%d' % cx)
    time.sleep(1.2)
else:
    print('   (列标题识别不到,不改排序 —— 后面靠剪贴板现读,行号只当大致位置)')

shot = drive.Shot(hwnd)
shot.save(os.path.join(OUT, '1b-sorted.png'))
rows = drive.bands_in_column(shot, 330, 700, threshold=70.0, min_rows=5, y_start=260)
data_rows = rows[1:] if len(rows) > 8 else rows     # 第一条大概率是列头
mid = len(data_rows) // 2
print('   排序后数据行 %d 条' % len(data_rows))

# 冻住列表。不冻的话「读一次路径 + 读一次 PID」这 8~10 秒里那一行早换人了,
# 判据2/3 会永远卡在「期间换行了」—— 这不是程序的问题,是测量方法的问题。
if not pause_sampling():
    fails.append('没能冻住列表(头部「暂停」按钮没点上),行会一直漂移,判据2/3 无法核对')

# ---- 判据 1:不同行 → 「复制 PID」→ 剪贴板必须是不同且非 0 的 pid
print()
print('   --- 判据1:右键「复制 PID」,不同行必须给出不同且非 0 的 PID ---')
seen = []          # [(cy, pid), ...]
for k in [mid, mid + 2, mid - 2, mid + 4, mid - 4, mid + 6, mid - 6, mid + 8]:
    if not (0 <= k < len(data_rows)):
        continue
    cy = (data_rows[k][0] + data_rows[k][1]) // 2
    pid = copy_pid_at(cy)
    print('     行[%2d] y=%4d -> 点「复制 PID」-> 剪贴板 %r' % (k, cy, pid))
    if pid is not None:
        seen.append((cy, pid))
    if len(seen) >= 5:
        break

nonzero = sorted({p for _cy, p in seen if p != 0})
if len(nonzero) < 2:
    fails.append('「复制 PID」只给出 %r —— 选中行的 PID 没取到,右键操作必然失效'
                 % sorted({p for _cy, p in seen}))
    print('   [!!] 只拿到 %r' % sorted({p for _cy, p in seen}))
else:
    print('   [OK] 拿到 %d 个互不相同的非 0 PID:%r' % (len(nonzero), nonzero))

# ---- 判据 2:程序自报的路径必须与 Python 独立查到的该 PID 路径一致
#
# 列表按 PID 升序,每秒还会因为进程启停挪行,所以「同一个 y 现在是谁」必须现问。
# 做法:同一行 复制 PID → 复制路径 → 再复制 PID,两次 PID 一致才算这一行没换人。
print()
print('   --- 判据2:程序自报的路径必须与 Python 独立查到的该 PID 路径一致 ---')
target = None
checked = 0
# 列表按 PID 降序 → 可见区最上面就是用户自己的进程(pid 大、刚启动的),路径一定读得到。
# 少数 pid 小到混进来的 SYSTEM 服务会读不到,跳过即可(那不是 bug,是权限模型)。
_attempts = 0
_unreadable = 0        # 路径连 Python 都读不到(系统/受保护进程,权限模型所致)
_appEmpty = 0          # 路径 Python 读得到、程序却报空 —— 这是**程序的 bug**,不是权限
for k in range(len(data_rows)):
    if _attempts >= 12:               # 每个候选要花 1~3 次菜单操作,给个上限免得跑飞
        break
    _attempts += 1
    if not (0 <= k < len(data_rows)):
        continue
    cy = (data_rows[k][0] + data_rows[k][1]) // 2
    pid = copy_pid_at(cy)
    if pid is None:
        continue
    mine = query_process_path(pid)
    if not mine:
        _unreadable += 1
        print('     行[%2d] y=%4d pid=%-6d -> Python 也查不到路径(系统/受保护进程),跳过'
              % (k, cy, pid))
        continue
    path = copy_path_at(cy)
    pid2 = copy_pid_at(cy)
    if pid2 != pid:
        print('     行[%2d] y=%4d pid=%-6d -> 期间换行了(第二次 pid=%s),跳过'
              % (k, cy, pid, pid2))
        continue
    same = bool(path) and os.path.normcase(path) == os.path.normcase(mine)
    if not path:
        _appEmpty += 1          # 程序报空、Python 却查得到 —— 就是路径缓存把失败钉住了
    print('     行[%2d] y=%4d pid=%-6d -> 「复制路径」%r  与 Python 查到的%s'
          % (k, cy, pid, path, '一致' if same else '不一致(期望 %r)' % mine))
    checked += 1
    if same:
        target = (cy, pid, path)
        break
    if checked >= 3:
        break

if target is None:
    fails.append('没能找到一行「程序自报路径 == Python 独立查到路径」的进程'
                 '(核对了 %d 行,其中 %d 行的路径连 Python 自己都读不到)' % (checked, _unreadable))
    print()
    print('!! 判据2 不成立,后面没法核对,先停')
    for f in fails:
        print('   -', f)
    set_clipboard_text(saved_clip)
    user32.SetCursorPos(saved_cursor.x, saved_cursor.y)
    subprocess.call(['cmd', '/c', 'taskkill', '/F', '/PID', str(proc.pid)],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sys.exit(1)

cy, tpid, tpath = target
want_folder = os.path.basename(os.path.dirname(tpath))
print('   [OK] 选定 pid=%d y=%4d 路径 %r(目录名 %r)' % (tpid, cy, tpath, want_folder))
if _appEmpty:
    # 程序自报路径为空、Python 却查得到 —— 不是权限问题,是路径缓存把查询失败钉住了
    fails.append('有 %d 行的路径 Python 读得到、程序却报空(路径缓存把失败缓存住了)' % _appEmpty)
    print('   [!!] 其中 %d 行 Python 读得到路径、程序却报空' % _appEmpty)

print()
print('   --- 判据3:右键「打开文件所在位置」必须新开资源管理器并落在该目录 ---')
before_explorer = explorer_windows()
opened = None
for attempt in range(3):
    # 右键会把光标下那一行选中,而列表在按 PID 升序的前提下仍可能因进程启停而挪行。
    # 所以先用「复制路径」问一次"这一行现在是谁",再去打开它的目录。
    set_clipboard_text('')
    okc, info, nitems = menu_action(cy, ITEM_COPY_PATH)
    if not okc:
        print('     第 %d 次:读路径失败 —— %s' % (attempt + 1, info))
        continue
    time.sleep(0.5)
    now = get_clipboard_text()
    if not now:
        print('     第 %d 次:这一行没有可执行文件路径,换一行' % (attempt + 1))
        continue

    okc, info, nitems = menu_action(cy, ITEM_REVEAL)
    print('     第 %d 次:右键 y=%4d(该行路径 %r);点第 %d 项「打开文件所在位置」-> %s'
          % (attempt + 1, cy, now, ITEM_REVEAL, '已点击' if okc else info))
    if not okc:
        continue

    time.sleep(3.0)
    after = explorer_windows()
    new_wins = {h: t for h, t in after.items() if h not in before_explorer}
    if not new_wins:
        print('        -> 没有新开资源管理器窗口')
        continue
    titles = list(new_wins.values())
    print('        -> 新开资源管理器:%r' % titles)
    folder = os.path.basename(os.path.dirname(now))
    if folder and any(folder in t for t in titles):
        opened = (titles, folder)
        break
    print('        -> 标题里没有目录名 %r(这期间列表换行了),重试' % folder)
    for h in new_wins:
        close_window(h)
    time.sleep(0.8)

if opened is None:
    fails.append('「打开文件所在位置」没能打开目标目录(试了 3 次)')
else:
    print('   [OK] 打开的目录名 %r 与右键那一行的路径一致' % opened[1])

# ---- 收尾
drive.Shot(hwnd).save(os.path.join(OUT, '4-after.png'))
print()
if fails:
    print('=== 结论:失败 ===')
    for f in fails:
        print('   -', f)
else:
    print('=== 结论:通过 ===')

set_clipboard_text(saved_clip)
user32.SetCursorPos(saved_cursor.x, saved_cursor.y)      # 别把用户的鼠标留在副屏
subprocess.call(['cmd', '/c', 'taskkill', '/F', '/PID', str(proc.pid)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print('已关掉测试实例')
sys.exit(1 if fails else 0)
