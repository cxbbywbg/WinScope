#!/usr/bin/env bash
#
# 把 dist/WinScope 那个绿色版目录打成一个**带向导的安装程序** dist/WinScope_setup.exe。
# 用户双击它会像普通软件一样:欢迎页 → 选安装路径 → 选快捷方式 → 装 → 完成,
# 并在「应用和功能」里登记一条可卸载的记录。
#
# 用法:
#   tools/package-setup.sh              # 需要 dist/WinScope 已存在,没有就先跑 package-release.sh
#   tools/package-setup.sh --release    # 先重新出绿色版,再打安装包
#   tools/package-setup.sh --per-user   # 出「免 UAC」版:装到 %LOCALAPPDATA%\Programs,
#                                       # 快捷方式只给当前用户,卸载项写 HKCU
#
# 环境变量:
#   NSIS_DIR    NSIS 免安装版所在目录(默认 .tools/nsis/nsis-3.10,没有会自动下载)
#   PAYLOAD     要打包的目录(默认 dist/WinScope)
#   OUT         产出的安装包(默认 dist/WinScope_setup.exe)
#
# 为什么用 NSIS 而不是 Inno Setup:
#   Inno Setup 只有安装程序版(装它要提权、要跑它自己的 installer);
#   NSIS 官方提供免安装 zip,解开直接有 makensis.exe,能在受限环境里跑。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

NSIS_VERSION="3.10"
NSIS_DIR="${NSIS_DIR:-$ROOT/.tools/nsis/nsis-$NSIS_VERSION}"
NSIS_URL="https://downloads.sourceforge.net/project/nsis/NSIS%203/$NSIS_VERSION/nsis-$NSIS_VERSION.zip"
PAYLOAD="${PAYLOAD:-$ROOT/dist/WinScope}"
APPICON="$ROOT/src/winres/WinScope.ico"
SRC_NSI="$ROOT/installer/WinScope.nsi"
WORK="$ROOT/build-setup"

DO_RELEASE=0
PERUSER=0
for arg in "$@"; do
    case "$arg" in
        --release)  DO_RELEASE=1 ;;
        --per-user) PERUSER=1 ;;
        *) echo "未知参数:$arg" >&2; exit 2 ;;
    esac
done

if [ "$PERUSER" = 1 ]; then
    OUT="${OUT:-$ROOT/dist/WinScope_setup-peruser.exe}"
    EXTRA_DEFINE='!define PERUSER 1'
else
    OUT="${OUT:-$ROOT/dist/WinScope_setup.exe}"
    EXTRA_DEFINE=''
fi

