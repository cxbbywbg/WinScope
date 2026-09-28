#pragma once

// 主窗口:左侧导航 + 顶部信息栏 + 右侧页面堆栈。
//
// 采样器在这里创建并启动,快照通过信号分发给所有页面。
// 页面不直接碰采样器,只消费快照。

#include "core/SystemSampler.h"
#include "core/Types.h"

#include <QElapsedTimer>
#include <QMainWindow>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QSystemTrayIcon;

namespace ws {

class PageBase;
class MiniWindow;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildPages();
    void wireSampler();

    void navigateTo(int index);
    void applyInterval(int intervalMs);
    void togglePause();
    void promptElevation();

    // 小窗模式:主界面藏起来,只留一个置顶小窗。采样范围同时收窄到
    // 小窗勾选的那几项需要的数据,后台开销跟着降下来
    void enterMiniMode();
    void exitMiniMode();
    void applySampleScope();

    // 托盘图标。小窗是 Qt::Tool,不占任务栏按钮,所以小窗模式下进出只能靠它
    void buildTray();
    void updateTrayToolTip();
    // Win11 默认把新托盘图标塞进「隐藏的图标」浮窗,这里代用户把它设成默认可见
    void ensureTrayIconPromoted();

    // 底部状态提示(不用弹窗,免得打断操作)
    void setStatus(const QString &text, bool isError = false);

    void refreshSidebarFooter();

    SystemSampler *m_sampler = nullptr;

    QListWidget *m_nav = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_pageTitle = nullptr;
    QLabel *m_pageSubtitle = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_footerLabel = nullptr;

    QComboBox *m_intervalBox = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_elevateButton = nullptr;
    QPushButton *m_miniButton = nullptr;

    // 小窗是顶层窗口,不能挂在主窗口下面 —— 父窗口一藏,子窗口跟着藏,
    // 小窗模式就变成"什么都看不见"了。所以它没有 parent,由这里手工管理生命周期
    MiniWindow *m_mini = nullptr;
    bool m_miniMode = false;

    // 只在托盘上显示,父对象是 this(托盘不参与窗口层级,挂主窗口下面没问题)
    QSystemTrayIcon *m_tray = nullptr;
    QString m_trayTip;
    int m_trayPromoteAttempt = 0;
    // 托盘图标显示的时刻。外壳在图标刚加入时偶尔会补发一发 NIN_SELECT,
    // 不挡掉的话会把刚进的小窗又关回去
    QElapsedTimer m_trayShownAt;

    QVector<PageBase *> m_pages;
    bool m_paused = false;
    int m_currentIndex = -1;
};

} // namespace ws
