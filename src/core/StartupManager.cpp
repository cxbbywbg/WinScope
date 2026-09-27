#include "core/StartupManager.h"

#include "core/Win32Utils.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <vector>

#include <windows.h>
#include <taskschd.h>

namespace ws {

namespace {

struct RegistryLocation {
    HKEY root;
    const wchar_t *subKey;
    const wchar_t *approvedSubKey;   // 对应的 StartupApproved 路径
    const char *label;
    bool perUser;
};

const RegistryLocation kRunKeys[] = {
    { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", "HKCU\\...\\Run", true },
    { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\RunOnce",
      "HKCU\\...\\RunOnce", true },
    { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", "HKLM\\...\\Run", false },
    { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\RunOnce",
      "HKLM\\...\\RunOnce", false },
    { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run",
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", "HKLM\\WOW6432\\...\\Run",
      false },
    { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\RunOnce",
      "HKLM\\WOW6432\\...\\RunOnce", false },
};

const wchar_t *kStartupFolderApproved =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";

// 从命令行里抠出可执行文件路径(去掉引号与参数)
QString executableFromCommand(const QString &command)
{
    QString cmd = command.trimmed();
    if (cmd.isEmpty())
        return QString();

    if (cmd.startsWith(QLatin1Char('"'))) {
        const int end = cmd.indexOf(QLatin1Char('"'), 1);
        if (end > 1)
            return cmd.mid(1, end - 1);
    }

    // 没有引号时,按 ".exe" 截断
    const int exePos = cmd.indexOf(QLatin1String(".exe"), 0, Qt::CaseInsensitive);
    if (exePos > 0)
        return cmd.left(exePos + 4);

    const int space = cmd.indexOf(QLatin1Char(' '));
    return space > 0 ? cmd.left(space) : cmd;
}

// StartupApproved 的标记:第 0 字节 2 = 启用,3 = 禁用
bool readApproved(HKEY root, const wchar_t *approvedSubKey, const QString &valueName, bool defaultValue)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, approvedSubKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return defaultValue;

    BYTE data[64] = {};
    DWORD size = sizeof(data);
    DWORD type = 0;
    const LONG rc = RegQueryValueExW(key, toWide(valueName).c_str(), nullptr, &type, data, &size);
    RegCloseKey(key);

    if (rc != ERROR_SUCCESS || size < 1)
        return defaultValue;
    // 12 字节全 0 表示"从未记录过",按启用处理
    bool allZero = true;
    for (DWORD i = 0; i < size; ++i) {
        if (data[i] != 0) {
            allZero = false;
            break;
        }
    }
    if (allZero)
        return true;
    return data[0] != 3;
}

bool writeApproved(HKEY root, const wchar_t *approvedSubKey, const QString &valueName, bool enabled)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, approvedSubKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    BYTE data[12] = {};
    data[0] = enabled ? 0x02 : 0x03;
    if (!enabled) {
        // 记录禁用时间,资源管理器用它排序
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        memcpy(data + 4, &ft, sizeof(ft));
    }

    const LONG rc = RegSetValueExW(key, toWide(valueName).c_str(), 0, REG_BINARY, data, sizeof(data));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

// 版本资源里的厂商名,拿不到就用"—"
QString publisherOf(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return QStringLiteral("—");
    const QString company = queryProcessPublisher(path);
    return company.isEmpty() ? QStringLiteral("—") : company;
}

// 任务计划:枚举含登录/开机触发器的任务
void collectScheduledTasks(QVector<StartupItem> *out)
{
    ITaskService *service = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                                        reinterpret_cast<void **>(&service));
    if (FAILED(hr) || !service)
        return;

    VARIANT empty;
    VariantInit(&empty);
    if (FAILED(service->Connect(empty, empty, empty, empty))) {
        service->Release();
        VariantClear(&empty);
        return;
    }

    // 广度优先遍历所有文件夹,计划任务可以放在任意子目录里
    QStringList folders;
    folders << QStringLiteral("\\");

    for (int fi = 0; fi < folders.size() && fi < 200; ++fi) {
        ITaskFolder *folder = nullptr;
        if (FAILED(service->GetFolder(BSTR(toWide(folders.at(fi)).c_str()), &folder)) || !folder)
            continue;

        // 子文件夹
        ITaskFolderCollection *subFolders = nullptr;
        if (SUCCEEDED(folder->GetFolders(0, &subFolders)) && subFolders) {
            LONG count = 0;
            subFolders->get_Count(&count);
            for (LONG i = 1; i <= count; ++i) {
                ITaskFolder *sub = nullptr;
                VARIANT idx;
                VariantInit(&idx);
                idx.vt = VT_I4;
                idx.lVal = i;
                if (SUCCEEDED(subFolders->get_Item(idx, &sub)) && sub) {
                    BSTR path = nullptr;
                    if (SUCCEEDED(sub->get_Path(&path)) && path) {
                        folders << fromWide(path);
                        SysFreeString(path);
                    }
                    sub->Release();
                }
                VariantClear(&idx);
            }
            subFolders->Release();
        }

        // 任务
        IRegisteredTaskCollection *tasks = nullptr;
        if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks)) && tasks) {
            LONG count = 0;
            tasks->get_Count(&count);
            for (LONG i = 1; i <= count; ++i) {
                VARIANT idx;
                VariantInit(&idx);
                idx.vt = VT_I4;
                idx.lVal = i;

                IRegisteredTask *task = nullptr;
                if (SUCCEEDED(tasks->get_Item(idx, &task)) && task) {
                    ITaskDefinition *def = nullptr;
                    if (SUCCEEDED(task->get_Definition(&def)) && def) {
                        bool loginTrigger = false;
                        ITriggerCollection *triggers = nullptr;
                        if (SUCCEEDED(def->get_Triggers(&triggers)) && triggers) {
                            LONG tcount = 0;
                            triggers->get_Count(&tcount);
                            for (LONG t = 1; t <= tcount; ++t) {
                                ITrigger *trigger = nullptr;
                                // 注意:ITriggerCollection::get_Item 收的是 LONG 下标,
                                // 而同文件里其它集合收的是 VARIANT,别抄错
                                if (SUCCEEDED(triggers->get_Item(t, &trigger)) && trigger) {
                                    TASK_TRIGGER_TYPE2 type = TASK_TRIGGER_EVENT;
                                    if (SUCCEEDED(trigger->get_Type(&type))) {
                                        // 8 = 开机时,9 = 登录时
                                        if (type == TASK_TRIGGER_BOOT || type == TASK_TRIGGER_LOGON)
                                            loginTrigger = true;
                                    }
                                    trigger->Release();
                                }
                            }
                            triggers->Release();
                        }

                        if (loginTrigger) {
                            StartupItem item;
                            item.source = StartupSource::ScheduledTask;
                            item.sourceLabel = QStringLiteral("计划任务");
                            item.canToggle = true;

                            BSTR taskPath = nullptr;
                            if (SUCCEEDED(task->get_Path(&taskPath)) && taskPath) {
                                item.location = fromWide(taskPath);
                                SysFreeString(taskPath);
                            }

                            // \Microsoft\Windows\ 下面全是系统自带的内部任务(语言同步、
                            // 磁盘诊断、遥测之类),任务管理器也不把它们算作启动项
                            if (item.location.startsWith(QStringLiteral("\\Microsoft\\Windows\\"), Qt::CaseInsensitive)) {
                                def->Release();
                                task->Release();
                                VariantClear(&idx);
                                continue;
                            }

                            item.name = item.location.section(QLatin1Char('\\'), -1);
                            // 有些任务名后面挂了 GUID,显示时去掉
                            const int brace = item.name.indexOf(QLatin1Char('{'));
                            if (brace > 0)
                                item.name = item.name.left(brace).trimmed();

                            VARIANT_BOOL enabled = VARIANT_FALSE;
                            if (SUCCEEDED(task->get_Enabled(&enabled)))
                                item.enabled = enabled != VARIANT_FALSE;

                            // 取第一个动作的命令行
                            IActionCollection *actions = nullptr;
                            if (SUCCEEDED(def->get_Actions(&actions)) && actions) {
                                LONG acount = 0;
                                actions->get_Count(&acount);
                                if (acount > 0) {
                                    IAction *action = nullptr;
                                    // IActionCollection::get_Item 同样是 LONG 下标
                                    if (SUCCEEDED(actions->get_Item(1, &action)) && action) {
                                        IExecAction *exec = nullptr;
                                        if (SUCCEEDED(action->QueryInterface(IID_IExecAction,
                                                                             reinterpret_cast<void **>(&exec)))
                                            && exec) {
                                            BSTR path = nullptr;
                                            BSTR args = nullptr;
                                            if (SUCCEEDED(exec->get_Path(&path)) && path) {
                                                item.filePath = fromWide(path);
                                                item.command = item.filePath;
                                                if (SUCCEEDED(exec->get_Arguments(&args)) && args
                                                    && SysStringLen(args) > 0) {
                                                    item.command += QLatin1Char(' ') + fromWide(args);
                                                }
                                                if (path)
                                                    SysFreeString(path);
                                                if (args)
                                                    SysFreeString(args);
                                            }
                                            exec->Release();
                                        }
                                        action->Release();
                                    }
                                }
                                actions->Release();
                            }

                            item.publisher = publisherOf(item.filePath);
                            // Windows 不给计划任务算启动影响,和注册表项一样标「未标注」。
                            // 启用与否是「状态」列的事,别混进这一列。
                            item.impact = QStringLiteral("未标注");
                            out->push_back(item);
                        }
                        def->Release();
                    }
                    task->Release();
                }
                VariantClear(&idx);
            }
            tasks->Release();
        }

