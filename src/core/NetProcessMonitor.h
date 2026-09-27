#pragma once

// 按进程统计网络流量。
//
// 这是"流量排行"唯一可靠的数据来源:内核网络栈只通过 ETW 对外暴露收发字节数,
// iphlpapi 的连接表只有地址和端口,没有流量。
//
// 事件来源:Microsoft-Windows-Kernel-Network
//   ID 10 / 11  TCP  IPv4  发送 / 接收
//   ID 26 / 27  TCP  IPv6  发送 / 接收
//   ID 42 / 43  UDP  IPv4  发送 / 接收
//   ID 58 / 59  UDP  IPv6  发送 / 接收
// 这些事件的载荷前两个字段固定是 PID 和 size,自 Vista 起未变过。
//
// 启动实时 ETW 会话需要管理员权限或加入"性能日志用户"组,
// 权限不足时 start() 返回 false,界面应提示以管理员身份重启。

#include <QHash>
#include <QMutex>
#include <QString>

#include <windows.h>
#include <evntrace.h>
// EVENT_RECORD 的完整定义与 PROCESS_TRACE_MODE_* 都在 evntcons.h,
// evntrace.h 只给了个前置声明
#include <evntcons.h>

#include <thread>

namespace ws {

struct NetTraffic {
    double rxBytesPerSec = 0.0;
    double txBytesPerSec = 0.0;
    quint64 rxBytesTotal = 0;
    quint64 txBytesTotal = 0;
};

class NetProcessMonitor
{
public:
    NetProcessMonitor();
    ~NetProcessMonitor();

    NetProcessMonitor(const NetProcessMonitor &) = delete;
    NetProcessMonitor &operator=(const NetProcessMonitor &) = delete;

    bool start();
    void stop();
    bool isRunning() const { return m_trace != INVALID_PROCESSTRACE_HANDLE; }

    const QString &lastError() const { return m_lastError; }

    // 取快照并清零区间计数,速率按距上次调用的间隔折算
    QHash<quint32, NetTraffic> takeSnapshot();

private:
    static void WINAPI eventRecordCallback(EVENT_RECORD *eventRecord);
    void handleEvent(const EVENT_RECORD *eventRecord);

    TRACEHANDLE m_session = 0;
    TRACEHANDLE m_trace = INVALID_PROCESSTRACE_HANDLE;
    std::thread m_thread;
    QString m_lastError;

    mutable QMutex m_mutex;
    QHash<quint32, QPair<quint64, quint64>> m_raw;        // pid -> (rx, tx) 累计
    QHash<quint32, QPair<quint64, quint64>> m_prevTotals; // 上次快照时的累计值
    qint64 m_lastSnapshotMs = 0;
};

} // namespace ws
