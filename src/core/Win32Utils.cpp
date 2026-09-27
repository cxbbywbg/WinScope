#include "core/Win32Utils.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <string>
#include <vector>

#include <objbase.h>
#include <shellapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <winver.h>

namespace ws {

// ------------------------------------------------------------ 编码

QString fromWide(const wchar_t *s, int len)
{
    if (!s)
        return QString();
    if (len < 0)
        return QString::fromWCharArray(s);
    return QString::fromWCharArray(s, len);
}

QString fromWide(const std::wstring &s)
{
    if (s.empty())
        return QString();
    return QString::fromWCharArray(s.c_str(), int(s.size()));
}

std::wstring toWide(const QString &s)
{
    return std::wstring(reinterpret_cast<const wchar_t *>(s.utf16()), size_t(s.size()));
}

QString fromUnicodeBuffer(const wchar_t *buf, int capacity)
{
    if (!buf || capacity <= 0)
        return QString();
    int len = 0;
    while (len < capacity && buf[len] != L'\0')
        ++len;
    return QString::fromWCharArray(buf, len);
}

// ------------------------------------------------------------ 格式化

QString formatBytes(quint64 bytes)
{
    static const char *units[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    double v = double(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        ++i;
    }
    if (i == 0)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(v, 0, 'f', v >= 100.0 ? 0 : (v >= 10.0 ? 1 : 2)).arg(QLatin1String(units[i]));
}

QString formatBytesPerSec(double bytesPerSec)
{
    if (bytesPerSec < 0)
        bytesPerSec = 0;
    static const char *units[] = { "B/s", "KB/s", "MB/s", "GB/s" };
    double v = bytesPerSec;
    int i = 0;
    while (v >= 1024.0 && i < 3) {
        v /= 1024.0;
        ++i;
    }
    if (i == 0)
        return QStringLiteral("%1 B/s").arg(int(v));
    return QStringLiteral("%1 %2").arg(v, 0, 'f', v >= 100.0 ? 0 : (v >= 10.0 ? 1 : 2)).arg(QLatin1String(units[i]));
}

QString formatCount(quint64 n)
{
    const QString s = QString::number(n);
    QString out;
    out.reserve(s.size() + s.size() / 3);
    const int len = s.size();
    for (int i = 0; i < len; ++i) {
        if (i > 0 && (len - i) % 3 == 0)
            out += QLatin1Char(',');
        out += s.at(i);
    }
    return out;
}

QString formatDuration(qint64 seconds)
{
    if (seconds < 0)
        seconds = 0;
    const qint64 d = seconds / 86400;
    const qint64 h = (seconds % 86400) / 3600;
    const qint64 m = (seconds % 3600) / 60;
    const qint64 s = seconds % 60;

    if (d > 0)
        return QStringLiteral("%1 天 %2 小时 %3 分").arg(d).arg(h).arg(m);
    if (h > 0)
        return QStringLiteral("%1 小时 %2 分 %3 秒").arg(h).arg(m).arg(s);
    if (m > 0)
        return QStringLiteral("%1 分 %2 秒").arg(m).arg(s);
    return QStringLiteral("%1 秒").arg(s);
}

QString formatDateTime(qint64 epochMs)
{
    if (epochMs <= 0)
        return QStringLiteral("—");
    return QDateTime::fromMSecsSinceEpoch(epochMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString formatPercent(double v, int decimals)
{
    return QStringLiteral("%1%").arg(v, 0, 'f', decimals);
}

QString formatWmiDate(const QString &raw)
{
    if (raw.size() >= 8) {
        const QString year = raw.mid(0, 4);
        const QString month = raw.mid(4, 2);
        const QString day = raw.mid(6, 2);
        bool ok = false;
        const int y = year.toInt(&ok);
        if (ok && y >= 1970 && y <= 2200)
            return QStringLiteral("%1-%2-%3").arg(year, month, day);
    }
    return raw;
}

// ------------------------------------------------------------ 权限

bool isProcessElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool enableDebugPrivilege()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    const BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(token);
    // AdjustTokenPrivileges 成功但没生效时返回 ERROR_NOT_ALL_ASSIGNED
    return ok && err == ERROR_SUCCESS;
}

bool relaunchElevated()
{
    wchar_t path[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH * 2))
        return false;

    // 不带命令行参数:WinScope 没有需要透传给新实例的启动参数
    const HINSTANCE r = ShellExecuteW(nullptr, L"runas", path, nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

// ------------------------------------------------------------ 时间

qint64 systemBootTimeMs()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const ULONGLONG upMs = GetTickCount64();
    return nowMs - qint64(upMs);
}

// ------------------------------------------------------------ 进程查询

QString queryProcessPath(quint32 pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return QString();

    std::vector<wchar_t> buf(32768);
    DWORD size = DWORD(buf.size());
    QString result;
    if (QueryFullProcessImageNameW(h, 0, buf.data(), &size))
        result = fromWide(buf.data(), int(size));
    CloseHandle(h);
    return result;
}

QString queryProcessUser(quint32 pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return QString();

    QString result;
    HANDLE token = nullptr;
    if (OpenProcessToken(h, TOKEN_QUERY, &token)) {
        DWORD need = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &need);
        if (need > 0) {
            std::vector<BYTE> buf(need);
            if (GetTokenInformation(token, TokenUser, buf.data(), need, &need)) {
                auto *tu = reinterpret_cast<TOKEN_USER *>(buf.data());
                wchar_t name[256] = {};
                wchar_t domain[256] = {};
                DWORD nameLen = 256;
                DWORD domainLen = 256;
                SID_NAME_USE use{};
                if (LookupAccountSidW(nullptr, tu->User.Sid, name, &nameLen, domain, &domainLen, &use)) {
                    result = domainLen > 0 ? QStringLiteral("%1\\%2").arg(fromWide(domain), fromWide(name))
                                           : fromWide(name);
                } else {
                    // 查不到就退回 SID 字符串,至少能区分不同账户
                    LPWSTR sidStr = nullptr;
                    if (ConvertSidToStringSidW(tu->User.Sid, &sidStr)) {
                        result = fromWide(sidStr);
                        LocalFree(sidStr);
                    }
                }
            }
        }
        CloseHandle(token);
    }
    CloseHandle(h);
    return result;
}

QString queryProcessArchitecture(HANDLE process)
{
    if (!process)
        return QString();

    // Windows 10 起用 IsWow64Process2,能区分 x86/x64/ARM64
    using PfnIsWow64Process2 = BOOL(WINAPI *)(HANDLE, USHORT *, USHORT *);
    static PfnIsWow64Process2 pIsWow64Process2 = []() -> PfnIsWow64Process2 {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        return k ? reinterpret_cast<PfnIsWow64Process2>(GetProcAddress(k, "IsWow64Process2")) : nullptr;
    }();

    if (pIsWow64Process2) {
        USHORT processMachine = 0;
        USHORT nativeMachine = 0;
        if (pIsWow64Process2(process, &processMachine, &nativeMachine)) {
            const USHORT machine = processMachine == IMAGE_FILE_MACHINE_UNKNOWN ? nativeMachine : processMachine;
            switch (machine) {
            case IMAGE_FILE_MACHINE_I386:
                return QStringLiteral("x86");
            case IMAGE_FILE_MACHINE_AMD64:
                return QStringLiteral("x64");
            case IMAGE_FILE_MACHINE_ARM64:
                return QStringLiteral("ARM64");
            case IMAGE_FILE_MACHINE_ARM:
                return QStringLiteral("ARM");
            default:
                break;
            }
        }
    }

    BOOL wow64 = FALSE;
    if (IsWow64Process(process, &wow64))
        return wow64 ? QStringLiteral("x86") : QStringLiteral("x64");
    return QString();
}

namespace {

// 读版本资源里的某个字段
QString versionString(const QString &filePath, const wchar_t *field)
{
    if (filePath.isEmpty())
        return QString();
    const std::wstring wpath = toWide(QDir::toNativeSeparators(filePath));

    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(wpath.c_str(), &dummy);
    if (size == 0)
        return QString();

    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(wpath.c_str(), 0, size, data.data()))
        return QString();

    struct LangCodePage {
        WORD language;
        WORD codePage;
    };
    LangCodePage *trans = nullptr;
    UINT transLen = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID *>(&trans), &transLen)
        || transLen < sizeof(LangCodePage)) {
        return QString();
    }

