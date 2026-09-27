#include "core/ProcessSampler.h"

#include "core/NetProcessMonitor.h"
#include "core/Win32Utils.h"
#include "core/WmiQuery.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <vector>

#include <windows.h>
#include <winternl.h>

namespace ws {

namespace {

constexpr ULONG kSystemProcessInformation = 5;
constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004);

// winternl.h 里的 SYSTEM_PROCESS_INFORMATION 只有声明没有字段,自己补一份。
// 字段顺序与顺序不能改,内核就是按这个布局写缓冲区的。
struct SystemProcessInfo {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
};

// 从 1601-01-01 到 1970-01-01 的 100ns 数
constexpr quint64 kFileTimeToUnixEpoch = 116444736000000000ULL;

qint64 fileTimeToEpochMs(const LARGE_INTEGER &t)
{
    const quint64 v = static_cast<quint64>(t.QuadPart);
    if (v < kFileTimeToUnixEpoch)
        return 0;
    return qint64((v - kFileTimeToUnixEpoch) / 10000ULL);
}

quint64 toU64(const LARGE_INTEGER &t)
{
    return static_cast<quint64>(t.QuadPart);
}

using PfnIsProcessCritical = BOOL(WINAPI *)(HANDLE, PBOOL);

PfnIsProcessCritical resolveIsProcessCritical()
{
    static PfnIsProcessCritical fn = []() -> PfnIsProcessCritical {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        return k ? reinterpret_cast<PfnIsProcessCritical>(GetProcAddress(k, "IsProcessCritical")) : nullptr;
    }();
    return fn;
}

// 这些是结束就会拖垮系统的进程,界面上要拦一道
bool isSystemCriticalName(const QString &name)
{
    static const char *names[] = { "system", "registry", "memory compression", "secure system", "smss.exe",
                                   "csrss.exe", "wininit.exe", "winlogon.exe", "services.exe", "lsass.exe",
                                   "fontdrvhost.exe" };
    const QString lower = name.toLower();
    for (const char *n : names) {
        if (lower == QLatin1String(n))
            return true;
    }
    return false;
}

} // namespace

ProcessSampler::ProcessSampler()
{
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    m_logicalCores = qMax(1, int(si.dwNumberOfProcessors));
}

void ProcessSampler::loadStaticInfo()
{
    // 命令行只有 WMI 能一次性给全,启动时预热一次
    refreshCommandLines();
}

void ProcessSampler::refreshCommandLines()
{
    WmiQuery wmi;
    const auto rows = wmi.query(QStringLiteral("SELECT ProcessId, CommandLine FROM Win32_Process"),
                                { QStringLiteral("ProcessId"), QStringLiteral("CommandLine") });
    for (const auto &row : rows) {
        bool ok = false;
        const quint32 pid = row.value(QStringLiteral("ProcessId")).toUInt(&ok);
        if (!ok)
            continue;
        const QString cmd = row.value(QStringLiteral("CommandLine"));
        if (!cmd.isEmpty())
            m_cmdCache.insert(pid, cmd);
    }
}

void ProcessSampler::fillDetails(ProcessInfo *info, bool allowQuery)
{
    const quint32 pid = info->pid;

    // ---- 路径(顺带确定文件说明)
    auto pathIt = m_pathCache.constFind(pid);
    if (pathIt == m_pathCache.constEnd() && allowQuery)
        pathIt = m_pathCache.insert(pid, queryProcessPath(pid));
    if (pathIt != m_pathCache.constEnd())
        info->path = pathIt.value();

    if (info->name.isEmpty() && !info->path.isEmpty())
        info->name = QFileInfo(info->path).fileName();

    if (!info->path.isEmpty()) {
        auto descIt = m_descByPath.constFind(info->path);
        if (descIt == m_descByPath.constEnd() && allowQuery)
            descIt = m_descByPath.insert(info->path, queryProcessDescription(info->path));
        if (descIt != m_descByPath.constEnd())
            info->description = descIt.value();
    }

    // ---- 用户 + 是否提权(两者来自同一个令牌,一次查完)
    auto userIt = m_userCache.constFind(pid);
    if (userIt == m_userCache.constEnd() && allowQuery) {
        userIt = m_userCache.insert(pid, queryProcessUser(pid));

        bool elevated = false;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            HANDLE token = nullptr;
            if (OpenProcessToken(h, TOKEN_QUERY, &token)) {
                TOKEN_ELEVATION te{};
                DWORD size = sizeof(te);
                if (GetTokenInformation(token, TokenElevation, &te, size, &size))
                    elevated = te.TokenIsElevated != 0;
                CloseHandle(token);
            }
            CloseHandle(h);
        }
        m_elevatedCache.insert(pid, elevated);
    }
    if (userIt != m_userCache.constEnd())
        info->user = userIt.value();
    info->elevated = m_elevatedCache.value(pid, false);

    // ---- 命令行
    info->commandLine = m_cmdCache.value(pid);

    // ---- 架构
    auto archIt = m_archCache.constFind(pid);
    if (archIt == m_archCache.constEnd() && allowQuery) {
        QString arch;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            arch = queryProcessArchitecture(h);
            CloseHandle(h);
        }
        archIt = m_archCache.insert(pid, arch);
    }
    if (archIt != m_archCache.constEnd())
        info->architecture = archIt.value();
}

