// 温度来源探针。
//
// Windows 没有通用的 CPU / GPU 温度接口,能不能读到完全看机器和驱动。这个程序
// 把「所有可能的路子」按代价从低到高试一遍,把返回码和原始值都打出来,
// 用来判断在具体某台机器上该走哪条路。
//
// 试的顺序:
//   1. PDH  \Thermal Zone Information(*)\Temperature      ACPI 热区,不用厂商 SDK
//   2. WMI  MSAcpi_ThermalZoneTemperature                 对照用
//   3. NVAPI  nvapi64.dll  (NVIDIA 驱动自带)               独显温度
//   4. ADL    atiadlxx.dll (AMD 驱动自带)                  AMD 显卡/核显温度
//   5. NVML   nvml.dll                                     独显温度的另一种读法
//
// 编译(不依赖 Qt):
//   g++ -O2 -o thermalprobe.exe thermalprobe.cpp -lpdh -lole32 -loleaut32 -lwbemuuid -ldxgi
// 运行:
//   ./thermalprobe.exe

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>

#define _WIN32_DCOM
#include <objbase.h>
#include <oleauto.h>
#include <wbemidl.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------- 小工具

static void hr(const char *what, long rc)
{
    printf("  %-46s rc=0x%08lX (%ld)%s\n", what, (unsigned long)rc, rc, rc == 0 ? "  OK" : "");
}

static std::string narrow(const wchar_t *w)
{
    if (!w)
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------- DXGI

static const GUID kIID_IDXGIFactory1 = { 0x770AAE78, 0xF26F, 0x4DBA, { 0xA8, 0x29, 0x25, 0x3C, 0x83, 0xD1, 0xB3, 0x87 } };

static void printDxgi()
{
    printf("\n=== DXGI 适配器(界面上的 GPU 0/1 就是这个顺序)===\n");
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(kIID_IDXGIFactory1, reinterpret_cast<void **>(&factory))) || !factory) {
        printf("  CreateDXGIFactory1 失败\n");
        return;
    }
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK || !adapter)
            break;
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d))) {
            printf("  [%u] %-36s luid.High=0x%08lX luid.Low=0x%08lX vram=%llu MB%s\n", i,
                   narrow(d.Description).c_str(), (unsigned long)d.AdapterLuid.HighPart,
                   (unsigned long)d.AdapterLuid.LowPart,
                   (unsigned long long)(d.DedicatedVideoMemory / 1024 / 1024),
                   (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "  [software]" : "");
        }
        adapter->Release();
    }
    factory->Release();
}

// ---------------------------------------------------------------- PDH 热区

static void printPdhThermal()
{
    printf("\n=== PDH:Thermal Zone Information ===\n");

    static const wchar_t *paths[] = {
        L"\\Thermal Zone Information(*)\\Temperature",
        L"\\Thermal Zone Information(*)\\High Precision Temperature",
    };

    for (const wchar_t *path : paths) {
        PDH_HQUERY query = nullptr;
        PDH_HCOUNTER counter = nullptr;
        printf("  --- %ls\n", path);

        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            printf("      PdhOpenQuery 失败\n");
            continue;
        }
        // 中文系统上计数器名会本地化,必须用英文名接口
        PDH_STATUS st = PdhAddEnglishCounterW(query, path, 0, &counter);
        if (st != ERROR_SUCCESS) {
            printf("      PdhAddEnglishCounter 失败 0x%08lX\n", (unsigned long)st);
            PdhCloseQuery(query);
            continue;
        }
        PdhCollectQueryData(query);
        Sleep(400);
        PdhCollectQueryData(query);

        DWORD size = 0;
        DWORD count = 0;
        st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, nullptr);
        if (st != PDH_MORE_DATA || size == 0) {
            printf("      没有实例(st=0x%08lX size=%lu)\n", (unsigned long)st, (unsigned long)size);
            PdhCloseQuery(query);
            continue;
        }
        std::vector<BYTE> buf(size);
        auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buf.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) {
            printf("      取值失败\n");
            PdhCloseQuery(query);
            continue;
        }
        for (DWORD i = 0; i < count; ++i) {
            printf("      %-40ls = %.3f\n", items[i].szName, items[i].FmtValue.doubleValue);
        }
        printf("      (共 %lu 个实例)\n", (unsigned long)count);
        PdhCloseQuery(query);
    }
}

// ---------------------------------------------------------------- WMI 对照

