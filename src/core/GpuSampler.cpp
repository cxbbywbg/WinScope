#include "core/GpuSampler.h"

#include "core/PdhQuery.h"
#include "core/ThermalProvider.h"
#include "core/Win32Utils.h"
#include "core/WmiQuery.h"

#include <QMap>

#include <algorithm>

#include <windows.h>

#include <dxgi.h>

namespace ws {

namespace {

// MinGW 的 dxgi 头里这些 IID 只有声明,自己定义一份避免链接期缺符号
const GUID kIID_IDXGIFactory1 = { 0x770AAE78, 0xF26F, 0x4DBA, { 0xA8, 0x29, 0x25, 0x3C, 0x83, 0xD1, 0xB3, 0x87 } };

// 核显判定:专用显存不到 1GB 的基本可以认为是核显(BIOS 划走的一小块显存),
// 独显哪怕是最低端也不会只有这么点。判错了只是标签难看,不影响数据。
constexpr quint64 kIntegratedVramThreshold = 1024ull * 1024 * 1024;

quint32 parseHex(const QString &raw)
{
    QString s = raw;
    if (s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        s = s.mid(2);
    bool ok = false;
    const quint32 v = s.toUInt(&ok, 16);
    return ok ? v : 0;
}

// 实例名形如 pid_1234_luid_0x00000000_0x0000ABCD_phys_0_eng_0_engtype_3D
// 老版本没有 phys_ 段,所以解析要按标记找,不能按固定下标切
quint32 parsePid(const QString &instance)
{
    if (!instance.startsWith(QLatin1String("pid_")))
        return 0;
    const int end = instance.indexOf(QLatin1Char('_'), 4);
    const QString num = end > 4 ? instance.mid(4, end - 4) : instance.mid(4);
    bool ok = false;
    const quint32 pid = num.toUInt(&ok);
    return ok ? pid : 0;
}

// luid_0x00000000_0x0000ABCD —— 实测第一段是 LUID 的 HighPart,第二段是 LowPart
struct LuidKey {
    quint32 high = 0;
    quint32 low = 0;
    bool valid = false;
};

LuidKey parseLuid(const QString &instance)
{
    LuidKey key;
    const int pos = instance.indexOf(QLatin1String("luid_"));
    if (pos < 0)
        return key;

    const QString rest = instance.mid(pos + 5);
    const int sep = rest.indexOf(QLatin1Char('_'));
    if (sep <= 0)
        return key;

    const QString second = rest.mid(sep + 1);
    const int sep2 = second.indexOf(QLatin1Char('_'));
    const QString lowStr = sep2 > 0 ? second.left(sep2) : second;

    key.high = parseHex(rest.left(sep));
    key.low = parseHex(lowStr);
    key.valid = true;
    return key;
}

QString parseEngineType(const QString &instance)
{
    const int pos = instance.indexOf(QLatin1String("engtype_"));
    if (pos < 0)
        return QString();
    return instance.mid(pos + 8);
}

// 把 PDH 报的引擎名归一化。
// 原始名字带编号与空格差异("Compute 0" / "VideoDecode 1"),而且会冒出
// Security / Timer / VR / LegacyOverlay 这类内部引擎 —— 这些任务管理器也不显示,
// 返回空字符串表示"不展示"。
QString normalizeEngineName(const QString &raw)
{
    QString s = raw.trimmed();
    while (!s.isEmpty() && (s.at(s.size() - 1).isDigit() || s.at(s.size() - 1) == QLatin1Char(' ')))
        s.chop(1);
    s = s.trimmed();

    static const QHash<QString, QString> mapping = {
        { QStringLiteral("3d"), QStringLiteral("3D") },
        { QStringLiteral("copy"), QStringLiteral("Copy") },
        { QStringLiteral("videodecode"), QStringLiteral("Video Decode") },
        { QStringLiteral("video decode"), QStringLiteral("Video Decode") },
        { QStringLiteral("videoenhance"), QStringLiteral("Video Enhance") },
        { QStringLiteral("videoencode"), QStringLiteral("Video Encode") },
        { QStringLiteral("video encode"), QStringLiteral("Video Encode") },
        { QStringLiteral("compute"), QStringLiteral("Compute") },
        { QStringLiteral("high priority compute"), QStringLiteral("Compute") },
    };
    return mapping.value(s.toLower(), QString());
}

} // namespace

QVector<GpuAdapterDesc> GpuSampler::adapters()
{
    QVector<GpuAdapterDesc> result;

    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(kIID_IDXGIFactory1, reinterpret_cast<void **>(&factory))) || !factory)
        return result;

    for (UINT i = 0;; ++i) {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK || !adapter)
            break;

        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            GpuAdapterDesc d;
            d.name = fromWide(desc.Description);
            d.dedicatedVideoMemory = quint64(desc.DedicatedVideoMemory);
            d.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            d.integrated = d.dedicatedVideoMemory < kIntegratedVramThreshold;
            result.push_back(d);
        }
        adapter->Release();
    }
    factory->Release();

    return result;
}