ProcessInfo ProcessSampler::describe(quint32 pid) const
{
    ProcessInfo info;
    for (const auto &p : m_last) {
        if (p.pid == pid) {
            info = p;
            break;
        }
    }
    if (info.pid == 0)
        info.pid = pid;
    // const 方法里要改缓存,这几张表用 mutable 语义处理:详情面板是低频操作,
    // 这里直接走一次性查询,不回写缓存
    auto *self = const_cast<ProcessSampler *>(this);
    self->fillDetails(&info, true);
    return info;
}

void ProcessSampler::dropStaleCache(const QVector<ProcessInfo> &list)
{
    // 缓存只在进程数明显缩水时清理,避免每帧做哈希重建
    if (m_pathCache.size() < list.size() + 512)
        return;

    QSet<quint32> alive;
    alive.reserve(list.size());
    for (const auto &p : list)
        alive.insert(p.pid);

    for (auto it = m_pathCache.begin(); it != m_pathCache.end();) {
        if (!alive.contains(it.key()))
            it = m_pathCache.erase(it);
        else
            ++it;
    }
    for (auto it = m_userCache.begin(); it != m_userCache.end();) {
        if (!alive.contains(it.key()))
            it = m_userCache.erase(it);
        else
            ++it;
    }
    for (auto it = m_elevatedCache.begin(); it != m_elevatedCache.end();) {
        if (!alive.contains(it.key()))
            it = m_elevatedCache.erase(it);
        else
            ++it;
    }
}

