#include "ui/pages/ToolsPage.h"

#include "app/Theme.h"
#include "core/Win32Utils.h"
#include "ui/widgets/Card.h"
#include "ui/widgets/FlowLayout.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace ws {

namespace {

// 工具箱里的按钮统一长相,省得每处重复设一遍
QPushButton *makeToolButton(const QString &text, const QString &tooltip, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setToolTip(tooltip);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumWidth(158);
    button->setMinimumHeight(34);
    return button;
}

// 「名称 → 目标」一条,用于铺按钮墙
struct ToolEntry {
    const char *label;
    const char *target;
    const char *tooltip;
};

} // namespace

ToolsPage::ToolsPage(QWidget *parent)
    : PageBase(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll, 1);

    auto *content = new QWidget(scroll);
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(20, 16, 20, 20);
    root->setSpacing(14);

    root->addWidget(buildWindowsToolsCard());
    root->addWidget(buildNetworkToolsCard());
    root->addWidget(buildSystemToolsCard());
    root->addStretch(1);

    scroll->setWidget(content);
}

ToolsPage::~ToolsPage()
{
    stopRunningCommand();
}

QString ToolsPage::pageTitle() const
{
    return QStringLiteral("工具箱");
}

QString ToolsPage::pageSubtitle() const
{
    return QStringLiteral("系统自带工具、网络诊断命令与常用系统操作,集中到一处");
}

// ---------------------------------------------------------------- Windows 工具

QWidget *ToolsPage::buildWindowsToolsCard()
{
    auto *card = new Card(QStringLiteral("Windows 工具"), this);
    card->setHint(QStringLiteral("点按钮直接用系统自带程序打开"));

    // 这些都是 shell 才能启动的目标(.msc / .cpl),CreateProcess 起不来
    static const ToolEntry entries[] = {
        { "设备管理器", "devmgmt.msc", "查看和管理硬件设备与驱动" },
        { "磁盘管理", "diskmgmt.msc", "分区、盘符、磁盘初始化" },
        { "服务", "services.msc", "Windows 服务管理器" },
        { "任务计划程序", "taskschd.msc", "查看和管理计划任务" },
        { "事件查看器", "eventvwr.msc", "系统与应用日志" },
        { "资源监视器", "resmon.exe", "实时查看 CPU / 磁盘 / 网络 / 内存占用" },
        { "系统信息", "msinfo32.exe", "系统摘要与硬件资源明细" },
        { "注册表编辑器", "regedit.exe", "打开注册表" },
        { "防火墙", "wf.msc", "高级安全 Windows Defender 防火墙" },
        { "网络连接", "ncpa.cpl", "网卡适配器列表与属性" },
        { "程序和功能", "appwiz.cpl", "卸载程序、启用 Windows 功能" },
        { "电源选项", "powercfg.cpl", "电源计划与休眠设置" },
    };

    auto *flow = new FlowLayout(nullptr, 0, 10, 10);
    for (const ToolEntry &entry : entries) {
        const QString label = QString::fromUtf8(entry.label);
        auto *button = makeToolButton(label, QString::fromUtf8(entry.tooltip), card);
        const QString target = QString::fromUtf8(entry.target);
        connect(button, &QPushButton::clicked, this, [this, target, label]() { openSystemTool(target, label); });
        flow->addWidget(button);
    }
    card->body()->addLayout(flow);
    return card;
}

// ---------------------------------------------------------------- 网络工具

