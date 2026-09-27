#include "core/ProcessController.h"

#include "core/Win32Utils.h"

#include <QDir>
#include <QFileInfo>

#include <vector>

#include <windows.h>
#include <tlhelp32.h>

namespace ws {

namespace {

QString lastErrorText()
{
    const DWORD code = GetLastError();
    switch (code) {
    case ERROR_ACCESS_DENIED:
        return QStringLiteral("拒绝访问:该进程需要管理员权限才能操作");
    case ERROR_INVALID_PARAMETER:
        return QStringLiteral("参数无效");
    case ERROR_NOT_ALL_ASSIGNED:
        return QStringLiteral("权限不足,未能获得必要的访问令牌");
    case ERROR_INVALID_HANDLE:
        return QStringLiteral("进程已退出");
    default:
        break;
    }
    LPWSTR buf = nullptr;
    const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                         | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString text = len > 0 && buf ? fromWide(buf).trimmed() : QStringLiteral("未知错误");
    if (buf)
        LocalFree(buf);
    return QStringLiteral("%1(错误码 %2)").arg(text).arg(code);
}

// 枚举该进程的所有顶层窗口,给它发 WM_CLOSE
BOOL CALLBACK closeWindowProc(HWND hwnd, LPARAM param)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == DWORD(param)) {
        // 不打扰隐藏窗口,只关用户看得见的
        if (IsWindowVisible(hwnd))
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

using PfnNtSuspendProcess = LONG(NTAPI *)(HANDLE);
using PfnNtResumeProcess = LONG(NTAPI *)(HANDLE);

PfnNtSuspendProcess resolveSuspend()
{
    static PfnNtSuspendProcess fn = []() -> PfnNtSuspendProcess {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<PfnNtSuspendProcess>(GetProcAddress(ntdll, "NtSuspendProcess")) : nullptr;
    }();
    return fn;
}

PfnNtResumeProcess resolveResume()
{
    static PfnNtResumeProcess fn = []() -> PfnNtResumeProcess {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<PfnNtResumeProcess>(GetProcAddress(ntdll, "NtResumeProcess")) : nullptr;
    }();
    return fn;
}

struct PriorityEntry {
    const char *name;
    DWORD value;
};

const PriorityEntry kPriorities[] = {
    { "实时 (Realtime)", REALTIME_PRIORITY_CLASS },
    { "高 (High)", HIGH_PRIORITY_CLASS },
    { "高于正常 (Above Normal)", ABOVE_NORMAL_PRIORITY_CLASS },
    { "正常 (Normal)", NORMAL_PRIORITY_CLASS },
    { "低于正常 (Below Normal)", BELOW_NORMAL_PRIORITY_CLASS },
    { "低 (Idle)", IDLE_PRIORITY_CLASS },
};

constexpr int kPriorityCount = int(sizeof(kPriorities) / sizeof(kPriorities[0]));

} // namespace

ActionResult ProcessController::closeGracefully(quint32 pid)
{
    if (pid <= 4)
        return ActionResult::failure(QStringLiteral("系统关键进程不能结束"));

    // 先确认进程还在,免得对着空气发消息还报成功
    HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!probe)
        return ActionResult::failure(lastErrorText());
    CloseHandle(probe);

    int sent = 0;
    EnumWindows(closeWindowProc, LPARAM(pid));
    Q_UNUSED(sent);

    // 有些进程没有窗口,这时退回强制结束更符合预期
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) {
        return ActionResult::success(QStringLiteral("已向该进程的窗口发送关闭请求"));
    }

    // 等一小会儿看它自己退不退
    const DWORD wait = WaitForSingleObject(h, 1500);
    if (wait == WAIT_OBJECT_0) {
        CloseHandle(h);
        return ActionResult::success(QStringLiteral("进程已正常退出"));
    }

    const BOOL killed = TerminateProcess(h, 0);
    CloseHandle(h);
    if (!killed)
        return ActionResult::failure(lastErrorText());
    return ActionResult::success(QStringLiteral("进程未响应关闭请求,已强制结束"));
}

ActionResult ProcessController::terminate(quint32 pid)
{
    if (pid <= 4)
        return ActionResult::failure(QStringLiteral("系统关键进程不能结束"));

    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h)
        return ActionResult::failure(lastErrorText());

    const BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    if (!ok)
        return ActionResult::failure(lastErrorText());
    return ActionResult::success(QStringLiteral("进程已强制结束"));
}

