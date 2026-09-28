#pragma once

// 桌面小窗。
//
// 一个无边框、置顶、可拖动的小窗口,只显示用户勾选的几个参数。主界面藏起来时
// 用它替代 —— 这时采样范围会被收窄到"这几个参数需要的那几路",其余通道整拍跳过
// (见 SystemSampler::setScope),后台开销跟着降下来。
//
// 交互:
//   左键拖动   移动窗口(无边框,没有标题栏可拖)
//   标题栏右上角的图钉  切换「窗口置顶」(点图钉不会误触发拖动)
//   双击       回到主界面
//   右键       菜单(选择参数 / 返回主界面 / 置顶 / 退出)
//
// 位置、勾选的参数、是否置顶都存 QSettings,下次启动接着用。
//
// 窗口是 Qt::Tool:Windows 上对应 WS_EX_TOOLWINDOW,不会在任务栏占一个按钮,
// 也不会进 Alt+Tab —— 小窗模式下改成靠右下角的托盘图标进出,和微信那类常驻
// 程序一样。代价是它不再有任务栏入口,所以托盘图标必须一起显示(见 MainWindow)。
//
// 窗口是方角的,没有做圆角透明:圆角要靠 WA_TranslucentBackground(分层窗口),
// 而分层窗口在 PrintWindow 抓图、远程桌面、部分老显卡驱动上都出过毛病。
// 为了一个装饰性圆角不值得冒这个险

#include "core/Metrics.h"
#include "core/Types.h"

#include <QPoint>
#include <QVector>
#include <QWidget>

namespace ws {

class MiniWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MiniWindow(QWidget *parent = nullptr);
    ~MiniWindow() override;

    // 用户勾选的参数,按显示顺序
    QVector<MetricId> metrics() const { return m_metrics; }
    void setMetrics(const QVector<MetricId> &metrics);

    // 打开参数选择对话框。托盘菜单也会调它,所以是 public
    void chooseMetrics();

    // 托盘图标的悬停提示:拿前两个有读数的参数拼一行,方便不展开小窗也能扫一眼
    QString trayToolTip() const;

    // 上次退出时是不是停在小窗模式。是的话下次启动直接进小窗
    static bool miniModeWasActive();
    static void setMiniModeActive(bool active);

    void onSystemSnapshot(const SystemSnapshot &snapshot);

signals:
    void metricsChanged();
    void returnToMainRequested();
    void quitRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void applyAlwaysOnTop(bool on);
    void relayout();
    void loadSettings();
    void saveSettings() const;
    void moveToDefaultCorner();
    // 标题栏右上角那个图钉的点击区
    QRect pinRect() const;

    QVector<MetricId> m_metrics;
    SystemSnapshot m_snapshot;

    QPoint m_dragOffset;
    // 上次存下来的位置。落在屏幕外就忽略它,回到默认角落
    QPoint m_savedPosition;
    bool m_dragging = false;
    bool m_alwaysOnTop = true;
    // 鼠标是不是停在图钉上(画悬停底板用)
    bool m_pinHovered = false;
    // 这一次按下是不是落在图钉上。松手时要靠它决定"存不存位置"
    bool m_pinPressed = false;
    bool m_quitting = false;
};

} // namespace ws