QWidget *ToolsPage::buildNetworkToolsCard()
{
    auto *card = new Card(QStringLiteral("网络工具"), this);
    card->setHint(QStringLiteral("输出显示在本页,不用切窗口"));

    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(10);

    m_hostEdit = new QLineEdit(card);
    m_hostEdit->setPlaceholderText(QStringLiteral("输入域名或 IP,例如 www.baidu.com"));
    m_hostEdit->setClearButtonEnabled(true);
    m_hostEdit->setMinimumWidth(240);
    toolbar->addWidget(m_hostEdit, 1);

    struct NetEntry {
        const char *label;
        const char *tooltip;
    };
    // 每条命令在下面单独处理,这里只放显示名和提示
    static const NetEntry netEntries[] = {
        { "Ping", "发 4 个 ICMP 包测试连通性" },
        { "Tracert", "追踪到目标的路由跳数(不解析主机名)" },
        { "NSLookup", "查询 DNS 解析结果" },
        { "Netstat", "列出全部 TCP/UDP 连接与监听端口" },
        { "IPConfig", "查看完整 IP 配置" },
        { "刷新 DNS", "清空 DNS 解析缓存" },
    };

    for (const NetEntry &entry : netEntries) {
        const QString label = QString::fromUtf8(entry.label);
        auto *button = makeToolButton(label, QString::fromUtf8(entry.tooltip), card);
        button->setMinimumWidth(96);
        connect(button, &QPushButton::clicked, this, [this, label]() {
            const QString host = m_hostEdit->text().trimmed();

            if (label == QLatin1String("Ping")) {
                if (host.isEmpty()) {
                    appendNote(QStringLiteral("请先填一个域名或 IP 再点 Ping。"));
                    return;
                }
                runCommand(QStringLiteral("ping -n 4 %1").arg(host),
                           { QStringLiteral("ping.exe"), QStringLiteral("-n"), QStringLiteral("4"), host });
            } else if (label == QLatin1String("Tracert")) {
                if (host.isEmpty()) {
                    appendNote(QStringLiteral("请先填一个域名或 IP 再点 Tracert。"));
                    return;
                }
                runCommand(QStringLiteral("tracert -d -h 20 %1").arg(host),
                           { QStringLiteral("tracert.exe"), QStringLiteral("-d"), QStringLiteral("-h"),
                             QStringLiteral("20"), host });
            } else if (label == QLatin1String("NSLookup")) {
                if (host.isEmpty()) {
                    appendNote(QStringLiteral("请先填一个域名或 IP 再点 NSLookup。"));
                    return;
                }
                runCommand(QStringLiteral("nslookup %1").arg(host),
                           { QStringLiteral("nslookup.exe"), host });
            } else if (label == QLatin1String("Netstat")) {
                runCommand(QStringLiteral("netstat -ano"),
                           { QStringLiteral("netstat.exe"), QStringLiteral("-ano") });
            } else if (label == QLatin1String("IPConfig")) {
                runCommand(QStringLiteral("ipconfig /all"),
                           { QStringLiteral("ipconfig.exe"), QStringLiteral("/all") });
            } else if (label == QStringLiteral("刷新 DNS")) {
                runCommand(QStringLiteral("ipconfig /flushdns"),
                           { QStringLiteral("ipconfig.exe"), QStringLiteral("/flushdns") });
            }
        });
        toolbar->addWidget(button);
    }

    card->body()->addLayout(toolbar);

    // ---------------- 输出面板
    m_output = new QPlainTextEdit(card);
    m_output->setReadOnly(true);
    m_output->setFont(theme::monoFont(12));
    m_output->setMinimumHeight(230);
    m_output->setPlaceholderText(QStringLiteral("点上面的按钮执行命令,输出会显示在这里。"));
    m_output->setLineWrapMode(QPlainTextEdit::NoWrap);
    card->body()->addWidget(m_output);

    auto *footer = new QHBoxLayout();
    footer->setSpacing(10);
    m_runStatus = new QLabel(QStringLiteral("就绪"), card);
    m_runStatus->setObjectName(QStringLiteral("CardHint"));
    footer->addWidget(m_runStatus);
    footer->addStretch(1);

    auto *stopButton = new QPushButton(QStringLiteral("停止"), card);
    stopButton->setObjectName(QStringLiteral("Ghost"));
    connect(stopButton, &QPushButton::clicked, this, [this]() {
        if (m_process) {
            stopRunningCommand();
            m_runStatus->setText(QStringLiteral("已停止"));
        }
    });
    footer->addWidget(stopButton);

    auto *clearButton = new QPushButton(QStringLiteral("清空"), card);
    clearButton->setObjectName(QStringLiteral("Ghost"));
    connect(clearButton, &QPushButton::clicked, this, [this]() {
        m_header.clear();
        m_rawOutput.clear();
        m_output->clear();
        m_runStatus->setText(QStringLiteral("就绪"));
    });
    footer->addWidget(clearButton);

    auto *copyButton = new QPushButton(QStringLiteral("复制输出"), card);
    copyButton->setObjectName(QStringLiteral("Ghost"));
    connect(copyButton, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_output->toPlainText());
        m_runStatus->setText(QStringLiteral("输出已复制到剪贴板"));
    });
    footer->addWidget(copyButton);

    card->body()->addLayout(footer);
    return card;
}