        folder->Release();
    }

    service->Release();
    VariantClear(&empty);
}

} // namespace

QVector<StartupItem> StartupManager::enumerate()
{
    QVector<StartupItem> result;

    // 计划任务走 COM,先确保本线程初始化过
    ComInitializer com;

    // ---------------- 注册表
    for (const auto &loc : kRunKeys) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(loc.root, loc.subKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
            continue;

        DWORD index = 0;
        wchar_t valueName[512] = {};
        BYTE data[8192] = {};
        while (true) {
            DWORD nameLen = 512;
            DWORD dataLen = sizeof(data);
            DWORD type = 0;
            const LONG rc = RegEnumValueW(key, index++, valueName, &nameLen, nullptr, &type, data, &dataLen);
            if (rc == ERROR_NO_MORE_ITEMS)
                break;
            if (rc != ERROR_SUCCESS)
                continue;
            if (type != REG_SZ && type != REG_EXPAND_SZ)
                continue;

            QString command = fromWide(reinterpret_cast<const wchar_t *>(data));
            if (type == REG_EXPAND_SZ) {
                // 展开 %ProgramFiles% 这类环境变量
                wchar_t expanded[8192] = {};
                if (ExpandEnvironmentStringsW(reinterpret_cast<const wchar_t *>(data), expanded, 8192))
                    command = fromWide(expanded);
            }

            StartupItem item;
            item.name = fromWide(valueName);
            item.command = command;
            item.filePath = executableFromCommand(command);
            item.location = QString::fromUtf8(loc.label);
            item.source = StartupSource::Registry;
            item.sourceLabel = QStringLiteral("注册表");
            item.registryKey = QString::fromWCharArray(loc.subKey);
            item.registryValue = item.name;
            item.canToggle = true;
            item.enabled = readApproved(loc.root, loc.approvedSubKey, item.name, true);
            item.publisher = publisherOf(item.filePath);
            item.impact = QStringLiteral("未标注");

            result.push_back(item);
        }
        RegCloseKey(key);
    }

    // ---------------- 启动文件夹
    // 注意:ApplicationsLocation 在 Windows 上指向 "...\Start Menu\Programs",
    // 启动目录是它下面的 Startup 子目录
    const QString userStartup = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation)
        + QStringLiteral("/Startup");
    const QString commonStartup = QString::fromLocal8Bit(qgetenv("ProgramData"))
        + QStringLiteral("/Microsoft/Windows/Start Menu/Programs/Startup");

    const QStringList startupDirs = { userStartup, commonStartup };

    for (const QString &rawDir : startupDirs) {
        const QString dir = QDir::cleanPath(rawDir);
        QDir d(dir);
        if (!d.exists())
            continue;

        const QFileInfoList entries = d.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo &fi : entries) {
            // desktop.ini 是系统自己放的
            if (fi.fileName().compare(QStringLiteral("desktop.ini"), Qt::CaseInsensitive) == 0)
                continue;

            StartupItem item;
            item.name = fi.completeBaseName();
            item.filePath = fi.absoluteFilePath();
            item.command = fi.absoluteFilePath();
            item.location = QDir::toNativeSeparators(dir);
            item.source = StartupSource::StartupFolder;
            item.sourceLabel = QStringLiteral("启动文件夹");
            item.canToggle = true;
            item.enabled = readApproved(HKEY_CURRENT_USER, kStartupFolderApproved, fi.fileName(), true);
            item.publisher = publisherOf(fi.absoluteFilePath());
            item.impact = QStringLiteral("未标注");

            result.push_back(item);
        }
    }

    // ---------------- 计划任务
    collectScheduledTasks(&result);

    return result;
}

