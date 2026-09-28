#!/usr/bin/env bash
#
# 把 WinScope 打成可以外发的绿色版:目标机器不需要装 Qt、不需要装 MinGW,
# 解压后双击 WinScope.exe 就能跑。
#
# 用法:
#   tools/package-release.sh              # 编译 + 打包 + 自检
#   tools/package-release.sh --no-build   # 跳过编译,直接用现成的 build-release/WinScope.exe
#   tools/package-release.sh --zip        # 顺便打成 dist/WinScope-<版本>-win64.zip
#
# 为什么不用 windeployqt:
#   本机 Qt 6.11.2 mingw 版的 windeployqt 一启动就报
#     Unable to query qtpaths: Error running binary qtpaths: pipe:
#   它内部用 QProcess 起 qtpaths,而本机起子进程会失败(和 windeployqt 坏掉同一个根因)。
#   所以这里按导入表手动收集运行库,并在最后用导入表做一遍完整性自检。
#
set -euo pipefail

QT_DIR="${QT_DIR:-E:/download/QT/6.11.2/mingw_64}"
MINGW_BIN="${MINGW_BIN:-E:/download/QT/Tools/mingw1310_64/bin}"
BUILD_DIR="${BUILD_DIR:-build-release}"
OUT_DIR="${OUT_DIR:-dist/WinScope}"
OBJDUMP="${OBJDUMP:-$MINGW_BIN/objdump.exe}"
SYS32="${SYS32:-/c/Windows/System32}"

DO_BUILD=1
DO_ZIP=0
for arg in "$@"; do
    case "$arg" in
        --no-build) DO_BUILD=0 ;;
        --zip)      DO_ZIP=1 ;;
        *) echo "未知参数:$arg" >&2; exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# ---------------------------------------------------------------- 运行库清单
# 主程序导入表里出现的 Qt 模块。
#
# Qt6Network 虽然在 CMakeLists 里 link 了,但源码里没有任何 QNetwork* / QHostAddress
# 之类的引用,链接器就没生成对应的导入项 —— objdump -p 的导入表里查不到它。
# 所以不拷,省 2MB。
QT_DLLS=(Qt6Core.dll Qt6Gui.dll Qt6Widgets.dll)

# MinGW 运行库。Qt6Core/Gui 的导入表里要 libwinpthread,Q字头几个都要带上
MINGW_DLLS=(libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)

# 平台插件:没有它 Qt GUI 程序直接起不来
PLATFORM_PLUGIN="platforms/qwindows.dll"

# 不带的插件及原因(改代码后要重新核对这几条):
#   styles/qmodernwindowsstyle.dll  —— 程序显式 setStyle(Fusion),用不到 Windows 风格插件
#   imageformats/*                  —— 全程序没有从文件加载图片,图标都是 QPainter 现画
#   tls/ networkinformation/        —— 没用到 QtNetwork
#   iconengines/qsvgicon.dll        —— 没有 SVG 图标

# ---------------------------------------------------------------- 编译
if [ "$DO_BUILD" = 1 ]; then
    echo "== 编译 Release =="
    if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
        E:/download/QT/Tools/CMake_64/bin/cmake.exe -S . -B "$BUILD_DIR" -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_COMPILER="$MINGW_BIN/g++.exe" \
            -DCMAKE_MAKE_PROGRAM=E:/download/QT/Tools/Ninja/ninja.exe \
            -DCMAKE_PREFIX_PATH="$QT_DIR" \
            -DWINSCOPE_BUILD_GUI=ON -DWINSCOPE_BUILD_SELFTEST=OFF
    fi
    E:/download/QT/Tools/CMake_64/bin/cmake.exe --build "$BUILD_DIR"
fi

if [ ! -f "$BUILD_DIR/WinScope.exe" ]; then
    echo "找不到 $BUILD_DIR/WinScope.exe,先去掉 --no-build 编译一次" >&2
    exit 1
fi

# ---------------------------------------------------------------- 组装
echo
echo "== 组装 $OUT_DIR =="
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/platforms"

cp "$BUILD_DIR/WinScope.exe" "$OUT_DIR/"
for d in "${QT_DLLS[@]}";    do cp "$QT_DIR/bin/$d"      "$OUT_DIR/"; done
for d in "${MINGW_DLLS[@]}"; do cp "$MINGW_BIN/$d"       "$OUT_DIR/"; done
cp "$QT_DIR/plugins/$PLATFORM_PLUGIN" "$OUT_DIR/platforms/"

