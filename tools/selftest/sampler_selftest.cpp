// WinScope 采样层自检。
//
// 不开界面,直接把每个采样器跑几轮并打印结果。
// 用途:改完 Win32 层后确认读数没有变味(尤其是 PDH 计数器名、ETW 事件解析、
// NtQuerySystemInformation 的结构体布局这几处最容易悄悄坏掉的地方)。
//
// 构建:cmake -DWINSCOPE_BUILD_SELFTEST=ON .. 然后编 sampler_selftest

#include "core/CpuSampler.h"
#include "core/DiskSampler.h"
#include "core/GpuSampler.h"
#include "core/MemorySampler.h"
#include "core/NetConnectionSampler.h"
#include "core/NetProcessMonitor.h"
#include "core/NetSampler.h"
#include "core/ProcessSampler.h"
#include "core/ServiceManager.h"
#include "core/StartupManager.h"
#include "core/SystemInfo.h"
#include "core/ThermalProvider.h"
#include "core/Win32Utils.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTextStream>
#include <QThread>

using namespace ws;

namespace {

QTextStream out(stdout);

void section(const QString &title)
{
    out << Qt::endl;
    out << QStringLiteral("================================================================") << Qt::endl;
    out << QStringLiteral("  ") << title << Qt::endl;
    out << QStringLiteral("================================================================") << Qt::endl;
}

void row(const QString &key, const QString &value)
{
    out << QStringLiteral("  %1").arg(key, -22) << value << Qt::endl;
}

QString cpuTemperatureSourceName(CpuTemperatureSource source)
{
    switch (source) {
    case CpuTemperatureSource::AcpiThermalZone:
        return QStringLiteral("ACPI 热区");
    case CpuTemperatureSource::IntegratedGpu:
        return QStringLiteral("核显传感器(本机无 ACPI 热区)");
    case CpuTemperatureSource::None:
        break;
    }
    return QStringLiteral("不可用");
}

void testCpu(ThermalProvider &thermal)
{
    section(QStringLiteral("CPU"));
    CpuSampler cpu;
    cpu.setThermalProvider(&thermal);
    cpu.loadStaticInfo();
    row(QStringLiteral("型号"), cpu.modelName());

    CpuInfo info;
    for (int i = 0; i < 3; ++i) {
        QThread::msleep(600);
        info = cpu.sample();
    }
    row(QStringLiteral("占用率"), QStringLiteral("%1%").arg(info.usage, 0, 'f', 1));
    row(QStringLiteral("物理/逻辑核心"), QStringLiteral("%1 / %2").arg(info.physicalCores).arg(info.logicalCores));
    row(QStringLiteral("当前/标称主频"), QStringLiteral("%1 / %2 MHz").arg(info.currentMHz, 0, 'f', 0).arg(info.baseMHz, 0, 'f', 0));
    row(QStringLiteral("最大睿频(WMI)"),
        info.maxClockMHz > 0 ? QStringLiteral("%1 MHz").arg(info.maxClockMHz, 0, 'f', 0) : QStringLiteral("不可用"));
    row(QStringLiteral("用户/内核/空闲"),
        QStringLiteral("%1 / %2 / %3").arg(formatDuration(qint64(info.userSeconds)),
                                           formatDuration(qint64(info.kernelSeconds)),
                                           formatDuration(qint64(info.idleSeconds))));
    row(QStringLiteral("温度"), info.temperatureC < 0 ? QStringLiteral("不可用") : QStringLiteral("%1 °C").arg(info.temperatureC, 0, 'f', 1));
    row(QStringLiteral("温度来源"), cpuTemperatureSourceName(info.temperatureSource));
    row(QStringLiteral("功耗"), info.powerW < 0 ? QStringLiteral("不可用") : QStringLiteral("%1 W").arg(info.powerW, 0, 'f', 1));
    row(QStringLiteral("运行时间"), formatDuration(qint64(info.uptimeSeconds)));

    QStringList cores;
    for (const auto &c : info.cores)
        cores << QStringLiteral("%1").arg(c.usage, 0, 'f', 0);
    row(QStringLiteral("各核心占用"), cores.join(QStringLiteral(" ")));
}

void testMemory()
{
    section(QStringLiteral("内存"));
    MemorySampler mem;
    const MemoryInfo m = mem.sample();
    row(QStringLiteral("总量"), formatBytes(m.totalBytes));
    row(QStringLiteral("已用"), QStringLiteral("%1 (%2%)").arg(formatBytes(m.usedBytes)).arg(m.usagePercent, 0, 'f', 1));
    row(QStringLiteral("可用"), formatBytes(m.availableBytes));
    row(QStringLiteral("缓存"), formatBytes(m.cachedBytes));
    row(QStringLiteral("提交 / 上限"), QStringLiteral("%1 / %2").arg(formatBytes(m.committedBytes), formatBytes(m.commitLimitBytes)));
    row(QStringLiteral("分页池 / 非分页池"), QStringLiteral("%1 / %2").arg(formatBytes(m.pagedPoolBytes), formatBytes(m.nonPagedPoolBytes)));
    row(QStringLiteral("页面文件"), QStringLiteral("%1 / %2").arg(formatBytes(m.pageFileUsedBytes), formatBytes(m.pageFileTotalBytes)));
}

void testGpu(ThermalProvider &thermal)
{
    section(QStringLiteral("GPU"));
    GpuSampler gpu;
    gpu.setThermalProvider(&thermal);
    gpu.loadStaticInfo();
    QVector<GpuInfo> infos;
    for (int i = 0; i < 3; ++i) {
        QThread::msleep(600);
        infos = gpu.sampleAll();
    }
    row(QStringLiteral("适配器数"), QString::number(infos.size()));
    for (const GpuInfo &info : infos) {
        row(QStringLiteral("GPU %1").arg(info.index),
            QStringLiteral("%1 (%2)").arg(info.name.isEmpty() ? QStringLiteral("未检测到") : info.name,
                                          info.integrated ? QStringLiteral("核显") : QStringLiteral("独显")));
        row(QStringLiteral("  驱动版本"), info.driverVersion.isEmpty() ? QStringLiteral("—") : info.driverVersion);
        row(QStringLiteral("  驱动日期"), info.driverDate.isEmpty() ? QStringLiteral("—") : info.driverDate);
        row(QStringLiteral("  综合占用"), QStringLiteral("%1%").arg(info.usagePercent, 0, 'f', 1));
        row(QStringLiteral("  温度"),
            info.temperatureC < 0 ? QStringLiteral("不可用") : QStringLiteral("%1 °C").arg(info.temperatureC, 0, 'f', 1));
        row(QStringLiteral("  显存(已用/总量)"),
            QStringLiteral("%1 / %2").arg(formatBytes(info.dedicatedBytes), formatBytes(info.totalVramBytes)));
        row(QStringLiteral("  共享内存"), formatBytes(info.sharedBytes));
        for (const auto &e : info.engines)
            row(QStringLiteral("    · %1").arg(e.name), QStringLiteral("%1%").arg(e.percent, 0, 'f', 1));
    }
    row(QStringLiteral("主显卡"), gpu.primary().name.isEmpty() ? QStringLiteral("未检测到") : gpu.primary().name);
    row(QStringLiteral("进程级显存条目"), QString::number(gpu.perProcessDedicated().size()));
}

void testDisk()
{
    section(QStringLiteral("磁盘"));
    DiskSampler disk;
    disk.loadStaticInfo();
    DiskInfo info;
    for (int i = 0; i < 3; ++i) {
        QThread::msleep(600);
        info = disk.sample();
    }
    row(QStringLiteral("活动时间"), QStringLiteral("%1%").arg(info.activePercent, 0, 'f', 1));
    row(QStringLiteral("读 / 写"), QStringLiteral("%1 / %2").arg(formatBytesPerSec(info.readBytesPerSec), formatBytesPerSec(info.writeBytesPerSec)));
    row(QStringLiteral("IOPS"), QStringLiteral("%1").arg(info.iops, 0, 'f', 1));
    row(QStringLiteral("响应时间"), QStringLiteral("%1 ms").arg(info.responseMs, 0, 'f', 2));
    row(QStringLiteral("队列长度"), QStringLiteral("%1").arg(info.queueLength, 0, 'f', 2));
    for (const auto &v : info.volumes) {
        row(QStringLiteral("  分区 %1").arg(v.letter),
            QStringLiteral("%1 可用 / %2 共 (%3) %4")
                .arg(formatBytes(v.freeBytes), formatBytes(v.totalBytes), v.fileSystem, v.devicePath));
    }
}

void testNetwork()
{
    section(QStringLiteral("网络适配器"));
    NetSampler net;
    net.sample();
    QThread::msleep(1000);
    const NetInfo info = net.sample();

    row(QStringLiteral("合计 下行 / 上行"),
        QStringLiteral("%1 / %2").arg(formatBytesPerSec(info.rxBytesPerSec), formatBytesPerSec(info.txBytesPerSec)));

    for (const auto &a : info.adapters) {
        out << QStringLiteral("  [%1] %2").arg(a.name, a.description) << Qt::endl;
        out << QStringLiteral("      类型=%1 已连接=%2 链路=%3")
                   .arg(int(a.kind))
                   .arg(a.connected ? QStringLiteral("是") : QStringLiteral("否"))
                   .arg(formatBytesPerSec(double(a.linkSpeedBps) / 8.0))
            << Qt::endl;
        if (!a.ipv4.isEmpty())
            out << QStringLiteral("      IPv4: %1").arg(a.ipv4.join(QStringLiteral(", "))) << Qt::endl;
        if (!a.mac.isEmpty())
            out << QStringLiteral("      MAC : %1").arg(a.mac) << Qt::endl;
        out << QStringLiteral("      速率: ↓%1 ↑%2")
                   .arg(formatBytesPerSec(a.rxBytesPerSec), formatBytesPerSec(a.txBytesPerSec))
            << Qt::endl;
    }
}

void testConnections()
{
    section(QStringLiteral("TCP / UDP 连接"));
    NetConnectionSampler conn;
    const auto list = conn.sample();
    row(QStringLiteral("TCP 条数"), QString::number(conn.tcpCount()));
    row(QStringLiteral("UDP 条数"), QString::number(conn.udpCount()));

    int shown = 0;
    for (const auto &c : list) {
        if (c.state != QLatin1String("ESTABLISHED"))
            continue;
        out << QStringLiteral("  %1  %2:%3 -> %4:%5  [%6]")
                   .arg(c.protocol, c.localAddress)
                   .arg(c.localPort)
                   .arg(c.remoteAddress)
                   .arg(c.remotePort)
                   .arg(c.processName)
            << Qt::endl;
        if (++shown >= 12)
            break;
    }
    if (shown == 0)
        out << QStringLiteral("  (没有已建立的连接)") << Qt::endl;
}

void testNetProcessMonitor()
{
    section(QStringLiteral("按进程网络流量(ETW)"));
    NetProcessMonitor monitor;
    if (!monitor.start()) {
        row(QStringLiteral("状态"), QStringLiteral("启动失败:") + monitor.lastError());
        out << QStringLiteral("  提示:该功能需要以管理员身份运行") << Qt::endl;
        return;
    }

    out << QStringLiteral("  会话已启动,采集 3 秒...") << Qt::endl;
    QThread::msleep(3000);
    const auto snapshot = monitor.takeSnapshot();
    monitor.stop();

    row(QStringLiteral("有流量的进程数"), QString::number(snapshot.size()));

    QVector<QPair<quint32, NetTraffic>> sorted;
    for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it)
        sorted.push_back({ it.key(), it.value() });
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
        return (a.second.rxBytesPerSec + a.second.txBytesPerSec) > (b.second.rxBytesPerSec + b.second.txBytesPerSec);
    });

    for (int i = 0; i < qMin(10, sorted.size()); ++i) {
        const auto &entry = sorted.at(i);
        out << QStringLiteral("  PID %1  ↓%2 ↑%3  累计 ↓%4 ↑%5")
                   .arg(entry.first, 6)
                   .arg(formatBytesPerSec(entry.second.rxBytesPerSec), -12)
                   .arg(formatBytesPerSec(entry.second.txBytesPerSec), -12)
                   .arg(formatBytes(entry.second.rxBytesTotal), -10)
                   .arg(formatBytes(entry.second.txBytesTotal))
            << Qt::endl;
    }
}

