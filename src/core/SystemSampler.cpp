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

    // 参数用裸掩码,省得为了跨线程再注册一个元类型
    void setScope(quint32 mask)
    {
        const SampleScope next(mask);
        // 从关到开的通道,下一拍只用来重建基线(见 tick)
        m_warmUp = next.mask() & ~m_scope.mask();
        m_scope = next;
    }

    void tick()
    {
        // 通道关着的时候不采样,但也不清空 —— 沿用上一帧的值,这样用户从小窗
        // 切回主界面时不会看到一片空白
        SystemSnapshot snapshot = m_state;
        snapshot.timestampMs = QDateTime::currentMSecsSinceEpoch();

        // 刚被打开的通道这一拍只用来重建基线,采出来的值丢掉:
        // CPU / 磁盘 / 网络的速率都是"两次采样的差值 ÷ 间隔",直接拿关掉期间
        // 攒了几十秒的差值来算,第一帧会变成"过去 N 秒的平均值",看着像数据不对。
        // 从来没采过的通道没这个问题(没有旧基线可攒),照常取新值,否则会先闪一下 0
        const quint32 rebuild = m_warmUp & m_sampled;
        m_warmUp = 0;

        if (m_scope.test(SampleScope::Cpu)) {
            const CpuInfo fresh = m_cpu.sample();
            m_sampled |= quint32(SampleScope::Cpu);
            if (!(rebuild & quint32(SampleScope::Cpu)))
                snapshot.cpu = fresh;
        }
        if (m_scope.test(SampleScope::Memory))
            snapshot.memory = m_mem.sample();
        if (m_scope.test(SampleScope::Gpu)) {
            // 核显 + 独显的机器上 gpus 会有多条;gpu 取主显卡(独显优先),
            // 概览页和小窗那种只放得下一个数的地方用它
            const QVector<GpuInfo> gpus = m_gpu.sampleAll();
            m_sampled |= quint32(SampleScope::Gpu);
            if (!(rebuild & quint32(SampleScope::Gpu))) {
                snapshot.gpus = gpus;
                snapshot.gpu = m_gpu.primary();
            }
        }
        if (m_scope.test(SampleScope::Disk)) {
            const DiskInfo fresh = m_disk.sample();
            m_sampled |= quint32(SampleScope::Disk);
            if (!(rebuild & quint32(SampleScope::Disk)))
                snapshot.disk = fresh;
        }
        if (m_scope.test(SampleScope::Network)) {
            const NetInfo fresh = m_net.sample();
            m_sampled |= quint32(SampleScope::Network);
            if (!(rebuild & quint32(SampleScope::Network)))
                snapshot.net = fresh;
        }

        // ---------- 进程快照:把 GPU/网络数据注进去
        ProcessSnapshot procSnap;
        bool haveProc = false;
        if (m_scope.test(SampleScope::Processes)) {
            const auto traffic = m_netMonitor.takeSnapshot();
            ProcessExtras extras;
            extras.gpuUsage = &m_gpu.perProcessUsage();
            extras.gpuDedicated = &m_gpu.perProcessDedicated();
            extras.gpuShared = &m_gpu.perProcessShared();
            extras.netTraffic = &traffic;

            procSnap = m_proc.sample(extras);

            // CPU 页要显示进程/线程/句柄总数,从这里回填
            snapshot.cpu.processCount = procSnap.totalProcesses;
            snapshot.cpu.threadCount = procSnap.totalThreads;
            snapshot.cpu.handleCount = procSnap.totalHandles;
            haveProc = true;
        }

        snapshot.valid = true;
        m_state = snapshot;
        // 系统快照先发:它最便宜,界面能立刻有内容;进程快照要枚举全系统,慢得多
        emit systemSnapshotReady(snapshot);
        if (haveProc)
            emit processSnapshotReady(procSnap);

        // ---------- 低频项
        if (m_tick % 5 == 0 && m_scope.test(SampleScope::Network)) {
            const auto connections = m_conn.sample();
            emit connectionsReady(connections);
        }
        if (m_tick % 30 == 0 && m_scope.test(SampleScope::Disk)) {
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

    SampleScope m_scope = sampleScopeAll();
    // 已经采过至少一次的通道。用来区分"关掉期间攒了旧基线"和"从来没采过"
    quint32 m_sampled = 0;
    // 刚被打开、下一拍要重建基线的通道
    quint32 m_warmUp = 0;
    // 上一帧的完整状态。关掉的通道就沿用它的对应字段
    SystemSnapshot m_state;
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

void SystemSampler::setScope(SampleScope scope)
{
    if (m_scope == scope)
        return;
    m_scope = scope;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "setScope", Qt::QueuedConnection,
                                  Q_ARG(quint32, scope.mask()));
    }
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
