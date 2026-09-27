#include "ui/pages/NetworkPage.h"

#include "app/Theme.h"
#include "core/Win32Utils.h"
#include "ui/widgets/Card.h"
#include "ui/widgets/LineChart.h"
#include "ui/widgets/StatRow.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace ws {

namespace {

QString adapterKindText(AdapterKind kind)
{
    switch (kind) {
    case AdapterKind::Ethernet:
        return QStringLiteral("以太网");
    case AdapterKind::WiFi:
        return QStringLiteral("无线");
    case AdapterKind::Virtual:
        return QStringLiteral("虚拟");
    case AdapterKind::Vpn:
        return QStringLiteral("VPN");
    case AdapterKind::Loopback:
        return QStringLiteral("回环");
    case AdapterKind::Other:
    default:
        return QStringLiteral("其它");
    }
}

QColor adapterKindColor(AdapterKind kind)
{
    switch (kind) {
    case AdapterKind::Ethernet:
        return theme::accent();
    case AdapterKind::WiFi:
        return theme::ok();
    case AdapterKind::Vpn:
        return theme::purple();
    case AdapterKind::Virtual:
        return theme::textDim();
    default:
        return theme::textFaint();
    }
}

QTreeWidget *makeTable(const QStringList &headers, QWidget *parent)
{
    auto *table = new QTreeWidget(parent);
    table->setColumnCount(headers.size());
    table->setHeaderLabels(headers);
    table->setRootIsDecorated(false);
    table->setAlternatingRowColors(true);
    table->setUniformRowHeights(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->header()->setStretchLastSection(false);
    table->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int i = 1; i < headers.size(); ++i)
        table->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    return table;
}

} // namespace

NetworkPage::NetworkPage(QWidget *parent)
    : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 18);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    auto *left = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(12);
    buildLeft(leftLayout);

    auto *right = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(12);
    buildRight(rightLayout);

    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({ 760, 560 });

    root->addWidget(splitter, 1);
}

QString NetworkPage::pageTitle() const
{
    return QStringLiteral("网络监控");
}

QString NetworkPage::pageSubtitle() const
{
    return QStringLiteral("网卡状态与速率、TCP/UDP 连接明细、按进程的流量排行");
}

void NetworkPage::buildLeft(QVBoxLayout *root)
{
    // ---------------- 流量曲线
    auto *chartCard = new Card(QStringLiteral("网络流量"), this);
    chartCard->setHint(QStringLiteral("最近 60 秒 · 仅物理网卡合计"));
    m_chart = new LineChart(chartCard);
    m_chart->setCapacity(60);
    m_chart->setFormatter([](double v) { return formatBytesPerSec(v); });
    m_seriesRx = m_chart->addSeries(QStringLiteral("下行"), theme::ok());
    m_seriesTx = m_chart->addSeries(QStringLiteral("上行"), theme::pink());
    chartCard->body()->addWidget(m_chart);

    m_totalRow = new StatRow(QStringLiteral("当前合计"), chartCard);
    chartCard->body()->addWidget(m_totalRow);
    root->addWidget(chartCard, 2);

    // ---------------- 连接列表
    auto *connectionCard = new Card(QStringLiteral("网络连接"), this);
    {
        auto *filter = new QComboBox(connectionCard);
        filter->addItems({ QStringLiteral("全部"), QStringLiteral("TCP"), QStringLiteral("UDP"),
                           QStringLiteral("LISTEN"), QStringLiteral("ESTABLISHED") });
        filter->setFixedWidth(126);
        m_protocolFilter = filter;
        connectionCard->addHeaderWidget(filter);

        m_connectionSearch = new QLineEdit(connectionCard);
        m_connectionSearch->setPlaceholderText(QStringLiteral("按进程 / 地址过滤…"));
        m_connectionSearch->setClearButtonEnabled(true);
        m_connectionSearch->setFixedWidth(190);
        connectionCard->addHeaderWidget(m_connectionSearch);

        m_connectionCount = new QLabel(connectionCard);
        m_connectionCount->setObjectName(QStringLiteral("CardHint"));
        connectionCard->addHeaderWidget(m_connectionCount);

        connect(filter, &QComboBox::currentIndexChanged, this, [this]() { updateConnections(); });
        connect(m_connectionSearch, &QLineEdit::textChanged, this, [this]() { updateConnections(); });
    }

    m_connectionTable = makeTable({ QStringLiteral("进程"), QStringLiteral("协议"), QStringLiteral("本地地址"),
                                    QStringLiteral("远程地址"), QStringLiteral("状态"), QStringLiteral("PID") },
                                  connectionCard);
    // 进程名这一列是 Stretch,但后面 5 列都是 ResizeToContents,面板一窄就被挤成
    // 「PI...」了。给它一个固定宽度,至少能看清是谁在连。
    m_connectionTable->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_connectionTable->setColumnWidth(0, 150);
    connectionCard->body()->addWidget(m_connectionTable);
    root->addWidget(connectionCard, 3);
}