    wchar_t subBlock[128] = {};
    // 取第一个语言块即可,中英文资源都能命中
    swprintf(subBlock, 128, L"\\StringFileInfo\\%04x%04x\\%s", trans[0].language, trans[0].codePage, field);

    wchar_t *value = nullptr;
    UINT valueLen = 0;
    if (VerQueryValueW(data.data(), subBlock, reinterpret_cast<LPVOID *>(&value), &valueLen) && valueLen > 0)
        return fromWide(value, int(valueLen > 1 ? valueLen - 1 : 0));
    return QString();
}

} // namespace

QString queryProcessDescription(const QString &filePath)
{
    return versionString(filePath, L"FileDescription");
}

QString queryProcessPublisher(const QString &filePath)
{
    QString company = versionString(filePath, L"CompanyName");
    if (company.isEmpty())
        company = versionString(filePath, L"ProductName");
    return company;
}

bool isProcessResponding(HANDLE process)
{
    if (!process)
        return false;
    // 用 SMTO_ABORTIFHUNG 探测主窗口是否在响应
    DWORD_PTR result = 0;
    const LRESULT r = SendMessageTimeoutW(reinterpret_cast<HWND>(HWND_BROADCAST), WM_NULL, 0, 0,
                                          SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result);
    Q_UNUSED(process);
    return r != 0;
}

