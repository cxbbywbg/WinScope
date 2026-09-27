#pragma once

// 进程采样。
//
// 一次 NtQuerySystemInformation(SystemProcessInformation) 就能拿到所有进程的
// CPU 时间、内存、线程/句柄数、读写字节数和父子关系 —— 这是最省开销的取法,
// 比逐个 OpenProcess + GetProcessTimes 快一个数量级。
//
// 名称之外的信息(路径/用户/命令行/文件说明)很贵,所以按 PID 缓存,只对没见过的进程查一次。

#include "core/Types.h"

#include <QHash>
#include <QString>

namespace ws {

struct NetTraffic;

// GPU 与网络数据由 SystemSampler 从别的采样器取来后注入,
// 避免 ProcessSampler 反向依赖它们
struct ProcessExtras {
    const QHash<quint32, double> *gpuUsage = nullptr;
    const QHash<quint32, quint64> *gpuDedicated = nullptr;
    const QHash<quint32, quint64> *gpuShared = nullptr;
    const QHash<quint32, NetTraffic> *netTraffic = nullptr;
};

class ProcessSampler
{
public:
    ProcessSampler();

    void loadStaticInfo();
    ProcessSnapshot sample(const ProcessExtras &extras);

    // 取最近一次快照里的某个进程
    bool findProcess(quint32 pid, ProcessInfo *out) const;

    // 详情面板用:对指定进程做一次完整的昂贵查询(路径/用户/命令行/架构)
    ProcessInfo describe(quint32 pid) const;

private:
    struct ProcTimes {
        quint64 kernel = 0;
        quint64 user = 0;
        quint64 readBytes = 0;
        quint64 writeBytes = 0;
    };

    void refreshCommandLines();
    // allowQuery 为 false 时只读缓存,不再发起任何系统调用
    void fillDetails(ProcessInfo *info, bool allowQuery);
    void dropStaleCache(const QVector<ProcessInfo> &list);

    QHash<quint32, ProcTimes> m_prev;
    QHash<quint32, QString> m_pathCache;
    QHash<quint32, QString> m_userCache;
    QHash<quint32, QString> m_cmdCache;
    QHash<quint32, QString> m_archCache;
    QHash<quint32, bool> m_elevatedCache;
    QHash<QString, QString> m_descByPath;   // 路径 -> 文件说明

    QVector<ProcessInfo> m_last;
    qint64 m_lastSampleMs = 0;
    qint64 m_lastCmdRefreshMs = 0;
    int m_logicalCores = 1;
    bool m_hasPrev = false;
};

} // namespace ws
