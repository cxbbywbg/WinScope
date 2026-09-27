#pragma once

// 进程操作。
//
// 所有动作都返回统一的结果结构,失败时带上系统错误码对应的中文说明 ——
// 界面上要能直接告诉用户"为什么没杀掉",而不是静默失败。

#include <QString>
#include <QStringList>

namespace ws {

struct ActionResult {
    bool ok = false;
    QString message;

    static ActionResult success(const QString &msg = QString()) { return { true, msg }; }
    static ActionResult failure(const QString &msg) { return { false, msg }; }
};

class ProcessController
{
public:
    // 温和结束:给进程的主窗口发 WM_CLOSE,由它自己收尾
    static ActionResult closeGracefully(quint32 pid);
    // 强制结束:TerminateProcess,不给人反应机会
    static ActionResult terminate(quint32 pid);
    // 重启:先结束,再按原命令行拉起来
    static ActionResult restart(quint32 pid, const QString &path, const QString &commandLine);

    static ActionResult suspend(quint32 pid);
    static ActionResult resume(quint32 pid);
    static bool isSuspended(quint32 pid);

    static ActionResult setPriority(quint32 pid, quint32 priorityClass);
    static ActionResult setAffinity(quint32 pid, quint64 affinityMask);

    static bool queryAffinity(quint32 pid, quint64 *mask);
    static quint32 queryPriority(quint32 pid);

    // 供下拉框使用,顺序与 winbase.h 的常量一致
    static QStringList priorityNames();
    static quint32 priorityValueAt(int index);
    static int priorityIndex(quint32 value);
};

} // namespace ws