# ---------------------------------------------------------------- 完整性自检
# 逐个 PE 读导入表:凡是 Windows 自己提供的都放过,其余必须在包里找得到。
# 这一步是防"在我机器上能跑"——开发机上 Qt 的插件/库能通过编译进去的前缀路径兜底,
# 换台没装 Qt 的机器就起不来了。
echo
echo "== 导入表完整性自检 =="
missing=0
is_system_dll() {
    case "$1" in
        api-ms-win-*|ext-ms-*) return 0 ;;   # Windows API set,虚拟的
    esac
    [ -f "$SYS32/$1" ]
}
for pe in "$OUT_DIR/WinScope.exe" "$OUT_DIR"/*.dll "$OUT_DIR"/platforms/*.dll; do
    [ -f "$pe" ] || continue
    while read -r dll; do
        [ -n "$dll" ] || continue
        is_system_dll "$dll" && continue
        if [ ! -f "$OUT_DIR/$dll" ]; then
            echo "  ✗ 缺 $dll  (被 $(basename "$pe") 依赖)"
            missing=1
        fi
    done < <("$OBJDUMP" -p "$pe" 2>/dev/null | sed -n 's/.*DLL Name: //p' | sort -u)
done
if [ "$missing" = 1 ]; then
    echo "自检不通过:上面列出的 DLL 没打进包里" >&2
    exit 1
fi
echo "  所有非系统依赖都在包内 ✓"

# 顺带确认没有把构建中间件或探针程序混进来
for bad in "$OUT_DIR"/*.obj "$OUT_DIR"/*.lib "$OUT_DIR"/*.a "$OUT_DIR"/*test.exe; do
    [ -e "$bad" ] || continue
    echo "  ✗ 包里有不该外发的东西:$(basename "$bad")" >&2
    exit 1
done

# ---------------------------------------------------------------- 使用说明
cat > "$OUT_DIR/使用说明.txt" <<'TXT'
WinScope —— Windows 系统监控与管理工具

【怎么运行】
双击 WinScope.exe 即可,不需要装 Qt 或任何运行库 —— 需要的东西都在这个文件夹里。
整个文件夹可以随便拷到别的电脑(U盘、网盘都行),建议整个文件夹一起拷,不要只拷 exe。
如果你拿到的是 WinScope_setup.exe,双击它走安装向导,安装位置可以自己选。

【关于管理员权限】
普通权限就能看全部监控数据。
下面这几项需要「以管理员身份运行」才可用,未提权时界面上会明确标注不可用,不会显示成空白:
  · 结束其它用户的进程
  · 启动 / 停止 / 重启系统服务
  · 启用 / 禁用开机启动项
  · 每进程网络流量统计
右键 WinScope.exe →「以管理员身份运行」即可。

【桌面小窗】
主界面右上角点「小窗模式」进入。进入后:
  · 主界面会隐藏,任务栏上不会有按钮(这是有意的,和微信那类常驻程序一样)
  · 右下角通知区域会出现 WinScope 图标
  · 左键单击图标 → 回主界面;右键 → 菜单(显示主界面 / 选择小窗参数 / 退出)
小窗要显示哪些参数由「选择小窗参数」里勾选,勾选后程序会只采集这几项数据,
后台开销明显低于完整模式。

【关于温度的说明】
CPU 温度的读取顺序是:主板 ACPI 热区 → 核显温度传感器 → 留空。
有些笔记本的固件没有实现 ACPI 热区,这时会退回用核显传感器(和 CPU 同一颗芯片)。
如果两者都拿不到,温度一栏会显示为空 —— 这是刻意的,不显示不可靠的数字。
独立显卡的温度绝不会被拿来冒充 CPU 温度。

【数据来源】
CPU / 内存 / 磁盘 / 网络:Windows 性能计数器(PDH)与系统 API
进程 / 服务 / 启动项:Windows 原生接口
硬件信息:WMI
显卡:DXGI,温度走厂商 SDK(AMD ADL / NVIDIA NVML),没有则留空
全部数据只在本机读取和显示,程序不联网、不上传任何信息。

【卸载】
绿色版:删掉整个文件夹即可。
安装版:开始菜单 → WinScope → 卸载 WinScope,或在「设置 → 应用」里卸载。
程序只在注册表 HKCU\Software\WinScope 下存了一点界面设置(小窗参数、窗口位置),
卸载时会问要不要一并删掉。
TXT

echo
echo "== 包内容 =="
( cd "$OUT_DIR" && find . -type f | sort | while read -r f; do
    printf "  %8s  %s\n" "$(stat -c%s "$f")" "${f#./}"
done )
echo "  合计 $(du -sh "$OUT_DIR" | cut -f1)"

# ---------------------------------------------------------------- 打包
if [ "$DO_ZIP" = 1 ]; then
    ZIP="dist/WinScope-$(date +%Y%m%d)-win64.zip"
    rm -f "$ZIP"
    echo
    echo "== 打包 $ZIP =="
    # Git Bash 里通常没有 zip 命令,用 Python 标准库打 ——
    # 别调 PowerShell:从 bash 里起 powershell 会被安全策略拦掉。
    # 压缩包里保留 WinScope/ 这一层目录,解压出来就是一个完整文件夹
    python -c "
import os, sys, zipfile
out, root = sys.argv[1], sys.argv[2]
parent = os.path.dirname(os.path.abspath(root))
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            full = os.path.join(dirpath, f)
            z.write(full, os.path.relpath(full, parent))
print('  已打包', os.path.getsize(out), '字节')
" "$ZIP" "$OUT_DIR" || { echo "打包失败" >&2; exit 1; }
fi

echo
echo "完成。目录:$OUT_DIR"
