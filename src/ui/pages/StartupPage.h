#pragma once

// 启动项管理页。
//
// 枚举要跑 WMI/COM/注册表,耗时几百毫秒,所以只在页面首次显示时加载一次,
// 之后靠手动刷新。

#include "core/Types.h"
#include "ui/pages/PageBase.h"

class QLabel;
class QPushButton;
class QTreeWidget;
class QVBoxLayout;

namespace ws {

class StartupPage : public PageBase
{
    Q_OBJECT

public:
    explicit StartupPage(QWidget *parent = nullptr);

    QString pageTitle() const override;
    QString pageSubtitle() const override;

    void onActivated() override;

private:
    void buildToolbar(QVBoxLayout *root);
    void reload();
    void updateButtonStates();
    StartupItem currentItem() const;

    void setEnabled(bool enabled);
    void removeCurrent();
    void openFileLocation();
    void openRegistryLocation();

    QTreeWidget *m_table = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_enableButton = nullptr;
    QPushButton *m_disableButton = nullptr;
    QPushButton *m_removeButton = nullptr;

    QVector<StartupItem> m_items;
    bool m_loaded = false;
};

} // namespace ws
