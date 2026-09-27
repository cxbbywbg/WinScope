// winsock2 必须在 windows.h 之前,否则会先被 winsock.h 占坑导致重定义。
// Win32Utils.h 内部会拉 windows.h,所以这几个头必须排在它前面。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <iptypes.h>
#include <windows.h>

#include "core/NetSampler.h"

#include "core/Win32Utils.h"
#include "core/WinsockInit.h"

#include <QDateTime>
#include <QHash>

#include <vector>

namespace ws {

namespace {

bool containsAny(const QString &haystack, const char *const *needles, int count)
{
    for (int i = 0; i < count; ++i) {
        if (haystack.contains(QLatin1String(needles[i])))
            return true;
    }
    return false;
}

AdapterKind classify(const QString &friendlyName, const QString &description, ULONG ifType)
{
    // 回环口必须先判,否则描述里的 "loopback" 会被当成虚拟网卡留下来
    if (ifType == IF_TYPE_SOFTWARE_LOOPBACK)
        return AdapterKind::Loopback;

    const QString desc = description.toLower();

    static const char *vpnHints[] = { "vpn", "openvpn", "wireguard", "zerotier", "tailscale", "softether",
                                      "sstp", "l2tp", "pptp", "anyconnect", "molesdn", "tunnel", "wintun" };
    if (containsAny(desc, vpnHints, int(sizeof(vpnHints) / sizeof(vpnHints[0]))))
        return AdapterKind::Vpn;

    static const char *virtualHints[] = { "virtual", "vmware", "virtualbox", "hyper-v", "tap",
                                          "docker", "wsl", "teredo", "isatap", "ip-https", "6to4", "wi-fi direct",
                                          "bluetooth", "kernel debug", "wan miniport", "remote ndis" };
    if (containsAny(desc, virtualHints, int(sizeof(virtualHints) / sizeof(virtualHints[0]))))
        return AdapterKind::Virtual;

    switch (ifType) {
    case IF_TYPE_IEEE80211:
        return AdapterKind::WiFi;
    case IF_TYPE_TUNNEL:
        return AdapterKind::Vpn;
    case IF_TYPE_ETHERNET_CSMACD:
        return AdapterKind::Ethernet;
    default:
        break;
    }
    Q_UNUSED(friendlyName);
    return AdapterKind::Other;
}

// 这些是系统自带的伪适配器,列出来只会干扰视线
bool isNoiseAdapter(const QString &description, AdapterKind kind)
{
    if (kind == AdapterKind::Loopback)
        return true;
    static const char *noise[] = { "wan miniport", "kernel debug", "wi-fi direct", "6to4", "ip-https",
                                   "teredo tunneling", "network monitor" };
    const QString desc = description.toLower();
    return containsAny(desc, noise, int(sizeof(noise) / sizeof(noise[0])));
}

QString macToString(const BYTE *addr, ULONG len)
{
    if (!addr || len == 0 || len > 8)
        return QString();
    QStringList parts;
    for (ULONG i = 0; i < len; ++i)
        parts << QStringLiteral("%1").arg(addr[i], 2, 16, QLatin1Char('0')).toUpper();
    return parts.join(QLatin1Char('-'));
}

QString sockaddrToString(const SOCKET_ADDRESS &addr)
{
    if (!addr.lpSockaddr)
        return QString();

    wchar_t buf[INET6_ADDRSTRLEN] = {};
    DWORD len = INET6_ADDRSTRLEN;
    if (WSAAddressToStringW(addr.lpSockaddr, addr.iSockaddrLength, nullptr, buf, &len) != 0)
        return QString();

    QString s = fromWide(buf);
    // IPv6 的 %scope 后缀界面上没必要显示
    const int pct = s.indexOf(QLatin1Char('%'));
    if (pct > 0)
        s = s.left(pct);
    return s;
}

} // namespace

NetSampler::NetSampler() = default;

NetInfo NetSampler::sample()
{
    // 没有这一步,下面 WSAAddressToString 会全部失败,IP/网关/DNS 一律是空的
    ensureWinsock();

    NetInfo info;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const double elapsed = m_hasPrev && m_lastSampleMs > 0 ? double(nowMs - m_lastSampleMs) / 1000.0 : 0.0;

    // GetAdaptersAddresses 每个网卡只返回一条记录。
    // 不用 GetIfTable2 是因为它按 NDIS 过滤驱动逐层展开,同一块 Intel 网卡
    // 会以 "WLAN-QoS Packet Scheduler-0000" 之类的名字重复出现四五次。
    ULONG bufLen = 32 * 1024;
    std::vector<BYTE> buf(bufLen);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_PREFIX;

    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()), &bufLen);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.assign(bufLen, 0);
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()), &bufLen);
    }
    if (rc != NO_ERROR)
        return info;

    QHash<quint64, Prev> current;
    double physicalRx = 0.0;
    double physicalTx = 0.0;
    double maxAnyRx = 0.0;
    double maxAnyTx = 0.0;

    for (auto *a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()); a; a = a->Next) {
        const QString friendly = fromWide(a->FriendlyName);
        const QString description = fromWide(a->Description);
        const AdapterKind kind = classify(friendly, description, a->IfType);
        if (isNoiseAdapter(description, kind))
            continue;

        NetAdapter ad;
        ad.name = friendly;
        ad.description = description;
        ad.kind = kind;
        ad.mac = macToString(a->PhysicalAddress, a->PhysicalAddressLength);

        for (auto *ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            const QString ip = sockaddrToString(ua->Address);
            if (ip.isEmpty())
                continue;
            if (ua->Address.lpSockaddr->sa_family == AF_INET)
                ad.ipv4 << ip;
            else if (ua->Address.lpSockaddr->sa_family == AF_INET6)
                ad.ipv6 << ip;
        }
        for (auto *ga = a->FirstGatewayAddress; ga; ga = ga->Next)
            ad.gateways << sockaddrToString(ga->Address);
        for (auto *da = a->FirstDnsServerAddress; da; da = da->Next)
            ad.dns << sockaddrToString(da->Address);

        // 字节计数只能从 MIB_IF_ROW2 拿,按 Luid 单独查一条
        MIB_IF_ROW2 row{};
        row.InterfaceLuid = a->Luid;
        if (GetIfEntry2(&row) == NO_ERROR) {
            ad.connected = (row.MediaConnectState == MediaConnectStateConnected);
            ad.hasLink = (row.OperStatus == IfOperStatusUp);
            ad.linkSpeedBps = row.ReceiveLinkSpeed;
            ad.rxBytesTotal = row.InOctets;
            ad.txBytesTotal = row.OutOctets;

            const quint64 key = a->Luid.Value;
            Prev p;
            p.in = row.InOctets;
            p.out = row.OutOctets;

            if (elapsed > 0.0) {
                const auto it = m_prev.constFind(key);
                if (it != m_prev.constEnd()) {
                    // 计数器回绕(网卡重连/重置)时给 0,别算出负值
                    if (p.in >= it->in)
                        ad.rxBytesPerSec = double(p.in - it->in) / elapsed;
                    if (p.out >= it->out)
                        ad.txBytesPerSec = double(p.out - it->out) / elapsed;
                }
            }
            current.insert(key, p);
        } else {
            ad.connected = (a->OperStatus == IfOperStatusUp);
            ad.hasLink = ad.connected;
        }

        // 物理网卡可以相加;虚拟/VPN 网卡的流量往往是物理链路上流量的转发,
        // 直接累加会重复计算
        if (kind == AdapterKind::Ethernet || kind == AdapterKind::WiFi) {
            physicalRx += ad.rxBytesPerSec;
            physicalTx += ad.txBytesPerSec;
            info.rxBytesTotal += ad.rxBytesTotal;
            info.txBytesTotal += ad.txBytesTotal;
        } else {
            maxAnyRx = qMax(maxAnyRx, ad.rxBytesPerSec);
            maxAnyTx = qMax(maxAnyTx, ad.txBytesPerSec);
        }

        info.adapters.push_back(ad);
    }

    // 全部走虚拟网卡(ZeroTier / Tailscale 之类的场景)时,物理网卡可能是 0,
    // 这时取虚拟网卡里的最大值,避免总览显示成 0
    if (physicalRx <= 0.0 && physicalTx <= 0.0) {
        info.rxBytesPerSec = maxAnyRx;
        info.txBytesPerSec = maxAnyTx;
    } else {
        info.rxBytesPerSec = physicalRx;
        info.txBytesPerSec = physicalTx;
    }

    m_prev = current;
    m_lastSampleMs = nowMs;
    m_hasPrev = true;
    m_totalRx = info.rxBytesPerSec;
    m_totalTx = info.txBytesPerSec;

    return info;
}

} // namespace ws
