cmake_minimum_required(VERSION 3.16)

project(WinScope VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# 关掉图形界面只编采样层,方便在没有显示器/CI 的环境里验证 Win32 读数
option(WINSCOPE_BUILD_GUI "构建 WinScope 图形界面主程序" ON)
option(WINSCOPE_BUILD_SELFTEST "构建采样层自检工具(tools/selftest)" OFF)
# 只有改动了 Icons.cpp 里的图标几何才需要开,平时不用编
option(WINSCOPE_BUILD_ICONTOOL "构建 .ico 生成工具(tools/make-icon)" OFF)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Network)

# ------------------------------------------------------------------ 采样层
# 图形界面与自检工具共用这一份,避免两边列的文件对不上
set(WINSCOPE_CORE_SOURCES
    src/core/Win32Utils.cpp
    src/core/WinsockInit.cpp
    src/core/PdhQuery.cpp
    src/core/WmiQuery.cpp
    src/core/CpuSampler.cpp
    src/core/MemorySampler.cpp
    src/core/GpuSampler.cpp
    src/core/ThermalProvider.cpp
    src/core/DiskSampler.cpp
    src/core/NetSampler.cpp
    src/core/NetConnectionSampler.cpp
    src/core/NetProcessMonitor.cpp
    src/core/ProcessSampler.cpp
    src/core/ProcessController.cpp
    src/core/ServiceManager.cpp
    src/core/StartupManager.cpp
    src/core/SystemInfo.cpp
    src/core/Metrics.cpp
    src/core/SystemSampler.cpp
)

set(WINSCOPE_CORE_HEADERS
    src/core/Types.h
    src/core/Win32Utils.h
    src/core/WinsockInit.h
    src/core/PdhQuery.h
    src/core/WmiQuery.h
    src/core/CpuSampler.h
    src/core/MemorySampler.h
    src/core/GpuSampler.h
    src/core/ThermalProvider.h
    src/core/DiskSampler.h
    src/core/NetSampler.h
    src/core/NetConnectionSampler.h
    src/core/NetProcessMonitor.h
    src/core/ProcessSampler.h
    src/core/ProcessController.h
    src/core/ServiceManager.h
    src/core/StartupManager.h
    src/core/SystemInfo.h
    src/core/Metrics.h
    src/core/SystemSampler.h
)

# Windows 原生依赖:
#   psapi      进程内存/模块
#   pdh        性能计数器(GPU 引擎占用、磁盘速率、CPU 功耗)
#   iphlpapi   网卡与 TCP/UDP 连接表
#   wbemuuid   WMI(硬件信息、温度、命令行)
#   dxgi       GPU 适配器与显存
#   powrprof   CPU 实时频率
#   taskschd   计划任务 COM 接口(启动项管理)
#   ws2_32     InetNtop / WSAAddressToString
#   ntdll      NtQuerySystemInformation
set(WINSCOPE_WIN_LIBS
    psapi pdh iphlpapi wbemuuid ole32 oleaut32 uuid
    dxgi powrprof taskschd advapi32 shell32 user32 version netapi32
    ws2_32 ntdll
)