void NetworkPage::buildRight(QVBoxLayout *root)
{
    // ---------------- 网卡
    auto *adapterCard = new Card(QStringLiteral("网络适配器"), this);
    m_adapterTable = makeTable({ QStringLiteral("连接名"), QStringLiteral("类型"), QStringLiteral("状态"),
                                 QStringLiteral("IPv4"), QStringLiteral("下行"), QStringLiteral("上行"),
                                 QStringLiteral("累计 ↓ / ↑") },
                               adapterCard);
    adapterCard->body()->addWidget(m_adapterTable);
    root->addWidget(adapterCard, 3);

    // ---------------- 流量排行
    auto *rankCard = new Card(QStringLiteral("流量排行"), this);
    rankCard->setHint(QStringLiteral("需要管理员权限"));
    m_rankingTable = makeTable({ QStringLiteral("进程"), QStringLiteral("PID"), QStringLiteral("下行"),
                                 QStringLiteral("上行"), QStringLiteral("合计") },
                               rankCard);
    rankCard->body()->addWidget(m_rankingTable);
    root->addWidget(rankCard, 3);
}

void NetworkPage::onSystemSnapshot(const SystemSnapshot &snapshot)
{
    if (m_paused || !snapshot.valid)
        return;

    m_chart->append(m_seriesRx, snapshot.net.rxBytesPerSec);
    m_chart->append(m_seriesTx, snapshot.net.txBytesPerSec);
    m_totalRow->setValue(QStringLiteral("↓ %1    ↑ %2")
                             .arg(formatBytesPerSec(snapshot.net.rxBytesPerSec),
                                  formatBytesPerSec(snapshot.net.txBytesPerSec)));

    updateAdapters(snapshot.net);
}

void NetworkPage::onConnections(const QVector<NetConnection> &connections)
{
    if (m_paused)
        return;
    m_connections = connections;
    updateConnections();
}

void NetworkPage::onProcessSnapshot(const ProcessSnapshot &snapshot)
{
    if (m_paused)
        return;

    // 顺手留一份 pid -> 进程名,连接表补名字用
    m_processNames.clear();
    m_processNames.reserve(snapshot.processes.size());
    for (const ProcessInfo &p : snapshot.processes) {
        if (!p.name.isEmpty())
            m_processNames.insert(p.pid, p.name);
    }

    updateRanking(snapshot);
}

void NetworkPage::onSamplingPaused(bool paused)
{
    m_paused = paused;
}

void NetworkPage::updateAdapters(const NetInfo &net)
{
    QSet<QString> seen;

    for (const NetAdapter &adapter : net.adapters) {
        const QString key = adapter.name;
        seen.insert(key);

        QTreeWidgetItem *item = m_adapterRows.value(key, nullptr);
        if (!item) {
            item = new QTreeWidgetItem(m_adapterTable);
            item->setText(0, adapter.name);
            item->setToolTip(0, adapter.description);
            m_adapterRows.insert(key, item);
        }

        item->setText(1, adapterKindText(adapter.kind));
        item->setForeground(1, adapterKindColor(adapter.kind));

        const QString status = adapter.connected ? QStringLiteral("已连接")
                                                 : (adapter.hasLink ? QStringLiteral("无连接") : QStringLiteral("已断开"));
        item->setText(2, status);
        item->setForeground(2, adapter.connected ? theme::ok() : theme::textFaint());

        item->setText(3, adapter.ipv4.isEmpty() ? QStringLiteral("—") : adapter.ipv4.join(QStringLiteral(", ")));
        item->setText(4, adapter.rxBytesPerSec > 0 ? formatBytesPerSec(adapter.rxBytesPerSec)
                                                   : QStringLiteral("—"));
        item->setText(5, adapter.txBytesPerSec > 0 ? formatBytesPerSec(adapter.txBytesPerSec)
                                                   : QStringLiteral("—"));
        item->setText(6, QStringLiteral("%1 / %2").arg(formatBytes(adapter.rxBytesTotal),
                                                       formatBytes(adapter.txBytesTotal)));

        if (!adapter.gateways.isEmpty() || !adapter.dns.isEmpty() || !adapter.mac.isEmpty()) {
            item->setToolTip(3, QStringLiteral("MAC: %1\n网关: %2\nDNS: %3")
                                    .arg(adapter.mac.isEmpty() ? QStringLiteral("—") : adapter.mac,
                                         adapter.gateways.join(QStringLiteral(", ")),
                                         adapter.dns.join(QStringLiteral(", "))));
        }
    }

    for (auto it = m_adapterRows.begin(); it != m_adapterRows.end();) {
        if (!seen.contains(it.key())) {
            delete it.value();
            it = m_adapterRows.erase(it);
        } else {
            ++it;
        }
    }
}

