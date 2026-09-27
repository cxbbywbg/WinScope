#pragma once

// 采样调度器。
//
// 所有系统调用都在独立线程里跑,采样结果通过排队信号送回 UI 线程 ——
// 这样即使某次 WMI 查询卡了 200ms,界面也不会掉帧。
//
// 节奏(默认 1 秒一拍):
//   每拍      系统快照(CPU/内存/GPU/磁盘/网络)
//   每拍      进程快照
//   每 5 拍   连接表(TCP/UDP)
//   每 30 拍  分区容量

#include "core/NetConnectionSampler.h"
#include "core/Types.h"

#include <QObject>
#include <QString>

class QThread;

namespace ws {

class SamplerWorker;

class SystemSampler : public QObject
{
    Q_OBJECT

public:
    explicit SystemSampler(QObject *parent = nullptr);
    ~SystemSampler() override;

    void start();
    void stop();

    // 采样间隔,单位毫秒
    void setInterval(int ms);
    int interval() const { return m_intervalMs; }

    // 当前进程是否以管理员身份运行
    static bool isElevated();

    // 每进程网络流量统计是否可用(需要管理员权限)
    bool networkMonitorRunning() const { return m_netMonitorRunning; }
    QString networkMonitorError() const { return m_netMonitorError; }

public slots:
    // 立刻补一次采样,不改变节奏
    void refreshNow();

signals:
    void systemSnapshotReady(const ws::SystemSnapshot &snapshot);
    void processSnapshotReady(const ws::ProcessSnapshot &snapshot);
    void connectionsReady(const QVector<ws::NetConnection> &connections);
    void networkMonitorStateChanged(bool running, const QString &error);

private:
    QThread *m_thread = nullptr;
    SamplerWorker *m_worker = nullptr;
    int m_intervalMs = 1000;
    bool m_netMonitorRunning = false;
    QString m_netMonitorError;
};

} // namespace ws
