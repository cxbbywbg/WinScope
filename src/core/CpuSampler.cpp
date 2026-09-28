#include "core/CpuSampler.h"

#include "core/PdhQuery.h"
#include "core/ThermalProvider.h"
#include "core/Win32Utils.h"
#include "core/WmiQuery.h"

#include <QDateTime>
#include <QHash>

#include <vector>

#include <powrprof.h>
#include <winternl.h>

namespace ws {

namespace {

// MinGW 的 powrprof.h 只声明了 CallNtPowerInformation,没有这个结构体,
// 自己按 Windows SDK 的定义补一份(字段顺序必须一致)
struct ProcessorPowerInfo {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

// winternl.h 里的 SYSTEM_INFORMATION_CLASS 只列了少数几个值,
// 这里用到的两个(5 / 8)得自己给常量
constexpr ULONG kSystemProcessInformation = 5;
constexpr ULONG kSystemProcessorPerformanceInformation = 8;

struct ProcessorPerfInfo {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
};

quint64 toU64(const LARGE_INTEGER &li)
{
    return static_cast<quint64>(li.QuadPart);
}

// 注册表里 CPU 的标称主频与型号
void readCpuRegistry(QString *model, double *baseMHz)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key)
        != ERROR_SUCCESS) {
        return;
    }

    wchar_t buf[512] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    if (model && RegQueryValueExW(key, L"ProcessorNameString", nullptr, &type,
                                  reinterpret_cast<LPBYTE>(buf), &size)
            == ERROR_SUCCESS) {
        *model = fromWide(buf);
    }

    DWORD mhz = 0;
    size = sizeof(mhz);
    if (baseMHz && RegQueryValueExW(key, L"~MHz", nullptr, &type, reinterpret_cast<LPBYTE>(&mhz), &size)
            == ERROR_SUCCESS) {
        *baseMHz = double(mhz);
    }

    RegCloseKey(key);
}

} // namespace

CpuSampler::CpuSampler() = default;

CpuSampler::~CpuSampler()
{
    delete static_cast<PdhQuery *>(m_powerQuery);
    m_powerQuery = nullptr;
    delete static_cast<PdhQuery *>(m_freqQuery);
    m_freqQuery = nullptr;
}

int CpuSampler::physicalCoreCount() const
{
    // 按"物理核心"关系枚举,能正确排除超线程
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0)
        return m_logicalCores;

    std::vector<BYTE> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buf.data()),
                                          &len)) {
        return m_logicalCores;
    }

    int count = 0;
    DWORD offset = 0;
    while (offset < len) {
        auto *info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buf.data() + offset);
        if (info->Relationship == RelationProcessorCore)
            ++count;
        if (info->Size == 0)
            break;
        offset += info->Size;
    }
    return count > 0 ? count : m_logicalCores;
}

void CpuSampler::loadStaticInfo()
{
    readCpuRegistry(&m_modelName, &m_baseMHz);

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    m_logicalCores = int(si.dwNumberOfProcessors);
    m_physicalCores = physicalCoreCount();
    if (m_physicalCores <= 0)
        m_physicalCores = m_logicalCores;

    // 最大睿频:注册表的 ~MHz 只是基准频率,睿频上限得问 WMI。
    // 查询较慢,但只在启动时做一次。
    WmiQuery wmi;
    const auto rows = wmi.query(QStringLiteral("SELECT MaxClockSpeed FROM Win32_Processor"),
                                { QStringLiteral("MaxClockSpeed") });
    if (!rows.isEmpty())
        m_maxClockMHz = rows.first().value(QStringLiteral("MaxClockSpeed")).toDouble();

    // 功耗计数器(Intel RAPL 等),挂不上就算了
    auto *pdh = new PdhQuery();
    if (pdh->open() && pdh->add(QStringLiteral("\\Energy Meter(*)\\Power")) >= 0) {
        pdh->collect();
        m_powerQuery = pdh;
    } else {
        delete pdh;
        m_powerQuery = nullptr;
    }

    // 实时主频:100 表示跑在基准频率上,可以超过 100(睿频)
    auto *freq = new PdhQuery();
    if (freq->open()) {
        m_idxFreqPerf = freq->add(QStringLiteral("\\Processor Information(_Total)\\% Processor Performance"));
        if (m_idxFreqPerf >= 0) {
            freq->collect();
            m_freqQuery = freq;
        } else {
            delete freq;
        }
    } else {
        delete freq;
    }
}