// ---------------------------------------------------------------- 系统工具

QWidget *ToolsPage::buildSystemToolsCard()
{
    auto *card = new Card(QStringLiteral("系统工具"), this);
    card->setHint(QStringLiteral("带提权的操作会弹 UAC 确认"));

    auto *flow = new FlowLayout(nullptr, 0, 10, 10);

    auto *flushDns = makeToolButton(QStringLiteral("清理 DNS 缓存"),
                                    QStringLiteral("执行 ipconfig /flushdns"), card);
    connect(flushDns, &QPushButton::clicked, this, [this]() {
        runCommand(QStringLiteral("ipconfig /flushdns"),
                   { QStringLiteral("ipconfig.exe"), QStringLiteral("/flushdns") });
    });
    flow->addWidget(flushDns);

    auto *restartExplorer = makeToolButton(QStringLiteral("重启资源管理器"),
                                           QStringLiteral("结束 explorer.exe 后重新拉起,任务栏卡死时用"), card);
    connect(restartExplorer, &QPushButton::clicked, this, [this]() {
        // 结束后再等一秒拉起,否则新进程可能和正在退出的旧进程打架
        runCommand(QStringLiteral("taskkill /f /im explorer.exe & timeout /t 1 /nobreak & start explorer.exe"),
                   { QStringLiteral("cmd.exe"), QStringLiteral("/c"),
                     QStringLiteral("taskkill /f /im explorer.exe & timeout /t 1 /nobreak >nul & start explorer.exe") });
    });
    flow->addWidget(restartExplorer);

    auto *restartNetwork = makeToolButton(QStringLiteral("重启网络"),
                                          QStringLiteral("释放并续订 IP、刷新 DNS(需要管理员权限)"), card);
    connect(restartNetwork, &QPushButton::clicked, this, [this]() {
        const bool ok = runElevatedCommand(
            QStringLiteral("cmd.exe"),
            QStringLiteral("/k ipconfig /release & ipconfig /renew & ipconfig /flushdns"));
        appendNote(ok ? QStringLiteral("已在管理员窗口中执行:释放并续订 IP + 刷新 DNS。")
                      : QStringLiteral("提权被取消或失败,重启网络未执行。"));
    });
    flow->addWidget(restartNetwork);

    auto *openCmd = makeToolButton(QStringLiteral("打开 CMD"),
                                   QStringLiteral("新开一个命令提示符窗口"), card);
    connect(openCmd, &QPushButton::clicked, this, [this]() {
        openSystemTool(QStringLiteral("cmd.exe"), QStringLiteral("命令提示符"));
    });
    flow->addWidget(openCmd);

    auto *openPowerShell = makeToolButton(QStringLiteral("打开 PowerShell"),
                                          QStringLiteral("新开一个 PowerShell 窗口"), card);
    connect(openPowerShell, &QPushButton::clicked, this, [this]() {
        openSystemTool(QStringLiteral("powershell.exe"), QStringLiteral("PowerShell"));
    });
    flow->addWidget(openPowerShell);

    auto *openTerminal = makeToolButton(QStringLiteral("打开 Windows 终端"),
                                        QStringLiteral("优先用 Windows Terminal,没装则退回 CMD"), card);
    connect(openTerminal, &QPushButton::clicked, this, [this]() {
        const QString wt = QStandardPaths::findExecutable(QStringLiteral("wt.exe"));
        if (!wt.isEmpty())
            openSystemTool(wt, QStringLiteral("Windows 终端"));
        else
            openSystemTool(QStringLiteral("cmd.exe"), QStringLiteral("命令提示符(未装 Windows 终端)"));
    });
    flow->addWidget(openTerminal);

    card->body()->addLayout(flow);
    return card;
}

