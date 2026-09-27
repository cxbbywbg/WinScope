#include "core/MemorySampler.h"

#include <windows.h>
#include <psapi.h>

namespace ws {

MemoryInfo MemorySampler::sample()
{
    MemoryInfo info;

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        info.totalBytes = ms.ullTotalPhys;
        info.availableBytes = ms.ullAvailPhys;
        info.usedBytes = info.totalBytes > info.availableBytes ? info.totalBytes - info.availableBytes : 0;
        info.pageFileTotalBytes = ms.ullTotalPageFile;
        info.pageFileUsedBytes = ms.ullTotalPageFile > ms.ullAvailPageFile ? ms.ullTotalPageFile - ms.ullAvailPageFile : 0;
        if (info.totalBytes > 0)
            info.usagePercent = 100.0 * double(info.usedBytes) / double(info.totalBytes);
    }

    PERFORMANCE_INFORMATION pi{};
    pi.cb = sizeof(pi);
    if (GetPerformanceInfo(&pi, sizeof(pi))) {
        const quint64 page = pi.PageSize;
        info.committedBytes = quint64(pi.CommitTotal) * page;
        info.commitLimitBytes = quint64(pi.CommitLimit) * page;
        info.cachedBytes = quint64(pi.SystemCache) * page;
        info.pagedPoolBytes = quint64(pi.KernelPaged) * page;
        info.nonPagedPoolBytes = quint64(pi.KernelNonpaged) * page;

        // 物理内存以 PerformanceInfo 为准更贴近任务管理器的读数
        if (pi.PhysicalTotal > 0 && info.totalBytes == 0) {
            info.totalBytes = quint64(pi.PhysicalTotal) * page;
            info.availableBytes = quint64(pi.PhysicalAvailable) * page;
            info.usedBytes = info.totalBytes - info.availableBytes;
            info.usagePercent = 100.0 * double(info.usedBytes) / double(info.totalBytes);
        }
    }

    return info;
}

} // namespace ws