GpuSampler::GpuSampler() = default;

GpuSampler::~GpuSampler()
{
    delete static_cast<PdhQuery *>(m_query);
    m_query = nullptr;
}

void GpuSampler::loadStaticInfo()
{
    // ---- 适配器清单:DXGI 给型号/显存/LUID,软件适配器(基本渲染驱动)跳过
    IDXGIFactory1 *factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(kIID_IDXGIFactory1, reinterpret_cast<void **>(&factory))) && factory) {
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1 *adapter = nullptr;
            if (factory->EnumAdapters1(i, &adapter) != S_OK || !adapter)
                break;

            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))
                && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                Adapter a;
                a.name = fromWide(desc.Description);
                a.dedicatedVideoMemory = quint64(desc.DedicatedVideoMemory);
                a.luidHigh = quint32(desc.AdapterLuid.HighPart);
                a.luidLow = quint32(desc.AdapterLuid.LowPart);
                a.integrated = a.dedicatedVideoMemory < kIntegratedVramThreshold;
                m_adapters.push_back(a);
            }
            adapter->Release();
        }
        factory->Release();
    }

    // ---- 驱动版本/日期只有 WMI 给,按型号名对齐到各适配器
    WmiQuery wmi;
    const auto rows = wmi.query(QStringLiteral("SELECT Name, DriverVersion, DriverDate FROM Win32_VideoController"),
                                { QStringLiteral("Name"), QStringLiteral("DriverVersion"),
                                  QStringLiteral("DriverDate") });
    for (Adapter &a : m_adapters) {
        for (const auto &row : rows) {
            const QString n = row.value(QStringLiteral("Name"));
            const bool exact = n.compare(a.name, Qt::CaseInsensitive) == 0;
            // DXGI 和 WMI 的型号名偶尔差一点(截断/带 (R)),退一步按前缀匹配
            const bool loose = !exact && !a.name.isEmpty() && n.startsWith(a.name.left(12), Qt::CaseInsensitive);
            if (exact || loose) {
                a.driverVersion = row.value(QStringLiteral("DriverVersion"));
                // WMI 给的是 "20200921000000.000000-000",转成人看的日期
                a.driverDate = formatWmiDate(row.value(QStringLiteral("DriverDate")));
                break;
            }
        }
    }

    // ---- PDH 计数器
    auto *pdh = new PdhQuery();
    if (pdh->open()) {
        m_idxEngine = pdh->add(QStringLiteral("\\GPU Engine(*)\\Utilization Percentage"));
        m_idxAdapterDedicated = pdh->add(QStringLiteral("\\GPU Adapter Memory(*)\\Dedicated Usage"));
        m_idxAdapterShared = pdh->add(QStringLiteral("\\GPU Adapter Memory(*)\\Shared Usage"));
        m_idxProcDedicated = pdh->add(QStringLiteral("\\GPU Process Memory(*)\\Dedicated Usage"));
        m_idxProcShared = pdh->add(QStringLiteral("\\GPU Process Memory(*)\\Shared Usage"));
        m_available = m_idxEngine >= 0 || m_idxAdapterDedicated >= 0;
        if (m_available) {
            pdh->collect();
            m_query = pdh;
        } else {
            delete pdh;
        }
    } else {
        delete pdh;
    }

    // ---- 温度交给 ThermalProvider,它自己会枚举适配器
}