void testProcesses()
{
    section(QStringLiteral("进程"));
    ProcessSampler proc;
    proc.loadStaticInfo();

    ProcessExtras extras;
    ProcessSnapshot snap;
    for (int i = 0; i < 3; ++i) {
        QThread::msleep(700);
        snap = proc.sample(extras);
    }

    row(QStringLiteral("进程数"), QString::number(snap.totalProcesses));
    row(QStringLiteral("线程数"), formatCount(snap.totalThreads));
    row(QStringLiteral("句柄数"), formatCount(snap.totalHandles));

    QVector<ProcessInfo> sorted = snap.processes;
    std::sort(sorted.begin(), sorted.end(),
              [](const ProcessInfo &a, const ProcessInfo &b) { return a.cpuPercent > b.cpuPercent; });

    out << Qt::endl;
    out << QStringLiteral("  按 CPU 排序前 12:") << Qt::endl;
    out << QStringLiteral("  %1 %2 %3 %4 %5")
               .arg(QStringLiteral("PID"), 8)
               .arg(QStringLiteral("名称"), -28)
               .arg(QStringLiteral("CPU%"), 8)
               .arg(QStringLiteral("内存"), 12)
               .arg(QStringLiteral("路径"))
        << Qt::endl;
    for (int i = 0; i < qMin(12, sorted.size()); ++i) {
        const auto &p = sorted.at(i);
        out << QStringLiteral("  %1 %2 %3 %4 %5")
                   .arg(QString::number(p.pid), 8)
                   .arg(p.name.left(28), -28)
                   .arg(QString::number(p.cpuPercent, 'f', 2), 8)
                   .arg(formatBytes(p.workingSetBytes), 12)
                   .arg(p.path)
            << Qt::endl;
    }

    // 抽查一个进程的详情,确认路径/用户/命令行都能取到
    for (const auto &p : snap.processes) {
        if (p.name.compare(QStringLiteral("explorer.exe"), Qt::CaseInsensitive) == 0) {
            const ProcessInfo detail = proc.describe(p.pid);
            out << Qt::endl;
            out << QStringLiteral("  explorer.exe 详情:") << Qt::endl;
            row(QStringLiteral("  路径"), detail.path);
            row(QStringLiteral("  用户"), detail.user);
            row(QStringLiteral("  架构"), detail.architecture);
            row(QStringLiteral("  命令行"), detail.commandLine);
            row(QStringLiteral("  线程 / 句柄"), QStringLiteral("%1 / %2").arg(detail.threadCount).arg(detail.handleCount));
            row(QStringLiteral("  启动时间"), formatDateTime(detail.startTimeMs));
            break;
        }
    }
}