// ------------------------------------------------------------ COM

ComInitializer::ComInitializer()
{
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_ready = SUCCEEDED(hr);
    m_owner = SUCCEEDED(hr);
    // RPC_E_CHANGED_MODE:别的代码已经用另一种模型初始化过了,COM 是可用的
    if (hr == RPC_E_CHANGED_MODE)
        m_ready = true;
}

ComInitializer::~ComInitializer()
{
    if (m_owner)
        CoUninitialize();
}

// ------------------------------------------------------------ 其它

void revealInExplorer(const QString &path)
{
    if (path.isEmpty())
        return;
    const QFileInfo fi(path);
    if (fi.exists()) {
        const QString native = QDir::toNativeSeparators(path);
        const QString args = QStringLiteral("/select,\"%1\"").arg(native);
        ShellExecuteW(nullptr, L"open", L"explorer.exe", toWide(args).c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        // 文件已不存在,退到打开所在目录
        const QString dir = QDir::toNativeSeparators(fi.absolutePath());
        if (!dir.isEmpty())
            ShellExecuteW(nullptr, L"open", L"explorer.exe", toWide(dir).c_str(), nullptr, SW_SHOWNORMAL);
    }
}

bool runElevatedCommand(const QString &exe, const QString &args)
{
    const HINSTANCE r = ShellExecuteW(nullptr, L"runas", toWide(exe).c_str(), toWide(args).c_str(), nullptr,
                                      SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

bool shellOpen(const QString &target, const QString &args, bool elevated)
{
    if (target.isEmpty())
        return false;

    const std::wstring wtarget = toWide(target);
    const std::wstring wargs = toWide(args);
    const HINSTANCE r = ShellExecuteW(nullptr, elevated ? L"runas" : L"open", wtarget.c_str(),
                                      args.isEmpty() ? nullptr : wargs.c_str(), nullptr, SW_SHOWNORMAL);
    // ShellExecute 返回值 >32 才算成功,<=32 是错误码
    return reinterpret_cast<INT_PTR>(r) > 32;
}

} // namespace ws
