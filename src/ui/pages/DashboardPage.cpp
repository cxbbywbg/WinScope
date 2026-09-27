#include "ui/pages/DashboardPage.h"

#include "app/Theme.h"
#include "core/Win32Utils.h"
#include "ui/widgets/Badge.h"
#include "ui/widgets/Card.h"
#include "ui/widgets/LineChart.h"
#include "ui/widgets/RingGauge.h"
#include "ui/widgets/StatRow.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

namespace ws {

namespace {

// 状态行:左边名称、中间胶囊、右边数值
QWidget *makeStatusRow(const QString &key, QLabel **labelOut, Badge **badgeOut, QLabel **valueOut, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *name = new QLabel(key, row);
    name->setObjectName(QStringLiteral("StatKey"));
    name->setMinimumWidth(56);
    layout->addWidget(name);

    auto *badge = new Badge(row);
    badge->setCompact(true);
    badge->setState(QStringLiteral("—"), theme::textFaint());
    layout->addWidget(badge);

    layout->addStretch(1);

    auto *value = new QLabel(QStringLiteral("—"), row);
    value->setObjectName(QStringLiteral("StatValue"));
    value->setFont(theme::monoFont(12));
    layout->addWidget(value);

    *labelOut = name;
    *badgeOut = badge;
    *valueOut = value;
    return row;
}

} // namespace

DashboardPage::DashboardPage(QWidget *parent)
    : PageBase(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto *content = new QWidget(scroll);
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(20, 18, 20, 20);
    root->setSpacing(14);

    // ------------------------------------------------ 第一行:四个仪表
    auto *gaugeRow = new QHBoxLayout();
    gaugeRow->setSpacing(14);

    m_cpu = buildMetricCard(QStringLiteral("CPU"), theme::accent(),
                            { QStringLiteral("当前频率"), QStringLiteral("核心"), QStringLiteral("运行时间") }, content,
                            gaugeRow);
    m_memory = buildMetricCard(QStringLiteral("内存"), theme::purple(),
                               { QStringLiteral("已用"), QStringLiteral("可用"), QStringLiteral("缓存") }, content,
                               gaugeRow);
    m_gpu = buildMetricCard(QStringLiteral("GPU"), theme::cyan(),
                            { QStringLiteral("显存"), QStringLiteral("共享内存"), QStringLiteral("适配器") }, content,
                            gaugeRow);
    m_disk = buildMetricCard(QStringLiteral("磁盘"), theme::warn(),
                             { QStringLiteral("读取"), QStringLiteral("写入"), QStringLiteral("响应时间") }, content,
                             gaugeRow);
    root->addLayout(gaugeRow);

    // ------------------------------------------------ 第二行:曲线
    auto *chartRow = new QHBoxLayout();
    chartRow->setSpacing(14);

    auto *resourceCard = new Card(QStringLiteral("实时曲线"), content);
    resourceCard->setHint(QStringLiteral("最近 60 秒"));
    m_resourceChart = new LineChart(resourceCard);
    m_resourceChart->setCapacity(60);
    m_resourceChart->setRange(0.0, 100.0);
    m_resourceChart->setFormatter([](double v) { return QStringLiteral("%1%").arg(v, 0, 'f', 0); });
    m_seriesCpu = m_resourceChart->addSeries(QStringLiteral("CPU"), theme::accent());
    m_seriesMemory = m_resourceChart->addSeries(QStringLiteral("内存"), theme::purple());
    m_seriesGpu = m_resourceChart->addSeries(QStringLiteral("GPU"), theme::cyan());
    m_seriesDisk = m_resourceChart->addSeries(QStringLiteral("磁盘"), theme::warn());
    resourceCard->body()->addWidget(m_resourceChart);
    chartRow->addWidget(resourceCard, 2);

    auto *netCard = new Card(QStringLiteral("网络流量"), content);
    netCard->setHint(QStringLiteral("最近 60 秒"));
    m_networkChart = new LineChart(netCard);
    m_networkChart->setCapacity(60);
    m_networkChart->setFormatter([](double v) { return formatBytesPerSec(v); });
    m_seriesRx = m_networkChart->addSeries(QStringLiteral("下行"), theme::ok());
    m_seriesTx = m_networkChart->addSeries(QStringLiteral("上行"), theme::pink());
    netCard->body()->addWidget(m_networkChart);
    chartRow->addWidget(netCard, 1);

    root->addLayout(chartRow, 1);

    // ------------------------------------------------ 第三行:状态 + 分区
    auto *bottomRow = new QHBoxLayout();
    bottomRow->setSpacing(14);

    auto *statusCard = new Card(QStringLiteral("系统状态"), content);
    {
        auto *body = statusCard->body();
        body->setSpacing(9);
        body->addWidget(makeStatusRow(QStringLiteral("CPU"), &m_statusCpu.label, &m_statusCpu.badge,
                                      &m_statusCpu.value, statusCard));
        body->addWidget(makeStatusRow(QStringLiteral("内存"), &m_statusMemory.label, &m_statusMemory.badge,
                                      &m_statusMemory.value, statusCard));
        body->addWidget(makeStatusRow(QStringLiteral("GPU"), &m_statusGpu.label, &m_statusGpu.badge,
                                      &m_statusGpu.value, statusCard));
        body->addWidget(makeStatusRow(QStringLiteral("磁盘"), &m_statusDisk.label, &m_statusDisk.badge,
                                      &m_statusDisk.value, statusCard));
        body->addWidget(makeStatusRow(QStringLiteral("网络"), &m_statusNetwork.label, &m_statusNetwork.badge,
                                      &m_statusNetwork.value, statusCard));

        body->addSpacing(4);
        m_uptimeRow = new StatRow(QStringLiteral("系统运行时间"), statusCard);
        m_processRow = new StatRow(QStringLiteral("进程数"), statusCard);
        m_threadRow = new StatRow(QStringLiteral("线程 / 句柄"), statusCard);
        body->addWidget(m_uptimeRow);
        body->addWidget(m_processRow);
        body->addWidget(m_threadRow);
        body->addStretch(1);
    }
    bottomRow->addWidget(statusCard, 1);

    auto *volumeCard = new Card(QStringLiteral("磁盘分区"), content);
    m_volumeLayout = volumeCard->body();
    m_volumeLayout->setSpacing(9);
    bottomRow->addWidget(volumeCard, 1);

    root->addLayout(bottomRow);
    root->addStretch(0);

    scroll->setWidget(content);
}

QString DashboardPage::pageTitle() const
{
    return QStringLiteral("系统概览");
}

QString DashboardPage::pageSubtitle() const
{
    return QStringLiteral("实时掌握 CPU、内存、GPU、磁盘与网络的整体状况");
}

DashboardPage::MetricCard *DashboardPage::buildMetricCard(const QString &caption, const QColor &accent,
                                                          const QStringList &rowKeys, QWidget *parent,
                                                          QHBoxLayout *rowLayout)
{
    auto *card = new Card(caption, parent);
    auto *metric = new MetricCard();

    auto *gauge = new RingGauge(card);
    gauge->setCaption(caption);
    gauge->setAccent(accent);
    gauge->setAutoColor(true);
    gauge->setMinimumHeight(118);
    metric->gauge = gauge;
    card->body()->addWidget(gauge, 1, Qt::AlignHCenter);

    for (const QString &key : rowKeys) {
        auto *row = new StatRow(key, card);
        metric->rows.push_back(row);
        card->body()->addWidget(row);
    }

    rowLayout->addWidget(card, 1);
    return metric;
}

void DashboardPage::onSystemSnapshot(const SystemSnapshot &snapshot)
{
    if (!snapshot.valid)
        return;

    const CpuInfo &cpu = snapshot.cpu;
    const MemoryInfo &mem = snapshot.memory;
    const GpuInfo &gpu = snapshot.gpu;
    const DiskInfo &disk = snapshot.disk;
    const NetInfo &net = snapshot.net;

    // ---------------- 仪表
    m_cpu->gauge->setValue(cpu.usage);
    m_cpu->rows[0]->setValue(cpu.currentMHz > 0 ? QStringLiteral("%1 MHz").arg(cpu.currentMHz, 0, 'f', 0)
                                                : QStringLiteral("—"));
    m_cpu->rows[1]->setValue(QStringLiteral("%1 核 / %2 线程").arg(cpu.physicalCores).arg(cpu.logicalCores));
    m_cpu->rows[2]->setValue(formatDuration(qint64(cpu.uptimeSeconds)));

    m_memory->gauge->setValue(mem.usagePercent);
    m_memory->rows[0]->setValue(QStringLiteral("%1 / %2").arg(formatBytes(mem.usedBytes), formatBytes(mem.totalBytes)));
    m_memory->rows[1]->setValue(formatBytes(mem.availableBytes));
    m_memory->rows[2]->setValue(formatBytes(mem.cachedBytes));

    m_gpu->gauge->setValue(gpu.present ? gpu.usagePercent : -1.0);
    m_gpu->rows[0]->setValue(gpu.present ? QStringLiteral("%1 / %2")
                                               .arg(formatBytes(gpu.dedicatedBytes), formatBytes(gpu.totalVramBytes))
                                         : QStringLiteral("—"));
    m_gpu->rows[1]->setValue(gpu.present ? formatBytes(gpu.sharedBytes) : QStringLiteral("—"));
    m_gpu->rows[2]->setValue(gpu.name.isEmpty() ? QStringLiteral("未检测到") : gpu.name);

    m_disk->gauge->setValue(disk.activePercent);
    m_disk->rows[0]->setValue(formatBytesPerSec(disk.readBytesPerSec));
    m_disk->rows[1]->setValue(formatBytesPerSec(disk.writeBytesPerSec));
    m_disk->rows[2]->setValue(QStringLiteral("%1 ms").arg(disk.responseMs, 0, 'f', 2));

    // ---------------- 曲线
    m_resourceChart->append(m_seriesCpu, cpu.usage);
    m_resourceChart->append(m_seriesMemory, mem.usagePercent);
    m_resourceChart->append(m_seriesGpu, gpu.present ? gpu.usagePercent : 0.0);
    m_resourceChart->append(m_seriesDisk, disk.activePercent);

    m_networkChart->append(m_seriesRx, net.rxBytesPerSec);
    m_networkChart->append(m_seriesTx, net.txBytesPerSec);

    // ---------------- 状态一览
    auto updateStatus = [](StatusRow &row, double percent, const QString &valueText) {
        const LoadLevel level = levelFor(percent);
        row.badge->setState(theme::levelText(level), theme::forLevel(level));
        row.value->setText(valueText);
        row.value->setStyleSheet(QStringLiteral("color: %1;").arg(theme::forLevel(level).name()));
    };

    updateStatus(m_statusCpu, cpu.usage, QStringLiteral("%1%").arg(cpu.usage, 0, 'f', 0));
    updateStatus(m_statusMemory, mem.usagePercent, QStringLiteral("%1%").arg(mem.usagePercent, 0, 'f', 0));

    if (gpu.present)
        updateStatus(m_statusGpu, gpu.usagePercent, QStringLiteral("%1%").arg(gpu.usagePercent, 0, 'f', 0));
    else
        m_statusGpu.badge->setState(QStringLiteral("不可用"), theme::textFaint());

    updateStatus(m_statusDisk, disk.activePercent, QStringLiteral("%1%").arg(disk.activePercent, 0, 'f', 0));

    // 网络没有"占用率"概念,用速率本身当文本,状态按合计速率粗分档
    {
        const double totalRate = net.rxBytesPerSec + net.txBytesPerSec;
        const double percent = qMin(100.0, totalRate / (12.5 * 1024 * 1024) * 100.0);   // 100Mbps 视作满
        const LoadLevel level = levelFor(percent);
        m_statusNetwork.badge->setState(theme::levelText(level), theme::forLevel(level));
        m_statusNetwork.value->setText(QStringLiteral("↓%1 ↑%2")
                                           .arg(formatBytesPerSec(net.rxBytesPerSec),
                                                formatBytesPerSec(net.txBytesPerSec)));
        m_statusNetwork.value->setStyleSheet(QStringLiteral("color: %1;").arg(theme::text().name()));
    }

    m_uptimeRow->setValue(formatDuration(qint64(cpu.uptimeSeconds)));
    m_processRow->setValue(QStringLiteral("%1 个").arg(cpu.processCount));
    m_threadRow->setValue(QStringLiteral("%1 / %2").arg(formatCount(cpu.threadCount), formatCount(cpu.handleCount)));

    // ---------------- 分区容量
    const auto &volumes = disk.volumes;
    if (m_volumeRows.size() != volumes.size()) {
        qDeleteAll(m_volumeRows);
        m_volumeRows.clear();
        for (const auto &v : volumes) {
            auto *row = new StatRow(QStringLiteral("%1 %2").arg(v.letter, v.label), this);
            m_volumeLayout->addWidget(row);
            m_volumeRows.push_back(row);
        }
        m_volumeLayout->addStretch(1);
    }

    for (int i = 0; i < volumes.size(); ++i) {
        const auto &v = volumes[i];
        const double used = v.usedPercent();
        const QColor color = used >= 90.0 ? theme::danger() : (used >= 75.0 ? theme::warn() : theme::text());
        m_volumeRows[i]->setValue(QStringLiteral("%1 / %2 (%3%)")
                                      .arg(formatBytes(v.totalBytes - v.freeBytes), formatBytes(v.totalBytes))
                                      .arg(used, 0, 'f', 0),
                                  color);
    }
}

void DashboardPage::onProcessSnapshot(const ProcessSnapshot &snapshot)
{
    // 进程数/线程数由系统快照里的汇总字段提供,这里只兜个底
    if (m_processRow && snapshot.totalProcesses > 0)
        m_processRow->setValue(QStringLiteral("%1 个").arg(snapshot.totalProcesses));
}

} // namespace ws