void testServices()
{
    section(QStringLiteral("服务"));
    QElapsedTimer timer;
    timer.start();
    const auto services = ServiceManager::enumerate();
    row(QStringLiteral("服务总数"), QString::number(services.size()));
    row(QStringLiteral("枚举耗时"), QStringLiteral("%1 ms").arg(timer.elapsed()));

    int running = 0;
    for (const auto &s : services) {
        if (s.state == QStringLiteral("正在运行"))
            ++running;
    }
    row(QStringLiteral("正在运行"), QString::number(running));

    for (int i = 0; i < qMin(8, services.size()); ++i) {
        const auto &s = services[i];
        out << QStringLiteral("  %1 | %2 | %3 | %4")
                   .arg(s.name, -32)
                   .arg(s.state, -10)
                   .arg(s.startType, -12)
                   .arg(s.displayName)
            << Qt::endl;
    }
}

void testStartup()
{
    section(QStringLiteral("启动项"));
    QElapsedTimer timer;
    timer.start();
    const auto items = StartupManager::enumerate();
    row(QStringLiteral("启动项总数"), QString::number(items.size()));
    row(QStringLiteral("枚举耗时"), QStringLiteral("%1 ms").arg(timer.elapsed()));

    for (const auto &item : items) {
        out << QStringLiteral("  [%1] %2 | %3 | %4")
                   .arg(item.sourceLabel, -8)
                   .arg(item.enabled ? QStringLiteral("启用") : QStringLiteral("禁用"), -4)
                   .arg(item.name, -30)
                   .arg(item.command.left(80))
            << Qt::endl;
    }
}

