#include "core/ThermalProvider.h"

#include "core/GpuSampler.h"
#include "core/Win32Utils.h"

#include <QDateTime>
#include <QStringList>

#include <vector>

#include <windows.h>

namespace ws {

namespace {

// 温度缓存的有效期。采样是 1 秒一拍,CPU 和 GPU 两条路径都要问同一个传感器,
// 缓存一下就不会重复调驱动
constexpr qint64 kCacheTtlMs = 900;

// 明显不合理的读数一律当失败(有的驱动会返回 0 或者垃圾值)
constexpr int kMinTempC = 1;
constexpr int kMaxTempC = 150;

// ---------------------------------------------------------------- ADL(AMD)

constexpr int kAdlMaxPath = 256;
constexpr int kAdlOk = 0;

// ADL_ODN_TEMP_EDGE:OverdriveN 的"核心边缘"温度
constexpr int kAdlOdnTempEdge = 1;
// ADL_PMLOG_TEMPERATURE_EDGE:PMLog 里的 EDGE 传感器下标
constexpr int kAdlPmLogTempEdge = 8;
// PMLog 缓冲区:开头一个 int 是 iSize,之后每 8 字节一个 {unsigned supported; int value;}
constexpr int kAdlPmLogBufSize = 4096;

using AdlMallocCallback = void *(__stdcall *)(int);

struct AdlAdapterInfo {
    int iSize;
    int iAdapterIndex;
    char strUDID[kAdlMaxPath];
    int iBusNumber;
    int iDeviceNumber;
    int iFunctionNumber;
    int iVendorID;
    char strAdapterName[kAdlMaxPath];
    char strDisplayName[kAdlMaxPath];
    int iPresent;
    int iExist;
    char strDriverPath[kAdlMaxPath];
    char strDriverPathExt[kAdlMaxPath];
    char strPNPString[kAdlMaxPath];
    int iOSDisplayIndex;
};

using AdlMainControlCreate = int(__stdcall *)(AdlMallocCallback, int, void **);
using AdlMainControlDestroy = int(__stdcall *)(void *);
using AdlAdapterNumberGet = int(__stdcall *)(void *, int *);
using AdlAdapterInfoGet = int(__stdcall *)(void *, void *, int);
using AdlOverdriveNTemperatureGet = int(__stdcall *)(void *, int, int, int *);
using AdlQueryPmLogDataGet = int(__stdcall *)(void *, int, void *);

void *__stdcall adlMalloc(int size)
{
    return malloc(size_t(size));
}

// ---------------------------------------------------------------- NVAPI(NVIDIA)

// nvapi_QueryInterface 返回的是**函数指针**。x64 上必须用 void* 接,
// 写成 int 会把高 32 位截断,拿到的地址是垃圾,一调就崩
using NvapiQueryInterface = void *(*)(unsigned int);
using NvapiInitialize = int (*)();
using NvapiEnumPhysicalGpus = int (*)(void **, int *);
using NvapiGpuGetThermalSettings = int (*)(void *, unsigned int, void *);
using NvapiGpuGetFullName = int (*)(void *, char *);

constexpr int kNvapiMaxThermalSensors = 3;
constexpr int kNvapiThermalTargetGpu = 1;

struct NvapiThermalSensor {
    int controller;
    int defaultMinTemp;
    int defaultMaxTemp;
    int currentTemp;
    int target;
};

struct NvapiThermalSettings {
    unsigned int version;
    int count;
    NvapiThermalSensor sensor[kNvapiMaxThermalSensors];
};

template <typename T>
constexpr unsigned int nvapiVersion(unsigned int v)
{
    return static_cast<unsigned int>(sizeof(T)) | (v << 16);
}

// ---------------------------------------------------------------- NVML(NVIDIA)

using NvmlInit = int (*)();
using NvmlShutdown = int (*)();
using NvmlDeviceGetCount = int (*)(unsigned int *);
using NvmlDeviceGetHandleByIndex = int (*)(unsigned int, void **);
using NvmlDeviceGetName = int (*)(void *, char *, unsigned int);
using NvmlDeviceGetTemperature = int (*)(void *, int, unsigned int *);

constexpr int kNvmlTemperatureGpu = 0;

// ---------------------------------------------------------------- 名字匹配

// 厂商 SDK 和 DXGI 报的型号名实测完全一致(都是 "AMD Radeon(TM) Graphics"),
// 但驱动版本不同偶尔会多出 (TM)/(R) 或者空格差异,所以去掉这些噪声再比。
// 不做"前缀相同就算同一个"的宽松匹配 —— 那会把 2060 和 2060 SUPER 混成一个
QString normalizeModelName(const QString &raw)
{
    QString s = raw.toUpper();
    s.remove(QStringLiteral("(TM)"));
    s.remove(QStringLiteral("(R)"));
    s.remove(QStringLiteral("(C)"));
    s.remove(QLatin1Char(' '));
    return s;
}

bool nameMatches(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty())
        return false;
    return normalizeModelName(a) == normalizeModelName(b);
}

bool looksNvidia(const QString &name)
{
    return name.contains(QLatin1String("NVIDIA"), Qt::CaseInsensitive)
        || name.contains(QLatin1String("GeForce"), Qt::CaseInsensitive);
}

// 判断型号名看起来是不是 AMD APU 的核显。
//
// 为什么值得费这个劲:CPU 温度的兜底路径要求"这块 GPU 和 CPU 在同一颗 die 上",
// 而这件事在 Windows 上没有可靠接口可查(MinGW 里没有 dxcore.h,手写 DXCore 的
// COM 接口风险太高),只能靠型号名。而误判的代价是不对称的 ——
//   漏判:CPU 温度留空,用户少看一个数;
//   误判:把独显温度当成 CPU 温度显示出去,用户看到的是错数据。
// 所以规则全部是"正面白名单",不认识的型号一律返回 false。
//
// 这里**刻意不**让 GpuAdapterDesc::integrated(按专用显存 <1GB 判的)参与判断:
// 那个字段在 GpuSampler 里的定位是"标签用,判错了不影响数据",而 BIOS 把核显的
// UMA 显存划到 1GB 以上时它就会漏判。用一个自己都标注了"低风险"的启发式去把守
// 数据正确性,迟早在某台机器上以最难查的方式炸掉。
bool looksLikeApuIntegratedName(const QString &adapterName)
{
    const QString s = normalizeModelName(adapterName);   // 大写,已去掉 (TM)/(R)/(C) 和空格
    if (!s.contains(QLatin1String("RADEON")))
        return false;

    // 独显的产品线标记,命中任意一个就直接否掉
    static const char *const kDiscreteMarkers[] = {
        "RX",          // RX 6600M / RX Vega 64。会误伤笔记本营销名 "Radeon RX Vega 8",
                       // 但驱动报出来的型号名不带 RX,可以接受
        "FIREPRO",
        "INSTINCT",
        "RADEONPRO",   // Radeon Pro WX / Radeon Pro Vega
        "VII",         // Radeon VII
        "HD",          // 老独显 Radeon HD 8750M。会误伤同代 APU 的 "Radeon HD 8650G",
                       // 但那是 2013 年的机器了,漏判只是温度留空
        "R7",          // Radeon R7 260X,会误伤 Kaveri 的 "Radeon R7 Graphics",同上
        "R9",
    };
    for (const char *marker : kDiscreteMarkers) {
        if (s.contains(QLatin1String(marker)))
            return false;
    }

    // 白名单一:核显的"占位名"。驱动没有具体型号可报时用这个,
    //           "AMD Radeon(TM) Graphics" 归一化后是 "AMDRADEONGRAPHICS"
    if (s.contains(QLatin1String("RADEONGRAPHICS")))
        return true;

    // 白名单二:Vega 核显,如 "AMD Radeon(TM) Vega 8 Graphics" / "Vega 11 Graphics"。
    //           注意中间夹着数字,不能用 "VEGAGRAPHICS" 这种连写去匹配。
    //           Vega 的独显型号一定带 RX(RX Vega 56/64)或 Pro 前缀,前面已经排除;
    //           剩下的 "Radeon Vega Frontier Edition" 这种没有 Graphics 字样,也进不来
    if (s.contains(QLatin1String("VEGA")) && s.contains(QLatin1String("GRAPHICS")))
        return true;

    // 白名单三:近几代 APU 的核显型号,"Radeon 680M" / "780M" / "890M",
    //           形如 <RADEON><三位数字>M 结尾。带 M 的独显移动版一定还有
    //           RX / Pro 前缀,前面已经排除了
    const int n = s.size();
    if (n >= 5 && s.endsWith(QLatin1Char('M'))) {
        const bool threeDigits =
            s.at(n - 4).isDigit() && s.at(n - 3).isDigit() && s.at(n - 2).isDigit();
        if (threeDigits && s.left(n - 4).endsWith(QLatin1String("RADEON")))
            return true;
    }

    return false;
}

// 温度数值到底是哪条路读出来的。CPU 温度代理必须来自 ADL(AMD),
// 因为只有 AMD 的核显能和 CPU 共处一颗 die;NVIDIA 的读数永远是独显
enum class ThermalBackend { None, Adl, Nvml, Nvapi };

struct Reading {
    double celsius = -1.0;
    ThermalBackend backend = ThermalBackend::None;
};

bool tempLooksSane(int celsius)
{
    return celsius >= kMinTempC && celsius <= kMaxTempC;
}

} // namespace

