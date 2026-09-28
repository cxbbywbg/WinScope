#include "core/Metrics.h"

#include "core/Win32Utils.h"

#include <QtGlobal>

namespace ws {

namespace {

// 温度统一按 100°C 折算横条长度。没有"满量程"这回事,只是给个直观的长度
constexpr double kTemperatureFullScale = 100.0;
// 功耗的参考刻度。同样是"给横条一个长度",不是任何硬件的真实上限
constexpr double kPowerFullScaleW = 65.0;

LoadLevel temperatureLevel(double celsius, double warnAt, double criticalAt)
{
    if (celsius >= criticalAt)
        return LoadLevel::Critical;
    if (celsius >= warnAt)
        return LoadLevel::Warning;
    return LoadLevel::Normal;
}

MetricValue readCpuUsage(const SystemSnapshot &s)
{
    MetricValue v;
    v.text = formatPercent(s.cpu.usage, 1);
    v.ratio = qBound(0.0, s.cpu.usage / 100.0, 1.0);
    v.level = levelFor(s.cpu.usage);
    return v;
}

MetricValue readCpuFrequency(const SystemSnapshot &s)
{
    MetricValue v;
    const double mhz = s.cpu.currentMHz;
    if (mhz <= 0.0) {
        v.text = QStringLiteral("—");
        return v;
    }
    // 小窗地方小,GHz 更短也更好读
    v.text = mhz >= 1000.0 ? QStringLiteral("%1 GHz").arg(mhz / 1000.0, 0, 'f', 2)
                           : QStringLiteral("%1 MHz").arg(mhz, 0, 'f', 0);

    // 主频的横条按"标称上限"折算。注意不少 AMD 笔记本上 Windows 报的标称值
    // 就是基准频率(实测 4800H 报 2900,真实睿频 4200),所以超过标称时只能封顶,
    // 顺便换成橙色提示"正在睿频",具体数字看文字
    const double rated = qMax(qMax(s.cpu.maxClockMHz, s.cpu.maxMHz), s.cpu.baseMHz);
    if (rated > 0.0) {
        v.ratio = qBound(0.0, mhz / rated, 1.0);
        if (mhz > rated * 1.02)
            v.level = LoadLevel::Warning;
    }
    return v;
}

MetricValue readCpuTemperature(const SystemSnapshot &s)
{
    MetricValue v;
    if (s.cpu.temperatureC < 0.0) {
        // 读不到就留空。CPU 温度是三级回退(ACPI 热区 -> 核显传感器 -> 空),
        // 空说明这台机器两条路都没有,不该编一个数出来
        return v;
    }
    v.text = QStringLiteral("%1 °C").arg(s.cpu.temperatureC, 0, 'f', 1);
    v.ratio = qBound(0.0, s.cpu.temperatureC / kTemperatureFullScale, 1.0);
    v.level = temperatureLevel(s.cpu.temperatureC, 75.0, 90.0);
    return v;
}

MetricValue readCpuPower(const SystemSnapshot &s)
{
    MetricValue v;
    if (s.cpu.powerW < 0.0)
        return v;
    v.text = QStringLiteral("%1 W").arg(s.cpu.powerW, 0, 'f', 1);
    v.ratio = qBound(0.0, s.cpu.powerW / kPowerFullScaleW, 1.0);
    return v;
}

MetricValue readMemoryUsage(const SystemSnapshot &s)
{
    MetricValue v;
    v.text = formatPercent(s.memory.usagePercent, 1);
    v.ratio = qBound(0.0, s.memory.usagePercent / 100.0, 1.0);
    v.level = levelFor(s.memory.usagePercent);
    return v;
}

MetricValue readGpuUsage(const SystemSnapshot &s)
{
    MetricValue v;
    if (!s.gpu.present) {
        v.text = QStringLiteral("—");
        return v;
    }
    v.text = formatPercent(s.gpu.usagePercent, 0);
    v.ratio = qBound(0.0, s.gpu.usagePercent / 100.0, 1.0);
    v.level = levelFor(s.gpu.usagePercent);
    return v;
}

MetricValue readGpuTemperature(const SystemSnapshot &s)
{
    MetricValue v;
    if (!s.gpu.present || s.gpu.temperatureC < 0.0)
        return v;
    v.text = QStringLiteral("%1 °C").arg(s.gpu.temperatureC, 0, 'f', 1);
    v.ratio = qBound(0.0, s.gpu.temperatureC / kTemperatureFullScale, 1.0);
    // 显卡的耐受温度比 CPU 高一些,阈值跟着放宽
    v.level = temperatureLevel(s.gpu.temperatureC, 80.0, 90.0);
    return v;
}

MetricValue readNetworkThroughput(const SystemSnapshot &s)
{
    MetricValue v;
    const double rx = s.net.rxBytesPerSec;
    const double tx = s.net.txBytesPerSec;
    v.text = QStringLiteral("↓%1 ↑%2").arg(formatBytesPerSec(rx), formatBytesPerSec(tx));
    // 刻意不给横条。吞吐量确实有个天然满量程(链路速率),但家用网络平时连
    // 千分之一都跑不到,按它折算出来永远是一颗小点,画了反而像是"刻度坏了";
    // 按峰值折算又得在界面里藏一份状态,不值得
    v.ratio = -1.0;
    return v;
}

MetricValue readDiskActivity(const SystemSnapshot &s)
{
    MetricValue v;
    v.text = formatPercent(s.disk.activePercent, 0);
    v.ratio = qBound(0.0, s.disk.activePercent / 100.0, 1.0);
    v.level = levelFor(s.disk.activePercent);
    return v;
}

MetricValue readUptime(const SystemSnapshot &s)
{
    MetricValue v;
    // 小窗地方小,formatDuration 那种"3 天 4 小时 12 分 5 秒"太长了
    const qint64 total = qint64(s.cpu.uptimeSeconds);
    const qint64 days = total / 86400;
    const qint64 hours = (total % 86400) / 3600;
    const qint64 minutes = (total % 3600) / 60;
    if (days > 0)
        v.text = QStringLiteral("%1 天 %2 小时").arg(days).arg(hours);
    else if (hours > 0)
        v.text = QStringLiteral("%1 小时 %2 分").arg(hours).arg(minutes);
    else
        v.text = QStringLiteral("%1 分").arg(minutes);
    return v;
}

} // namespace

const QVector<MetricDef> &metricDefs()
{
    // 只建一次。这个顺序就是选择对话框里从上到下的顺序
    static const QVector<MetricDef> kDefs = {
        { MetricId::CpuUsage, QStringLiteral("cpuUsage"), QStringLiteral("CPU 占用"),
          QStringLiteral("整机 CPU 占用率"), SampleScope::Cpu, &readCpuUsage },

        { MetricId::CpuFrequency, QStringLiteral("cpuFrequency"), QStringLiteral("CPU 频率"),
          QStringLiteral("各核心平均的实时主频"), SampleScope::Cpu, &readCpuFrequency },

        { MetricId::CpuTemperature, QStringLiteral("cpuTemperature"), QStringLiteral("CPU 温度"),
          QStringLiteral("优先取 CPU 传感器;读不到时退到核显传感器,再读不到就留空"),
          SampleScope::Cpu, &readCpuTemperature },

        { MetricId::CpuPower, QStringLiteral("cpuPower"), QStringLiteral("CPU 功耗"),
          QStringLiteral("封装功耗,需要驱动支持 RAPL"), SampleScope::Cpu, &readCpuPower },

        { MetricId::MemoryUsage, QStringLiteral("memoryUsage"), QStringLiteral("内存占用"),
          QStringLiteral("物理内存使用率"), SampleScope::Memory, &readMemoryUsage },

        { MetricId::GpuUsage, QStringLiteral("gpuUsage"), QStringLiteral("GPU 占用"),
          QStringLiteral("主显卡的综合占用率"), SampleScope::Gpu, &readGpuUsage },

        { MetricId::GpuTemperature, QStringLiteral("gpuTemperature"), QStringLiteral("GPU 温度"),
          QStringLiteral("主显卡温度,走厂商 SDK"), SampleScope::Gpu, &readGpuTemperature },

        { MetricId::NetworkThroughput, QStringLiteral("network"), QStringLiteral("网络流量"),
          QStringLiteral("所有物理网卡的下行 / 上行速率"),
          SampleScope::Network, &readNetworkThroughput },

        { MetricId::DiskActivity, QStringLiteral("diskActivity"), QStringLiteral("磁盘活动"),
          QStringLiteral("所有物理磁盘的活动时间占比"), SampleScope::Disk, &readDiskActivity },

        { MetricId::Uptime, QStringLiteral("uptime"), QStringLiteral("运行时间"),
          QStringLiteral("开机以来经过的时间"), SampleScope::Cpu, &readUptime },
    };
    return kDefs;
}

const MetricDef *metricDef(MetricId id)
{
    const QVector<MetricDef> &defs = metricDefs();
    for (const MetricDef &def : defs) {
        if (def.id == id)
            return &def;
    }
    return nullptr;
}

const MetricDef *metricDefByKey(const QString &key)
{
    if (key.isEmpty())
        return nullptr;
    const QVector<MetricDef> &defs = metricDefs();
    for (const MetricDef &def : defs) {
        if (def.key == key)
            return &def;
    }
    return nullptr;
}

SampleScope scopeForMetrics(const QVector<MetricId> &ids)
{
    quint32 mask = 0;
    for (MetricId id : ids) {
        if (const MetricDef *def = metricDef(id))
            mask |= def->channels;
    }
    return SampleScope(mask);
}

} // namespace ws
