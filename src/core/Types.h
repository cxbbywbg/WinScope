#pragma once

// WinScope 的统一数据模型。
// 采样层只负责填这些结构体,界面层只负责渲染,两边不互相依赖 Win32 类型。

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ws {

// ---------------------------------------------------------------- 通用

enum class LoadLevel { Normal, Warning, Critical };

// 按占用率给出告警级别,阈值对所有资源统一
inline LoadLevel levelFor(double percent)
{
    if (percent >= 90.0)
        return LoadLevel::Critical;
    if (percent >= 70.0)
        return LoadLevel::Warning;
    return LoadLevel::Normal;
}

// ---------------------------------------------------------------- 采样范围

// 采样通道。
//
// 小窗模式下只开需要的几路,其余整拍跳过。省下来的主要是这两项:
//   · 进程快照 —— 要枚举全系统句柄,还要为每个进程查路径/命令行/属主
//   · 连接表   —— TCP/UDP 全表枚举
// 用位掩码而不是一堆 bool 参数:"要哪几路"这个集合是由小窗勾了哪些指标算出来的,
// 位运算组合最省事,整包比较也方便
class SampleScope
{
public:
    enum Bit : quint32 {
        Cpu = 1u << 0,
        Memory = 1u << 1,
        Gpu = 1u << 2,
        Disk = 1u << 3,
        Network = 1u << 4,
        Processes = 1u << 5,
        All = Cpu | Memory | Gpu | Disk | Network | Processes,
    };

    constexpr SampleScope() = default;
    constexpr SampleScope(Bit bit) : m_mask(quint32(bit)) {}
    constexpr explicit SampleScope(quint32 mask) : m_mask(mask) {}

    constexpr quint32 mask() const { return m_mask; }
    constexpr bool test(Bit bit) const { return (m_mask & quint32(bit)) != 0; }
    constexpr bool isEmpty() const { return m_mask == 0; }

    constexpr SampleScope operator|(const SampleScope &other) const
    {
        return SampleScope(m_mask | other.m_mask);
    }
    SampleScope &operator|=(const SampleScope &other)
    {
        m_mask |= other.m_mask;
        return *this;
    }
    constexpr bool operator==(const SampleScope &other) const { return m_mask == other.m_mask; }
    constexpr bool operator!=(const SampleScope &other) const { return m_mask != other.m_mask; }

private:
    quint32 m_mask = 0;
};

inline constexpr SampleScope sampleScopeAll()
{
    return SampleScope(quint32(SampleScope::All));
}

// ---------------------------------------------------------------- CPU

// CPU 温度的来源。Windows 上真正能读到"CPU 温度"的只有 ACPI 热区
// (PDH Thermal Zone / WMI MSAcpi_ThermalZoneTemperature),但不少笔记本固件
// 根本没实现它。核显和 CPU 核心在同一颗 die 上,拿不到热区时可以退一步用核显
// 传感器 —— 但界面必须如实标出来源,不能让人以为读到了 CPU 自己的传感器。
enum class CpuTemperatureSource {
    None,
    AcpiThermalZone,
    IntegratedGpu,
};

struct CpuCore {
    double usage = 0.0;    // 0..100
    double freqMHz = 0.0;  // 当前频率,取不到为 0
};

struct CpuInfo {
    QString model;                       // 处理器型号
    double usage = 0.0;                  // 总体占用率 0..100
    QVector<CpuCore> cores;              // 每个逻辑核心
    int physicalCores = 0;
    int logicalCores = 0;
    double currentMHz = 0.0;             // 当前主频(各核心平均)
    double baseMHz = 0.0;                // 标称/基准主频(注册表 ~MHz)
    double maxMHz = 0.0;                 // 计数器报告的最大主频
    double maxClockMHz = 0.0;            // 厂商标称的最大睿频(WMI MaxClockSpeed)
    double temperatureC = -1.0;          // -1 表示不可用
    CpuTemperatureSource temperatureSource = CpuTemperatureSource::None;
    double powerW = -1.0;                // -1 表示不可用
    double userSeconds = 0.0;            // 开机以来累计
    double kernelSeconds = 0.0;
    double idleSeconds = 0.0;
    double uptimeSeconds = 0.0;
    int processCount = 0;
    int threadCount = 0;
    int handleCount = 0;
};

// ---------------------------------------------------------------- 内存

struct MemoryInfo {
    quint64 totalBytes = 0;
    quint64 usedBytes = 0;
    quint64 availableBytes = 0;
    quint64 cachedBytes = 0;         // 待机缓存 + 已修改
    quint64 committedBytes = 0;      // 提交大小
    quint64 commitLimitBytes = 0;
    quint64 pagedPoolBytes = 0;
    quint64 nonPagedPoolBytes = 0;
    quint64 pageFileTotalBytes = 0;
    quint64 pageFileUsedBytes = 0;
    double usagePercent = 0.0;
};

// ---------------------------------------------------------------- GPU

struct GpuEngine {
    QString name;       // 3D / Copy / VideoDecode / VideoEncode / Compute ...
    double percent = 0.0;
};

struct GpuInfo {
    bool present = false;
    QString name;
    QString driverVersion;
    QString driverDate;
    quint64 dedicatedBytes = 0;      // 已用独显显存
    quint64 sharedBytes = 0;         // 已用共享内存
    quint64 totalVramBytes = 0;      // 适配器专用显存总量
    double usagePercent = 0.0;       // 综合占用(取各引擎最大值)
    double memoryPercent = 0.0;
    double temperatureC = -1.0;      // 走厂商 SDK(ADL/NVML/NVAPI),取不到为 -1
    double powerW = -1.0;
    double freqMHz = -1.0;
    QVector<GpuEngine> engines;      // 各引擎细分占用
    int index = 0;                   // 适配器序号(0 起),界面显示 "GPU 0"
    bool integrated = false;         // 核显:几乎没有独立显存,和系统内存共用
};

// ---------------------------------------------------------------- 磁盘

struct DiskVolume {
    QString letter;      // "C:"
    QString label;
    QString fileSystem;
    QString devicePath;  // 对应的物理盘,例如 "\\.\PhysicalDrive0"
    quint64 totalBytes = 0;
    quint64 freeBytes = 0;
    bool isRemovable = false;
    bool isNetwork = false;

    double usedPercent() const
    {
        return totalBytes ? 100.0 * double(totalBytes - freeBytes) / double(totalBytes) : 0.0;
    }
};

struct DiskInfo {
    double activePercent = 0.0;      // 活动时间占比
    double readBytesPerSec = 0.0;
    double writeBytesPerSec = 0.0;
    double iops = 0.0;
    double responseMs = 0.0;         // 平均响应时间
    double queueLength = 0.0;
    QVector<DiskVolume> volumes;
};

// ---------------------------------------------------------------- 网络

enum class AdapterKind { Ethernet, WiFi, Virtual, Vpn, Loopback, Other };

struct NetAdapter {
    QString name;            // 连接名,例如 "以太网"
    QString description;     // 网卡型号
    QString mac;
    QStringList ipv4;
    QStringList ipv6;
    QStringList gateways;
    QStringList dns;
    AdapterKind kind = AdapterKind::Other;
    bool connected = false;
    bool hasLink = false;
    quint64 linkSpeedBps = 0;
    double rxBytesPerSec = 0.0;
    double txBytesPerSec = 0.0;
    quint64 rxBytesTotal = 0;
    quint64 txBytesTotal = 0;
};

struct NetInfo {
    QVector<NetAdapter> adapters;
    double rxBytesPerSec = 0.0;   // 所有物理网卡合计
    double txBytesPerSec = 0.0;
    quint64 rxBytesTotal = 0;
    quint64 txBytesTotal = 0;
    int tcpCount = 0;
    int udpCount = 0;
};

// ---------------------------------------------------------------- 进程

struct ProcessInfo {
    quint32 pid = 0;
    quint32 parentPid = 0;
    QString name;
    QString description;      // 文件说明(版本资源)
    QString path;
    QString commandLine;
    QString user;
    QString architecture;     // x64 / x86 / ARM64
    int sessionId = 0;

    double cpuPercent = 0.0;        // 占整机 CPU 的百分比(与任务管理器一致)
    quint64 workingSetBytes = 0;    // 工作集(物理内存)
    quint64 privateBytes = 0;       // 提交大小
    quint64 virtualBytes = 0;
    double gpuPercent = 0.0;
    quint64 gpuDedicatedBytes = 0;
    quint64 gpuSharedBytes = 0;
    quint64 diskReadBytesPerSec = 0;
    quint64 diskWriteBytesPerSec = 0;
    double netRxBytesPerSec = 0.0;
    double netTxBytesPerSec = 0.0;

    quint64 cpuTimeMs = 0;      // 累计 CPU 时间
    int threadCount = 0;
    int handleCount = 0;
    qint64 startTimeMs = 0;     // epoch 毫秒
    bool elevated = false;
    bool responding = true;
    bool suspended = false;
    bool critical = false;      // 系统关键进程,结束时有额外风险
};

// ---------------------------------------------------------------- 服务

struct ServiceInfo {
    QString name;             // 服务名
    QString displayName;
    QString description;
    QString state;            // Running / Stopped / Paused / Start Pending ...
    QString startType;        // Automatic / Manual / Disabled / Auto (Delayed)
    QString binaryPath;
    QString account;
    quint32 pid = 0;
    bool canStop = false;
    bool canStart = false;
    bool canPause = false;
    bool delayedStart = false;
    bool isDriver = false;
};

// ---------------------------------------------------------------- 启动项

enum class StartupSource { Registry, StartupFolder, ScheduledTask, Service };

struct StartupItem {
    QString name;
    QString command;
    QString filePath;
    QString publisher;
    QString location;         // 例如 "HKLM\\...\\Run"
    QString impact;           // 高 / 中 / 低 / 未标注
    QString sourceLabel;      // 注册表 / 启动文件夹 / 计划任务
    StartupSource source = StartupSource::Registry;
    QString registryKey;      // 用于启停
    QString registryValue;
    bool enabled = true;
    bool canToggle = false;
};

// ---------------------------------------------------------------- 系统信息

struct KeyValue {
    QString key;
    QString value;
};

struct SystemInfoData {
    QVector<KeyValue> os;
    QVector<KeyValue> cpu;
    QVector<KeyValue> gpu;
    QVector<KeyValue> memory;
    QVector<KeyValue> motherboard;
    QVector<KeyValue> storage;
    QVector<KeyValue> network;
};

// ---------------------------------------------------------------- 快照

struct SystemSnapshot {
    qint64 timestampMs = 0;
    CpuInfo cpu;
    MemoryInfo memory;
    // 主显卡(有独显时优先取独显),概览页那种只放得下一个仪表的地方用它
    GpuInfo gpu;
    // 全部硬件适配器,按 DXGI 枚举顺序。核显 + 独显的机器上这里会有两条
    QVector<GpuInfo> gpus;
    DiskInfo disk;
    NetInfo net;
    bool valid = false;
};

// 进程快照单独一档:进程列表刷新频率与曲线不同
struct ProcessSnapshot {
    qint64 timestampMs = 0;
    QVector<ProcessInfo> processes;
    int totalProcesses = 0;
    int totalThreads = 0;
    int totalHandles = 0;
};

} // namespace ws

// 跨线程用排队信号投递,必须先注册
Q_DECLARE_METATYPE(ws::SystemSnapshot)
Q_DECLARE_METATYPE(ws::ProcessSnapshot)