# ------------------------------------------------------------------ 图形界面
if (WINSCOPE_BUILD_GUI)
    # PE 资源:资源管理器里的 exe 图标、右键「属性 → 详细信息」都要靠它。
    # 界面图标是 QPainter 现画的,shell 读不到 —— 没有这份资源,文件夹里就没图标。
    # .ico 由 tools/make-icon 从 Icons.cpp 的同一份几何生成,见该目录注释。
    set(WINSCOPE_WIN_RESOURCES
        src/winres/WinScope.rc
    )

    set(WINSCOPE_GUI_SOURCES
        src/main.cpp

        src/app/Theme.cpp
        src/app/Icons.cpp
        src/app/MainWindow.cpp

        src/ui/widgets/Card.cpp
        src/ui/widgets/RingGauge.cpp
        src/ui/widgets/LineChart.cpp
        src/ui/widgets/StatRow.cpp
        src/ui/widgets/Badge.cpp
        src/ui/widgets/FlowLayout.cpp
        src/ui/widgets/BarRow.cpp
        src/ui/widgets/SegmentBar.cpp
        src/ui/widgets/MiniWindow.cpp

        src/ui/pages/DashboardPage.cpp
        src/ui/pages/ProcessPage.cpp
        src/ui/pages/PerformancePage.cpp
        src/ui/pages/NetworkPage.cpp
        src/ui/pages/StartupPage.cpp
        src/ui/pages/ServicePage.cpp
        src/ui/pages/SystemPage.cpp
        src/ui/pages/ToolsPage.cpp
    )

    set(WINSCOPE_GUI_HEADERS
        src/app/Theme.h
        src/app/MainWindow.h
        src/app/Icons.h

        src/ui/pages/PageBase.h

        src/ui/widgets/Card.h
        src/ui/widgets/RingGauge.h
        src/ui/widgets/LineChart.h
        src/ui/widgets/StatRow.h
        src/ui/widgets/Badge.h
        src/ui/widgets/FlowLayout.h
        src/ui/widgets/BarRow.h
        src/ui/widgets/SegmentBar.h
        src/ui/widgets/MiniWindow.h

        src/ui/pages/DashboardPage.h
        src/ui/pages/ProcessPage.h
        src/ui/pages/PerformancePage.h
        src/ui/pages/NetworkPage.h
        src/ui/pages/StartupPage.h
        src/ui/pages/ServicePage.h
        src/ui/pages/SystemPage.h
        src/ui/pages/ToolsPage.h
    )

    add_executable(WinScope WIN32
        ${WINSCOPE_GUI_SOURCES}
        ${WINSCOPE_GUI_HEADERS}
        ${WINSCOPE_CORE_SOURCES}
        ${WINSCOPE_CORE_HEADERS}
        ${WINSCOPE_WIN_RESOURCES}
    )

    target_include_directories(WinScope PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/winres   # windres 靠它找到 .rc 里引用的 .ico
    )

    target_link_libraries(WinScope PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Network
        ${WINSCOPE_WIN_LIBS}
    )

    if (MSVC)
        # 源码是 UTF-8,不加这个参数会把中文字面量当成 ANSI
        target_compile_options(WinScope PRIVATE /utf-8)
    endif()

    # 编译 .rc 要显式打开 RC 语言(project() 只声明了 CXX)
    enable_language(RC)
    if (NOT MSVC)
        # .rc 是 UTF-8 存的,windres 默认按系统 ANSI 代码页读,不指定会出乱码
        string(APPEND CMAKE_RC_FLAGS " --codepage=65001")
    endif()

    install(TARGETS WinScope RUNTIME DESTINATION bin)
endif()

# ------------------------------------------------------------------ 自检工具
if (WINSCOPE_BUILD_SELFTEST)
    # 不开界面,把各采样器跑几轮打印结果,用来确认 Win32 层读数正确
    add_executable(sampler_selftest
        tools/selftest/sampler_selftest.cpp
        ${WINSCOPE_CORE_SOURCES}
        ${WINSCOPE_CORE_HEADERS}
    )
    target_include_directories(sampler_selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(sampler_selftest PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        ${WINSCOPE_WIN_LIBS}
    )
    if (MSVC)
        target_compile_options(sampler_selftest PRIVATE /utf-8)
    endif()
endif()

# ------------------------------------------------------------------ 图标生成
# 把 appIcon() 那份图形导出成真正的 .ico,供 WinScope.rc 嵌进 PE。
# 单独一个开关,因为它只在图标几何变了的时候才需要重跑一次。
if (WINSCOPE_BUILD_ICONTOOL)
    add_executable(make_icon
        tools/make-icon/make_icon.cpp
        src/app/Icons.cpp
        src/app/Icons.h
    )
    target_include_directories(make_icon PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(make_icon PRIVATE Qt${QT_VERSION_MAJOR}::Gui)
    if (MSVC)
        target_compile_options(make_icon PRIVATE /utf-8)
    endif()
endif()