ProcessSnapshot ProcessSampler::sample(const ProcessExtras &extras)
{
    ProcessSnapshot snapshot;
    snapshot.timestampMs = QDateTime::currentMSecsSinceEpoch();

    const qint64 nowMs = snapshot.timestampMs;
    const double elapsed = m_hasPrev && m_lastSampleMs > 0 ? double(nowMs - m_lastSampleMs) / 1000.0 : 0.0;

    // ---- 一次性拉全部进程
    ULONG bufSize = 1024 * 1024;
    std::vector<BYTE> buffer;
    LONG status = 0;
    for (int attempt = 0; attempt < 6; ++attempt) {
        buffer.assign(bufSize, 0);
        ULONG returned = 0;
        status = NtQuerySystemInformation(
            static_cast<SYSTEM_INFORMATION_CLASS>(kSystemProcessInformation), buffer.data(), bufSize, &returned);
        if (status != kStatusInfoLengthMismatch)
            break;
        bufSize *= 2;
    }
    if (status != 0)
        return snapshot;

    QHash<quint32, ProcTimes> current;
    QHash<quint32, ProcTimes> readWriteNow;   // 单独存,避免和 CPU 时间混在一起
    QVector<ProcessInfo> list;

    BYTE *p = buffer.data();
    while (true) {
        auto *sp = reinterpret_cast<SystemProcessInfo *>(p);

        ProcessInfo info;
        info.pid = quint32(reinterpret_cast<quintptr>(sp->UniqueProcessId));
        info.parentPid = quint32(reinterpret_cast<quintptr>(sp->InheritedFromUniqueProcessId));
        info.threadCount = int(sp->NumberOfThreads);
        info.handleCount = int(sp->HandleCount);
        info.sessionId = int(sp->SessionId);
        info.workingSetBytes = quint64(sp->WorkingSetSize);
        info.privateBytes = quint64(sp->PrivatePageCount);
        info.virtualBytes = quint64(sp->VirtualSize);
        info.cpuTimeMs = (toU64(sp->KernelTime) + toU64(sp->UserTime)) / 10000ULL;
        info.startTimeMs = fileTimeToEpochMs(sp->CreateTime);

        if (sp->ImageName.Buffer && sp->ImageName.Length > 0) {
            info.name = fromUnicodeBuffer(sp->ImageName.Buffer, int(sp->ImageName.Length / sizeof(wchar_t)));
        } else if (info.pid == 0) {
            info.name = QStringLiteral("系统空闲进程");
        }

        // ---- CPU 占用
        ProcTimes times;
        times.kernel = toU64(sp->KernelTime);
        times.user = toU64(sp->UserTime);
        times.readBytes = toU64(sp->ReadTransferCount);
        times.writeBytes = toU64(sp->WriteTransferCount);

        if (elapsed > 0.0) {
            const auto prevIt = m_prev.constFind(info.pid);
            if (prevIt != m_prev.constEnd()) {
                const quint64 dKernel = times.kernel >= prevIt->kernel ? times.kernel - prevIt->kernel : 0;
                const quint64 dUser = times.user >= prevIt->user ? times.user - prevIt->user : 0;
                // 100ns -> 秒,再除以"经过时间 × 核心数",得到占整机百分比
                const double busySeconds = double(dKernel + dUser) / 1e7;
                info.cpuPercent = qBound(0.0, 100.0 * busySeconds / (elapsed * m_logicalCores), 100.0);

                if (times.readBytes >= prevIt->readBytes)
                    info.diskReadBytesPerSec = quint64(double(times.readBytes - prevIt->readBytes) / elapsed);
                if (times.writeBytes >= prevIt->writeBytes)
                    info.diskWriteBytesPerSec = quint64(double(times.writeBytes - prevIt->writeBytes) / elapsed);
            }
        }

        current.insert(info.pid, times);
        list.push_back(info);

        if (sp->NextEntryOffset == 0)
            break;
        p += sp->NextEntryOffset;
    }

    // ---- 注入 GPU / 网络
    for (auto &info : list) {
        if (extras.gpuUsage)
            info.gpuPercent = extras.gpuUsage->value(info.pid, 0.0);
        if (extras.gpuDedicated)
            info.gpuDedicatedBytes = extras.gpuDedicated->value(info.pid, 0);
        if (extras.gpuShared)
            info.gpuSharedBytes = extras.gpuShared->value(info.pid, 0);
        if (extras.netTraffic) {
            const auto it = extras.netTraffic->constFind(info.pid);
            if (it != extras.netTraffic->constEnd()) {
                info.netRxBytesPerSec = it->rxBytesPerSec;
                info.netTxBytesPerSec = it->txBytesPerSec;
            }
        }
    }

    // ---- 补全路径/用户/命令行
    // 只有首次见到的 PID 才发起昂贵查询,之后一律读缓存
    for (auto &info : list) {
        const bool firstSight = !m_pathCache.contains(info.pid);
        fillDetails(&info, firstSight);

        if (info.name.isEmpty())
            info.name = QStringLiteral("PID %1").arg(info.pid);

        info.critical = isSystemCriticalName(info.name) || info.pid <= 4;
    }

    snapshot.totalProcesses = list.size();
    for (const auto &info : list) {
        snapshot.totalThreads += info.threadCount;
        snapshot.totalHandles += info.handleCount;
    }

    m_prev = current;
    m_lastSampleMs = nowMs;
    m_hasPrev = true;
    m_last = list;
    snapshot.processes = list;

    // 命令行每 60 秒重扫一次,进程会不断新增
    if (nowMs - m_lastCmdRefreshMs > 60000) {
        m_lastCmdRefreshMs = nowMs;
        refreshCommandLines();
    }

    dropStaleCache(list);

    return snapshot;
}

bool ProcessSampler::findProcess(quint32 pid, ProcessInfo *out) const
{
    if (!out)
        return false;
    for (const auto &p : m_last) {
        if (p.pid == pid) {
            *out = p;
            return true;
        }
    }
    return false;
}

} // namespace ws
