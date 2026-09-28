; WinScope 安装程序脚本(NSIS 3)
;
; 产出:单个 WinScope_setup.exe —— 自带全部运行库,安装时让用户选路径,
; 建开始菜单/桌面快捷方式,写「应用和功能」里的卸载项。
;
; 编译:tools/package-setup.sh
;
; 编码:本文件存成**不带 BOM 的 UTF-8** 就行,不用自己加 BOM —— 脚本会先剥掉可能存在的
;   BOM,再在生成的临时脚本开头重新写一个(顺便把 PAYLOAD / OUTFILE 这些绝对路径宏以
;   !define 的形式注入进去)。BOM 统一由脚本一处生成,是因为 NSIS 3 靠 UTF-8 BOM 判断
;   脚本是 Unicode;没有 BOM 时它按系统 ANSI 代码页读,下面所有中文都会静默变乱码 ——
;   makensis 不报任何错,只是安装界面全花。交给一处生成也免得编辑器把它悄悄吃掉。
;   但**别改成 GBK**:脚本是按 UTF-8 重新编码的。
;
; 可用命令行覆盖的宏:
;   /DPAYLOAD=<目录>  要打包的绿色版目录,默认 ..\dist\WinScope
;   /DOUTFILE=<路径>  产出的安装包,默认 ..\dist\WinScope_setup.exe
;   /DAPPICON=<ico>   安装包自身的图标,默认 ..\src\winres\WinScope.ico

Unicode true

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

!ifndef PAYLOAD
  !define PAYLOAD "..\dist\WinScope"
!endif
!ifndef OUTFILE
  !define OUTFILE "..\dist\WinScope_setup.exe"
!endif
!ifndef APPICON
  !define APPICON "..\src\winres\WinScope.ico"
!endif

!define APPNAME    "WinScope"
!define APPVERSION "1.0.0"
!define APPEXE     "WinScope.exe"
!define PUBLISHER  "WinScope"
!define UNINSTKEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\WinScope"

; ------------------------------------------------------------------ 安装范围
; 默认:装到 Program Files、快捷方式建给所有用户、卸载项写 HKLM,装的时候弹 UAC。
; 这是大家最熟悉的那个流程。
;
; 加 -DPERUSER=1 编译(见 tools/package-setup.sh --per-user)会换成:
;   装到 %LOCALAPPDATA%\Programs\WinScope、快捷方式只给当前用户、卸载项写 HKCU,
;   全程不弹 UAC。给没有管理员权限的机器用。
;
; 这两条路走的是同一份安装/卸载代码,只是「装到哪、写哪个注册表根、快捷方式给谁」不同。
!ifdef PERUSER
  !define REQLEVEL  user
  !define REGROOT   HKCU
  !define SHELLCTX  current
  !define DEFDIR    "$LOCALAPPDATA\Programs\${APPNAME}"
!else
  !define REQLEVEL  admin
  !define REGROOT   HKLM
  !define SHELLCTX  all
  !define DEFDIR    "$PROGRAMFILES64\${APPNAME}"
!endif

Name "${APPNAME} ${APPVERSION}"
OutFile "${OUTFILE}"

; 默认装这里,用户可以在「选择安装位置」那页随便改
InstallDir "${DEFDIR}"
RequestExecutionLevel ${REQLEVEL}

; 34MB 的运行库,用 LZMA 固实压缩,字典给大一点
SetCompressor /SOLID lzma
SetCompressorDictSize 64

; 安装包自己的版本信息(右键属性 → 详细信息)
VIProductVersion "1.0.0.0"
VIAddVersionKey /LANG=2052 "ProductName"     "${APPNAME}"
VIAddVersionKey /LANG=2052 "FileDescription" "${APPNAME} 安装程序"
VIAddVersionKey /LANG=2052 "FileVersion"     "${APPVERSION}"
VIAddVersionKey /LANG=2052 "ProductVersion"  "${APPVERSION}"
VIAddVersionKey /LANG=2052 "CompanyName"     "${PUBLISHER}"
VIAddVersionKey /LANG=2052 "LegalCopyright"  "Copyright (C) 2026 ${PUBLISHER}"

; 安装包和卸载程序都用自己的图标,不然任务栏/资源管理器里是个默认方块
!define MUI_ICON   "${APPICON}"
!define MUI_UNICON "${APPICON}"
!define MUI_ABORTWARNING