ActionResult ProcessController::restart(quint32 pid, const QString &path, const QString &commandLine)
{
    if (path.isEmpty())
        return ActionResult::failure(QStringLiteral("拿不到该进程的可执行文件路径,无法重启"));

    if (!QFileInfo::exists(path))
        return ActionResult::failure(QStringLiteral("可执行文件已不存在:%1").arg(path));

    // 先结束旧进程;已经退出的话不算错
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h) {
        TerminateProcess(h, 0);
        WaitForSingleObject(h, 3000);
        CloseHandle(h);
    }

    QString cmd = commandLine.trimmed();
    if (cmd.isEmpty())
        cmd = QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(path));

    std::wstring cmdBuf = toWide(cmd);
    std::vector<wchar_t> mutableCmd(cmdBuf.begin(), cmdBuf.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    const std::wstring workDir = toWide(QFileInfo(path).absolutePath());
    const BOOL ok = CreateProcessW(toWide(path).c_str(), mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                   workDir.c_str(), &si, &pi);
    if (!ok)
        return ActionResult::failure(lastErrorText());

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ActionResult::success(QStringLiteral("已重启,新 PID %1").arg(pi.dwProcessId));
}

ActionResult ProcessController::suspend(quint32 pid)
{
    const auto fn = resolveSuspend();
    if (!fn)
        return ActionResult::failure(QStringLiteral("当前系统不支持挂起进程"));

    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!h)
        return ActionResult::failure(lastErrorText());

    const LONG status = fn(h);
    CloseHandle(h);
    if (status != 0)
        return ActionResult::failure(QStringLiteral("挂起失败(NTSTATUS 0x%1)").arg(quint32(status), 8, 16, QLatin1Char('0')));
    return ActionResult::success(QStringLiteral("进程已挂起"));
}

ActionResult ProcessController::resume(quint32 pid)
{
    const auto fn = resolveResume();
    if (!fn)
        return ActionResult::failure(QStringLiteral("当前系统不支持恢复进程"));

    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!h)
        return ActionResult::failure(lastErrorText());

    const LONG status = fn(h);
    CloseHandle(h);
    if (status != 0)
        return ActionResult::failure(QStringLiteral("恢复失败(NTSTATUS 0x%1)").arg(quint32(status), 8, 16, QLatin1Char('0')));
    return ActionResult::success(QStringLiteral("进程已恢复"));
}

bool ProcessController::isSuspended(quint32 pid)
{
    // 判断挂起的简易办法:看主线程是否在等待态且没有任何用户态时间推进,
    // 这里用一个更直接的近似 —— 线程挂起计数大于 0 视为挂起
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    bool suspended = false;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid)
                continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th)
                continue;
            // SuspendThread 返回上一次的挂起计数
            const DWORD prev = SuspendThread(th);
            if (prev != DWORD(-1)) {
                if (prev > 0)
                    suspended = true;
                ResumeThread(th);   // 还原
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return suspended;
}

ActionResult ProcessController::setPriority(quint32 pid, quint32 priorityClass)
{
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!h)
        return ActionResult::failure(lastErrorText());

    const BOOL ok = SetPriorityClass(h, priorityClass);
    CloseHandle(h);
    if (!ok)
        return ActionResult::failure(lastErrorText());
    return ActionResult::success(QStringLiteral("优先级已更新"));
}

ActionResult ProcessController::setAffinity(quint32 pid, quint64 affinityMask)
{
    if (affinityMask == 0)
        return ActionResult::failure(QStringLiteral("至少要勾选一个 CPU 核心"));

    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h)
        return ActionResult::failure(lastErrorText());

    // 不能跨处理器组设置,取低 64 位即可覆盖单组内的全部核心
    const BOOL ok = SetProcessAffinityMask(h, DWORD_PTR(affinityMask));
    CloseHandle(h);
    if (!ok)
        return ActionResult::failure(lastErrorText());
    return ActionResult::success(QStringLiteral("CPU 亲和性已更新"));
}

bool ProcessController::queryAffinity(quint32 pid, quint64 *mask)
{
    if (!mask)
        return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h)
        return false;

    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    const BOOL ok = GetProcessAffinityMask(h, &processMask, &systemMask);
    CloseHandle(h);
    if (!ok)
        return false;

    *mask = quint64(processMask);
    return true;
}

quint32 ProcessController::queryPriority(quint32 pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h)
        return NORMAL_PRIORITY_CLASS;
    const DWORD cls = GetPriorityClass(h);
    CloseHandle(h);
    return cls;
}

QStringList ProcessController::priorityNames()
{
    QStringList names;
    for (int i = 0; i < kPriorityCount; ++i)
        names << QString::fromUtf8(kPriorities[i].name);
    return names;
}

quint32 ProcessController::priorityValueAt(int index)
{
    if (index < 0 || index >= kPriorityCount)
        return NORMAL_PRIORITY_CLASS;
    return kPriorities[index].value;
}

int ProcessController::priorityIndex(quint32 value)
{
    for (int i = 0; i < kPriorityCount; ++i) {
        if (kPriorities[i].value == value)
            return i;
    }
    return 3;   // 正常
}

} // namespace ws
