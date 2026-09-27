// winsock2 必须在 windows.h 之前,否则会先被 winsock.h 占坑导致重定义。
// Win32Utils.h 内部会拉 windows.h,所以这几个头必须排在它前面。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include "core/NetConnectionSampler.h"

#include "core/Win32Utils.h"
#include "core/WinsockInit.h"

#include <QFileInfo>

#include <vector>

namespace ws {

namespace {

QString ipv4ToString(DWORD addr)
{
    // 表里是小端存放的
    IN_ADDR a{};
    a.S_un.S_addr = addr;
    wchar_t buf[INET_ADDRSTRLEN] = {};
    if (InetNtopW(AF_INET, &a, buf, INET_ADDRSTRLEN))
        return fromWide(buf);
    return QString();
}

QString ipv6ToString(const UCHAR *bytes)
{
    IN6_ADDR a{};
    memcpy(a.u.Byte, bytes, 16);
    wchar_t buf[INET6_ADDRSTRLEN] = {};
    if (InetNtopW(AF_INET6, &a, buf, INET6_ADDRSTRLEN))
        return fromWide(buf);
    return QString();
}

QString tcpStateToString(DWORD state)
{
    switch (state) {
    case MIB_TCP_STATE_CLOSED:
        return QStringLiteral("CLOSED");
    case MIB_TCP_STATE_LISTEN:
        return QStringLiteral("LISTEN");
    case MIB_TCP_STATE_SYN_SENT:
        return QStringLiteral("SYN_SENT");
    case MIB_TCP_STATE_SYN_RCVD:
        return QStringLiteral("SYN_RCVD");
    case MIB_TCP_STATE_ESTAB:
        return QStringLiteral("ESTABLISHED");
    case MIB_TCP_STATE_FIN_WAIT1:
        return QStringLiteral("FIN_WAIT1");
    case MIB_TCP_STATE_FIN_WAIT2:
        return QStringLiteral("FIN_WAIT2");
    case MIB_TCP_STATE_CLOSE_WAIT:
        return QStringLiteral("CLOSE_WAIT");
    case MIB_TCP_STATE_CLOSING:
        return QStringLiteral("CLOSING");
    case MIB_TCP_STATE_LAST_ACK:
        return QStringLiteral("LAST_ACK");
    case MIB_TCP_STATE_TIME_WAIT:
        return QStringLiteral("TIME_WAIT");
    case MIB_TCP_STATE_DELETE_TCB:
        return QStringLiteral("DELETE_TCB");
    default:
        return QStringLiteral("UNKNOWN");
    }
}

// 统一签名,方便下面用一个模板函数收口"两次调用"的样板
using TableFn = DWORD (*)(PVOID, PDWORD, BOOL, ULONG, ULONG, ULONG);

DWORD tcpTableFn(PVOID table, PDWORD size, BOOL order, ULONG family, ULONG cls, ULONG reserved)
{
    return GetExtendedTcpTable(table, size, order, family, TCP_TABLE_CLASS(cls), reserved);
}

DWORD udpTableFn(PVOID table, PDWORD size, BOOL order, ULONG family, ULONG cls, ULONG reserved)
{
    return GetExtendedUdpTable(table, size, order, family, UDP_TABLE_CLASS(cls), reserved);
}

// 所有 *OWNER_PID* 接口都是"先问长度,再自己给缓冲"的两段式
bool fetchTable(TableFn fn, ULONG family, ULONG cls, std::vector<BYTE> *out)
{
    ULONG size = 0;
    DWORD rc = fn(nullptr, &size, FALSE, family, cls, 0);
    if (rc != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return false;

    out->assign(size, 0);
    rc = fn(out->data(), &size, FALSE, family, cls, 0);
    return rc == NO_ERROR;
}

} // namespace

QString NetConnectionSampler::processNameFor(quint32 pid)
{
    if (pid == 0)
        return QStringLiteral("System Idle / 未知");

    const auto it = m_nameCache.constFind(pid);
    if (it != m_nameCache.constEnd())
        return it.value();

    QString name;
    const QString path = queryProcessPath(pid);
    if (!path.isEmpty())
        name = QFileInfo(path).fileName();
    if (name.isEmpty())
        name = QStringLiteral("PID %1").arg(pid);

    m_nameCache.insert(pid, name);
    return name;
}

QVector<NetConnection> NetConnectionSampler::sample()
{
    ensureWinsock();

    QVector<NetConnection> result;

    // 缓存别无限涨,进程来来去去很快
    if (m_nameCache.size() > 4096)
        m_nameCache.clear();

    // ---------------- TCP IPv4
    std::vector<BYTE> buf;
    if (fetchTable(tcpTableFn, AF_INET, TCP_TABLE_OWNER_PID_ALL, &buf)) {
        auto *table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID *>(buf.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const auto &row = table->table[i];
            NetConnection c;
            c.protocol = QStringLiteral("TCP");
            c.localAddress = ipv4ToString(row.dwLocalAddr);
            c.localPort = ntohs(static_cast<u_short>(row.dwLocalPort));
            c.remoteAddress = ipv4ToString(row.dwRemoteAddr);
            c.remotePort = ntohs(static_cast<u_short>(row.dwRemotePort));
            c.state = tcpStateToString(row.dwState);
            c.pid = row.dwOwningPid;
            c.processName = processNameFor(c.pid);
            result.push_back(c);
        }
    }

    // ---------------- TCP IPv6
    if (fetchTable(tcpTableFn, AF_INET6, TCP_TABLE_OWNER_PID_ALL, &buf)) {
        auto *table = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID *>(buf.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const auto &row = table->table[i];
            NetConnection c;
            c.protocol = QStringLiteral("TCPv6");
            c.localAddress = ipv6ToString(row.ucLocalAddr);
            c.localPort = ntohs(static_cast<u_short>(row.dwLocalPort));
            c.remoteAddress = ipv6ToString(row.ucRemoteAddr);
            c.remotePort = ntohs(static_cast<u_short>(row.dwRemotePort));
            c.state = tcpStateToString(row.dwState);
            c.pid = row.dwOwningPid;
            c.processName = processNameFor(c.pid);
            c.ipv6 = true;
            result.push_back(c);
        }
    }
    m_tcpCount = result.size();

    // ---------------- UDP IPv4
    if (fetchTable(udpTableFn, AF_INET, UDP_TABLE_OWNER_PID, &buf)) {
        auto *table = reinterpret_cast<MIB_UDPTABLE_OWNER_PID *>(buf.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const auto &row = table->table[i];
            NetConnection c;
            c.protocol = QStringLiteral("UDP");
            c.localAddress = ipv4ToString(row.dwLocalAddr);
            c.localPort = ntohs(static_cast<u_short>(row.dwLocalPort));
            c.state = QStringLiteral("—");
            c.pid = row.dwOwningPid;
            c.processName = processNameFor(c.pid);
            result.push_back(c);
        }
    }

    // ---------------- UDP IPv6
    if (fetchTable(udpTableFn, AF_INET6, UDP_TABLE_OWNER_PID, &buf)) {
        auto *table = reinterpret_cast<MIB_UDP6TABLE_OWNER_PID *>(buf.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const auto &row = table->table[i];
            NetConnection c;
            c.protocol = QStringLiteral("UDPv6");
            c.localAddress = ipv6ToString(row.ucLocalAddr);
            c.localPort = ntohs(static_cast<u_short>(row.dwLocalPort));
            c.state = QStringLiteral("—");
            c.pid = row.dwOwningPid;
            c.processName = processNameFor(c.pid);
            c.ipv6 = true;
            result.push_back(c);
        }
    }
    m_udpCount = result.size() - m_tcpCount;

    return result;
}

} // namespace ws
