#include "app/MainWindow.h"

#include "app/Icons.h"
#include "app/Theme.h"
#include "core/Metrics.h"
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
#include "ui/widgets/MiniWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTimer>
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

// 托盘图标的「可见性」状态
enum class TrayPromotion {
    NotFound,        // 外壳还没给这个 exe 建登记条目(刚 show 出来时会这样)
    AlreadyVisible,  // 已经登记且本来就可见,什么都不用做
    Promoted,        // 这次把它改成可见了 —— 调用方要重加一次图标才会生效
};

// 把「自己」那个托盘图标设成默认可见。
//
// Windows 11 会把新注册的托盘图标丢进「隐藏的图标」浮窗里,要用户手动拖出来一次。
// 这个开关存在外壳管的 HKCU\Control Panel\NotifyIconSettings\<id>\IsPromoted 上,
// 没有公开 API —— 系统设置里的「其他系统托盘图标」开关改的就是它。
//
// 因为项目要发给别人用,总不能要求每个使用者都去手动拖一次,所以这里代劳。
// 两条约束:
//   1. 只改 ExecutablePath 和当前 exe 完全一致的那一条,别的程序一律不碰;
//   2. 只做一次(调用方用 QSettings 记着),之后用户再在系统设置里关掉,我们不再管。
TrayPromotion promoteOwnNotifyIcon()
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return TrayPromotion::NotFound;
    const QString self = QString::fromWCharArray(exePath);

    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root)
        != ERROR_SUCCESS)
        return TrayPromotion::NotFound;

    TrayPromotion result = TrayPromotion::NotFound;
    for (DWORD index = 0;; ++index) {
        wchar_t subName[256] = {};
        DWORD subLen = DWORD(sizeof(subName) / sizeof(subName[0]) - 1);
        if (RegEnumKeyExW(root, index, subName, &subLen, nullptr, nullptr, nullptr, nullptr)
            != ERROR_SUCCESS)
            break;

        HKEY sub = nullptr;
        if (RegOpenKeyExW(root, subName, 0, KEY_READ, &sub) != ERROR_SUCCESS)
            continue;

        wchar_t stored[MAX_PATH * 2] = {};
        DWORD storedLen = DWORD(sizeof(stored) - sizeof(wchar_t));
        DWORD type = 0;
        const LONG got = RegQueryValueExW(sub, L"ExecutablePath", nullptr, &type,
                                          reinterpret_cast<LPBYTE>(stored), &storedLen);
        RegCloseKey(sub);
        if (got != ERROR_SUCCESS || type != REG_SZ)
            continue;
        if (QString::fromWCharArray(stored).compare(self, Qt::CaseInsensitive) != 0)
            continue;

        HKEY writable = nullptr;
        if (RegOpenKeyExW(root, subName, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &writable)
            != ERROR_SUCCESS)
            break;

        DWORD promoted = 0;
        DWORD size = sizeof(promoted);
        DWORD valueType = 0;
        const bool exists = RegQueryValueExW(writable, L"IsPromoted", nullptr, &valueType,
                                             reinterpret_cast<LPBYTE>(&promoted), &size)
                            == ERROR_SUCCESS;
        if (exists && promoted != 0) {
            result = TrayPromotion::AlreadyVisible;
        } else {
            const DWORD one = 1;
            const bool ok = RegSetValueExW(writable, L"IsPromoted", 0, REG_DWORD,
                                           reinterpret_cast<const BYTE *>(&one), sizeof(one))
                            == ERROR_SUCCESS;
            result = ok ? TrayPromotion::Promoted : TrayPromotion::AlreadyVisible;
        }
        RegCloseKey(writable);
        break;
    }

    RegCloseKey(root);
    return result;
}

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

        m_miniButton = new QPushButton(QStringLiteral("小窗模式"), header);
        m_miniButton->setToolTip(QStringLiteral("只留一个置顶小窗显示选中的参数,\n"
                                                "同时停掉其余通道的采集以降后台开销"));
        headerLayout->addWidget(m_miniButton);

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
    buildTray();

    // 页面就绪后才能选中导航项(选中会触发 navigateTo)
    m_nav->setCurrentRow(0);
    navigateTo(0);
    refreshSidebarFooter();

    m_sampler->setInterval(1000);
    m_sampler->start();

    // 上次退出时停在小窗模式的话这次直接进小窗 —— 小窗本来就是"常驻桌面"的用法,
    // 每次都还要手点一下反而别扭。
    // 用 0 延时而不能在构造函数里直接调:main() 拿到窗口后还会 show() 一次,
    // 那一下会把这里的 hide() 抵消掉
    if (MiniWindow::miniModeWasActive())
        QTimer::singleShot(0, this, &MainWindow::enterMiniMode);
}