void CpuSampler::refreshAcpiTemperatureAndPower()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastSlowQueryMs < 10000)
        return;
    m_lastSlowQueryMs = now;

    // 这是唯一的"真 CPU 温度"来源。很多笔记本固件没实现 ACPI 热区,
    // 这时查询会安静地返回空,不是出错
    const QString temp = wmiThermalZoneCelsiusText();
    m_acpiTemperatureC = temp.isEmpty() ? -1.0 : temp.toDouble();

    if (m_powerQuery) {
        auto *pdh = static_cast<PdhQuery *>(m_powerQuery);
        if (pdh->collect()) {
            const auto values = pdh->instances(0);
            double sum = 0.0;
            for (double v : values)
                sum += v;
            // Energy Meter 的 Power 计数器单位是毫瓦
            const double watts = sum / 1000.0;
            // 明显不合理的读数(没装 RAPL 驱动时会给垃圾值)宁可显示不可用
            m_powerW = (watts > 0.0 && watts < 500.0) ? watts : -1.0;
        }
    }
}

void CpuSampler::resolveTemperature()
{
    if (m_acpiTemperatureC >= 0.0) {
        m_temperatureSource = CpuTemperatureSource::AcpiThermalZone;
        return;
    }

    // 没实现 ACPI 热区,退一步用核显传感器。核显和 CPU 核心在同一颗 die 上,
    // 读到的是这颗芯片的温度,当参考值是合理的 —— 但界面上必须标出来源,
    // 不能让人以为这是 CPU 自己的传感器。
    // 门槛卡在 ThermalProvider 里(只认 ADL 读数 + AMD APU 核显型号名),
    // 拿不到就返回 -1,界面留空,绝不退回用独显温度冒充
    if (m_thermal && m_thermal->cpuProxyTemperatureC() >= 0.0) {
        m_temperatureSource = CpuTemperatureSource::IntegratedGpu;
        return;
    }

    m_temperatureSource = CpuTemperatureSource::None;
}

double CpuSampler::currentTemperatureC() const
{
    switch (m_temperatureSource) {
    case CpuTemperatureSource::AcpiThermalZone:
        return m_acpiTemperatureC;
    case CpuTemperatureSource::IntegratedGpu:
        // 核显温度变化比 WMI 快,每拍现取,让横条跟着动
        return m_thermal ? m_thermal->cpuProxyTemperatureC() : -1.0;
    case CpuTemperatureSource::None:
        break;
    }
    return -1.0;
}

