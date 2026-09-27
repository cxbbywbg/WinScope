#pragma once

// 网络监控页。
//
// 三块内容:
//   1. 网卡列表(每张网卡的地址、链路状态、实时速率、累计流量)
//   2. 连接列表(TCP/UDP,带归属进程)
//   3. 按进程的流量排行(数据来自 ETW,未提权时为空)

#include "ui/pages/PageBase.h"

#include <QHash>

class QComboBox;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

namespace ws {

class Card;
class LineChart;
class StatRow;

class NetworkPage : public PageBase
{
    Q_OBJECT

public:
    explicit NetworkPage(QWidget *parent = nullptr);

    QString pageTitle() const override;
    QString pageSubtitle() const override;

    void onSystemSnapshot(const SystemSnapshot &snapshot) override;
    void onProcessSnapshot(const ProcessSnapshot &snapshot) override;
    void onConnections(const QVector<NetConnection> &connections) override;
    void onSamplingPaused(bool paused) override;

private:
    void buildLeft(QVBoxLayout *root);
    void buildRight(QVBoxLayout *root);

    void updateAdapters(const NetInfo &net);
    void updateConnections();
    void updateRanking(const ProcessSnapshot &snapshot);

    LineChart *m_chart = nullptr;
    int m_seriesRx = -1;
    int m_seriesTx = -1;
    StatRow *m_totalRow = nullptr;

    QTreeWidget *m_adapterTable = nullptr;
    QTreeWidget *m_connectionTable = nullptr;
    QTreeWidget *m_rankingTable = nullptr;
    QComboBox *m_protocolFilter = nullptr;
    QLineEdit *m_connectionSearch = nullptr;
    QLabel *m_connectionCount = nullptr;

    QVector<NetConnection> m_connections;
    QHash<QString, QTreeWidgetItem *> m_adapterRows;   // 网卡名 -> 行
    // 连接表自带的进程名要靠 OpenProcess 查,系统进程查不到会退化成「PID n」。
    // 进程快照里的名字是内核直接给的,覆盖面更广,用它补一下。
    QHash<quint32, QString> m_processNames;
    bool m_paused = false;
};

} // namespace ws
