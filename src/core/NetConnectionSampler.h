#pragma once

// TCP / UDP 连接表。
// 数据源是 iphlpapi 的 *OWNER_PID* 系列接口,每条连接都带归属进程 PID。

#include <QHash>
#include <QMetaType>
#include <QString>
#include <QVector>

namespace ws {

struct NetConnection {
    QString protocol;        // TCP / TCPv6 / UDP / UDPv6
    QString localAddress;
    quint16 localPort = 0;
    QString remoteAddress;
    quint16 remotePort = 0;
    QString state;           // ESTABLISHED / LISTEN / ...
    quint32 pid = 0;
    QString processName;
    bool ipv6 = false;
};

class NetConnectionSampler
{
public:
    QVector<NetConnection> sample();

    int tcpCount() const { return m_tcpCount; }
    int udpCount() const { return m_udpCount; }

private:
    QString processNameFor(quint32 pid);

    int m_tcpCount = 0;
    int m_udpCount = 0;
    QHash<quint32, QString> m_nameCache;
};

} // namespace ws

Q_DECLARE_METATYPE(ws::NetConnection)
Q_DECLARE_METATYPE(QVector<ws::NetConnection>)
