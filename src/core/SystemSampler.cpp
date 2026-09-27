#include "core/SystemSampler.h"

#include "core/CpuSampler.h"
#include "core/DiskSampler.h"
#include "core/GpuSampler.h"
#include "core/MemorySampler.h"
#include "core/NetProcessMonitor.h"
#include "core/NetSampler.h"
#include "core/ProcessSampler.h"
#include "core/ThermalProvider.h"
#include "core/Win32Utils.h"

#include <QDateTime>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

namespace ws {

// 真正的采样逻辑,整个对象活在采样线程里
class SamplerWorker : public QObject
{
    Q_OBJECT

public:
    SamplerWorker() = default;

public slots:
    void start()
    {
        // 温度来源要最先准备好:GpuSampler 载入适配器时会顺手把型号名交给它。
        // 厂商 SDK 全部动态加载,一个都没有也不影响其它功能
        m_thermal.load();
        m_cpu.setThermalProvider(&m_thermal);
        m_gpu.setThermalProvider(&m_thermal);

        // 静态信息只取一次(型号、核心数、计数器句柄、分区表)
        m_cpu.loadStaticInfo();
        m_gpu.loadStaticInfo();
        m_disk.loadStaticInfo();
        m_proc.loadStaticInfo();

        // 每进程流量统计尽力而为,拿不到权限也不影响其它功能
        const bool netOk = m_netMonitor.start();
        emit networkMonitorStateChanged(netOk, netOk ? QString() : m_netMonitor.lastError());

        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::CoarseTimer);
        connect(m_timer, &QTimer::timeout, this, &SamplerWorker::tick);
        m_timer->start(m_intervalMs);

        // 先出一帧,界面不用等满一个周期
        tick();
    }

    void stop()
    {
        if (m_timer)
            m_timer->stop();
        m_netMonitor.stop();
    }

    void setInterval(int ms)
    {
        m_intervalMs = qMax(200, ms);
        if (m_timer)
            m_timer->start(m_intervalMs);
    }

    void tick()
    {
        // ---------- 系统快照:先发这个,它最便宜,界面能立刻有内容
        SystemSnapshot snapshot;
        snapshot.timestampMs = QDateTime::currentMSecsSinceEpoch();
        snapshot.cpu = m_cpu.sample();
        snapshot.memory = m_mem.sample();
        // 核显 + 独显的机器上 gpus 会有多条;gpu 取主显卡(独显优先),
        // 概览页那种只放得下一个仪表的地方用它
        snapshot.gpus = m_gpu.sampleAll();
        snapshot.gpu = m_gpu.primary();
        snapshot.disk = m_disk.sample();
        snapshot.net = m_net.sample();

        // ---------- 进程快照:把 GPU/网络数据注进去
        const auto traffic = m_netMonitor.takeSnapshot();
        ProcessExtras extras;
        extras.gpuUsage = &m_gpu.perProcessUsage();
        extras.gpuDedicated = &m_gpu.perProcessDedicated();
        extras.gpuShared = &m_gpu.perProcessShared();
        extras.netTraffic = &traffic;

        ProcessSnapshot procSnap = m_proc.sample(extras);

        // CPU 页要显示进程/线程/句柄总数,从这里回填
        snapshot.cpu.processCount = procSnap.totalProcesses;
        snapshot.cpu.threadCount = procSnap.totalThreads;
        snapshot.cpu.handleCount = procSnap.totalHandles;
        snapshot.valid = true;

        emit systemSnapshotReady(snapshot);
        emit processSnapshotReady(procSnap);

        // ---------- 低频项
        if (m_tick % 5 == 0) {
            const auto connections = m_conn.sample();
            emit connectionsReady(connections);
        }
        if (m_tick % 30 == 0) {
            m_disk.refreshVolumes();
        }

        ++m_tick;
    }

signals:
    void systemSnapshotReady(const ws::SystemSnapshot &snapshot);
    void processSnapshotReady(const ws::ProcessSnapshot &snapshot);
    void connectionsReady(const QVector<ws::NetConnection> &connections);
    void networkMonitorStateChanged(bool running, const QString &error);

private:
    CpuSampler m_cpu;
    MemorySampler m_mem;
    GpuSampler m_gpu;
    DiskSampler m_disk;
    NetSampler m_net;
    NetConnectionSampler m_conn;
    ProcessSampler m_proc;
    NetProcessMonitor m_netMonitor;
    ThermalProvider m_thermal;

    QTimer *m_timer = nullptr;
    int m_intervalMs = 1000;
    int m_tick = 0;
};

SystemSampler::SystemSampler(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<ws::SystemSnapshot>("ws::SystemSnapshot");
    qRegisterMetaType<ws::ProcessSnapshot>("ws::ProcessSnapshot");
    qRegisterMetaType<QVector<ws::NetConnection>>("QVector<ws::NetConnection>");

    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("WinScopeSampler"));

    m_worker = new SamplerWorker();
    m_worker->moveToThread(m_thread);

    connect(m_thread, &QThread::started, m_worker, &SamplerWorker::start);
    connect(m_worker, &SamplerWorker::systemSnapshotReady, this, &SystemSampler::systemSnapshotReady);
    connect(m_worker, &SamplerWorker::processSnapshotReady, this, &SystemSampler::processSnapshotReady);
    connect(m_worker, &SamplerWorker::connectionsReady, this, &SystemSampler::connectionsReady);
    connect(m_worker, &SamplerWorker::networkMonitorStateChanged, this,
            [this](bool running, const QString &error) {
                m_netMonitorRunning = running;
                m_netMonitorError = error;
                emit networkMonitorStateChanged(running, error);
            });
}

SystemSampler::~SystemSampler()
{
    stop();
}

void SystemSampler::start()
{
    if (!m_thread->isRunning())
        m_thread->start();
}

void SystemSampler::stop()
{
    if (!m_thread->isRunning())
        return;

    // 阻塞式调用,确保停止逻辑(含 ETW 会话回收)在采样线程里跑完再退出
    QMetaObject::invokeMethod(m_worker, "stop", Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait(5000);

    delete m_worker;
    m_worker = nullptr;
}

void SystemSampler::setInterval(int ms)
{
    m_intervalMs = ms;
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "setInterval", Qt::QueuedConnection, Q_ARG(int, ms));
}

void SystemSampler::refreshNow()
{
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "tick", Qt::QueuedConnection);
}

bool SystemSampler::isElevated()
{
    return isProcessElevated();
}

} // namespace ws

#include "SystemSampler.moc"
