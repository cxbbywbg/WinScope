#include "core/ServiceManager.h"

#include "core/Win32Utils.h"

#include <QHash>
#include <QSet>

#include <vector>

#include <windows.h>
#include <winsvc.h>

namespace ws {

namespace {

QString lastErrorText()
{
    const DWORD code = GetLastError();
    switch (code) {
    case ERROR_ACCESS_DENIED:
        return QStringLiteral("拒绝访问:修改服务需要管理员权限");
    case ERROR_SERVICE_DOES_NOT_EXIST:
        return QStringLiteral("服务不存在");
    case ERROR_SERVICE_CANNOT_ACCEPT_CTRL:
        return QStringLiteral("该服务当前不接受此控制命令");
    case ERROR_SERVICE_NOT_ACTIVE:
        return QStringLiteral("服务未运行");
    case ERROR_SERVICE_ALREADY_RUNNING:
        return QStringLiteral("服务已在运行");
    case ERROR_INVALID_PARAMETER:
        return QStringLiteral("参数无效");
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

QString stateText(DWORD state)
{
    switch (state) {
    case SERVICE_STOPPED:
        return QStringLiteral("已停止");
    case SERVICE_START_PENDING:
        return QStringLiteral("正在启动");
    case SERVICE_STOP_PENDING:
        return QStringLiteral("正在停止");
    case SERVICE_RUNNING:
        return QStringLiteral("正在运行");
    case SERVICE_CONTINUE_PENDING:
        return QStringLiteral("正在恢复");
    case SERVICE_PAUSE_PENDING:
        return QStringLiteral("正在暂停");
    case SERVICE_PAUSED:
        return QStringLiteral("已暂停");
    default:
        return QStringLiteral("未知");
    }
}

QString startTypeText(DWORD type)
{
    switch (type) {
    case SERVICE_BOOT_START:
        return QStringLiteral("引导启动");
    case SERVICE_SYSTEM_START:
        return QStringLiteral("系统启动");
    case SERVICE_AUTO_START:
        return QStringLiteral("自动");
    case SERVICE_DEMAND_START:
        return QStringLiteral("手动");
    case SERVICE_DISABLED:
        return QStringLiteral("已禁用");
    default:
        return QStringLiteral("未知");
    }
}

// PID 不能从 EnumServicesStatusEx 直接拿,得单独查一次
QHash<QString, quint32> queryServicePids(SC_HANDLE scm)
{
    QHash<QString, quint32> pids;

    DWORD needed = 0;
    DWORD count = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, nullptr, 0, &needed, &count,
                          nullptr, nullptr);
    if (needed == 0)
        return pids;

    std::vector<BYTE> buf(needed);
    if (!EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, buf.data(), needed, &needed,
                               &count, nullptr, nullptr)) {
        return pids;
    }

    auto *items = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW *>(buf.data());
    for (DWORD i = 0; i < count; ++i)
        pids.insert(fromWide(items[i].lpServiceName), items[i].ServiceStatusProcess.dwProcessId);
    return pids;
}

bool isDelayedAutoStart(SC_HANDLE service)
{
    SERVICE_DELAYED_AUTO_START_INFO info{};
    DWORD needed = 0;
    if (QueryServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                             reinterpret_cast<LPBYTE>(&info), sizeof(info), &needed)) {
        return info.fDelayedAutostart != FALSE;
    }
    return false;
}

QString serviceDescription(SC_HANDLE service)
{
    DWORD needed = 0;
    QueryServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &needed);
    if (needed == 0)
        return QString();

    std::vector<BYTE> buf(needed);
    if (!QueryServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, buf.data(), needed, &needed))
        return QString();

    auto *desc = reinterpret_cast<SERVICE_DESCRIPTIONW *>(buf.data());
    return desc->lpDescription ? fromWide(desc->lpDescription).trimmed() : QString();
}