// ---------------------------------------------------------------- Impl

struct ThermalProvider::Impl {
    QVector<ThermalTarget> targets;

    // ---- ADL
    HMODULE adlDll = nullptr;
    void *adlContext = nullptr;
    AdlMainControlDestroy adlDestroy = nullptr;
    AdlOverdriveNTemperatureGet adlOverdriveNTemp = nullptr;
    AdlQueryPmLogDataGet adlPmLog = nullptr;
    struct AdlEntry {
        int index = 0;
        QString name;
    };
    QVector<AdlEntry> adlAdapters;

    // ---- NVML
    HMODULE nvmlDll = nullptr;
    NvmlShutdown nvmlShutdown = nullptr;
    NvmlDeviceGetTemperature nvmlGetTemp = nullptr;
    QVector<void *> nvmlDevices;
    QVector<QString> nvmlNames;

    // ---- NVAPI
    HMODULE nvapiDll = nullptr;
    NvapiGpuGetThermalSettings nvapiGetThermal = nullptr;
    QVector<void *> nvapiGpus;
    QVector<QString> nvapiNames;

    bool loaded = false;
    QString summary;

    mutable QVector<Reading> cache;
    mutable qint64 cacheMs = 0;

    bool loadAdl();
    bool loadNvml();
    bool loadNvapi();