struct ComInit {
    bool owned = false;
    ComInit()
    {
        HRESULT r = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned = SUCCEEDED(r);
    }
    ~ComInit()
    {
        if (owned)
            CoUninitialize();
    }
};

static void printWmiThermal()
{
    printf("\n=== WMI:MSAcpi_ThermalZoneTemperature(对照,当前程序用的就是它)===\n");

    IWbemLocator *locator = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                reinterpret_cast<void **>(&locator)))
        || !locator) {
        printf("  CoCreateInstance 失败\n");
        return;
    }
    IWbemServices *services = nullptr;
    if (FAILED(locator->ConnectServer(BSTR(L"ROOT\\WMI"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services))
        || !services) {
        printf("  ConnectServer ROOT\\WMI 失败\n");
        locator->Release();
        return;
    }
    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    IEnumWbemClassObject *en = nullptr;
    if (SUCCEEDED(services->ExecQuery(BSTR(L"WQL"),
                                      BSTR(L"SELECT InstanceName, CurrentTemperature FROM MSAcpi_ThermalZoneTemperature"),
                                      WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en))
        && en) {
        int n = 0;
        for (;;) {
            IWbemClassObject *obj = nullptr;
            ULONG got = 0;
            if (en->Next(3000, 1, &obj, &got) != S_OK || got == 0 || !obj)
                break;
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(obj->Get(BSTR(L"CurrentTemperature"), 0, &v, nullptr, nullptr)) && v.vt == VT_I4) {
                printf("  实例温度 %.1f °C(原始 %ld 十分之一开尔文)\n", v.lVal / 10.0 - 273.15, (long)v.lVal);
                ++n;
            }
            VariantClear(&v);
            obj->Release();
        }
        printf("  共 %d 个实例%s\n", n, n == 0 ? "  —— 这台机器没有实现 ACPI 热区" : "");
        en->Release();
    } else {
        printf("  ExecQuery 失败\n");
    }
    services->Release();
    locator->Release();
}

// ---------------------------------------------------------------- NVAPI

// 注意:nvapi_QueryInterface 返回的是**函数指针**,x64 上必须用 void* 接。
// 写成 int 会把高 32 位截断,拿到的地址是垃圾,一调就崩。
typedef void *(*NvAPI_QueryInterface_t)(unsigned int);
typedef int (*NvAPI_Initialize_t)();
typedef int (*NvAPI_EnumPhysicalGPUs_t)(void **, int *);
typedef int (*NvAPI_GPU_GetThermalSettings_t)(void *, unsigned int, void *);

#define NVAPI_MAX_THERMAL_SENSORS_PER_GPU 3
#define NVAPI_THERMAL_TARGET_GPU 1

struct NvThermalSensor {
    int controller;
    int defaultMinTemp;
    int defaultMaxTemp;
    int currentTemp;
    int target;
};

struct NvThermalSettings {
    unsigned int version;
    int count;
    NvThermalSensor sensor[NVAPI_MAX_THERMAL_SENSORS_PER_GPU];
};

#define NVAPI_VERSION(T, v) ((unsigned int)(sizeof(T) | ((v) << 16)))

static void printNvapi()
{
    printf("\n=== NVAPI:nvapi64.dll(独显温度)===\n");

    HMODULE dll = LoadLibraryW(L"nvapi64.dll");
    if (!dll) {
        printf("  加载 nvapi64.dll 失败\n");
        return;
    }
    printf("  nvapi64.dll 已加载\n");

    auto query = reinterpret_cast<NvAPI_QueryInterface_t>(GetProcAddress(dll, "nvapi_QueryInterface"));
    if (!query) {
        printf("  没有 nvapi_QueryInterface 导出\n");
        FreeLibrary(dll);
        return;
    }

    auto init = reinterpret_cast<NvAPI_Initialize_t>(query(0x0150E828));
    auto enumGpus = reinterpret_cast<NvAPI_EnumPhysicalGPUs_t>(query(0xE5AC921F));
    auto getThermal = reinterpret_cast<NvAPI_GPU_GetThermalSettings_t>(query(0xE3640A56));
    printf("  函数地址: Initialize=%p EnumPhysicalGPUs=%p GetThermalSettings=%p\n",
           (void *)init, (void *)enumGpus, (void *)getThermal);
    if (!init || !enumGpus || !getThermal) {
        FreeLibrary(dll);
        return;
    }

    hr("NvAPI_Initialize", init());

    void *gpus[64] = {};
    int gpuCount = 0;
    long rc = enumGpus(gpus, &gpuCount);
    hr("NvAPI_EnumPhysicalGPUs", rc);
    printf("  物理 GPU 数 = %d\n", gpuCount);

    for (int i = 0; i < gpuCount && i < 64; ++i) {
        NvThermalSettings ts{};
        ts.version = NVAPI_VERSION(NvThermalSettings, 2);
        rc = getThermal(gpus[i], 0, &ts);
        printf("  GPU[%d] GetThermalSettings rc=0x%08lX count=%d\n", i, (unsigned long)rc, ts.count);
        for (int s = 0; s < ts.count && s < NVAPI_MAX_THERMAL_SENSORS_PER_GPU; ++s) {
            printf("      sensor[%d] target=%d currentTemp=%d °C (min %d / max %d)\n", s, ts.sensor[s].target,
                   ts.sensor[s].currentTemp, ts.sensor[s].defaultMinTemp, ts.sensor[s].defaultMaxTemp);
        }
    }
    FreeLibrary(dll);
}

