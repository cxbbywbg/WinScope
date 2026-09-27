#pragma once

// 网络适配器采样。
//
// 速率不靠 PDH,而是直接读 GetIfTable2 的 InOctets/OutOctets 自己算差值 ——
// 这样能顺带拿到每个网卡的链路状态、类型、链路速率,也避免 PDH 实例名在
// 中文系统上被本地化。
// IP/网关/DNS 由 GetAdaptersAddresses 补齐。

#include "core/Types.h"

#include <QHash>
#include <QVector>

namespace ws {

class NetSampler
{
public:
    NetSampler();

    NetInfo sample();

    // 按适配器索引取当前速率,供网络页做实时曲线
    double rxBytesPerSec() const { return m_totalRx; }
    double txBytesPerSec() const { return m_totalTx; }

private:
    struct Prev {
        quint64 in = 0;
        quint64 out = 0;
    };

    QHash<quint64, Prev> m_prev;   // key = InterfaceLuid
    qint64 m_lastSampleMs = 0;
    double m_totalRx = 0.0;
    double m_totalTx = 0.0;
    bool m_hasPrev = false;
};

} // namespace ws
