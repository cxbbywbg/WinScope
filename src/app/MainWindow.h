#pragma once

// 主窗口:左侧导航 + 顶部信息栏 + 右侧页面堆栈。
//
// 采样器在这里创建并启动,快照通过信号分发给所有页面。
// 页面不直接碰采样器,只消费快照。

#include "core/SystemSampler.h"
#include "core/Types.h"

#include <QMainWindow>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;

namespace ws {

class PageBase;

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

    QVector<PageBase *> m_pages;
    bool m_paused = false;
    int m_currentIndex = -1;
};

} // namespace ws