    double adlTemperature(int adapterIndex) const;
    Reading readOne(const ThermalTarget &target) const;
    void refreshCache() const;
};

// ---------------------------------------------------------------- ADL 初始化

bool ThermalProvider::Impl::loadAdl()
{
    HMODULE dll = LoadLibraryW(L"atiadlxx.dll");
    if (!dll)
        return false;

    auto create = reinterpret_cast<AdlMainControlCreate>(GetProcAddress(dll, "ADL2_Main_Control_Create"));
    auto numberGet = reinterpret_cast<AdlAdapterNumberGet>(
        GetProcAddress(dll, "ADL2_Adapter_NumberOfAdapters_Get"));
    auto infoGet = reinterpret_cast<AdlAdapterInfoGet>(GetProcAddress(dll, "ADL2_Adapter_AdapterInfo_Get"));
    adlDestroy = reinterpret_cast<AdlMainControlDestroy>(GetProcAddress(dll, "ADL2_Main_Control_Destroy"));
    adlOverdriveNTemp = reinterpret_cast<AdlOverdriveNTemperatureGet>(
        GetProcAddress(dll, "ADL2_OverdriveN_Temperature_Get"));
    adlPmLog = reinterpret_cast<AdlQueryPmLogDataGet>(GetProcAddress(dll, "ADL2_New_QueryPMLogData_Get"));

    if (!create || !numberGet || !infoGet || (!adlOverdriveNTemp && !adlPmLog)) {
        FreeLibrary(dll);
        return false;
    }

    void *ctx = nullptr;
    if (create(adlMalloc, 1, &ctx) != kAdlOk || !ctx) {
        FreeLibrary(dll);
        return false;
    }

    int count = 0;
    if (numberGet(ctx, &count) != kAdlOk || count <= 0) {
        if (adlDestroy)
            adlDestroy(ctx);
        FreeLibrary(dll);
        return false;
    }

    std::vector<AdlAdapterInfo> infos(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        infos[size_t(i)].iSize = sizeof(AdlAdapterInfo);
    if (infoGet(ctx, infos.data(), int(sizeof(AdlAdapterInfo) * size_t(count))) != kAdlOk) {
        if (adlDestroy)
            adlDestroy(ctx);
        FreeLibrary(dll);
        return false;
    }

    adlDll = dll;
    adlContext = ctx;
    for (int i = 0; i < count; ++i) {
        if (!infos[size_t(i)].iPresent)
            continue;
        AdlEntry e;
        e.index = infos[size_t(i)].iAdapterIndex;
        e.name = QString::fromLatin1(infos[size_t(i)].strAdapterName);
        adlAdapters.push_back(e);
    }
    return !adlAdapters.isEmpty();
}

// ---------------------------------------------------------------- NVML 初始化

bool ThermalProvider::Impl::loadNvml()
{
    HMODULE dll = LoadLibraryW(L"nvml.dll");
    // 老驱动把 nvml.dll 放在 NVSMI 目录里,系统目录找不到就再试一次
    if (!dll) {
        dll = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        if (!dll)
            return false;
    }

    auto init = reinterpret_cast<NvmlInit>(GetProcAddress(dll, "nvmlInit_v2"));
    nvmlShutdown = reinterpret_cast<NvmlShutdown>(GetProcAddress(dll, "nvmlShutdown"));
    auto countGet = reinterpret_cast<NvmlDeviceGetCount>(GetProcAddress(dll, "nvmlDeviceGetCount_v2"));
    auto handleGet = reinterpret_cast<NvmlDeviceGetHandleByIndex>(
        GetProcAddress(dll, "nvmlDeviceGetHandleByIndex_v2"));
    auto nameGet = reinterpret_cast<NvmlDeviceGetName>(GetProcAddress(dll, "nvmlDeviceGetName"));
    nvmlGetTemp = reinterpret_cast<NvmlDeviceGetTemperature>(GetProcAddress(dll, "nvmlDeviceGetTemperature"));

    if (!init || !countGet || !handleGet || !nvmlGetTemp || init() != 0) {
        FreeLibrary(dll);
        return false;
    }

    unsigned int n = 0;
    if (countGet(&n) != 0 || n == 0) {
        if (nvmlShutdown)
            nvmlShutdown();
        FreeLibrary(dll);
        return false;
    }

    for (unsigned int i = 0; i < n; ++i) {
        void *dev = nullptr;
        if (handleGet(i, &dev) != 0 || !dev)
            continue;
        nvmlDevices.push_back(dev);

        QString name;
        if (nameGet) {
            char buf[96] = {};
            if (nameGet(dev, buf, sizeof(buf)) == 0)
                name = QString::fromLatin1(buf);
        }
        nvmlNames.push_back(name);
    }

    if (nvmlDevices.isEmpty()) {
        if (nvmlShutdown)
            nvmlShutdown();
        FreeLibrary(dll);
        return false;
    }

    nvmlDll = dll;
    return true;
}

// ---------------------------------------------------------------- NVAPI 初始化

bool ThermalProvider::Impl::loadNvapi()
{
    HMODULE dll = LoadLibraryW(L"nvapi64.dll");
    if (!dll)
        dll = LoadLibraryW(L"nvapi.dll");
    if (!dll)
        return false;

    auto query = reinterpret_cast<NvapiQueryInterface>(GetProcAddress(dll, "nvapi_QueryInterface"));
    if (!query) {
        FreeLibrary(dll);
        return false;
    }

    auto init = reinterpret_cast<NvapiInitialize>(query(0x0150E828));
    auto enumGpus = reinterpret_cast<NvapiEnumPhysicalGpus>(query(0xE5AC921F));
    nvapiGetThermal = reinterpret_cast<NvapiGpuGetThermalSettings>(query(0xE3640A56));
    auto getFullName = reinterpret_cast<NvapiGpuGetFullName>(query(0xCEEE8E9F));

    if (!init || !enumGpus || !nvapiGetThermal || init() != 0) {
        nvapiGetThermal = nullptr;
        FreeLibrary(dll);
        return false;
    }

    void *gpus[64] = {};
    int gpuCount = 0;
    if (enumGpus(gpus, &gpuCount) != 0 || gpuCount <= 0) {
        nvapiGetThermal = nullptr;
        FreeLibrary(dll);
        return false;
    }
    gpuCount = qMin(gpuCount, 64);

    for (int i = 0; i < gpuCount; ++i) {
        nvapiGpus.push_back(gpus[i]);
        QString name;
        if (getFullName) {
            char buf[64] = {};
            if (getFullName(gpus[i], buf) == 0)
                name = QString::fromLatin1(buf);
        }
        nvapiNames.push_back(name);
    }

    nvapiDll = dll;
    return true;
}

// ---------------------------------------------------------------- 读温度

double ThermalProvider::Impl::adlTemperature(int adapterIndex) const
{
    if (!adlContext)
        return -1.0;

    // OverdriveN 是新接口,EDGE 是核心边缘温度(单位:毫摄氏度)
    if (adlOverdriveNTemp) {
        int value = -1;
        if (adlOverdriveNTemp(adlContext, adapterIndex, kAdlOdnTempEdge, &value) == kAdlOk) {
            const int celsius = value / 1000;
            if (tempLooksSane(celsius))
                return double(celsius);
        }
    }

    // 退回 PMLog:传感器表里 sensor[8] 是 EDGE 温度(单位:摄氏度)
    if (adlPmLog) {
        std::vector<unsigned char> buf(size_t(kAdlPmLogBufSize), 0);
        *reinterpret_cast<int *>(buf.data()) = kAdlPmLogBufSize;
        if (adlPmLog(adlContext, adapterIndex, buf.data()) == kAdlOk) {
            const auto *ints = reinterpret_cast<const int *>(buf.data());
            const int total = kAdlPmLogBufSize / 4;
            const int s = kAdlPmLogTempEdge;
            // 布局:[0]=iSize,之后每 2 个 int 一个传感器:{supported, value}
            if (s * 2 + 2 < total) {
                const unsigned supported = static_cast<unsigned>(ints[s * 2 + 1]);
                const int value = ints[s * 2 + 2];
                if (supported != 0 && tempLooksSane(value))
                    return double(value);
            }
        }
    }

    return -1.0;
}

Reading ThermalProvider::Impl::readOne(const ThermalTarget &target) const
{
    Reading result;
    if (target.name.isEmpty())
        return result;

    // ---- AMD:ADL 的条目是"显示适配器",一块卡会拆成好几条,
    //      所以只能按型号名匹配,不能拿序号对应
    if (!adlAdapters.isEmpty() && (adlOverdriveNTemp || adlPmLog)) {
        for (const AdlEntry &e : adlAdapters) {
            if (!nameMatches(e.name, target.name))
                continue;
            const double t = adlTemperature(e.index);
            if (t >= 0.0) {
                result.celsius = t;
                result.backend = ThermalBackend::Adl;
                return result;
            }
            // 名字对上了但读不到,说明这块卡不走 ADL(NVIDIA 的条目也会出现在
            // ADL 列表里,一律返回 ADL_ERR_NOT_SUPPORTED),别再试同名的其它条目
            break;
        }
    }

    // ---- NVIDIA:NVML 优先,它能顺带报出型号名
    if (nvmlGetTemp) {
        int idx = -1;
        for (int i = 0; i < nvmlNames.size(); ++i) {
            if (nameMatches(nvmlNames.at(i), target.name)) {
                idx = i;
                break;
            }
        }
        // 只有一块 NVIDIA 卡时,名字对不上也认了(驱动版本差异)
        if (idx < 0 && nvmlDevices.size() == 1 && looksNvidia(target.name))
            idx = 0;
        if (idx >= 0) {
            unsigned int value = 0;
            if (nvmlGetTemp(nvmlDevices.at(idx), kNvmlTemperatureGpu, &value) == 0
                && tempLooksSane(int(value))) {
                result.celsius = double(value);
                result.backend = ThermalBackend::Nvml;
                return result;
            }
        }
    }

    // ---- NVIDIA:NVAPI 兜底
    if (nvapiGetThermal) {
        int idx = -1;
        for (int i = 0; i < nvapiNames.size(); ++i) {
            if (nameMatches(nvapiNames.at(i), target.name)) {
                idx = i;
                break;
            }
        }
        if (idx < 0 && nvapiGpus.size() == 1 && looksNvidia(target.name))
            idx = 0;
        if (idx >= 0) {
            NvapiThermalSettings ts{};
            ts.version = nvapiVersion<NvapiThermalSettings>(2);
            if (nvapiGetThermal(nvapiGpus.at(idx), 0, &ts) == 0) {
                for (int s = 0; s < ts.count && s < kNvapiMaxThermalSensors; ++s) {
                    // 只认 GPU 核心的传感器,PCB / 显存的不算
                    if (ts.sensor[s].target == kNvapiThermalTargetGpu
                        && tempLooksSane(ts.sensor[s].currentTemp)) {
                        result.celsius = double(ts.sensor[s].currentTemp);
                        result.backend = ThermalBackend::Nvapi;
                        return result;
                    }
                }
            }
        }
    }

    return result;
}

void ThermalProvider::Impl::refreshCache() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (cache.size() == targets.size() && now - cacheMs < kCacheTtlMs)
        return;

    cacheMs = now;
    cache.resize(targets.size());
    for (int i = 0; i < targets.size(); ++i)
        cache[i] = readOne(targets.at(i));
}

