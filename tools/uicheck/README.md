# tools/uicheck — 界面自动截图核对

改完界面后,用这几个脚本把 WinScope 的 8 个页面挨个截下来,肉眼过一遍。
纯 Python 标准库实现(ctypes 调 GDI + zlib 手写 PNG),不需要装 Pillow 之类的依赖。

## 为什么不用普通截屏

- **不抢鼠标**:切页面靠给窗口发 `WM_LBUTTONDOWN/UP`,不是 `SetCursorPos` + 模拟点击,
  所以不会把用户的鼠标挪走。
- **不怕被挡**:用 `PrintWindow(..., PW_RENDERFULLCONTENT)` 让窗口自己画到内存 DC,
  即使浏览器盖在最上面也能截到 WinScope 的内容,不用去抢前台焦点。
- **导航项自动定位**:不写死坐标。脚本先扫侧边栏里文字像素的「亮带」,
  按亮带中心算每个导航项的位置,改布局后不用改脚本。

## 用法

先启动 WinScope,拿到 PID:

```bash
cd build && ./WinScope.exe &
PID=$(tasklist //FI "IMAGENAME eq WinScope.exe" //FO CSV //NH | head -1 | cut -d',' -f2 | tr -d '"')
```

### 逐页截图

```bash
python tools/uicheck/drive.py $PID build/pages
```

会在 `build/pages/` 下生成 `01_dashboard.png` … `08_tools.png`。
加 `--scan` 只打印检测到的导航项位置,不点击、不截图(排查定位问题时用)。

### 放大某个区域

核对小字号文本时用。坐标是**相对窗口左上角**的(和 `drive.py` 输出的 PNG 一致)。

```bash
# 先切到第 6 个导航项(服务),再把 (780,150) 起 460x36 的区域放大 3 倍
python tools/uicheck/zoom.py $PID 780 150 460 36 3 build/zoom.png --click 5

# --click 是侧边栏第 N 项(0 起),--tab 是页签第 M 个(0 起)
# 下面这条:切到性能监控(第 3 项)、再点开 GPU 页签,放大一块区域
python tools/uicheck/zoom.py $PID 1440 430 1060 90 2 build/gpu.png --click 2 --tab 2
```

### 某个页面的所有页签一起截

性能监控页的 CPU / 内存 / GPU / 磁盘 在 QTabWidget 里,`drive.py` 进不去。

```bash
python tools/uicheck/tabshot.py $PID build/tabs --nav 3 --names cpu,mem,gpu,disk
```

### 量一条横条填了多少

**不要靠肉眼看缩略图估比例** —— 缩略图会把两三个像素的字糊在一起,很容易读错
(把 `19.9 GB` 看成 `25.9 GB` 这种事发生过)。这个脚本直接把窗口那一行的颜色
按「连续同色段」列出来,填充色占了多长一目了然。

```bash
# 量性能监控 → 内存页,图像 y=950 那一行,x∈[380,2560)
python tools/uicheck/measure.py $PID 950 380 2560 --click 2 --tab 1
```

输出:

```
y=950  x∈[380,2560)  窗口 2578x1558  (只列 ≥16px 的色段)
  x   491..  959  rgb( 77,141,247)    469px
  x   962.. 1429  rgb( 77,141,247)    468px
  x  1432.. 1697  rgb( 75,137,241)    266px
  x  1700.. 1900  rgb( 35, 40, 50)    201px
  x  1903.. 2371  rgb( 35, 40, 50)    469px
  (另有 87px 碎片,主要是文字和抗锯齿)
```

轨道是 `491..2371`(1881px),填充到 1697 → 64%,和 `12.4 / 19.9 GB` 对得上。

### 只截一张

```bash
python tools/uicheck/capture.py $PID build/shot.png
```

### 托盘图标和窗口图标

小窗是 `Qt::Tool`,不占任务栏按钮,进出全靠右下角的托盘图标 —— 这几件事用
`traycheck.py` 查:

```bash
python tools/uicheck/traycheck.py $PID status        # 扩展样式 + 托盘登记状态
python tools/uicheck/traycheck.py $PID icons         # 主窗口 / 小窗的图标逐像素比对
python tools/uicheck/traycheck.py $PID menu out.png  # 模拟右键,截菜单
python tools/uicheck/traycheck.py $PID click         # 模拟左键单击托盘图标
```

