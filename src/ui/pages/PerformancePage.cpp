#include "ui/pages/PerformancePage.h"

#include "app/Theme.h"
#include "core/Win32Utils.h"
#include "ui/widgets/BarRow.h"
#include "ui/widgets/Card.h"
#include "ui/widgets/FlowLayout.h"
#include "ui/widgets/LineChart.h"
#include "ui/widgets/SegmentBar.h"
#include "ui/widgets/StatRow.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>

namespace ws {

// 单个逻辑核心的竖直占用条
class CoreMeter : public QWidget
{
public:
    explicit CoreMeter(int index, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_index(index)
    {
        setFixedSize(44, 76);
    }

    void setUsage(double percent)
    {
        if (qFuzzyCompare(m_usage + 1.0, percent + 1.0))
            return;
        m_usage = percent;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF track(10, 2, width() - 20, height() - 22);
        p.setPen(Qt::NoPen);
        p.setBrush(theme::surfaceAlt());
        p.drawRoundedRect(track, 4, 4);

        const double ratio = qBound(0.0, m_usage, 100.0) / 100.0;
        if (ratio > 0.0) {
            const qreal filledHeight = qMax(3.0, track.height() * ratio);
            QRectF filled(track.left(), track.bottom() - filledHeight, track.width(), filledHeight);
            p.setBrush(theme::forLevel(levelFor(m_usage)));
            p.drawRoundedRect(filled, 4, 4);
        }

        p.setFont(theme::monoFont(10));
        p.setPen(theme::textDim());
        p.drawText(QRectF(0, track.bottom() + 2, width(), 16), Qt::AlignCenter,
                   QStringLiteral("%1").arg(m_index));
    }

private:
    int m_index = 0;
    double m_usage = 0.0;
};

namespace {

// 把内容包进一个可滚动的页面,窗口小的时候不至于挤爆
QWidget *wrapScrollable(QWidget *content)
{
    auto *scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}

// 明细里所有横条的左右列宽度统一,整列数字才能对齐。
// 名称列要放得下"温度(核显传感器)"这种带来源标注的标题;
// 数值列要放得下 "3046 / 2900 MHz" / "12.5 / 19.9 GB" 这类带单位的组合值 ——
// BarRow 装不下会省略成 "…",把单位吃掉,所以宁可给宽一点
constexpr int kKeyWidth = 136;
constexpr int kValueWidth = 128;
// GPU 列窄一些,单独一套
constexpr int kGpuKeyWidth = 88;
constexpr int kGpuValueWidth = 104;

BarRow *makeBar(QWidget *parent, const QString &key, int keyWidth, int valueWidth)
{
    auto *bar = new BarRow(key, parent);
    bar->setKeyWidth(keyWidth);
    bar->setValueWidth(valueWidth);
    return bar;
}

// 百分比文本
QString percentText(double value, int decimals = 1)
{
    return QStringLiteral("%1%").arg(value, 0, 'f', decimals);
}

// 按负载等级取色(绿→橙→红),给横条用
QColor loadColor(double percent)
{
    return theme::forLevel(levelFor(percent));
}

// 温度:没有统一量程,按 100°C 折算,90°C 以上标红
QColor temperatureColor(double celsius)
{
    if (celsius >= 90.0)
        return theme::danger();
    if (celsius >= 75.0)
        return theme::warn();
    return theme::ok();
}

QString joinCounts(int a, int b, int c)
{
    return QStringLiteral("%1 / %2 / %3").arg(formatCount(quint64(a)), formatCount(quint64(b)),
                                              formatCount(quint64(c)));
}

} // namespace

PerformancePage::PerformancePage(QWidget *parent)
    : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 18);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildCpuTab(), QStringLiteral("CPU"));
    m_tabs->addTab(buildMemoryTab(), QStringLiteral("内存"));
    m_tabs->addTab(buildGpuTab(), QStringLiteral("GPU"));
    m_tabs->addTab(buildDiskTab(), QStringLiteral("磁盘"));
    root->addWidget(m_tabs, 1);
}

QString PerformancePage::pageTitle() const
{
    return QStringLiteral("性能监控");
}

QString PerformancePage::pageSubtitle() const
{
    return QStringLiteral("CPU 每核心占用与频率、内存构成、每块 GPU 的引擎与显存、磁盘吞吐的详细指标");
}

// ---------------------------------------------------------------- CPU