void NetworkPage::updateConnections()
{
    if (!m_connectionTable)
        return;

    const QString filter = m_protocolFilter ? m_protocolFilter->currentText() : QString();
    const QString keyword = m_connectionSearch ? m_connectionSearch->text().trimmed() : QString();

    m_connectionTable->setUpdatesEnabled(false);
    m_connectionTable->clear();

    int shown = 0;
    for (const NetConnection &c : m_connections) {
        if (filter == QLatin1String("TCP") && !c.protocol.startsWith(QLatin1String("TCP")))
            continue;
        if (filter == QLatin1String("UDP") && !c.protocol.startsWith(QLatin1String("UDP")))
            continue;
        if ((filter == QLatin1String("LISTEN") || filter == QLatin1String("ESTABLISHED"))
            && c.state != filter)
            continue;

        if (!keyword.isEmpty()) {
            const bool hit = c.processName.contains(keyword, Qt::CaseInsensitive)
                || c.localAddress.contains(keyword) || c.remoteAddress.contains(keyword)
                || QString::number(c.pid) == keyword;
            if (!hit)
                continue;
        }

        auto *item = new QTreeWidgetItem(m_connectionTable);
        // 连接采样器拿不到名字时会填「PID n」,这时用进程快照里的名字顶上
        QString processName = c.processName;
        if (processName.startsWith(QLatin1String("PID "))) {
            const auto it = m_processNames.constFind(c.pid);
            if (it != m_processNames.constEnd())
                processName = it.value();
        }
        item->setText(0, processName);
        item->setText(1, c.protocol);
        item->setText(2, QStringLiteral("%1:%2").arg(c.localAddress).arg(c.localPort));
        item->setText(3, c.remoteAddress.isEmpty() ? QStringLiteral("—")
                                                   : QStringLiteral("%1:%2").arg(c.remoteAddress).arg(c.remotePort));
        item->setText(4, c.state);
        item->setText(5, QString::number(c.pid));

        if (c.state == QLatin1String("ESTABLISHED"))
            item->setForeground(4, theme::ok());
        else if (c.state == QLatin1String("LISTEN"))
            item->setForeground(4, theme::accent());
        else
            item->setForeground(4, theme::textDim());

        ++shown;
    }

    m_connectionTable->setUpdatesEnabled(true);

    if (m_connectionCount) {
        m_connectionCount->setText(QStringLiteral("显示 %1 / %2 条").arg(shown).arg(m_connections.size()));
    }
}

void NetworkPage::updateRanking(const ProcessSnapshot &snapshot)
{
    if (!m_rankingTable)
        return;

    QVector<ProcessInfo> ranked;
    ranked.reserve(snapshot.processes.size());
    for (const ProcessInfo &p : snapshot.processes) {
        if (p.netRxBytesPerSec > 0.0 || p.netTxBytesPerSec > 0.0)
            ranked.push_back(p);
    }

    std::sort(ranked.begin(), ranked.end(), [](const ProcessInfo &a, const ProcessInfo &b) {
        return (a.netRxBytesPerSec + a.netTxBytesPerSec) > (b.netRxBytesPerSec + b.netTxBytesPerSec);
    });

    m_rankingTable->setUpdatesEnabled(false);
    m_rankingTable->clear();

    for (int i = 0; i < ranked.size() && i < 60; ++i) {
        const ProcessInfo &p = ranked.at(i);
        auto *item = new QTreeWidgetItem(m_rankingTable);
        item->setText(0, p.name);
        item->setText(1, QString::number(p.pid));
        item->setText(2, p.netRxBytesPerSec > 0 ? formatBytesPerSec(p.netRxBytesPerSec) : QStringLiteral("—"));
        item->setText(3, p.netTxBytesPerSec > 0 ? formatBytesPerSec(p.netTxBytesPerSec) : QStringLiteral("—"));
        item->setText(4, formatBytesPerSec(p.netRxBytesPerSec + p.netTxBytesPerSec));
        item->setForeground(2, theme::ok());
        item->setForeground(3, theme::pink());
    }

    m_rankingTable->setUpdatesEnabled(true);
}

} // namespace ws