// ---------------------------------------------------------------- ADL

#define ADL_MAX_PATH 256

typedef void *(__stdcall *ADL_MAIN_MALLOC_CALLBACK)(int);

struct AdlAdapterInfo {
    int iSize;
    int iAdapterIndex;
    char strUDID[ADL_MAX_PATH];
    int iBusNumber;
    int iDeviceNumber;
    int iFunctionNumber;
    int iVendorID;
    char strAdapterName[ADL_MAX_PATH];
    char strDisplayName[ADL_MAX_PATH];
    int iPresent;
    int iExist;
    char strDriverPath[ADL_MAX_PATH];
    char strDriverPathExt[ADL_MAX_PATH];
    char strPNPString[ADL_MAX_PATH];
    int iOSDisplayIndex;
};

struct AdlTemperature {
    int iSize;
    int iTemperature;   // 毫摄氏度
};

typedef int(__stdcall *ADL2_Main_Control_Create_t)(ADL_MAIN_MALLOC_CALLBACK, int, void **);
typedef int(__stdcall *ADL2_Main_Control_Destroy_t)(void *);
typedef int(__stdcall *ADL2_Adapter_NumberOfAdapters_Get_t)(void *, int *);
typedef int(__stdcall *ADL2_Adapter_AdapterInfo_Get_t)(void *, void *, int);
typedef int(__stdcall *ADL2_Overdrive5_Temperature_Get_t)(void *, int, int, AdlTemperature *);
typedef int(__stdcall *ADL2_OverdriveN_Temperature_Get_t)(void *, int, int, int *);
typedef int(__stdcall *ADL2_New_QueryPMLogData_Get_t)(void *, int, void *);

// PMLog 传感器下标(ADL SDK 的 ADL_PMLOG_* 常量)
#define ADL_PMLOG_TEMPERATURE_EDGE 8
#define ADL_PMLOG_TEMPERATURE_MEM 9
#define ADL_PMLOG_TEMPERATURE_SOC 14
#define ADL_PMLOG_TEMPERATURE_HOTSPOT 15

static void *__stdcall adlMalloc(int size)
{
    return malloc(size_t(size));
}