QWidget *PerformancePage::buildCpuTab()
{
    auto *content = new QWidget();
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto *chartRow = new QHBoxLayout();
    chartRow->setSpacing(12);

    auto *usageCard = new Card(QStringLiteral("CPU 总占用率"), content);
    usageCard->setHint(QStringLiteral("最近 60 秒"));
    m_cpuChart = new LineChart(usageCard);
    m_cpuChart->setCapacity(60);
    m_cpuChart->setRange(0.0, 100.0);
    m_cpuChart->setFormatter([](double v) { return percentText(v, 0); });
    m_seriesCpu = m_cpuChart->addSeries(QStringLiteral("总占用"), theme::accent());
    usageCard->body()->addWidget(m_cpuChart);
    chartRow->addWidget(usageCard, 1);

    auto *freqCard = new Card(QStringLiteral("CPU 主频"), content);
    freqCard->setHint(QStringLiteral("各核心平均"));
    m_cpuFreqChart = new LineChart(freqCard);
    m_cpuFreqChart->setCapacity(60);
    m_cpuFreqChart->setFormatter([](double v) { return QStringLiteral("%1M").arg(v, 0, 'f', 0); });
    m_seriesFreq = m_cpuFreqChart->addSeries(QStringLiteral("MHz"), theme::cyan());
    freqCard->body()->addWidget(m_cpuFreqChart);
    chartRow->addWidget(freqCard, 1);

    root->addLayout(chartRow, 2);

    // ---- 每核心
    auto *coreCard = new Card(QStringLiteral("每核心占用"), content);
    m_coreContainer = new QWidget(coreCard);
    auto *flow = new FlowLayout(m_coreContainer, 0, 6, 8);
    m_coreContainer->setLayout(flow);
    coreCard->body()->addWidget(m_coreContainer);
    root->addWidget(coreCard, 0);

    // ---- 明细
    m_cpuDetailCard = new Card(QStringLiteral("CPU 明细"), content);
    m_cpuDetailHint = QStringLiteral("横条按各项满量程折算");
    m_cpuDetailCard->setHint(m_cpuDetailHint);
    auto *detailCard = m_cpuDetailCard;

    m_cpuModel = new StatRow(QStringLiteral("型号"), detailCard);
    m_cpuCores = new StatRow(QStringLiteral("物理 / 逻辑核心"), detailCard);
    m_cpuCounts = new StatRow(QStringLiteral("进程 / 线程 / 句柄"), detailCard);
    detailCard->body()->addWidget(m_cpuModel);
    detailCard->body()->addWidget(m_cpuCores);
    detailCard->body()->addWidget(m_cpuCounts);

    m_cpuUsageBar = makeBar(detailCard, QStringLiteral("总占用率"), kKeyWidth, kValueWidth);
    m_cpuUsageBar->setMarker(0.9, theme::danger());
    m_cpuFreqBar = makeBar(detailCard, QStringLiteral("当前主频"), kKeyWidth, kValueWidth);
    m_cpuTempBar = makeBar(detailCard, QStringLiteral("温度"), kKeyWidth, kValueWidth);
    m_cpuTempBar->setMarker(0.9, theme::danger());
    m_cpuPowerBar = makeBar(detailCard, QStringLiteral("功耗"), kKeyWidth, kValueWidth);
    for (auto *bar : { m_cpuUsageBar, m_cpuFreqBar, m_cpuTempBar, m_cpuPowerBar })
        detailCard->body()->addWidget(bar);

    auto *timeLabel = new QLabel(QStringLiteral("开机以来 CPU 时间构成"), detailCard);
    timeLabel->setObjectName(QStringLiteral("CardHint"));
    detailCard->body()->addWidget(timeLabel);

    m_cpuTimeBar = new SegmentBar(detailCard);
    m_cpuTimeBar->setBarHeight(12);
    m_cpuTimeBar->setFormatter([](double v) { return percentText(v, 1); });
    detailCard->body()->addWidget(m_cpuTimeBar);

    m_cpuTimes = new StatRow(QStringLiteral("累计 用户 / 内核 / 空闲"), detailCard);
    detailCard->body()->addWidget(m_cpuTimes);

    root->addWidget(detailCard, 0);
    root->addStretch(1);

    return wrapScrollable(content);
}

// ---------------------------------------------------------------- 内存

