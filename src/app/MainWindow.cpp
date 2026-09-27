#include "app/MainWindow.h"

#include "app/Icons.h"
#include "app/Theme.h"
#include "core/Win32Utils.h"
#include "ui/pages/DashboardPage.h"
#include "ui/pages/NetworkPage.h"
#include "ui/pages/PageBase.h"
#include "ui/pages/PerformancePage.h"
#include "ui/pages/ProcessPage.h"
#include "ui/pages/ServicePage.h"
#include "ui/pages/StartupPage.h"
#include "ui/pages/SystemPage.h"
#include "ui/pages/ToolsPage.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace ws {

namespace {

struct NavEntry {
    icons::Nav icon;
    const char *label;
};

const NavEntry kNavEntries[] = {
    { icons::Nav::Dashboard, "系统概览" },   { icons::Nav::Processes, "进程管理" },
    { icons::Nav::Performance, "性能监控" }, { icons::Nav::Network, "网络监控" },
    { icons::Nav::Startup, "启动项" },       { icons::Nav::Services, "服务" },
    { icons::Nav::System, "系统信息" },      { icons::Nav::Tools, "工具箱" },
};

constexpr int kNavCount = int(sizeof(kNavEntries) / sizeof(kNavEntries[0]));

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("WinScope — Windows 系统监控与管理工具"));
    resize(1420, 900);
    setMinimumSize(1080, 680);

    auto *central = new QWidget(this);
    auto *rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // ---------------- 左侧导航
    auto *sidebar = new QWidget(central);
    sidebar->setObjectName(QStringLiteral("Sidebar"));
    sidebar->setFixedWidth(214);
    {
        auto *layout = new QVBoxLayout(sidebar);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        auto *brand = new QWidget(sidebar);
        auto *brandLayout = new QVBoxLayout(brand);
        brandLayout->setContentsMargins(20, 20, 20, 16);
        brandLayout->setSpacing(2);
        auto *title = new QLabel(QStringLiteral("WinScope"), brand);
        title->setObjectName(QStringLiteral("BrandTitle"));
        auto *subtitle = new QLabel(QStringLiteral("Windows 系统监控与管理"), brand);
        subtitle->setObjectName(QStringLiteral("BrandSubtitle"));
        brandLayout->addWidget(title);
        brandLayout->addWidget(subtitle);
        layout->addWidget(brand);

        m_nav = new QListWidget(sidebar);
        m_nav->setObjectName(QStringLiteral("NavList"));
        m_nav->setFrameShape(QFrame::NoFrame);
        m_nav->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_nav->setSelectionMode(QAbstractItemView::SingleSelection);
        m_nav->setFocusPolicy(Qt::NoFocus);
        for (int i = 0; i < kNavCount; ++i) {
            auto *item = new QListWidgetItem(QString::fromUtf8(kNavEntries[i].label), m_nav);
            item->setIcon(icons::nav(kNavEntries[i].icon, theme::textDim()));
            item->setSizeHint(QSize(0, 40));
        }
        // 给导航列表拉伸因子:它的 sizeHint 只有 200 出头,不加这个会被压到
        // 只显示 4 项,剩下的项要滚动才看得到,而滚动条又是隐藏的 —— 等于点不到。
        layout->addWidget(m_nav, 1);

        m_footerLabel = new QLabel(sidebar);
        m_footerLabel->setObjectName(QStringLiteral("SidebarFooter"));
        m_footerLabel->setWordWrap(true);
        layout->addWidget(m_footerLabel);
    }

    // ---------------- 右侧:顶部信息栏 + 页面堆栈
    auto *rightSide = new QWidget(central);
    auto *rightLayout = new QVBoxLayout(rightSide);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    auto *header = new QWidget(rightSide);
    header->setObjectName(QStringLiteral("HeaderBar"));
    header->setFixedHeight(72);
    {
        auto *headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(24, 0, 20, 0);
        headerLayout->setSpacing(12);

        auto *titleBox = new QVBoxLayout();
        titleBox->setContentsMargins(0, 0, 0, 0);
        titleBox->setSpacing(1);
        m_pageTitle = new QLabel(header);
        m_pageTitle->setObjectName(QStringLiteral("PageTitle"));
        m_pageSubtitle = new QLabel(header);
        m_pageSubtitle->setObjectName(QStringLiteral("PageSubtitle"));
        titleBox->addWidget(m_pageTitle);
        titleBox->addWidget(m_pageSubtitle);
        headerLayout->addLayout(titleBox);
        headerLayout->addStretch(1);

        m_statusLabel = new QLabel(header);
        m_statusLabel->setObjectName(QStringLiteral("PageSubtitle"));
        m_statusLabel->setVisible(false);
        headerLayout->addWidget(m_statusLabel);

        auto *intervalLabel = new QLabel(QStringLiteral("刷新"), header);
        intervalLabel->setObjectName(QStringLiteral("PageSubtitle"));
        headerLayout->addWidget(intervalLabel);

        m_intervalBox = new QComboBox(header);
        m_intervalBox->addItem(QStringLiteral("0.5 秒"), 500);
        m_intervalBox->addItem(QStringLiteral("1 秒"), 1000);
        m_intervalBox->addItem(QStringLiteral("2 秒"), 2000);
        m_intervalBox->addItem(QStringLiteral("5 秒"), 5000);
        m_intervalBox->setCurrentIndex(1);
        m_intervalBox->setFixedWidth(88);
        headerLayout->addWidget(m_intervalBox);

        m_pauseButton = new QPushButton(QStringLiteral("暂停"), header);
        headerLayout->addWidget(m_pauseButton);

        m_elevateButton = new QPushButton(QStringLiteral("以管理员身份重启"), header);
        m_elevateButton->setObjectName(QStringLiteral("Primary"));
        m_elevateButton->setVisible(!SystemSampler::isElevated());
        headerLayout->addWidget(m_elevateButton);
    }

    m_stack = new QStackedWidget(rightSide);
    rightLayout->addWidget(header);
    rightLayout->addWidget(m_stack, 1);

    rootLayout->addWidget(sidebar);
    rootLayout->addWidget(rightSide, 1);
    setCentralWidget(central);

    buildPages();
    wireSampler();

    // 页面就绪后才能选中导航项(选中会触发 navigateTo)
    m_nav->setCurrentRow(0);
    navigateTo(0);
    refreshSidebarFooter();

    m_sampler->setInterval(1000);
    m_sampler->start();
}