void testSystemInfo()
{
    section(QStringLiteral("系统信息"));
    QElapsedTimer timer;
    timer.start();
    const SystemInfoData data = SystemInfo::collect();
    row(QStringLiteral("采集耗时"), QStringLiteral("%1 ms").arg(timer.elapsed()));

    const QVector<QPair<QString, QVector<KeyValue>>> groups = {
        { QStringLiteral("操作系统"), data.os },      { QStringLiteral("CPU"), data.cpu },
        { QStringLiteral("GPU"), data.gpu },          { QStringLiteral("内存"), data.memory },
        { QStringLiteral("主板/BIOS"), data.motherboard }, { QStringLiteral("存储"), data.storage },
        { QStringLiteral("网络"), data.network },
    };

    for (const auto &group : groups) {
        out << Qt::endl << QStringLiteral("  --- %1 ---").arg(group.first) << Qt::endl;
        for (const auto &kv : group.second)
            row(QStringLiteral("  ") + kv.key, kv.value);
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    out << QStringLiteral("WinScope 采样层自检") << Qt::endl;
    row(QStringLiteral("管理员权限"), isProcessElevated() ? QStringLiteral("是") : QStringLiteral("否"));

    // 拿到别的用户进程的路径/命令行需要这个特权
    const bool priv = enableDebugPrivilege();
    row(QStringLiteral("SeDebugPrivilege"), priv ? QStringLiteral("已启用") : QStringLiteral("未启用"));

    // 温度层:厂商 SDK 全部动态加载,加载失败只是温度不可用,不影响其它项
    ThermalProvider thermal;
    const bool thermalOk = thermal.load();
    row(QStringLiteral("温度来源"), thermal.backendSummary());
    row(QStringLiteral("温度层可用"), thermalOk ? QStringLiteral("是") : QStringLiteral("否"));

    QElapsedTimer total;
    total.start();

    testCpu(thermal);
    testMemory();
    testGpu(thermal);
    testDisk();
    testNetwork();
    testConnections();
    testNetProcessMonitor();
    testProcesses();
    testServices();
    testStartup();
    testSystemInfo();

    out << Qt::endl;
    out << QStringLiteral("全部完成,总耗时 %1 ms").arg(total.elapsed()) << Qt::endl;
    out.flush();
    return 0;
}