MainWindow::~MainWindow()
{
    // 采样线程里挂着 ETW 会话,必须显式回收
    if (m_sampler)
        m_sampler->stop();

    // 小窗是顶层窗口,没有 parent,得自己删
    delete m_mini;
    m_mini = nullptr;
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
        // 小窗模式下主界面是藏着的,页面不用重绘;而且采样范围收窄后部分字段
        // 本来就是旧的,再让八个页面跑一遍布局纯属白费
        if (m_miniMode) {
            if (m_mini) {
                m_mini->onSystemSnapshot(snapshot);
                updateTrayToolTip();
            }
            return;
        }
        for (PageBase *page : m_pages)
            page->onSystemSnapshot(snapshot);
    });

    connect(m_sampler, &SystemSampler::processSnapshotReady, this, [this](const ProcessSnapshot &snapshot) {
        if (m_paused || m_miniMode)
            return;
        for (PageBase *page : m_pages)
            page->onProcessSnapshot(snapshot);
    });

    connect(m_sampler, &SystemSampler::connectionsReady, this, [this](const QVector<NetConnection> &connections) {
        if (m_paused || m_miniMode)
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
    connect(m_miniButton, &QPushButton::clicked, this, &MainWindow::enterMiniMode);
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

void MainWindow::buildTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    // 图标和主窗口、小窗同一份,免得任务栏和托盘上两个样子
    m_tray = new QSystemTrayIcon(icons::appIcon(theme::accent()), this);
    m_tray->setToolTip(QStringLiteral("WinScope 小窗"));

    auto *menu = new QMenu(this);
    menu->addAction(QStringLiteral("显示主界面"), this, &MainWindow::exitMiniMode);
    menu->addAction(QStringLiteral("选择小窗参数…"), this, [this] {
        // 小窗还没建过就先建一个(比如启动就直接进小窗的情形)
        if (!m_mini)
            return;
        m_mini->chooseMetrics();
    });
    menu->addSeparator();
    menu->addAction(QStringLiteral("退出 WinScope"), this, [] { QApplication::quit(); });
    m_tray->setContextMenu(menu);

    // 左键单击/双击都回主界面 —— 小窗模式没有任务栏按钮,托盘是最顺手的入口。
    //
    // 但图标刚 show() 出来的那一小会儿不能理会:外壳在图标新加入时偶尔会补发一发
    // NIN_SELECT,照单全收的话刚进的小窗会被立刻关掉(启动即进小窗那条路径上
    // 实测遇到过一次)。给 1 秒宽限期,人不可能在这个窗口期内点到它
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason != QSystemTrayIcon::Trigger && reason != QSystemTrayIcon::DoubleClick)
                    return;
                if (m_trayShownAt.isValid() && m_trayShownAt.elapsed() < 1000)
                    return;
                exitMiniMode();
            });
}

void MainWindow::ensureTrayIconPromoted()
{
    if (!m_tray)
        return;
    // 只代劳一次。之后用户在系统设置里怎么调都随他,不再覆盖
    if (QSettings().value(QStringLiteral("tray/promoted"), false).toBool())
        return;

    // 外壳给新图标建 NotifyIconSettings 条目是异步的,show() 之后要等一拍才查得到,
    // 所以查不到就再等一轮,最多试 4 次
    if (m_trayPromoteAttempt++ >= 4)
        return;

    const TrayPromotion state = promoteOwnNotifyIcon();
    if (state == TrayPromotion::NotFound) {
        QTimer::singleShot(1500, this, &MainWindow::ensureTrayIconPromoted);
        return;
    }

    QSettings().setValue(QStringLiteral("tray/promoted"), true);
    if (state != TrayPromotion::Promoted)
        return;

    // 改完注册表外壳不会立刻重读,得把图标删掉再加一次。
    // 这一步会闪一下,但一辈子只发生一次
    m_tray->hide();
    QTimer::singleShot(200, this, [this] {
        if (m_tray && m_miniMode) {
            m_tray->show();
            m_trayShownAt.start();
        }
    });
}

void MainWindow::updateTrayToolTip()
{
    if (!m_tray || !m_mini)
        return;

    const QString tip = m_mini->trayToolTip();
    // 每秒都写一次托盘会白白惊动一次外壳进程,文字没变就别写
    if (tip == m_trayTip)
        return;
    m_trayTip = tip;
    m_tray->setToolTip(tip);
}

void MainWindow::enterMiniMode()
{
    if (m_miniMode)
        return;

    if (!m_mini) {
        // 刻意不给 parent:父窗口一藏,子窗口会跟着藏,小窗模式就变成
        // "什么都看不见"了。生命周期由 ~MainWindow 手工收
        m_mini = new MiniWindow(nullptr);
        connect(m_mini, &MiniWindow::returnToMainRequested, this, &MainWindow::exitMiniMode);
        connect(m_mini, &MiniWindow::quitRequested, this, [] { QApplication::quit(); });
        // 改了小窗勾选的参数,采样范围要跟着重算
        connect(m_mini, &MiniWindow::metricsChanged, this, &MainWindow::applySampleScope);
    }

    m_miniMode = true;
    MiniWindow::setMiniModeActive(true);

    m_mini->show();
    m_mini->raise();
    applySampleScope();

    hide();

    // 主界面藏了、小窗又是 Qt::Tool,这时候整个进程在任务栏上一点痕迹都没有。
    // 托盘图标必须跟着出来,否则用户就找不着入口了
    if (m_tray) {
        m_tray->show();
        m_trayShownAt.start();
        updateTrayToolTip();
        ensureTrayIconPromoted();
    }
}

void MainWindow::exitMiniMode()
{
    if (!m_miniMode)
        return;

    m_miniMode = false;
    MiniWindow::setMiniModeActive(false);
    if (m_mini)
        m_mini->hide();
    if (m_tray)
        m_tray->hide();

    show();
    raise();
    activateWindow();
    applySampleScope();
}

void MainWindow::applySampleScope()
{
    if (!m_sampler)
        return;

    // 小窗模式下只开勾选的那几项需要的数据,其余通道整拍跳过。
    // 回到主界面就全开
    const SampleScope scope = (m_miniMode && m_mini) ? scopeForMetrics(m_mini->metrics())
                                                     : sampleScopeAll();
    m_sampler->setScope(scope);
    // 顺手补一拍,免得刚展开的页面要等满一个刷新周期才有新数据
    m_sampler->refreshNow();
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