QVector<GpuInfo> GpuSampler::sampleAll()
{
    QVector<GpuInfo> result;

    // 静态部分先铺好,后面拿到多少动态数据就填多少
    for (int i = 0; i < m_adapters.size(); ++i) {
        const Adapter &a = m_adapters.at(i);
        GpuInfo info;
        info.present = true;
        info.index = i;
        info.name = a.name;
        info.driverVersion = a.driverVersion;
        info.driverDate = a.driverDate;
        info.totalVramBytes = a.dedicatedVideoMemory;
        info.integrated = a.integrated;
        // 温度来自厂商 SDK(ADL/NVML/NVAPI),按型号名匹配,读不到就是 -1
        if (m_thermal)
            info.temperatureC = m_thermal->temperatureC(i);
        result.push_back(info);
    }

    m_procUsage.clear();
    m_procDedicated.clear();
    m_procShared.clear();

    if (m_adapters.isEmpty() || !m_query) {
        m_primary = result.isEmpty() ? GpuInfo() : result.first();
        return result;
    }

    auto *pdh = static_cast<PdhQuery *>(m_query);
    if (!pdh->collect()) {
        m_primary = result.first();
        return result;
    }

    // 适配器查找:PDH 实例里的 luid 对回 DXGI 的 LUID
    auto adapterIndexOf = [this](const LuidKey &key) -> int {
        if (!key.valid)
            return -1;
        for (int i = 0; i < m_adapters.size(); ++i) {
            if (m_adapters.at(i).luidHigh == key.high && m_adapters.at(i).luidLow == key.low)
                return i;
        }
        return -1;
    };

    // ---- 引擎占用:先按适配器拆开,再在适配器内部按引擎类型聚合。
    // 必须按"归一化之后"的名字聚合,否则 Compute 0/1/2 会各占一行。
    QVector<QMap<QString, double>> byAdapter(result.size());
    if (m_idxEngine >= 0) {
        const auto engineValues = pdh->instances(m_idxEngine);
        for (auto it = engineValues.constBegin(); it != engineValues.constEnd(); ++it) {
            const QString friendly = normalizeEngineName(parseEngineType(it.key()));
            if (friendly.isEmpty())
                continue;

            const int ai = adapterIndexOf(parseLuid(it.key()));
            if (ai >= 0)
                byAdapter[ai][friendly] += it.value();

            // 进程列表里的 GPU 占用是跨适配器合计的,和任务管理器口径一致
            const quint32 pid = parsePid(it.key());
            if (pid != 0)
                m_procUsage[pid] += it.value();
        }
    }

    for (int i = 0; i < result.size(); ++i) {
        double maxEngine = 0.0;
        for (auto it = byAdapter[i].constBegin(); it != byAdapter[i].constEnd(); ++it) {
            GpuEngine e;
            e.name = it.key();
            e.percent = qBound(0.0, it.value(), 100.0);
            maxEngine = qMax(maxEngine, e.percent);
            result[i].engines.push_back(e);
        }
        std::sort(result[i].engines.begin(), result[i].engines.end(),
                  [](const GpuEngine &a, const GpuEngine &b) { return a.percent > b.percent; });
        result[i].usagePercent = maxEngine;
    }

    // 进程级占用超过 100 说明跨引擎累加了,夹到 100
    for (auto it = m_procUsage.begin(); it != m_procUsage.end(); ++it)
        it.value() = qBound(0.0, it.value(), 100.0);

    // ---- 进程级显存:跨适配器按 pid 累加,供进程列表用;
    // 顺便按适配器各存一份,万一 GPU Adapter Memory 计数器不可用可以兜底
    QVector<quint64> adapterDedicatedFallback(result.size(), 0);
    QVector<quint64> adapterSharedFallback(result.size(), 0);
    if (m_idxProcDedicated >= 0) {
        const auto dedicated = pdh->instances(m_idxProcDedicated);
        for (auto it = dedicated.constBegin(); it != dedicated.constEnd(); ++it) {
            const quint64 bytes = quint64(qMax(0.0, it.value()));
            const quint32 pid = parsePid(it.key());
            if (pid != 0)
                m_procDedicated[pid] += bytes;
            const int ai = adapterIndexOf(parseLuid(it.key()));
            if (ai >= 0)
                adapterDedicatedFallback[ai] += bytes;
        }
    }
    if (m_idxProcShared >= 0) {
        const auto shared = pdh->instances(m_idxProcShared);
        for (auto it = shared.constBegin(); it != shared.constEnd(); ++it) {
            const quint64 bytes = quint64(qMax(0.0, it.value()));
            const quint32 pid = parsePid(it.key());
            if (pid != 0)
                m_procShared[pid] += bytes;
            const int ai = adapterIndexOf(parseLuid(it.key()));
            if (ai >= 0)
                adapterSharedFallback[ai] += bytes;
        }
    }

    // ---- 适配器级显存:GPU Adapter Memory 就是按适配器给的,不用自己加总
    bool adapterDedicatedOk = false;
    if (m_idxAdapterDedicated >= 0) {
        const auto values = pdh->instances(m_idxAdapterDedicated);
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            const int ai = adapterIndexOf(parseLuid(it.key()));
            if (ai >= 0) {
                result[ai].dedicatedBytes = quint64(qMax(0.0, it.value()));
                adapterDedicatedOk = true;
            }
        }
    }
    bool adapterSharedOk = false;
    if (m_idxAdapterShared >= 0) {
        const auto values = pdh->instances(m_idxAdapterShared);
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            const int ai = adapterIndexOf(parseLuid(it.key()));
            if (ai >= 0) {
                result[ai].sharedBytes = quint64(qMax(0.0, it.value()));
                adapterSharedOk = true;
            }
        }
    }
    // 该计数器在部分驱动上不存在,退回按进程求和
    if (!adapterDedicatedOk) {
        for (int i = 0; i < result.size(); ++i)
            result[i].dedicatedBytes = adapterDedicatedFallback.at(i);
    }
    if (!adapterSharedOk) {
        for (int i = 0; i < result.size(); ++i)
            result[i].sharedBytes = adapterSharedFallback.at(i);
    }

    for (GpuInfo &info : result) {
        if (info.totalVramBytes > 0) {
            info.memoryPercent
                = qBound(0.0, 100.0 * double(info.dedicatedBytes) / double(info.totalVramBytes), 100.0);
        }
    }

    // ---- 主显卡:独显优先(专用显存最大的那块),没有独显就取第一块
    int best = 0;
    for (int i = 0; i < result.size(); ++i) {
        const bool better = result[i].totalVramBytes > result[best].totalVramBytes;
        if (better)
            best = i;
    }
    m_primary = result.at(best);

    return result;
}

} // namespace ws