static void printAdl()
{
    printf("\n=== ADL:atiadlxx.dll(AMD 显卡 / 核显温度)===\n");

    HMODULE dll = LoadLibraryW(L"atiadlxx.dll");
    if (!dll) {
        printf("  加载 atiadlxx.dll 失败\n");
        return;
    }
    printf("  atiadlxx.dll 已加载\n");

    auto create = reinterpret_cast<ADL2_Main_Control_Create_t>(GetProcAddress(dll, "ADL2_Main_Control_Create"));
    auto destroy = reinterpret_cast<ADL2_Main_Control_Destroy_t>(GetProcAddress(dll, "ADL2_Main_Control_Destroy"));
    auto numAdapters = reinterpret_cast<ADL2_Adapter_NumberOfAdapters_Get_t>(
        GetProcAddress(dll, "ADL2_Adapter_NumberOfAdapters_Get"));
    auto adapterInfo = reinterpret_cast<ADL2_Adapter_AdapterInfo_Get_t>(
        GetProcAddress(dll, "ADL2_Adapter_AdapterInfo_Get"));
    auto tempGet = reinterpret_cast<ADL2_Overdrive5_Temperature_Get_t>(
        GetProcAddress(dll, "ADL2_Overdrive5_Temperature_Get"));
    auto odTempGet = reinterpret_cast<ADL2_OverdriveN_Temperature_Get_t>(
        GetProcAddress(dll, "ADL2_OverdriveN_Temperature_Get"));
    auto pmLogGet = reinterpret_cast<ADL2_New_QueryPMLogData_Get_t>(
        GetProcAddress(dll, "ADL2_New_QueryPMLogData_Get"));

    printf("  导出: Create=%p Destroy=%p NumAdapters=%p AdapterInfo=%p\n"
           "        Overdrive5_TempGet=%p OverdriveN_TempGet=%p PMLogGet=%p\n",
           (void *)create, (void *)destroy, (void *)numAdapters, (void *)adapterInfo, (void *)tempGet,
           (void *)odTempGet, (void *)pmLogGet);

    if (!create || !numAdapters || !adapterInfo) {
        FreeLibrary(dll);
        return;
    }

    void *ctx = nullptr;
    long rc = create(adlMalloc, 1, &ctx);
    hr("ADL2_Main_Control_Create", rc);
    if (rc != 0 || !ctx) {
        FreeLibrary(dll);
        return;
    }

    int count = 0;
    rc = numAdapters(ctx, &count);
    hr("ADL2_Adapter_NumberOfAdapters_Get", rc);
    printf("  适配器数 = %d\n", count);

    if (count > 0) {
        std::vector<AdlAdapterInfo> infos(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i)
            infos[size_t(i)].iSize = sizeof(AdlAdapterInfo);
        rc = adapterInfo(ctx, infos.data(), int(sizeof(AdlAdapterInfo) * size_t(count)));
        hr("ADL2_Adapter_AdapterInfo_Get", rc);

        for (int i = 0; i < count; ++i) {
            printf("  [%d] adapterIndex=%d vendor=0x%04X present=%d exist=%d\n", i, infos[size_t(i)].iAdapterIndex,
                   infos[size_t(i)].iVendorID, infos[size_t(i)].iPresent, infos[size_t(i)].iExist);
            printf("      name=\"%s\" display=\"%s\"\n", infos[size_t(i)].strAdapterName,
                   infos[size_t(i)].strDisplayName);
        }

        if (tempGet) {
            printf("  --- ADL2_Overdrive5_Temperature_Get\n");
            for (int i = 0; i < count; ++i) {
                if (!infos[size_t(i)].iPresent)
                    continue;
                for (int ctrl = 0; ctrl < 2; ++ctrl) {
                    AdlTemperature t{};
                    t.iSize = sizeof(AdlTemperature);
                    rc = tempGet(ctx, infos[size_t(i)].iAdapterIndex, ctrl, &t);
                    printf("  adapterIndex=%d ctrl=%d -> rc=0x%08lX temp=%.1f °C\n",
                           infos[size_t(i)].iAdapterIndex, ctrl, (unsigned long)rc, t.iTemperature / 1000.0);
                }
            }
        }

        // OverdriveN:较新的卡走这条,ADL_ODN_TEMP_EDGE = 1
        if (odTempGet) {
            printf("  --- ADL2_OverdriveN_Temperature_Get\n");
            static const struct { int id; const char *name; } kOdn[] = {
                { 1, "EDGE" }, { 2, "PLX" }, { 3, "HOTSPOT" }, { 4, "MEM" },
            };
            for (int i = 0; i < count; ++i) {
                if (!infos[size_t(i)].iPresent)
                    continue;
                for (const auto &t : kOdn) {
                    int value = -1;
                    rc = odTempGet(ctx, infos[size_t(i)].iAdapterIndex, t.id, &value);
                    printf("  adapterIndex=%d %-8s -> rc=0x%08lX value=%d\n",
                           infos[size_t(i)].iAdapterIndex, t.name, (unsigned long)rc, value);
                }
            }
        }

        // PMLog:AMD 现在的标准读法,sensors[] 里每个传感器有 supported 标志
        if (pmLogGet) {
            printf("  --- ADL2_New_QueryPMLogData_Get(只打有值的传感器)\n");
            const int kBufSize = 4096;
            for (int i = 0; i < count; ++i) {
                if (!infos[size_t(i)].iPresent)
                    continue;

                std::vector<unsigned char> buf(size_t(kBufSize), 0);
                *reinterpret_cast<int *>(buf.data()) = kBufSize;
                rc = pmLogGet(ctx, infos[size_t(i)].iAdapterIndex, buf.data());
                printf("  adapterIndex=%d rc=0x%08lX\n", infos[size_t(i)].iAdapterIndex, (unsigned long)rc);
                if (rc != 0)
                    continue;

                // iSize 之后是 sensors[]:{ unsigned supported; int value; }
                const auto *ints = reinterpret_cast<const int *>(buf.data());
                const int total = kBufSize / 4;
                for (int s = 0; s * 2 + 2 < total && s < 64; ++s) {
                    const unsigned supported = static_cast<unsigned>(ints[s * 2 + 1]);
                    const int value = ints[s * 2 + 2];
                    if (supported == 0)
                        continue;
                    const char *tag = "";
                    if (s == ADL_PMLOG_TEMPERATURE_EDGE) tag = "  <- EDGE 温度";
                    else if (s == ADL_PMLOG_TEMPERATURE_MEM) tag = "  <- MEM 温度";
                    else if (s == ADL_PMLOG_TEMPERATURE_SOC) tag = "  <- SOC 温度";
                    else if (s == ADL_PMLOG_TEMPERATURE_HOTSPOT) tag = "  <- HOTSPOT 温度";
                    printf("      sensor[%2d] supported=0x%08X value=%d%s\n", s, supported, value, tag);
                }
            }
        }
    }

    if (destroy)
        destroy(ctx);
    FreeLibrary(dll);
}