CpuInfo CpuSampler::sample()
{
    CpuInfo info;
    info.model = m_modelName;
    info.physicalCores = m_physicalCores;
    info.logicalCores = m_logicalCores;

    // ---- 总体
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        Times cur;
        cur.idle = (quint64(idle.dwHighDateTime) << 32) | idle.dwLowDateTime;
        // kernel 时间包含 idle,所以总量 = kernel + user
        cur.kernel = (quint64(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
        cur.user = (quint64(user.dwHighDateTime) << 32) | user.dwLowDateTime;

        info.idleSeconds = double(cur.idle) / 1e7;
        // kernel 时间包含 idle,减掉才是真正的内核态占用。
        // 不减的话"用户 + 内核 + 空闲"会重复计算,构成条也会失真。
        info.kernelSeconds = cur.kernel > cur.idle ? double(cur.kernel - cur.idle) / 1e7 : 0.0;
        info.userSeconds = double(cur.user) / 1e7;

        if (m_hasPrev) {
            const quint64 dIdle = cur.idle - m_prevTotal.idle;
            const quint64 dKernel = cur.kernel - m_prevTotal.kernel;
            const quint64 dUser = cur.user - m_prevTotal.user;
            const quint64 dTotal = dKernel + dUser;   // kernel 已含 idle
            if (dTotal > 0) {
                const double busy = double(dTotal - qMin(dIdle, dTotal));
                info.usage = qBound(0.0, 100.0 * busy / double(dTotal), 100.0);
            }
        }
        m_prevTotal = cur;
    }

    // ---- 每核心
    const ULONG need = ULONG(sizeof(ProcessorPerfInfo) * size_t(qMax(1, m_logicalCores)));
    std::vector<BYTE> buf(need + sizeof(ProcessorPerfInfo) * 8);
    ULONG returned = 0;
    const LONG status = NtQuerySystemInformation(static_cast<SYSTEM_INFORMATION_CLASS>(kSystemProcessorPerformanceInformation),
                                                 buf.data(), ULONG(buf.size()), &returned);
    if (status == 0 && returned >= sizeof(ProcessorPerfInfo)) {
        const int count = int(returned / sizeof(ProcessorPerfInfo));
        auto *arr = reinterpret_cast<ProcessorPerfInfo *>(buf.data());

        QVector<Times> curCores;
        curCores.reserve(count);
        for (int i = 0; i < count; ++i) {
            Times t;
            t.idle = toU64(arr[i].IdleTime);
            t.kernel = toU64(arr[i].KernelTime);
            t.user = toU64(arr[i].UserTime);
            curCores.push_back(t);
        }

        info.cores.reserve(count);
        for (int i = 0; i < count; ++i) {
            CpuCore core;
            if (m_hasPrev && i < m_prevCores.size()) {
                const quint64 dIdle = curCores[i].idle - m_prevCores[i].idle;
                const quint64 dKernel = curCores[i].kernel - m_prevCores[i].kernel;
                const quint64 dUser = curCores[i].user - m_prevCores[i].user;
                const quint64 dTotal = dKernel + dUser;
                if (dTotal > 0) {
                    const double busy = double(dTotal - qMin(dIdle, dTotal));
                    core.usage = qBound(0.0, 100.0 * busy / double(dTotal), 100.0);
                }
            }
            info.cores.push_back(core);
        }
        m_prevCores = curCores;
    }
    if (info.cores.isEmpty() && m_logicalCores > 0) {
        // 拿不到每核心数据时,用总体值兜底,界面不至于空着
        for (int i = 0; i < m_logicalCores; ++i) {
            CpuCore core;
            core.usage = info.usage;
            info.cores.push_back(core);
        }
    }

    m_hasPrev = true;

    // ---- 频率:一次问所有核心的实时主频
    const DWORD powerBufSize = DWORD(sizeof(ProcessorPowerInfo) * size_t(qMax(1, m_logicalCores)));
    std::vector<BYTE> powerBuf(powerBufSize);
    double sumMHz = 0.0;
    int freqCount = 0;
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, powerBuf.data(), powerBufSize) == 0) {
        auto *pi = reinterpret_cast<ProcessorPowerInfo *>(powerBuf.data());
        const int n = qMin(m_logicalCores, int(powerBufSize / sizeof(ProcessorPowerInfo)));
        for (int i = 0; i < n; ++i) {
            if (pi[i].CurrentMhz > 0) {
                sumMHz += double(pi[i].CurrentMhz);
                ++freqCount;
                if (i < info.cores.size())
                    info.cores[i].freqMHz = double(pi[i].CurrentMhz);
            }
            if (pi[i].MaxMhz > 0)
                info.maxMHz = double(pi[i].MaxMhz);
        }
    }
    info.currentMHz = freqCount > 0 ? sumMHz / freqCount : 0.0;
    info.baseMHz = m_baseMHz;
    info.maxClockMHz = m_maxClockMHz;
    if (info.maxMHz <= 0)
        info.maxMHz = m_baseMHz;

    // PDH 的 % Processor Performance 比上面的接口准(100 = 基准频率,睿频可超 100)。
    // AMD 机器上 CallNtPowerInformation 常年只返回标称值,曲线是一条直线,
    // 所以这里拿到有效值就盖掉。
    if (m_freqQuery && m_baseMHz > 0.0) {
        auto *pdh = static_cast<PdhQuery *>(m_freqQuery);
        if (pdh->collect()) {
            const double perf = pdh->instances(m_idxFreqPerf).value(QStringLiteral("_Total"), 0.0);
            const double mhz = m_baseMHz * perf / 100.0;
            // 超出合理范围说明计数器没意义(某些虚拟机/驱动会给垃圾值),宁可不改
            if (mhz > 100.0 && mhz < m_baseMHz * 3.0)
                info.currentMHz = mhz;
        }
    }

    // ---- 温度 / 功耗
    refreshAcpiTemperatureAndPower();
    resolveTemperature();
    info.temperatureC = currentTemperatureC();
    info.temperatureSource = m_temperatureSource;
    info.powerW = m_powerW;

    // ---- 运行时间
    info.uptimeSeconds = double(GetTickCount64()) / 1000.0;

    return info;
}

} // namespace ws