QWidget *PerformancePage::buildMemoryTab()
{
    auto *content = new QWidget();
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto *chartCard = new Card(QStringLiteral("内存占用率"), content);
    chartCard->setHint(QStringLiteral("最近 60 秒"));
    m_memChart = new LineChart(chartCard);
    m_memChart->setCapacity(60);
    m_memChart->setRange(0.0, 100.0);
    m_memChart->setFormatter([](double v) { return percentText(v, 0); });
    m_seriesMem = m_memChart->addSeries(QStringLiteral("已用"), theme::purple());
    chartCard->body()->addWidget(m_memChart);
    root->addWidget(chartCard, 2);

    // ---- 构成条:一眼看出物理内存是怎么分掉的
    auto *compositionCard = new Card(QStringLiteral("内存构成"), content);
    compositionCard->setHint(QStringLiteral("已使用 / 缓存 / 空闲"));
    m_memComposition = new SegmentBar(compositionCard);
    m_memComposition->setBarHeight(14);
    m_memComposition->setFormatter([](double v) { return formatBytes(quint64(qMax(0.0, v))); });
    compositionCard->body()->addWidget(m_memComposition);
    root->addWidget(compositionCard, 0);

    auto *detailCard = new Card(QStringLiteral("内存明细"), content);
    detailCard->setHint(QStringLiteral("横条按各项满量程折算"));

    m_memUsageBar = makeBar(detailCard, QStringLiteral("使用率"), kKeyWidth, kValueWidth);
    m_memUsageBar->setMarker(0.9, theme::danger());
    m_memCommitBar = makeBar(detailCard, QStringLiteral("提交大小"), kKeyWidth, kValueWidth);
    m_memPageFileBar = makeBar(detailCard, QStringLiteral("页面文件"), kKeyWidth, kValueWidth);
    for (auto *bar : { m_memUsageBar, m_memCommitBar, m_memPageFileBar })
        detailCard->body()->addWidget(bar);

    const QStringList keys = {
        QStringLiteral("物理内存总量"), QStringLiteral("已使用"),       QStringLiteral("可用"),
        QStringLiteral("缓存"),         QStringLiteral("提交上限"),     QStringLiteral("分页池"),
        QStringLiteral("非分页池"),     QStringLiteral("页面文件总量"),
    };
    for (const QString &key : keys) {
        auto *row = new StatRow(key, detailCard);
        m_memRows.push_back(row);
        detailCard->body()->addWidget(row);
    }
    root->addWidget(detailCard, 0);
    root->addStretch(1);

    return wrapScrollable(content);
}

// ---------------------------------------------------------------- GPU

QWidget *PerformancePage::buildGpuTab()
{
    auto *content = new QWidget();
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    m_gpuGrid = new QWidget(content);
    auto *grid = new QGridLayout(m_gpuGrid);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(12);
    root->addWidget(m_gpuGrid, 0);

    m_gpuEmpty = new QLabel(QStringLiteral("正在检测显卡适配器…"), content);
    m_gpuEmpty->setObjectName(QStringLiteral("CardHint"));
    m_gpuEmpty->setAlignment(Qt::AlignCenter);
    m_gpuEmpty->setMinimumHeight(140);
    root->addWidget(m_gpuEmpty, 0);

    root->addStretch(1);

    return wrapScrollable(content);
}