`status` 会告诉你小窗有没有 `WS_EX_TOOLWINDOW`(没有的话任务栏就会多一个按钮),
以及托盘登记条目是不是可见。

**不用真鼠标**是重点 —— 会抢用户的鼠标。Qt 的托盘图标用的是
`NOTIFYICON_VERSION_4`,外壳把事件投给 `Qt*TrayIconMessageWindowClass` 这个隐藏窗口:

| | |
|---|---|
| 消息号 | `WM_APP + 101` |
| `lParam` | `MAKELONG(事件, 图标ID)`,事件是 `NIN_SELECT`(单击)/ `WM_CONTEXTMENU`(右键)|
| `wParam` | `MAKELONG(x, y)` 屏幕坐标,只有右键用得到 |

**托盘右键菜单是 Qt 自绘的弹出窗**(类名 `Qt*QWindowPopupDropShadowSaveBits`),
不是原生 `#32768`,别按原生菜单的类名去找。

## 踩过的坑

- **`PrintWindow` 抓窗口有时只画出上半截**。窗口在屏幕外时(比如最大化后
  `left=-2569`)尤其明显:同一个窗口 `drive.py` 存出来的 PNG 是完整的,
  紧接着现抓的却只有上半部分。**要量像素就解码已经存下来的 PNG,别依赖现抓。**
- **DIB 里是 BGRA**:`px[i]=B, px[i+1]=G, px[i+2]=R`。按 RGB 取通道会静默取错,
  找蓝色按钮时会一无所获。
- **找按钮别用「这一行所有匹配像素的 min/max」**。按钮旁边的灰蓝色文字也会命中,
  会把左边缘一路带到屏幕左边。要按**最长连续同色段**找实心色块。
- **PNG 行数据要逐像素交错**。一开始按 R 平面 / G 平面 / B 平面拼,行字节数正好对得上
  (`width*3`),所以 PNG 不报错,但画面会变成三份横向压缩的副本 —— 很像「窗口被平铺了」,
  很容易误判成布局 bug。
- **又宽又扁的图(如整条任务栏 1920x60)有些看图流程处理不了**,裁成接近方形的块再放大。
- **`PrintWindow` 的 DC 原点是窗口矩形左上角**,不是客户区原点。算点击坐标时要注意换算:
  `客户区坐标 = 图像坐标 + (窗口左上角 - 客户区原点)`。
- **前台焦点抢不过浏览器**。`SetForegroundWindow` 对非前台进程会被静默忽略,
  得先 `AttachThreadInput` 挂到当前前台线程上再调。不过既然用了 `PrintWindow`,
  这一步只是为了点击能被正常派发。
- **侧边栏导航会被用户滚动**,点击前先滚回顶部,否则点到的是别的项。
- **看缩略图会看错**。截图被缩放后小字号数字会糊在一起,判断"横条填了多少""某个字是
  2 还是 3"这类事情一定要 `zoom.py` 放大或 `measure.py` 量,别猜。
- **`zoom.py` 的缩放倍数只吃整数**,传 `1.6` 直接 `ValueError`。
- **`removeWidget` 不会隐藏控件**。Qt 里把控件从布局里摘掉后它还是父对象的子控件,
  会停在原来的位置继续画,必须先 `hide()` 再 `deleteLater()`。
- **hover / 拖动这类"鼠标移动"语义,`PostMessage` 合成消息驱动不了**。点击可以
  (`WM_LBUTTONDOWN/UP` 能进 `mousePressEvent`),但 `WM_MOUSEMOVE` 喂不进 Qt 的
  `mouseMoveEvent` —— 得用 `SetCursorPos` 移真实光标。
- **光标一步跳进窗口,Qt 只发 `Enter` 不发 `MouseMove`**(它认为"导致进入的那一次移动"
  已经由 Enter 表达了)。要**分十几小步**挪进去,窗口内才会产生 move。
- **`SetWindowPos` 别忘了去掉 `SWP_NOMOVE`**,否则窗口根本没动,光标落在别的窗口上,
  结论全错。测之前用 `WindowFromPoint` 确认光标真的在目标窗口上。
- **`PrintWindow` 有时只画出左半截**(不止是"上半截")。拿这种图当基准比像素会得出
  相反的结论 —— 抓完先校验右侧有内容,不合格就重试。
- 上面这些连同"拖动只能验位移比例、不能验绝对位移"的完整说明在技能
  `qt-gui-visual-verify` 里。