// 打开服务并执行一次控制命令,统一收口错误处理
ActionResult controlService(const QString &name, DWORD control, const QString &okMessage)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return ActionResult::failure(lastErrorText());

    SC_HANDLE service = OpenServiceW(scm, toWide(name).c_str(), SERVICE_START | SERVICE_STOP | SERVICE_PAUSE_CONTINUE
                                                                   | SERVICE_QUERY_STATUS);
    if (!service) {
        const QString err = lastErrorText();
        CloseServiceHandle(scm);
        return ActionResult::failure(err);
    }

    SERVICE_STATUS status{};
    const BOOL ok = ControlService(service, control, &status);
    const QString err = ok ? QString() : lastErrorText();
    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    if (!ok)
        return ActionResult::failure(err);
    return ActionResult::success(okMessage);
}

} // namespace

QVector<ServiceInfo> ServiceManager::enumerate()
{
    QVector<ServiceInfo> result;

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm)
        return result;

    const QHash<QString, quint32> pids = queryServicePids(scm);

    DWORD needed = 0;
    DWORD count = 0;
    DWORD resume = 0;
    // 服务数量会变,循环重试直到缓冲够大
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD rc = EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                                               SERVICE_STATE_ALL, nullptr, 0, &needed, &count, &resume, nullptr);
        if (rc != ERROR_MORE_DATA && rc != ERROR_SUCCESS)
            break;

        std::vector<BYTE> buf(needed);
        resume = 0;
        if (!EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buf.data(), needed,
                                   &needed, &count, &resume, nullptr)) {
            if (GetLastError() == ERROR_MORE_DATA)
                continue;
            break;
        }

        auto *items = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW *>(buf.data());
        result.reserve(int(count));
        for (DWORD i = 0; i < count; ++i) {
            ServiceInfo s;
            s.name = fromWide(items[i].lpServiceName);
            s.displayName = fromWide(items[i].lpDisplayName);
            s.state = stateText(items[i].ServiceStatusProcess.dwCurrentState);
            s.pid = items[i].ServiceStatusProcess.dwProcessId;
            s.canStop = items[i].ServiceStatusProcess.dwControlsAccepted & SERVICE_ACCEPT_STOP;
            s.canPause = items[i].ServiceStatusProcess.dwControlsAccepted & SERVICE_ACCEPT_PAUSE_CONTINUE;
            s.canStart = (items[i].ServiceStatusProcess.dwCurrentState == SERVICE_STOPPED);
            if (s.pid == 0)
                s.pid = pids.value(s.name, 0);
            result.push_back(s);
        }
        break;
    }

    // 逐项补齐配置信息(启动类型/路径/账户),这部分没有批量接口
    for (auto &s : result) {
        SC_HANDLE service = OpenServiceW(scm, toWide(s.name).c_str(), SERVICE_QUERY_CONFIG);
        if (!service)
            continue;

        QUERY_SERVICE_CONFIGW *cfgPtr = nullptr;
        DWORD needed2 = 0;
        QueryServiceConfigW(service, nullptr, 0, &needed2);
        if (needed2 > 0) {
            std::vector<BYTE> buf(needed2);
            if (QueryServiceConfigW(service, reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buf.data()), needed2,
                                    &needed2)) {
                cfgPtr = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buf.data());
                s.startType = startTypeText(cfgPtr->dwStartType);
                s.delayedStart = isDelayedAutoStart(service);
                if (s.delayedStart && cfgPtr->dwStartType == SERVICE_AUTO_START)
                    s.startType = QStringLiteral("自动(延迟)");
                s.binaryPath = cfgPtr->lpBinaryPathName ? fromWide(cfgPtr->lpBinaryPathName) : QString();
                s.account = cfgPtr->lpServiceStartName ? fromWide(cfgPtr->lpServiceStartName) : QString();
                s.isDriver = (cfgPtr->dwServiceType & SERVICE_DRIVER) != 0;
            }
        }
        s.description = serviceDescription(service);
        CloseServiceHandle(service);
    }

    CloseServiceHandle(scm);
    return result;
}