# OutFile 必须是绝对路径。makensis 会先把工作目录切到 .nsi 所在目录,
# 相对路径会被再解析一次(实测 build-setup/xxx.exe 变成 build-setup/build-setup/xxx.exe,
# 报 "Can't open output file")。
case "$OUT" in
    /*|[A-Za-z]:[\\/]*) ;;
    *) OUT="$ROOT/$OUT" ;;
esac

PY="${PY:-python}"

# ---------------------------------------------------------------- 绿色版
if [ "$DO_RELEASE" = 1 ] || [ ! -f "$PAYLOAD/WinScope.exe" ]; then
    echo "== 先出绿色版 =="
    "$ROOT/tools/package-release.sh"
fi
[ -f "$PAYLOAD/WinScope.exe" ] || { echo "找不到 $PAYLOAD/WinScope.exe" >&2; exit 1; }

# ---------------------------------------------------------------- NSIS
if [ ! -x "$NSIS_DIR/makensis.exe" ]; then
    echo "== 下载 NSIS $NSIS_VERSION(免安装版)=="
    mkdir -p "$(dirname "$NSIS_DIR")"
    TMP_ZIP="$ROOT/.tools/nsis-$NSIS_VERSION.zip"
    curl -L --fail -sS -o "$TMP_ZIP" "$NSIS_URL"
    "$PY" -c "
import sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
" "$TMP_ZIP" "$(dirname "$NSIS_DIR")"
    rm -f "$TMP_ZIP"
fi
MAKENSIS="$NSIS_DIR/makensis.exe"
[ -x "$MAKENSIS" ] || { echo "NSIS 没准备好:$MAKENSIS" >&2; exit 1; }

# ---------------------------------------------------------------- 生成脚本
# 两个坑都在这一步解决:
#
# 1. BOM。NSIS 3 靠 UTF-8 BOM 判断脚本是 Unicode;没有 BOM 它按系统 ANSI 代码页读,
#    .nsi 里所有中文都会静默变乱码(makensis 不报错,只是安装界面全花)。
#
# 2. 路径。makensis 的参数 /DPAYLOAD=... 在 Git Bash 里会被 MSYS 当成 POSIX 路径
#    改写掉,所以不传命令行宏,改成把绝对路径以 !define 的形式写在临时脚本开头。
#    原脚本用的是 !ifndef,这样正好被覆盖。
mkdir -p "$WORK"
# 向导里的横幅/页眉图,和程序同一套配色(默认那条 NSIS 蓝横幅太老气)
echo "== 生成向导位图 =="
"$PY" "$ROOT/tools/make-installer-art.py" "$APPICON" "$WORK/art"
BANNER="$WORK/art/welcome.bmp"
HEADER="$WORK/art/header.bmp"

"$PY" - "$SRC_NSI" "$WORK/WinScope.nsi" "$PAYLOAD" "$OUT" "$APPICON" "$EXTRA_DEFINE" \
       "$BANNER" "$HEADER" <<'PY'
import sys
src, dst, payload, out, icon, extra, banner, header = sys.argv[1:9]

def win(p):
    # NSIS 认反斜杠路径;正斜杠也吃,但统一一下省得踩坑
    return p.replace('/', '\\')

header_defs = ''
if banner and header:
    header_defs = ('!define BANNERBMP "%s"\n!define HEADERBMP "%s"\n'
                   % (win(banner), win(header)))

header = (
    '; —— 本文件由 tools/package-setup.sh 生成,不要直接改 ——\n'
    '; 真正要改的是 installer/WinScope.nsi\n'
    '!define PAYLOAD "%s"\n'
    '!define OUTFILE "%s"\n'
    '!define APPICON "%s"\n'
    '%s'
    '%s\n'
    '\n' % (win(payload), win(out), win(icon), header_defs, extra)
)
body = open(src, 'rb').read()
if body.startswith(b'\xef\xbb\xbf'):
    body = body[3:]
# 开头的注释块里就有中文,所以 BOM 必须写在最前面
open(dst, 'wb').write(b'\xef\xbb\xbf' + header.encode('utf-8') + body)
print('已生成 %s(UTF-8 with BOM)%s' % (dst, '  [per-user]' if extra else ''))
PY

# ---------------------------------------------------------------- 编译
echo
echo "== 编译安装包 =="
rm -f "$OUT"
# 两个参数都容易被 Git Bash 搞坏,所以:
#   · 开关用 -V2 而不是 /V2 —— MSYS 会把 /V2 当成 POSIX 路径改写成 ".../PortableGit/.../V2"
#   · 脚本路径先 cygpath -w 转成 D:\... ,否则 /d/... 会被 makensis 当成 /D 宏定义
# (-V2 = 只报警告和错误;不然 34MB 的固实压缩会刷一屏)
MSYS_NO_PATHCONV=1 "$MAKENSIS" -V2 "$(cygpath -w "$WORK/WinScope.nsi")"

[ -f "$OUT" ] || { echo "编译失败,没产出 $OUT" >&2; exit 1; }

# ---------------------------------------------------------------- 校验
echo
echo "== 产物 =="
printf '  %s\n  %s 字节\n' "$OUT" "$(stat -c%s "$OUT")"

# 必须是**单个**文件:安装包自带全部运行库,旁边不该再有别的依赖
SIDE="$(find "$(dirname "$OUT")" -maxdepth 1 -type f \
        \( -name '*.dll' -o -name '*.dat' -o -name '*.cab' -o -name '*.bin' \) \
        -newer "$OUT" -print 2>/dev/null || true)"
if [ -n "$SIDE" ]; then
    echo "  注意:同目录下还有这些可能是安装包副产物的文件:" >&2
    echo "$SIDE" >&2
fi
echo "  单文件自包含 ✓"