void PerformancePage::rebuildGpuColumns(int count)
{
    if (m_gpuColumns.size() == count)
        return;

    auto *grid = qobject_cast<QGridLayout *>(m_gpuGrid->layout());
    for (GpuColumn &col : m_gpuColumns) {
        if (grid)
            grid->removeWidget(col.root);
        // 只 removeWidget 的话控件还是 m_gpuGrid 的子对象,会停在旧位置继续显示,
        // 必须先藏起来再交给事件循环销毁
        col.root->hide();
        col.root->deleteLater();
    }
    m_gpuColumns.clear();

    // 一块显卡就铺满,两块以上排成两列(四块显卡就是两行)
    const int columns = count <= 1 ? 1 : 2;

    for (int i = 0; i < count; ++i) {
        GpuColumn col;
        col.root = new QWidget(m_gpuGrid);
        auto *colLayout = new QVBoxLayout(col.root);
        colLayout->setContentsMargins(0, 0, 0, 0);
        colLayout->setSpacing(12);

        col.card = new Card(QStringLiteral("GPU %1").arg(i), col.root);
        colLayout->addWidget(col.card, 1);

        col.chart = new LineChart(col.card);
        col.chart->setCapacity(60);
        col.chart->setRange(0.0, 100.0);
        col.chart->setFormatter([](double v) { return percentText(v, 0); });
        col.seriesUsage = col.chart->addSeries(QStringLiteral("占用率"), theme::cyan());
        col.seriesVram = col.chart->addSeries(QStringLiteral("显存占用率"), theme::pink());
        col.card->body()->addWidget(col.chart);

        col.usageBar = makeBar(col.card, QStringLiteral("占用率"), kGpuKeyWidth, kGpuValueWidth);
        col.usageBar->setMarker(0.9, theme::danger());
        col.vramBar = makeBar(col.card, QStringLiteral("显存占用"), kGpuKeyWidth, kGpuValueWidth);
        col.vramBar->setMarker(0.9, theme::danger());
        col.temperatureBar = makeBar(col.card, QStringLiteral("温度"), kGpuKeyWidth, kGpuValueWidth);
        col.temperatureBar->setMarker(0.9, theme::danger());
        col.card->body()->addWidget(col.usageBar);
        col.card->body()->addWidget(col.vramBar);
        col.card->body()->addWidget(col.temperatureBar);

        col.engineLabel = new QLabel(QStringLiteral("引擎占用"), col.card);
        col.engineLabel->setObjectName(QStringLiteral("CardHint"));
        col.card->body()->addWidget(col.engineLabel);

        col.engineBox = new QWidget(col.card);
        auto *engineLayout = new QVBoxLayout(col.engineBox);
        engineLayout->setContentsMargins(0, 0, 0, 0);
        engineLayout->setSpacing(6);
        col.card->body()->addWidget(col.engineBox);

        col.driverRow = new StatRow(QStringLiteral("驱动版本"), col.card);
        col.dateRow = new StatRow(QStringLiteral("驱动日期"), col.card);
        col.totalRow = new StatRow(QStringLiteral("显存总量"), col.card);
        col.sharedRow = new StatRow(QStringLiteral("共享内存"), col.card);
        for (auto *row : { col.driverRow, col.dateRow, col.totalRow, col.sharedRow })
            col.card->body()->addWidget(row);

        m_gpuColumns.push_back(col);
        if (grid)
            grid->addWidget(col.root, i / columns, i % columns);
    }

    m_gpuEmpty->setVisible(count == 0);
}

void PerformancePage::updateGpuColumn(int index, const GpuInfo &gpu)
{
    GpuColumn &col = m_gpuColumns[index];

    // 标题只在适配器变化时改,避免每帧触发布局重算
    const QString title = gpu.name.isEmpty()
        ? QStringLiteral("GPU %1").arg(gpu.index)
        : QStringLiteral("GPU %1 · %2").arg(gpu.index).arg(gpu.name);
    if (title != col.lastTitle) {
        col.lastTitle = title;
        col.card->setTitle(title);
    }

    // 说明行:核显/独显 + 温度读不到时的原因。温度不可用时数值栏留空,
    // 但必须在说明里写清为什么,不做"静默显示空数据"
    const QString hint = gpu.temperatureC < 0.0
        ? QStringLiteral("%1 · 最近 60 秒 · 该卡温度读不到(厂商接口未提供),温度行留空")
              .arg(gpu.integrated ? QStringLiteral("核显") : QStringLiteral("独显"))
        : QStringLiteral("%1 · 最近 60 秒")
              .arg(gpu.integrated ? QStringLiteral("核显") : QStringLiteral("独显"));
    if (hint != col.lastHint) {
        col.lastHint = hint;
        col.card->setHint(hint);
    }

    col.chart->append(col.seriesUsage, gpu.usagePercent);
    col.chart->append(col.seriesVram, gpu.memoryPercent);

    col.usageBar->setValue(gpu.usagePercent / 100.0, percentText(gpu.usagePercent), loadColor(gpu.usagePercent));
    col.vramBar->setValue(gpu.memoryPercent / 100.0, formatBytes(gpu.dedicatedBytes),
                          gpu.memoryPercent >= 90.0 ? theme::danger()
                                                    : (gpu.memoryPercent >= 75.0 ? theme::warn() : theme::pink()));

    // ---- 温度:走厂商 SDK(ADL/NVML/NVAPI),没有通用接口,读不到就留空
    if (gpu.temperatureC < 0.0) {
        col.temperatureBar->setValue(0.0, QString(), theme::textFaint());
    } else {
        col.temperatureBar->setValue(gpu.temperatureC / 100.0,
                                     QStringLiteral("%1 °C").arg(gpu.temperatureC, 0, 'f', 1),
                                     temperatureColor(gpu.temperatureC));
    }

    // ---- 引擎:不同程序用不同引擎,列表会变,按名字重建
    QStringList names;
    names.reserve(gpu.engines.size());
    for (const GpuEngine &engine : gpu.engines)
        names.push_back(engine.name);

    if (names != col.engineNames) {
        col.engineNames = names;
        auto *layout = qobject_cast<QVBoxLayout *>(col.engineBox->layout());
        for (BarRow *bar : col.engineBars) {
            if (layout)
                layout->removeWidget(bar);
            bar->hide();
            bar->deleteLater();
        }
        col.engineBars.clear();
        for (const QString &name : names) {
            auto *bar = makeBar(col.engineBox, name, kGpuKeyWidth, kGpuValueWidth);
            col.engineBars.push_back(bar);
            if (layout)
                layout->addWidget(bar);
        }
        col.engineLabel->setText(names.isEmpty() ? QStringLiteral("引擎占用(当前无活动)")
                                                 : QStringLiteral("引擎占用"));
    }

    for (int i = 0; i < col.engineBars.size() && i < gpu.engines.size(); ++i) {
        const double percent = gpu.engines[i].percent;
        col.engineBars[i]->setValue(percent / 100.0, percentText(percent),
                                    percent > 0.5 ? loadColor(percent) : theme::textFaint());
    }

    col.driverRow->setValue(gpu.driverVersion.isEmpty() ? QStringLiteral("—") : gpu.driverVersion);
    col.dateRow->setValue(gpu.driverDate.isEmpty() ? QStringLiteral("—") : gpu.driverDate);
    col.totalRow->setValue(gpu.totalVramBytes > 0 ? formatBytes(gpu.totalVramBytes) : QStringLiteral("—"));
    col.sharedRow->setValue(formatBytes(gpu.sharedBytes));
}

