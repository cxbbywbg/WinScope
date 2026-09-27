#include "core/NetProcessMonitor.h"

#include "core/Win32Utils.h"

#include <QDateTime>

#include <cstring>
#include <vector>

namespace ws {

namespace {

const wchar_t *kSessionName = L"WinScope-NetworkTrace";

// Microsoft-Windows-Kernel-Network
const GUID kProviderGuid = { 0x7dd42a49, 0x5329, 0x4832, { 0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88 } };

// 单次事件大小上限,超过这个值基本可以断定解析错了(或字段偏移变了)
constexpr quint32 kMaxReasonableBytes = 64u * 1024u * 1024u;

ULONG buildProperties(std::vector<BYTE> &buf)
{
    const ULONG nameBytes = ULONG((wcslen(kSessionName) + 1) * sizeof(wchar_t));
    const ULONG total = ULONG(sizeof(EVENT_TRACE_PROPERTIES)) + nameBytes;
    buf.assign(total, 0);

    auto *props = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(buf.data());
    props->Wnode.BufferSize = total;
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1;                 // 时间戳用 QPC
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    props->BufferSize = 64;                          // KB
    props->MinimumBuffers = 8;
    props->MaximumBuffers = 64;
    props->FlushTimer = 1;                           // 每秒刷一次,实时性够用

    memcpy(buf.data() + sizeof(EVENT_TRACE_PROPERTIES), kSessionName, nameBytes);
    return total;
}

QString traceErrorText(ULONG code)
{
    switch (code) {
    case ERROR_ACCESS_DENIED:
        return QStringLiteral("拒绝访问:统计每进程网络流量需要管理员权限");
    case ERROR_ALREADY_EXISTS:
        return QStringLiteral("同名跟踪会话已存在");
    case ERROR_BAD_LENGTH:
        return QStringLiteral("会话参数长度不正确");
    case ERROR_NO_SYSTEM_RESOURCES:
        return QStringLiteral("系统资源不足,无法创建跟踪会话");
    default:
        break;
    }
    return QStringLiteral("启动跟踪会话失败(错误码 %1)").arg(code);
}

} // namespace

NetProcessMonitor::NetProcessMonitor() = default;

NetProcessMonitor::~NetProcessMonitor()
{
    stop();
}

bool NetProcessMonitor::start()
{
    if (isRunning())
        return true;

    std::vector<BYTE> props;
    buildProperties(props);

    // 上一次异常退出可能留下同名会话,先清掉
    ULONG rc = StartTraceW(&m_session, kSessionName,
                           reinterpret_cast<EVENT_TRACE_PROPERTIES *>(props.data()));
    if (rc == ERROR_ALREADY_EXISTS) {
        ControlTraceW(0, kSessionName, reinterpret_cast<EVENT_TRACE_PROPERTIES *>(props.data()),
                      EVENT_TRACE_CONTROL_STOP);
        buildProperties(props);
        rc = StartTraceW(&m_session, kSessionName, reinterpret_cast<EVENT_TRACE_PROPERTIES *>(props.data()));
    }

    if (rc != ERROR_SUCCESS) {
        m_lastError = traceErrorText(rc);
        m_session = 0;
        return false;
    }

    // MatchAnyKeyword = 0 表示不过滤,IPv4/IPv6 的收发事件全要
    rc = EnableTraceEx2(m_session, &kProviderGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        m_lastError = traceErrorText(rc);
        ControlTraceW(m_session, kSessionName, reinterpret_cast<EVENT_TRACE_PROPERTIES *>(props.data()),
                      EVENT_TRACE_CONTROL_STOP);
        m_session = 0;
        return false;
    }

    EVENT_TRACE_LOGFILEW logFile{};
    logFile.LoggerName = const_cast<LPWSTR>(kSessionName);
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.Context = this;
    logFile.EventRecordCallback = &NetProcessMonitor::eventRecordCallback;

    m_trace = OpenTraceW(&logFile);
    if (m_trace == INVALID_PROCESSTRACE_HANDLE) {
        m_lastError = QStringLiteral("打开实时跟踪失败(错误码 %1)").arg(GetLastError());
        ControlTraceW(m_session, kSessionName, reinterpret_cast<EVENT_TRACE_PROPERTIES *>(props.data()),
                      EVENT_TRACE_CONTROL_STOP);
        m_session = 0;
        return false;
    }

    m_lastError.clear();
    m_lastSnapshotMs = QDateTime::currentMSecsSinceEpoch();

    // ProcessTrace 是阻塞的,丢到独立线程;停止时靠 CloseTrace 把它踢出来
    m_thread = std::thread([this]() {
        ProcessTrace(&m_trace, 1, nullptr, nullptr);
    });

    return true;
}

void NetProcessMonitor::stop()
{
    if (m_trace != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(m_trace);
        m_trace = INVALID_PROCESSTRACE_HANDLE;
    }
    if (m_thread.joinable())
        m_thread.join();

    if (m_session) {
        std::vector<BYTE> props;
        buildProperties(props);
        ControlTraceW(m_session, kSessionName, reinterpret_cast<EVENT_TRACE_PROPERTIES *>(props.data()),
                      EVENT_TRACE_CONTROL_STOP);
        m_session = 0;
    }
}

void WINAPI NetProcessMonitor::eventRecordCallback(EVENT_RECORD *eventRecord)
{
    if (!eventRecord || !eventRecord->UserContext)
        return;
    static_cast<NetProcessMonitor *>(eventRecord->UserContext)->handleEvent(eventRecord);
}

void NetProcessMonitor::handleEvent(const EVENT_RECORD *eventRecord)
{
    const USHORT id = eventRecord->EventHeader.EventDescriptor.Id;

    bool isReceive = false;
    switch (id) {
    case 10:   // TCP  IPv4 数据发送
    case 26:   // TCP  IPv6 数据发送
    case 42:   // UDP  IPv4 数据发送
    case 58:   // UDP  IPv6 数据发送
        isReceive = false;
        break;
    case 11:   // TCP  IPv4 数据接收
    case 27:   // TCP  IPv6 数据接收
    case 43:   // UDP  IPv4 数据接收
    case 59:   // UDP  IPv6 数据接收
        isReceive = true;
        break;
    default:
        // 连接建立/断开/重传等事件没有字节数,直接忽略
        return;
    }

    if (eventRecord->UserDataLength < sizeof(quint32) * 2 || !eventRecord->UserData)
        return;

    // 载荷是 { UInt32 PID; UInt32 size; ... },用 memcpy 避免非对齐访问
    quint32 pid = 0;
    quint32 size = 0;
    memcpy(&pid, eventRecord->UserData, sizeof(pid));
    memcpy(&size, static_cast<const BYTE *>(eventRecord->UserData) + sizeof(pid), sizeof(size));

    if (pid == 0 || size == 0 || size > kMaxReasonableBytes)
        return;

    QMutexLocker locker(&m_mutex);
    auto it = m_raw.find(pid);
    if (it == m_raw.end())
        it = m_raw.insert(pid, { 0, 0 });
    if (isReceive)
        it.value().first += size;
    else
        it.value().second += size;
}

QHash<quint32, NetTraffic> NetProcessMonitor::takeSnapshot()
{
    QHash<quint32, NetTraffic> out;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMutexLocker locker(&m_mutex);

    const double elapsed = m_lastSnapshotMs > 0 ? double(now - m_lastSnapshotMs) / 1000.0 : 0.0;
    m_lastSnapshotMs = now;

    for (auto it = m_raw.constBegin(); it != m_raw.constEnd(); ++it) {
        const quint32 pid = it.key();
        const quint64 rx = it.value().first;
        const quint64 tx = it.value().second;

        NetTraffic t;
        t.rxBytesTotal = rx;
        t.txBytesTotal = tx;

        const auto prev = m_prevTotals.constFind(pid);
        if (prev != m_prevTotals.constEnd() && elapsed > 0.0) {
            if (rx >= prev->first)
                t.rxBytesPerSec = double(rx - prev->first) / elapsed;
            if (tx >= prev->second)
                t.txBytesPerSec = double(tx - prev->second) / elapsed;
        }

        out.insert(pid, t);
    }

    m_prevTotals = m_raw;
    return out;
}

} // namespace ws