ActionResult ServiceManager::start(const QString &serviceName)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return ActionResult::failure(lastErrorText());

    SC_HANDLE service = OpenServiceW(scm, toWide(serviceName).c_str(), SERVICE_START | SERVICE_QUERY_STATUS);
    if (!service) {
        const QString err = lastErrorText();
        CloseServiceHandle(scm);
        return ActionResult::failure(err);
    }

    const BOOL ok = StartServiceW(service, 0, nullptr);
    const QString err = ok ? QString() : lastErrorText();
    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    if (!ok)
        return ActionResult::failure(err);
    return ActionResult::success(QStringLiteral("服务启动指令已发送"));
}

ActionResult ServiceManager::stop(const QString &serviceName)
{
    return controlService(serviceName, SERVICE_CONTROL_STOP, QStringLiteral("服务停止指令已发送"));
}

ActionResult ServiceManager::pause(const QString &serviceName)
{
    return controlService(serviceName, SERVICE_CONTROL_PAUSE, QStringLiteral("服务已暂停"));
}

ActionResult ServiceManager::resume(const QString &serviceName)
{
    return controlService(serviceName, SERVICE_CONTROL_CONTINUE, QStringLiteral("服务已恢复"));
}

ActionResult ServiceManager::restart(const QString &serviceName)
{
    // 没有原子的重启接口,先停再起
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return ActionResult::failure(lastErrorText());

    SC_HANDLE service = OpenServiceW(scm, toWide(serviceName).c_str(),
                                     SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service) {
        const QString err = lastErrorText();
        CloseServiceHandle(scm);
        return ActionResult::failure(err);
    }

    SERVICE_STATUS status{};
    if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        // 等服务真正停下,最多等 10 秒
        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        for (int i = 0; i < 40; ++i) {
            if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp),
                                      &needed))
                break;
            if (ssp.dwCurrentState == SERVICE_STOPPED)
                break;
            Sleep(250);
        }
    }

    const BOOL ok = StartServiceW(service, 0, nullptr);
    const QString err = ok ? QString() : lastErrorText();
    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    if (!ok)
        return ActionResult::failure(err);
    return ActionResult::success(QStringLiteral("服务已重启"));
}

ActionResult ServiceManager::setStartType(const QString &serviceName, const QString &startType)
{
    DWORD type = SERVICE_DEMAND_START;
    bool delayed = false;
    if (startType == QLatin1String("auto")) {
        type = SERVICE_AUTO_START;
    } else if (startType == QLatin1String("auto-delayed")) {
        type = SERVICE_AUTO_START;
        delayed = true;
    } else if (startType == QLatin1String("disabled")) {
        type = SERVICE_DISABLED;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return ActionResult::failure(lastErrorText());

    SC_HANDLE service = OpenServiceW(scm, toWide(serviceName).c_str(), SERVICE_CHANGE_CONFIG);
    if (!service) {
        const QString err = lastErrorText();
        CloseServiceHandle(scm);
        return ActionResult::failure(err);
    }

    const BOOL ok = ChangeServiceConfigW(service, SERVICE_NO_CHANGE, type, SERVICE_NO_CHANGE, nullptr, nullptr,
                                         nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!ok) {
        const QString err = lastErrorText();
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return ActionResult::failure(err);
    }

    // 延迟启动是单独的配置项,失败不算致命
    if (type == SERVICE_AUTO_START) {
        SERVICE_DELAYED_AUTO_START_INFO info{};
        info.fDelayedAutostart = delayed ? TRUE : FALSE;
        ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &info);
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return ActionResult::success(QStringLiteral("启动类型已更新"));
}

QStringList ServiceManager::startTypeOptions()
{
    return { QStringLiteral("自动"), QStringLiteral("自动(延迟)"), QStringLiteral("手动"), QStringLiteral("已禁用") };
}

} // namespace ws