// ---------------------------------------------------------------- 磁盘

QWidget *PerformancePage::buildDiskTab()
{
    auto *content = new QWidget();
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto *chartCard = new Card(QStringLiteral("磁盘吞吐"), content);
    chartCard->setHint(QStringLiteral("最近 60 秒"));
    m_diskChart = new LineChart(chartCard);
    m_diskChart->setCapacity(60);
    m_diskChart->setFormatter([](double v) { return formatBytesPerSec(v); });
    m_seriesRead = m_diskChart->addSeries(QStringLiteral("读取"), theme::ok());
    m_seriesWrite = m_diskChart->addSeries(QStringLiteral("写入"), theme::warn());
    chartCard->body()->addWidget(m_diskChart);
    root->addWidget(chartCard, 2);

    auto *row = new QHBoxLayout();
    row->setSpacing(12);

    auto *detailCard = new Card(QStringLiteral("磁盘明细"), content);
    detailCard->setHint(QStringLiteral("横条按当前量程折算"));

    m_diskActiveBar = makeBar(detailCard, QStringLiteral("活动时间"), kKeyWidth, kValueWidth);
    m_diskActiveBar->setMarker(0.9, theme::danger());
    m_diskReadBar = makeBar(detailCard, QStringLiteral("读取速率"), kKeyWidth, kValueWidth);
    m_diskWriteBar = makeBar(detailCard, QStringLiteral("写入速率"), kKeyWidth, kValueWidth);
    m_diskIopsBar = makeBar(detailCard, QStringLiteral("IOPS"), kKeyWidth, kValueWidth);
    m_diskResponseBar = makeBar(detailCard, QStringLiteral("平均响应时间"), kKeyWidth, kValueWidth);
    m_diskQueueBar = makeBar(detailCard, QStringLiteral("队列长度"), kKeyWidth, kValueWidth);
    m_diskQueueBar->setMarker(0.5, theme::warn());
    for (auto *bar : { m_diskActiveBar, m_diskReadBar, m_diskWriteBar, m_diskIopsBar, m_diskResponseBar,
                       m_diskQueueBar })
        detailCard->body()->addWidget(bar);
    row->addWidget(detailCard, 1);

    auto *volumeCard = new Card(QStringLiteral("分区容量"), content);
    volumeCard->setHint(QStringLiteral("已用 / 总量"));
    m_volumeContainer = new QWidget(volumeCard);
    auto *volumeLayout = new QVBoxLayout(m_volumeContainer);
    volumeLayout->setContentsMargins(0, 0, 0, 0);
    volumeLayout->setSpacing(8);
    volumeCard->body()->addWidget(m_volumeContainer);
    row->addWidget(volumeCard, 1);

    root->addLayout(row, 0);
    root->addStretch(1);

    return wrapScrollable(content);
}