MainWindow::~MainWindow()
{
    // 采样线程里挂着 ETW 会话,必须显式回收
    if (m_sampler)
        m_sampler->stop();
}

void MainWindow::buildPages()
{
    m_pages = {
        new DashboardPage(this),   new ProcessPage(this),     new PerformancePage(this),
        new NetworkPage(this),     new StartupPage(this),     new ServicePage(this),
        new SystemPage(this),      new ToolsPage(this),
    };

    for (PageBase *page : m_pages)
        m_stack->addWidget(page);

    connect(m_nav, &QListWidget::currentRowChanged, this, &MainWindow::navigateTo);
}

void MainWindow::wireSampler()
{
    m_sampler = new SystemSampler(this);

    connect(m_sampler, &SystemSampler::systemSnapshotReady, this, [this](const SystemSnapshot &snapshot) {
        if (m_paused)
            return;
        for (PageBase *page : m_pages)
            page->onSystemSnapshot(snapshot);
    });

    connect(m_sampler, &SystemSampler::processSnapshotReady, this, [this](const ProcessSnapshot &snapshot) {
        if (m_paused)
            return;
        for (PageBase *page : m_pages)
            page->onProcessSnapshot(snapshot);
    });

    connect(m_sampler, &SystemSampler::connectionsReady, this, [this](const QVector<NetConnection> &connections) {
        if (m_paused)
            return;
        for (PageBase *page : m_pages)
            page->onConnections(connections);
    });

    connect(m_sampler, &SystemSampler::networkMonitorStateChanged, this, [this](bool running, const QString &error) {
        // 采样线程初始化完成后才刷页脚,这时才知道 ETW 到底起没起来
        refreshSidebarFooter();
        if (!running && !error.isEmpty())
            setStatus(QStringLiteral("每进程网络流量统计不可用:%1").arg(error), false);
    });

    connect(m_intervalBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        applyInterval(m_intervalBox->itemData(index).toInt());
    });

    connect(m_pauseButton, &QPushButton::clicked, this, &MainWindow::togglePause);
    connect(m_elevateButton, &QPushButton::clicked, this, &MainWindow::promptElevation);
}

