// 确认 PDH 的 GPU 实例名里的 luid 和 DXGI 适配器怎么对应。
// 关键问题:实例名 luid_0xAAAAAAAA_0xBBBBBBBB 里,哪个是 HighPart、哪个是 LowPart。
//
// 这是当初为了做「核显 + 独显分开显示」写的一次性探针,结论已经固化进
// src/core/GpuSampler.cpp 的 parseLuid();留着是因为换机器/换驱动时,
// 万一多显卡归属不对,可以第一时间拿它对比两边的 luid。
//
// 编译(不依赖 Qt,只要 MinGW):
//   g++ -O2 -o luidtest.exe luidtest.cpp -ldxgi -lpdh -lole32
// 运行:
//   ./luidtest.exe
//
// 实测结论(AMD 核显 + RTX 2060 的笔记本):
//   [0] AMD Radeon(TM) Graphics   luid.High=0x00000000 luid.Low=0x0000E7F7 vram=496 MB
//   [1] NVIDIA GeForce RTX 2060   luid.High=0x00000000 luid.Low=0x000105F2 vram=5955 MB
//   GPU Adapter Memory(*)\Dedicated Usage:
//     luid_0x00000000_0x000105F2_phys_0 = 498769920
//     luid_0x00000000_0x0000E7F7_phys_0 = 406056960
// 即第一段是 HighPart、第二段是 LowPart,且 GPU Adapter Memory 直接按适配器给值。
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#include <cstdio>
#include <string>
#include <vector>

static const GUID kIID_IDXGIFactory1 = { 0x770AAE78, 0xF26F, 0x4DBA, { 0xA8, 0x29, 0x25, 0x3C, 0x83, 0xD1, 0xB3, 0x87 } };

static void printDxgi()
{
    printf("=== DXGI 适配器 ===\n");
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(kIID_IDXGIFactory1, reinterpret_cast<void **>(&factory))) || !factory) {
        printf("CreateDXGIFactory1 失败\n");
        return;
    }
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK || !adapter)
            break;
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d))) {
            printf("  [%u] %-45ls luid.High=0x%08lX luid.Low=0x%08lX vram=%llu MB software=%d\n",
                   i, d.Description, d.AdapterLuid.HighPart, d.AdapterLuid.LowPart,
                   (unsigned long long)(d.DedicatedVideoMemory / (1024 * 1024)),
                   (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? 1 : 0);
        }
        adapter->Release();
    }
    factory->Release();
}

static void printPdhInstances(const wchar_t *path, const char *label)
{
    printf("=== PDH %s ===\n", label);
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
        printf("  PdhOpenQuery 失败\n");
        return;
    }
    PDH_HCOUNTER counter = nullptr;
    PDH_STATUS st = PdhAddEnglishCounterW(query, path, 0, &counter);
    if (st != ERROR_SUCCESS)
        st = PdhAddCounterW(query, path, 0, &counter);
    if (st != ERROR_SUCCESS) {
        printf("  添加计数器失败 0x%08lX\n", (unsigned long)st);
        PdhCloseQuery(query);
        return;
    }
    PdhCollectQueryData(query);

    DWORD size = 0;
    DWORD count = 0;
    PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (size == 0) {
        printf("  没有实例\n");
        PdhCloseQuery(query);
        return;
    }
    std::vector<BYTE> buf(size);
    auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buf.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) {
        printf("  取值失败\n");
        PdhCloseQuery(query);
        return;
    }

    int shown = 0;
    for (DWORD i = 0; i < count && shown < 12; ++i) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
            continue;
        printf("  %-70ls = %.2f\n", items[i].szName, items[i].FmtValue.doubleValue);
        ++shown;
    }
    printf("  (共 %lu 个实例)\n", (unsigned long)count);
    PdhCloseQuery(query);
}

int main()
{
    printDxgi();
    printPdhInstances(L"\\GPU Engine(*)\\Utilization Percentage", "GPU Engine(*)\\Utilization Percentage");
    printPdhInstances(L"\\GPU Adapter Memory(*)\\Dedicated Usage", "GPU Adapter Memory(*)\\Dedicated Usage");
    printPdhInstances(L"\\GPU Adapter Memory(*)\\Dedicated Limit", "GPU Adapter Memory(*)\\Dedicated Limit");
    return 0;
}
