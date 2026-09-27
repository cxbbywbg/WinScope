#pragma once

// 系统概览页。
//
// 布局自上而下三行:
//   1. 四个环形仪表(CPU / 内存 / GPU / 磁盘),各带三条明细
//   2. 实时曲线(四种资源) + 网络流量曲线
//   3. 系统状态一览 + 分区容量

#include "ui/pages/PageBase.h"

#include <QColor>
#include <QStringList>

class QHBoxLayout;
class QLabel;
class QVBoxLayout;

namespace ws {

class Badge;
class Card;
class LineChart;
class RingGauge;
class StatRow;

class DashboardPage : public PageBase
{
    Q_OBJECT

public:
    explicit DashboardPage(QWidget *parent = nullptr);

    QString pageTitle() const override;
    QString pageSubtitle() const override;

    void onSystemSnapshot(const SystemSnapshot &snapshot) override;
    void onProcessSnapshot(const ProcessSnapshot &snapshot) override;

private:
    struct MetricCard {
        RingGauge *gauge = nullptr;
        QVector<StatRow *> rows;
    };

    MetricCard *buildMetricCard(const QString &caption, const QColor &accent, const QStringList &rowKeys,
                                QWidget *parent, QHBoxLayout *rowLayout);

    MetricCard *m_cpu = nullptr;
    MetricCard *m_memory = nullptr;
    MetricCard *m_gpu = nullptr;
    MetricCard *m_disk = nullptr;

    LineChart *m_resourceChart = nullptr;
    LineChart *m_networkChart = nullptr;
    int m_seriesCpu = -1;
    int m_seriesMemory = -1;
    int m_seriesGpu = -1;
    int m_seriesDisk = -1;
    int m_seriesRx = -1;
    int m_seriesTx = -1;

    // 系统状态行
    struct StatusRow {
        QLabel *label = nullptr;
        Badge *badge = nullptr;
        QLabel *value = nullptr;
    };
    StatusRow m_statusCpu;
    StatusRow m_statusMemory;
    StatusRow m_statusGpu;
    StatusRow m_statusDisk;
    StatusRow m_statusNetwork;
    StatRow *m_uptimeRow = nullptr;
    StatRow *m_processRow = nullptr;
    StatRow *m_threadRow = nullptr;

    QVBoxLayout *m_volumeLayout = nullptr;
    QVector<StatRow *> m_volumeRows;
};

} // namespace ws