// ---------------------------------------------------------------- 数据更新

void PerformancePage::onSystemSnapshot(const SystemSnapshot &snapshot)
{
    if (m_paused || !snapshot.valid)
        return;

    updateCpu(snapshot.cpu);
    updateMemory(snapshot.memory);
    // 有 gpus 就按列表来;老路径(只有单块 gpu)时退回单元素列表
    updateGpu(snapshot.gpus.isEmpty() ? QVector<GpuInfo>{ snapshot.gpu } : snapshot.gpus);
    updateDisk(snapshot.disk);
}

void PerformancePage::onSamplingPaused(bool paused)
{
    m_paused = paused;
}

void PerformancePage::updateCpu(const CpuInfo &cpu)
{
    m_cpuChart->append(m_seriesCpu, cpu.usage);
    m_cpuFreqChart->append(m_seriesFreq, cpu.currentMHz);

    // 核心数变化时重建占用条
    if (m_coreMeters.size() != cpu.cores.size() && m_coreContainer) {
        auto *layout = qobject_cast<FlowLayout *>(m_coreContainer->layout());
        for (auto *meter : m_coreMeters) {
            if (layout)
                layout->removeWidget(meter);
            meter->deleteLater();
        }
        m_coreMeters.clear();
        for (int i = 0; i < cpu.cores.size(); ++i) {
            auto *meter = new CoreMeter(i, m_coreContainer);
            m_coreMeters.push_back(meter);
            if (layout)
                layout->addWidget(meter);
        }
    }

    for (int i = 0; i < m_coreMeters.size() && i < cpu.cores.size(); ++i)
        m_coreMeters[i]->setUsage(cpu.cores[i].usage);

    m_cpuModel->setValue(cpu.model.isEmpty() ? QStringLiteral("—") : cpu.model);
    m_cpuCores->setValue(QStringLiteral("%1 / %2").arg(cpu.physicalCores).arg(cpu.logicalCores));
    m_cpuCounts->setValue(joinCounts(cpu.processCount, cpu.threadCount, cpu.handleCount));

    // ---- 总占用率
    m_cpuUsageBar->setValue(cpu.usage / 100.0, percentText(cpu.usage), loadColor(cpu.usage));

    // ---- 主频。Windows 在不少 AMD 笔记本上只报基准频率(实测 4800H 的
    //      Win32_Processor.MaxClockSpeed 和注册表 ~MHz 都是 2900,真实睿频 4200),
    //      这种机器上横条会常年贴顶,所以超过标称上限时换成橙色提示"正在睿频",
    //      具体数字看右边。
    const double rated = qMax(qMax(cpu.maxClockMHz, cpu.maxMHz), cpu.baseMHz);
    if (!m_cpuFreqRangeSet && rated > 0.0) {
        m_cpuFreqRangeSet = true;
        // 固件把基准频率当最大频率报时,曲线得自己留出睿频空间,不然会顶出画布
        const bool ratedLooksLikeBase = cpu.maxClockMHz <= cpu.baseMHz * 1.05;
        m_cpuFreqChart->setRange(0.0, rated * (ratedLooksLikeBase ? 1.5 : 1.1));
    }

    const bool boosting = rated > 0.0 && cpu.currentMHz > rated * 1.02;
    m_cpuFreqBar->setValue(rated > 0.0 ? cpu.currentMHz / rated : 0.0,
                           QStringLiteral("%1 / %2 MHz").arg(cpu.currentMHz, 0, 'f', 0).arg(rated, 0, 'f', 0),
                           boosting ? theme::warn() : theme::cyan());

    // ---- 温度:没有"满量程"这回事,统一按 100°C 折算。
    //      三级回退:ACPI 热区 -> 核显传感器 -> 留空。来源要标在名称列里
    //      (数值列窄,写不下),否则核显传感器读到的温度会被当成 CPU 自己的传感器;
    //      彻底读不到时数值栏留空,原因写在卡片说明里,不做"静默显示空数据"
    if (cpu.temperatureSource != m_cpuTempSource) {
        m_cpuTempSource = cpu.temperatureSource;
        m_cpuTempBar->setKey(m_cpuTempSource == CpuTemperatureSource::IntegratedGpu
                                 ? QStringLiteral("温度(核显传感器)")
                                 : QStringLiteral("温度"));
    }

    // 说明文字单独算,不能跟着上面那个"来源变了"的分支走:m_cpuTempSource 的初值就是
    // None,如果一台机器从头到尾都读不到温度,来源永远不变,说明就会一直停在构造函数
    // 给的默认文案上 —— 恰好把"为什么留空"漏掉,而那正是要发给别人用的场景
    QString tempHint;
    switch (m_cpuTempSource) {
    case CpuTemperatureSource::AcpiThermalZone:
        tempHint = QStringLiteral("横条按各项满量程折算");
        break;
    case CpuTemperatureSource::IntegratedGpu:
        tempHint = QStringLiteral("横条按各项满量程折算 · 本机无 ACPI 热区,温度取自核显传感器");
        break;
    case CpuTemperatureSource::None:
        tempHint = QStringLiteral("横条按各项满量程折算 · 本机读不到 CPU 温度:"
                                  "固件没有实现 ACPI 热区,核显传感器也拿不到,温度行留空");
        break;
    }
    if (tempHint != m_cpuDetailHint) {
        m_cpuDetailHint = tempHint;
        m_cpuDetailCard->setHint(tempHint);
    }

    if (cpu.temperatureC < 0.0) {
        m_cpuTempBar->setValue(0.0, QString(), theme::textFaint());
    } else {
        m_cpuTempBar->setValue(cpu.temperatureC / 100.0, QStringLiteral("%1 °C").arg(cpu.temperatureC, 0, 'f', 1),
                               temperatureColor(cpu.temperatureC));
    }

    // ---- 功耗:同样没有满量程,按运行期峰值折算
    if (cpu.powerW < 0.0) {
        m_cpuPowerBar->setValue(0.0, QStringLiteral("不可用"), theme::textFaint());
    } else {
        m_cpuPowerPeak = qMax(m_cpuPowerPeak, cpu.powerW);
        const double scale = qMax(m_cpuPowerPeak * 1.1, 15.0);
        m_cpuPowerBar->setValue(cpu.powerW / scale, QStringLiteral("%1 W").arg(cpu.powerW, 0, 'f', 1),
                                theme::purple());
    }

    // ---- 累计时间构成
    const double total = cpu.userSeconds + cpu.kernelSeconds + cpu.idleSeconds;
    if (total > 0.0) {
        m_cpuTimeBar->setSegments({
            { QStringLiteral("用户"), 100.0 * cpu.userSeconds / total, theme::accent() },
            { QStringLiteral("内核"), 100.0 * cpu.kernelSeconds / total, theme::purple() },
            { QStringLiteral("空闲"), 100.0 * cpu.idleSeconds / total, theme::borderLight() },
        });
    }
    m_cpuTimes->setValue(QStringLiteral("%1 / %2 / %3")
                             .arg(formatDuration(qint64(cpu.userSeconds)),
                                  formatDuration(qint64(cpu.kernelSeconds)),
                                  formatDuration(qint64(cpu.idleSeconds))));
}

