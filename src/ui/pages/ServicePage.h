#pragma once

// Windows 服务管理页。
//
// 枚举 300 个服务的配置要逐项 QueryServiceConfig,耗时约 250ms,
// 所以同样只在首次显示时加载,并提供手动刷新。

#include "core/Types.h"
#include "ui/pages/PageBase.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QVBoxLayout;

namespace ws {

class ServicePage : public PageBase
{
    Q_OBJECT

public:
    explicit ServicePage(QWidget *parent = nullptr);

    QString pageTitle() const override;
    QString pageSubtitle() const override;

    void onActivated() override;

private:
    void buildToolbar(QVBoxLayout *root);
    void reload();
    void updateButtonStates();
    ServiceInfo currentService() const;
    void control(int action);
    // key 为空时弹框让用户选;否则直接用给定值("auto"/"auto-delayed"/"manual"/"disabled")
    void changeStartType(const QString &key = QString());

    QTreeWidget *m_table = nullptr;
    QLineEdit *m_search = nullptr;
    QComboBox *m_stateFilter = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_resumeButton = nullptr;

    QVector<ServiceInfo> m_services;
    bool m_loaded = false;
};

} // namespace ws