ActionResult StartupManager::setEnabled(const StartupItem &item, bool enabled)
{
    switch (item.source) {
    case StartupSource::Registry: {
        for (const auto &loc : kRunKeys) {
            if (item.registryKey != QString::fromWCharArray(loc.subKey))
                continue;
            const HKEY root = loc.root;
            if (writeApproved(root, loc.approvedSubKey, item.registryValue, enabled))
                return ActionResult::success(enabled ? QStringLiteral("已启用") : QStringLiteral("已禁用"));
            return ActionResult::failure(QStringLiteral("写入启动项状态失败,可能需要管理员权限"));
        }
        return ActionResult::failure(QStringLiteral("找不到对应的注册表位置"));
    }
    case StartupSource::StartupFolder: {
        const QString fileName = QFileInfo(item.filePath).fileName();
        if (writeApproved(HKEY_CURRENT_USER, kStartupFolderApproved, fileName, enabled))
            return ActionResult::success(enabled ? QStringLiteral("已启用") : QStringLiteral("已禁用"));
        return ActionResult::failure(QStringLiteral("写入启动项状态失败"));
    }
    case StartupSource::ScheduledTask: {
        ITaskService *service = nullptr;
        if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                                    reinterpret_cast<void **>(&service)))
            || !service) {
            return ActionResult::failure(QStringLiteral("无法连接任务计划服务"));
        }

        VARIANT empty;
        VariantInit(&empty);
        ActionResult result = ActionResult::failure(QStringLiteral("任务计划操作失败"));
        if (SUCCEEDED(service->Connect(empty, empty, empty, empty))) {
            ITaskFolder *folder = nullptr;
            const QString folderPath = item.location.section(QLatin1Char('\\'), 0, -2);
            const QString taskName = item.location.section(QLatin1Char('\\'), -1);
            const QString dir = folderPath.isEmpty() ? QStringLiteral("\\") : folderPath + QStringLiteral("\\");

            if (SUCCEEDED(service->GetFolder(BSTR(toWide(dir).c_str()), &folder)) && folder) {
                IRegisteredTask *task = nullptr;
                if (SUCCEEDED(folder->GetTask(BSTR(toWide(taskName).c_str()), &task)) && task) {
                    const HRESULT hr = task->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE);
                    if (SUCCEEDED(hr))
                        result = ActionResult::success(enabled ? QStringLiteral("已启用") : QStringLiteral("已禁用"));
                    else
                        result = ActionResult::failure(QStringLiteral("修改任务状态被拒绝,需要管理员权限"));
                    task->Release();
                }
                folder->Release();
            }
        }
        VariantClear(&empty);
        service->Release();
        return result;
    }
    case StartupSource::Service:
        return ActionResult::failure(QStringLiteral("服务类启动项请到「服务」页面管理"));
    }

    return ActionResult::failure(QStringLiteral("不支持的启动项类型"));
}

ActionResult StartupManager::remove(const StartupItem &item)
{
    if (item.source == StartupSource::Registry) {
        for (const auto &loc : kRunKeys) {
            if (item.registryKey != QString::fromWCharArray(loc.subKey))
                continue;

            HKEY key = nullptr;
            if (RegOpenKeyExW(loc.root, loc.subKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
                return ActionResult::failure(QStringLiteral("打开注册表项失败,可能需要管理员权限"));

            const LONG rc = RegDeleteValueW(key, toWide(item.registryValue).c_str());
            RegCloseKey(key);
            if (rc == ERROR_SUCCESS)
                return ActionResult::success(QStringLiteral("已删除该启动项"));
            return ActionResult::failure(QStringLiteral("删除失败,可能需要管理员权限"));
        }
    }

    if (item.source == StartupSource::StartupFolder) {
        QFile file(item.filePath);
        // 移到回收站而不是直接删,给用户留条后路
        if (file.moveToTrash())
            return ActionResult::success(QStringLiteral("已删除该启动项"));
        return ActionResult::failure(QStringLiteral("删除失败:%1").arg(item.filePath));
    }

    return ActionResult::failure(QStringLiteral("该类型启动项不支持删除,请在对应页面处理"));
}

} // namespace ws