void PerformancePage::updateMemory(const MemoryInfo &memory)
{
    m_memChart->append(m_seriesMem, memory.usagePercent);

    const QColor usageColor = memory.usagePercent >= 90.0 ? theme::danger()
        : (memory.usagePercent >= 75.0 ? theme::warn() : theme::purple());

    // ---- 构成条:已使用 + 缓存 + 空闲 = 物理内存总量。
    // 缓存是可用内存的一部分,得先减掉,否则会重复计入。
    const quint64 cached = qMin(memory.cachedBytes, memory.availableBytes);
    const quint64 idleBytes = memory.availableBytes > cached ? memory.availableBytes - cached : 0;
    m_memComposition->setSegments({
        { QStringLiteral("已使用"), double(memory.usedBytes), usageColor },
        { QStringLiteral("缓存"), double(cached), theme::cyan() },
        { QStringLiteral("空闲"), double(idleBytes), theme::borderLight() },
    });

    // ---- 明细横条
    m_memUsageBar->setValue(memory.usagePercent / 100.0, percentText(memory.usagePercent), usageColor);
    m_memCommitBar->setValue(memory.commitLimitBytes ? double(memory.committedBytes) / double(memory.commitLimitBytes)
                                                     : 0.0,
                             formatBytes(memory.committedBytes), theme::accent());
    m_memPageFileBar->setValue(memory.pageFileTotalBytes ? double(memory.pageFileUsedBytes) / double(memory.pageFileTotalBytes)
                                                         : 0.0,
                               formatBytes(memory.pageFileUsedBytes), theme::warn());

    m_memRows[0]->setValue(formatBytes(memory.totalBytes));
    m_memRows[1]->setValue(formatBytes(memory.usedBytes));
    m_memRows[2]->setValue(formatBytes(memory.availableBytes));
    m_memRows[3]->setValue(formatBytes(memory.cachedBytes));
    m_memRows[4]->setValue(formatBytes(memory.commitLimitBytes));
    m_memRows[5]->setValue(formatBytes(memory.pagedPoolBytes));
    m_memRows[6]->setValue(formatBytes(memory.nonPagedPoolBytes));
    m_memRows[7]->setValue(formatBytes(memory.pageFileTotalBytes));
}