; 向导里的横幅。默认那条蓝色老式横幅和 WinScope 的深色界面完全不搭,
; 所以 tools/make-installer-art.py 会按 Theme.h 的调色板现画两张(从 appIcon 取图形)。
; 没传路径时就不定义,退回 NSIS 默认外观 —— 保证脚本单独也能编译。
!ifdef BANNERBMP
  !define MUI_WELCOMEFINISHPAGE_BITMAP "${BANNERBMP}"
!endif
!ifdef HEADERBMP
  !define MUI_HEADERIMAGE
  !define MUI_HEADERIMAGE_RIGHT
  !define MUI_HEADERIMAGE_BITMAP "${HEADERBMP}"
!endif

; ------------------------------------------------------------------ 页面
!define MUI_WELCOMEPAGE_TITLE "${APPNAME} ${APPVERSION} 安装向导"
!define MUI_WELCOMEPAGE_TEXT "这是 ${APPNAME} —— 一个 Windows 系统监控与管理工具,$\r$\n对标任务管理器并做了增强。$\r$\n$\r$\n接下来可以选择安装位置,以及要不要建快捷方式。$\r$\n$\r$\n安装包已包含全部运行库,装完即可使用,不需要另外安装 Qt 或任何组件。"

!define MUI_DIRECTORYPAGE_TEXT_TOP "请选择 ${APPNAME} 的安装位置。可以直接改上面的路径,或点「浏览」挑一个目录。"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "立即运行 ${APPNAME}"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchApp
!define MUI_FINISHPAGE_TEXT "${APPNAME} 已安装完成。$\r$\n$\r$\n开始菜单和桌面上的快捷方式可以直接启动它;需要结束其它用户的进程、启停服务或改启动项时,右键快捷方式选「以管理员身份运行」。$\r$\n$\r$\n详细说明见安装目录下的 使用说明.txt。"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"

; ------------------------------------------------------------------ 初始化
Function .onInit
    ; 本程序是 64 位构建,32 位 Windows 上跑不起来,别让它装到一半才报错
    ${IfNot} ${RunningX64}
        MessageBox MB_OK|MB_ICONSTOP "WinScope 是 64 位程序,无法在 32 位 Windows 上运行。"
        Abort
    ${EndIf}

    ; 我们的注册表项写在 64 位视图里(x86 安装程序默认会落到 Wow6432Node)
    SetRegView 64
    SetShellVarContext ${SHELLCTX}

    ; 之前装过就沿用原来的路径,别每次都跳回默认目录。
    ;
    ; 但要先看用户是不是在命令行上给了 /D=<路径> —— 静默安装靠它指定位置,
    ; 而 NSIS 是在 .onInit **之前**把 /D 写进 $INSTDIR 的,这里无条件覆盖会把
    ; /D 吃掉(实测:传了 /D 还是装到默认目录)。
    ; 判据:没给 /D 时 $INSTDIR 就是 InstallDir 那个默认值。
    ${If} $INSTDIR == "${DEFDIR}"
        ReadRegStr $0 ${REGROOT} "Software\${APPNAME}" "InstallDir"
        ${If} $0 != ""
            StrCpy $INSTDIR $0
        ${EndIf}
    ${EndIf}
FunctionEnd

Function un.onInit
    SetRegView 64
    SetShellVarContext ${SHELLCTX}
FunctionEnd

; 结束正在运行的实例。不这么做的话 WinScope.exe 被占用,新文件覆盖不上去。
; 先不带 /F 发一次 WM_CLOSE 让它自己退(它会存好窗口位置),
; 等一秒还在,再强杀。
!macro KillRunningApp
    nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM ${APPEXE}'
    Pop $0
    Sleep 1000
    nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /IM ${APPEXE}'
    Pop $0
    Sleep 300
!macroend

