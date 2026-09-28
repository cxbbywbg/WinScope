#pragma once

// 性能监控页:CPU / 内存 / GPU / 磁盘 四个页签。
//
// 明细部分统一是「图 + 文字」——纯数字看不出轻重缓急,所以
//   横条(BarRow)      用来表示"占了多少",一眼看出余量
//   构成条(SegmentBar) 用来表示"由什么组成",例如内存的已用/缓存/空闲
// 文字仍然保留,只是挪到横条右边,数值精度不丢。
//
// GPU 页按适配器分列:核显和独显各占一列,各自有曲线、引擎占用和明细。
// 只有一块显卡时就是单列铺满。

#include "ui/pages/PageBase.h"

#include <QStringList>
#include <QVector>

class QLabel;
class QTabWidget;

namespace ws {

class BarRow;
class Card;
class CoreMeter;
class LineChart;
class SegmentBar;
class StatRow;

class PerformancePage : public PageBase
{
    Q_OBJECT

public:
    explicit PerformancePage(QWidget *parent = nullptr);

    QString pageTitle() const override;
    QString pageSubtitle() const override;

    void onSystemSnapshot(const SystemSnapshot &snapshot) override;
    void onSamplingPaused(bool paused) override;

private:
    QWidget *buildCpuTab();
    QWidget *buildMemoryTab();
    QWidget *buildGpuTab();
    QWidget *buildDiskTab();

    void updateCpu(const CpuInfo &cpu);
    void updateMemory(const MemoryInfo &memory);
    void updateGpu(const QVector<GpuInfo> &gpus);
    void updateDisk(const DiskInfo &disk);

    void rebuildGpuColumns(int count);
    void updateGpuColumn(int index, const GpuInfo &gpu);

    QTabWidget *m_tabs = nullptr;
    bool m_paused = false;

    // ---- CPU
    LineChart *m_cpuChart = nullptr;
    LineChart *m_cpuFreqChart = nullptr;
    int m_seriesCpu = -1;
    int m_seriesFreq = -1;
    QVector<CoreMeter *> m_coreMeters;
    QWidget *m_coreContainer = nullptr;
    Card *m_cpuDetailCard = nullptr;
    StatRow *m_cpuModel = nullptr;
    StatRow *m_cpuCores = nullptr;
    StatRow *m_cpuCounts = nullptr;
    StatRow *m_cpuTimes = nullptr;
    BarRow *m_cpuUsageBar = nullptr;
    BarRow *m_cpuFreqBar = nullptr;
    BarRow *m_cpuTempBar = nullptr;
    BarRow *m_cpuPowerBar = nullptr;
    SegmentBar *m_cpuTimeBar = nullptr;
    // 温度来源会变(ACPI 热区 / 核显传感器),变了才改名称列的标题,免得每帧触发布局。
    // 说明文字不跟着它走 —— 它的初值恰好就是"读不到",见 .cpp 里的说明
    CpuTemperatureSource m_cpuTempSource = CpuTemperatureSource::None;
    QString m_cpuDetailHint;
    // 主频曲线的量程要等第一次采样拿到睿频上限才能定,定一次就够
    bool m_cpuFreqRangeSet = false;
    // 功耗/IOPS/响应时间都没有通用量程(拿不到 TDP、不知道盘是 HDD 还是 SSD),
    // 用运行期间观察到的峰值折算。横条反映的是"相对自己最猛的时候占几成",
    // 绝对值看右边的文字。
    double m_cpuPowerPeak = 0.0;

    // ---- 内存
    LineChart *m_memChart = nullptr;
    int m_seriesMem = -1;
    SegmentBar *m_memComposition = nullptr;
    BarRow *m_memUsageBar = nullptr;
    BarRow *m_memCommitBar = nullptr;
    BarRow *m_memPageFileBar = nullptr;
    QVector<StatRow *> m_memRows;

    // ---- GPU:一块适配器一列
    struct GpuColumn {
        QWidget *root = nullptr;
        Card *card = nullptr;
        LineChart *chart = nullptr;
        int seriesUsage = -1;
        int seriesVram = -1;
        BarRow *usageBar = nullptr;
        BarRow *vramBar = nullptr;
        BarRow *temperatureBar = nullptr;
        QLabel *engineLabel = nullptr;
        QWidget *engineBox = nullptr;
        QVector<BarRow *> engineBars;
        QStringList engineNames;
        StatRow *driverRow = nullptr;
        StatRow *dateRow = nullptr;
        StatRow *totalRow = nullptr;
        StatRow *sharedRow = nullptr;
        QString lastTitle;
        // 说明行的内容。温度不可用时要在说明里写清原因,这里记着上一次写过的内容,
        // 免得每帧都去改 QLabel 触发重排
        QString lastHint;
    };
    QVector<GpuColumn> m_gpuColumns;
    QWidget *m_gpuGrid = nullptr;
    QLabel *m_gpuEmpty = nullptr;

    // ---- 磁盘
    LineChart *m_diskChart = nullptr;
    int m_seriesRead = -1;
    int m_seriesWrite = -1;
    BarRow *m_diskActiveBar = nullptr;
    BarRow *m_diskReadBar = nullptr;
    BarRow *m_diskWriteBar = nullptr;
    BarRow *m_diskIopsBar = nullptr;
    BarRow *m_diskResponseBar = nullptr;
    BarRow *m_diskQueueBar = nullptr;
    QWidget *m_volumeContainer = nullptr;
    QVector<BarRow *> m_volumeRows;
    QStringList m_volumeKeys;
    double m_diskIopsPeak = 0.0;
    double m_diskResponsePeak = 0.0;
};

} // namespace ws