void PerformancePage::updateGpu(const QVector<GpuInfo> &gpus)
{
    rebuildGpuColumns(gpus.size());

    for (int i = 0; i < m_gpuColumns.size() && i < gpus.size(); ++i)
        updateGpuColumn(i, gpus.at(i));
}

void PerformancePage::updateDisk(const DiskInfo &disk)
{
    m_diskChart->append(m_seriesRead, disk.readBytesPerSec);
    m_diskChart->append(m_seriesWrite, disk.writeBytesPerSec);

    // 读写横条和上面的曲线共用同一量程,横条长度和曲线高度才对得上
    const double rateMax = qMax(1.0, m_diskChart->upperBound());

    m_diskActiveBar->setValue(disk.activePercent / 100.0, percentText(disk.activePercent),
                              loadColor(disk.activePercent));
    m_diskReadBar->setValue(disk.readBytesPerSec / rateMax, formatBytesPerSec(disk.readBytesPerSec), theme::ok());
    m_diskWriteBar->setValue(disk.writeBytesPerSec / rateMax, formatBytesPerSec(disk.writeBytesPerSec), theme::warn());

    m_diskIopsPeak = qMax(m_diskIopsPeak, disk.iops);
    m_diskIopsBar->setValue(disk.iops / qMax(m_diskIopsPeak * 1.1, 200.0), QStringLiteral("%1").arg(disk.iops, 0, 'f', 1),
                            theme::cyan());

    m_diskResponsePeak = qMax(m_diskResponsePeak, disk.responseMs);
    const double responseScale = qMax(m_diskResponsePeak * 1.1, 20.0);
    m_diskResponseBar->setValue(disk.responseMs / responseScale, QStringLiteral("%1 ms").arg(disk.responseMs, 0, 'f', 2),
                                disk.responseMs >= 50.0 ? theme::warn() : theme::text());
    m_diskResponseBar->setMarker(50.0 / responseScale, theme::danger());

    // 队列长度按 8 算满量程:任务管理器里常年个位数,超过 4 就该注意了
    m_diskQueueBar->setValue(disk.queueLength / 8.0, QStringLiteral("%1").arg(disk.queueLength, 0, 'f', 2),
                             disk.queueLength >= 4.0 ? theme::warn() : theme::text());

    // ---- 分区容量:每个分区一条容量条
    QStringList keys;
    keys.reserve(disk.volumes.size());
    for (const DiskVolume &volume : disk.volumes) {
        keys.push_back(volume.label.isEmpty() ? volume.letter
                                              : QStringLiteral("%1 %2").arg(volume.letter, volume.label));
    }

    if (keys != m_volumeKeys && m_volumeContainer) {
        m_volumeKeys = keys;
        auto *layout = qobject_cast<QVBoxLayout *>(m_volumeContainer->layout());
        for (BarRow *bar : m_volumeRows) {
            if (layout)
                layout->removeWidget(bar);
            bar->hide();
            bar->deleteLater();
        }
        m_volumeRows.clear();
        for (const QString &key : keys) {
            auto *bar = makeBar(m_volumeContainer, key, 96, 176);
            m_volumeRows.push_back(bar);
            if (layout)
                layout->addWidget(bar);
        }
    }

    for (int i = 0; i < m_volumeRows.size() && i < disk.volumes.size(); ++i) {
        const DiskVolume &volume = disk.volumes.at(i);
        const double used = volume.usedPercent();
        const quint64 usedBytes = volume.totalBytes > volume.freeBytes ? volume.totalBytes - volume.freeBytes : 0;
        const QColor color = used >= 90.0 ? theme::danger() : (used >= 75.0 ? theme::warn() : theme::ok());
        m_volumeRows[i]->setValue(used / 100.0,
                                  QStringLiteral("%1 / %2").arg(formatBytes(usedBytes), formatBytes(volume.totalBytes)),
                                  color);
    }
}

} // namespace ws