// ---------------------------------------------------------------- NVML

typedef int (*nvmlInit_t)();
typedef int (*nvmlShutdown_t)();
typedef int (*nvmlDeviceGetCount_t)(unsigned int *);
typedef int (*nvmlDeviceGetHandleByIndex_t)(unsigned int, void **);
typedef int (*nvmlDeviceGetTemperature_t)(void *, int, unsigned int *);

#define NVML_TEMPERATURE_GPU 0

static void printNvml()
{
    printf("\n=== NVML:nvml.dll(独显温度的另一种读法)===\n");
    HMODULE dll = LoadLibraryW(L"nvml.dll");
    if (!dll) {
        printf("  加载 nvml.dll 失败\n");
        return;
    }
    auto init = reinterpret_cast<nvmlInit_t>(GetProcAddress(dll, "nvmlInit_v2"));
    auto shutdown = reinterpret_cast<nvmlShutdown_t>(GetProcAddress(dll, "nvmlShutdown"));
    auto getCount = reinterpret_cast<nvmlDeviceGetCount_t>(GetProcAddress(dll, "nvmlDeviceGetCount_v2"));
    auto getHandle = reinterpret_cast<nvmlDeviceGetHandleByIndex_t>(GetProcAddress(dll, "nvmlDeviceGetHandleByIndex_v2"));
    auto getTemp = reinterpret_cast<nvmlDeviceGetTemperature_t>(GetProcAddress(dll, "nvmlDeviceGetTemperature"));

    if (!init || !getCount || !getHandle || !getTemp) {
        printf("  nvml.dll 导出不全,跳过\n");
        FreeLibrary(dll);
        return;
    }
    hr("nvmlInit_v2", init());
    unsigned int n = 0;
    hr("nvmlDeviceGetCount_v2", getCount(&n));
    for (unsigned int i = 0; i < n; ++i) {
        void *dev = nullptr;
        if (getHandle(i, &dev) != 0)
            continue;
        unsigned int t = 0;
        long rc = getTemp(dev, NVML_TEMPERATURE_GPU, &t);
        printf("  device[%u] nvmlDeviceGetTemperature rc=0x%08lX temp=%u °C\n", i, (unsigned long)rc, t);
    }
    if (shutdown)
        shutdown();
    FreeLibrary(dll);
}

// ---------------------------------------------------------------- main

static bool isElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION e{};
    DWORD size = sizeof(e);
    const bool ok = GetTokenInformation(token, TokenElevation, &e, size, &size);
    CloseHandle(token);
    return ok && e.TokenIsElevated;
}

int main()
{
    // 探针本身可能因为某个厂商 SDK 崩掉,输出不能缓存在缓冲区里,不然看不到崩在哪
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("WinScope 温度来源探针\n");
    printf("管理员权限: %s\n", isElevated() ? "是" : "否");

    ComInit com;   // WMI / NVAPI 都可能要 COM
    printDxgi();
    printPdhThermal();
    printWmiThermal();
    printNvapi();
    printAdl();
    printNvml();

    printf("\n完成。\n");
    return 0;
}