void MainWindow::navigateTo(int index)
{
    if (index < 0 || index >= m_pages.size())
        return;
    if (index == m_currentIndex)
        return;

    m_currentIndex = index;
    m_stack->setCurrentIndex(index);
    m_pageTitle->setText(m_pages[index]->pageTitle());
    m_pageSubtitle->setText(m_pages[index]->pageSubtitle());

    // 当前导航项换强调色图标
    for (int i = 0; i < m_nav->count() && i < kNavCount; ++i) {
        if (auto *item = m_nav->item(i))
            item->setIcon(icons::nav(kNavEntries[i].icon, i == index ? theme::accent() : theme::textDim()));
    }

    m_pages[index]->onActivated();
}

void MainWindow::applyInterval(int intervalMs)
{
    if (intervalMs <= 0)
        return;
    m_sampler->setInterval(intervalMs);
    setStatus(QStringLiteral("刷新间隔已设为 %1 毫秒").arg(intervalMs));
}

void MainWindow::togglePause()
{
    m_paused = !m_paused;
    m_pauseButton->setText(m_paused ? QStringLiteral("继续") : QStringLiteral("暂停"));
    for (PageBase *page : m_pages)
        page->onSamplingPaused(m_paused);
    setStatus(m_paused ? QStringLiteral("采样已暂停") : QStringLiteral("采样已恢复"));
}

void MainWindow::promptElevation()
{
    const auto answer = QMessageBox::question(
        this, QStringLiteral("以管理员身份重启"),
        QStringLiteral("WinScope 会关闭当前窗口并重新以管理员身份启动。\n\n"
                       "提权后可用:结束其它用户的进程、启停服务、修改启动项、\n"
                       "按进程统计网络流量。\n\n是否继续?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    if (relaunchElevated())
        QApplication::quit();
    else
        setStatus(QStringLiteral("提权启动失败,可能被拒绝或需要输入管理员密码"), true);
}

void MainWindow::setStatus(const QString &text, bool isError)
{
    if (!m_statusLabel)
        return;
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(isError ? theme::danger().name() : theme::textDim().name()));
    m_statusLabel->setVisible(!text.isEmpty());
}

void MainWindow::refreshSidebarFooter()
{
    if (!m_footerLabel || !m_sampler)
        return;

    const bool elevated = SystemSampler::isElevated();
    QStringList lines;
    lines << (elevated ? QStringLiteral("● 管理员模式") : QStringLiteral("○ 普通用户模式"));
    if (!elevated)
        lines << QStringLiteral("部分功能受限,可点右上角提权");
    lines << (m_sampler->networkMonitorRunning() ? QStringLiteral("● 每进程流量统计:已启用")
                                                 : QStringLiteral("○ 每进程流量统计:未启用"));
    lines << QStringLiteral("WinScope 1.0.0");

    m_footerLabel->setText(lines.join(QLatin1Char('\n')));
    m_footerLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(elevated ? theme::ok().name() : theme::textFaint().name()));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_sampler)
        m_sampler->stop();
    event->accept();
}

} // namespace ws