; ------------------------------------------------------------------ 安装
Section "WinScope 主程序" SecMain
    SectionIn RO

    ; 快捷方式建到哪一档(所有用户 / 当前用户),跟安装范围保持一致
    SetShellVarContext ${SHELLCTX}

    !insertmacro KillRunningApp

    SetOutPath "$INSTDIR"
    ; /r 递归:platforms\qwindows.dll 这类子目录结构要原样保留
    File /r "${PAYLOAD}\*"

    ; 记下安装位置,下次装的时候默认还在这儿
    WriteRegStr ${REGROOT} "Software\${APPNAME}" "InstallDir" "$INSTDIR"
    WriteRegStr ${REGROOT} "Software\${APPNAME}" "Version" "${APPVERSION}"

    ; 「应用和功能」/「程序和功能」里那一行
    WriteRegStr   ${REGROOT} "${UNINSTKEY}" "DisplayName"     "${APPNAME} — Windows 系统监控与管理"
    WriteRegStr   ${REGROOT} "${UNINSTKEY}" "DisplayVersion"  "${APPVERSION}"
    WriteRegStr   ${REGROOT} "${UNINSTKEY}" "Publisher"       "${PUBLISHER}"
    WriteRegStr   ${REGROOT} "${UNINSTKEY}" "DisplayIcon"     "$INSTDIR\${APPEXE},0"
    WriteRegStr   ${REGROOT} "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr   ${REGROOT} "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninst.exe"'
    WriteRegStr   ${REGROOT} "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\uninst.exe" /S'
    WriteRegDWORD ${REGROOT} "${UNINSTKEY}" "NoModify" 1
    WriteRegDWORD ${REGROOT} "${UNINSTKEY}" "NoRepair" 1

    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    WriteRegDWORD ${REGROOT} "${UNINSTKEY}" "EstimatedSize" $0

    WriteUninstaller "$INSTDIR\uninst.exe"
SectionEnd

Section "创建桌面快捷方式" SecDesktop
    CreateShortCut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${APPEXE}" "" "$INSTDIR\${APPEXE}" 0
SectionEnd

Section "创建开始菜单快捷方式" SecStartMenu
    CreateDirectory "$SMPROGRAMS\${APPNAME}"
    CreateShortCut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${APPEXE}" "" "$INSTDIR\${APPEXE}" 0
    CreateShortCut "$SMPROGRAMS\${APPNAME}\使用说明.lnk" "$INSTDIR\使用说明.txt"
    CreateShortCut "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk" "$INSTDIR\uninst.exe"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecMain}      "WinScope 主程序及运行库,必需。"
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop}   "在桌面上放一个 WinScope 快捷方式。"
    !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "在开始菜单里建一个 WinScope 文件夹,含启动、说明和卸载。"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; 装完后从完成页点「立即运行」走这里。
; 用 explorer 转一手,让 WinScope 以普通权限启动 —— 安装程序自己是提权的,
; 直接 Exec 会把程序也带成管理员,那样它就不再走「普通用户模式」那套提示了。
Function LaunchApp
    Exec '"$WINDIR\explorer.exe" "$INSTDIR\${APPEXE}"'
FunctionEnd

; ------------------------------------------------------------------ 卸载
Section "Uninstall"
    SetShellVarContext ${SHELLCTX}

    !insertmacro KillRunningApp

    Delete "$INSTDIR\${APPEXE}"
    Delete "$INSTDIR\使用说明.txt"
    Delete "$INSTDIR\uninst.exe"
    Delete "$INSTDIR\platforms\qwindows.dll"
    RMDir "$INSTDIR\platforms"
    ; 兜底:万一以后往包里加了别的东西,这里也能清干净
    RMDir /r "$INSTDIR"

    Delete "$DESKTOP\${APPNAME}.lnk"
    Delete "$SMPROGRAMS\${APPNAME}\*.lnk"
    RMDir  "$SMPROGRAMS\${APPNAME}"

    DeleteRegKey ${REGROOT} "${UNINSTKEY}"
    ; 只删安装器自己记的那两个值。
    ; 注意别在这里 DeleteRegKey "Software\${APPNAME}" —— 那是递归删,
    ; 会把程序自己的界面设置(HKCU\Software\WinScope\WinScope\...)一起带走,
    ; 下面那句「要不要删设置」的询问就形同虚设了。
    DeleteRegValue ${REGROOT} "Software\${APPNAME}" "InstallDir"
    DeleteRegValue ${REGROOT} "Software\${APPNAME}" "Version"
    DeleteRegKey /ifempty ${REGROOT} "Software\${APPNAME}"

    ; 界面设置(小窗参数、窗口位置)是程序自己写在 HKCU\Software\WinScope 下的。
    ; 删掉是干净,但用户的偏好也没了 —— 所以问一句,静默卸载时默认保留。
    MessageBox MB_YESNO|MB_ICONQUESTION \
        "是否同时删除 WinScope 的界面设置?$\r$\n(小窗显示哪些参数、窗口位置等。删除后重装会回到默认。)" \
        /SD IDNO IDNO keep_settings
        DeleteRegKey HKCU "Software\WinScope"
    keep_settings:
SectionEnd