// ---------------------------------------------------------------- 对外接口

ThermalProvider::ThermalProvider()
    : m_impl(new Impl)
{
}

ThermalProvider::~ThermalProvider()
{
    // 这里刻意不调 ADL2_Main_Control_Destroy / nvmlShutdown,也不 FreeLibrary:
    // ThermalProvider 是采样线程的成员,但析构发生在 UI 线程(采样线程已经退出),
    // 而 ADL 上下文是在采样线程里建的。跨线程做厂商 SDK 的清理属于"出错就静默
    // 卡死"的操作,而这是进程退出路径,系统马上就会回收,不值得冒这个险。
    // 另外 NVAPI 根本没有反初始化接口,本来就只能留给进程回收。
    delete m_impl;
    m_impl = nullptr;
}

bool ThermalProvider::load()
{
    if (m_impl->loaded)
        return m_impl->nvmlGetTemp || !m_impl->adlAdapters.isEmpty() || m_impl->nvapiGetThermal;
    m_impl->loaded = true;

    // 自己枚举一遍适配器,而不是等 GpuSampler 推过来 —— 否则 CPU 采样只要早于
    // GPU 采样一次,核显温度就是空的,这种"谁先谁后"的隐式依赖太难查
    for (const GpuAdapterDesc &a : GpuSampler::adapters()) {
        if (a.software || a.name.isEmpty())
            continue;
        ThermalTarget target;
        target.name = a.name;
        target.integrated = a.integrated;
        m_impl->targets.push_back(target);
    }

    // NVAPI / ADL 都要求当前线程初始化过 COM,没初始化的话会创建失败,
    // 而且失败得很安静
    ComInitializer com;

    QStringList backends;
    if (m_impl->loadAdl())
        backends << QStringLiteral("ADL");
    if (m_impl->loadNvml())
        backends << QStringLiteral("NVML");
    if (m_impl->loadNvapi())
        backends << QStringLiteral("NVAPI");

    m_impl->summary = backends.isEmpty() ? QStringLiteral("无(温度不可用)") : backends.join(QStringLiteral(" + "));
    return !backends.isEmpty();
}

double ThermalProvider::temperatureC(int index) const
{
    m_impl->refreshCache();
    if (index < 0 || index >= m_impl->cache.size())
        return -1.0;
    return m_impl->cache.at(index).celsius;
}

bool ThermalProvider::nameLooksLikeApuIntegrated(const QString &adapterName)
{
    return looksLikeApuIntegratedName(adapterName);
}

double ThermalProvider::cpuProxyTemperatureC() const
{
    m_impl->refreshCache();
    const int n = qMin(m_impl->targets.size(), m_impl->cache.size());
    for (int i = 0; i < n; ++i) {
        const Reading &reading = m_impl->cache.at(i);
        if (reading.celsius < 0.0)
            continue;
        // 两条同时成立才算数:读数确实出自 ADL,且型号名是 AMD APU 的核显。
        // 少任何一条都返回 -1 让界面留空 —— 宁可空着,不能拿独显温度冒充 CPU 温度
        if (reading.backend != ThermalBackend::Adl)
            continue;
        if (!looksLikeApuIntegratedName(m_impl->targets.at(i).name))
            continue;
        return reading.celsius;
    }
    return -1.0;
}

QString ThermalProvider::backendSummary() const
{
    return m_impl->summary;
}

} // namespace ws