// ---------------------------------------------------------------- 行为

void ToolsPage::openSystemTool(const QString &target, const QString &displayName)
{
    if (!shellOpen(target)) {
        if (m_runStatus)
            m_runStatus->setText(QStringLiteral("打开 %1 失败").arg(displayName));
        appendNote(QStringLiteral("无法打开 %1(%2),可能这台机器上不存在该工具。").arg(displayName, target));
        return;
    }
    if (m_runStatus)
        m_runStatus->setText(QStringLiteral("已打开 %1").arg(displayName));
}

void ToolsPage::runCommand(const QString &displayName, const QStringList &programAndArgs)
{
    if (programAndArgs.isEmpty())
        return;

    stopRunningCommand();

    m_rawOutput.clear();
    m_header = QStringLiteral("> %1\n\n").arg(displayName);
    m_output->setPlainText(m_header);
    m_runStatus->setText(QStringLiteral("运行中:%1").arg(displayName));

    auto *process = new QProcess(this);
    m_process = process;
    // 合并 stderr,ping/tracert 的失败信息也走 stderr
    process->setProcessChannelMode(QProcess::MergedChannels);

    connect(process, &QProcess::readyRead, this, &ToolsPage::refreshProcessOutput);
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        refreshProcessOutput();
        if (m_process != process)
            return;   // 已经被下一次运行接管了
        if (status == QProcess::CrashExit)
            m_runStatus->setText(QStringLiteral("命令异常结束"));
        else
            m_runStatus->setText(code == 0 ? QStringLiteral("命令完成")
                                           : QStringLiteral("命令结束,退出码 %1").arg(code));
        m_process = nullptr;
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError) {
        if (m_process != process)
            return;
        appendNote(QStringLiteral("启动失败:%1").arg(process->errorString()));
        m_runStatus->setText(QStringLiteral("启动失败"));
    });

    process->start(programAndArgs.first(), programAndArgs.mid(1));
}

void ToolsPage::stopRunningCommand()
{
    if (!m_process)
        return;

    QProcess *process = m_process;
    m_process = nullptr;   // 先摘掉引用,免得 finished 回调又回来动它
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(1000);
    }
    process->deleteLater();
}

void ToolsPage::refreshProcessOutput()
{
    if (!m_process || !m_output)
        return;

    m_rawOutput.append(m_process->readAll());
    // 每次整体解码:命令输出是本地代码页(中文 Windows 上是 GBK),
    // 分块解码会把多字节汉字切成两半,整体解码没这个问题
    m_output->setPlainText(m_header + QString::fromLocal8Bit(m_rawOutput));

    QScrollBar *bar = m_output->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void ToolsPage::appendNote(const QString &text)
{
    if (!m_output)
        return;

    if (m_header.isEmpty())
        m_header = QStringLiteral("> 提示\n\n");
    m_rawOutput.append(text.toLocal8Bit());
    m_rawOutput.append("\n");
    m_output->setPlainText(m_header + QString::fromLocal8Bit(m_rawOutput));

    QScrollBar *bar = m_output->verticalScrollBar();
    bar->setValue(bar->maximum());
}

} // namespace ws
