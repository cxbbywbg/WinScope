#include "ui/pages/StartupPage.h"

#include "app/Theme.h"
#include "core/StartupManager.h"
#include "core/Win32Utils.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ws {

namespace {

QColor impactColor(const QString &impact)
{
    if (impact == QStringLiteral("高"))
        return theme::danger();
    if (impact == QStringLiteral("中"))
        return theme::warn();
    if (impact == QStringLiteral("低"))
        return theme::ok();
    if (impact == QStringLiteral("已禁用"))
        return theme::textFaint();
    return theme::textDim();
}

} // namespace

StartupPage::StartupPage(QWidget *parent)
    : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 18);
    root->setSpacing(12);

    buildToolbar(root);

    m_table = new QTreeWidget(this);
    m_table->setColumnCount(6);
    m_table->setHeaderLabels({ QStringLiteral("名称"), QStringLiteral("状态"), QStringLiteral("来源"),
                               QStringLiteral("发布者"), QStringLiteral("启动影响"),
                               QStringLiteral("启动命令 / 位置") });
    m_table->setRootIsDecorated(false);
    m_table->setAlternatingRowColors(true);
    m_table->setUniformRowHeights(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->header()->setStretchLastSection(true);
    m_table->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (int i = 1; i <= 4; ++i)
        m_table->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    connect(m_table, &QTreeWidget::currentItemChanged, this, [this]() { updateButtonStates(); });
    connect(m_table, &QTreeWidget::itemDoubleClicked, this, [this]() { setEnabled(!currentItem().enabled); });
    connect(m_table, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (!m_table->currentItem())
            return;
        const StartupItem item = currentItem();
        QMenu menu(this);
        menu.addAction(item.enabled ? QStringLiteral("禁用") : QStringLiteral("启用"), this,
                       [this, item]() { setEnabled(!item.enabled); });
        menu.addAction(QStringLiteral("打开文件位置"), this, [this]() { openFileLocation(); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("复制启动命令"), this, [item]() {
            QApplication::clipboard()->setText(item.command);
        });
        menu.addAction(QStringLiteral("复制位置"), this, [item]() {
            QApplication::clipboard()->setText(item.location);
        });
        menu.addSeparator();
        menu.addAction(QStringLiteral("删除此项"), this, [this]() { removeCurrent(); });
        menu.exec(m_table->viewport()->mapToGlobal(pos));
    });

    root->addWidget(m_table, 1);
}

QString StartupPage::pageTitle() const
{
    return QStringLiteral("启动项");
}

QString StartupPage::pageSubtitle() const
{
    return QStringLiteral("管理注册表 Run、启动文件夹与计划任务中的开机自启项");
}

void StartupPage::buildToolbar(QVBoxLayout *root)
{
    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("PageSubtitle"));
    toolbar->addWidget(m_summary);

    toolbar->addStretch(1);

    m_enableButton = new QPushButton(QStringLiteral("启用"), this);
    connect(m_enableButton, &QPushButton::clicked, this, [this]() { setEnabled(true); });
    toolbar->addWidget(m_enableButton);

    m_disableButton = new QPushButton(QStringLiteral("禁用"), this);
    connect(m_disableButton, &QPushButton::clicked, this, [this]() { setEnabled(false); });
    toolbar->addWidget(m_disableButton);

    auto *locationButton = new QPushButton(QStringLiteral("打开文件位置"), this);
    connect(locationButton, &QPushButton::clicked, this, [this]() { openFileLocation(); });
    toolbar->addWidget(locationButton);

    auto *registryButton = new QPushButton(QStringLiteral("查看注册表位置"), this);
    connect(registryButton, &QPushButton::clicked, this, [this]() { openRegistryLocation(); });
    toolbar->addWidget(registryButton);

    m_removeButton = new QPushButton(QStringLiteral("删除"), this);
    m_removeButton->setObjectName(QStringLiteral("Danger"));
    connect(m_removeButton, &QPushButton::clicked, this, [this]() { removeCurrent(); });
    toolbar->addWidget(m_removeButton);

    auto *refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    connect(refreshButton, &QPushButton::clicked, this, [this]() { reload(); });
    toolbar->addWidget(refreshButton);

    root->addLayout(toolbar);
}

void StartupPage::onActivated()
{
    if (!m_loaded)
        reload();
}

void StartupPage::reload()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_items = StartupManager::enumerate();
    QApplication::restoreOverrideCursor();

    m_loaded = true;

    m_table->clear();
    int enabled = 0;
    for (int i = 0; i < m_items.size(); ++i) {
        const StartupItem &item = m_items.at(i);
        if (item.enabled)
            ++enabled;

        auto *row = new QTreeWidgetItem(m_table);
        row->setText(0, item.name);
        row->setText(1, item.enabled ? QStringLiteral("已启用") : QStringLiteral("已禁用"));
        row->setForeground(1, item.enabled ? theme::ok() : theme::textFaint());
        row->setText(2, item.sourceLabel);
        row->setText(3, item.publisher);
        row->setText(4, item.impact);
        row->setForeground(4, impactColor(item.impact));
        row->setText(5, item.command);
        row->setToolTip(5, QStringLiteral("%1\n位置: %2").arg(item.command, item.location));
        // 行与 m_items 用下标关联,currentItem() 靠它取回原始数据
        row->setData(0, Qt::UserRole, i);

        if (!item.enabled)
            row->setForeground(0, theme::textDim());
    }

    m_summary->setText(QStringLiteral("共 %1 项,其中 %2 项已启用").arg(m_items.size()).arg(enabled));
    updateButtonStates();
}

void StartupPage::updateButtonStates()
{
    const bool has = m_table && m_table->currentItem();
    const StartupItem item = has ? currentItem() : StartupItem();
    const bool canToggle = has && item.canToggle;

    if (m_enableButton)
        m_enableButton->setEnabled(canToggle && !item.enabled);
    if (m_disableButton)
        m_disableButton->setEnabled(canToggle && item.enabled);
    if (m_removeButton)
        m_removeButton->setEnabled(has && item.source != StartupSource::ScheduledTask);
}

StartupItem StartupPage::currentItem() const
{
    if (!m_table || !m_table->currentItem())
        return StartupItem();
    const int index = m_table->currentItem()->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= m_items.size())
        return StartupItem();
    return m_items.at(index);
}

void StartupPage::setEnabled(bool enabled)
{
    const StartupItem item = currentItem();
    if (item.name.isEmpty())
        return;

    const auto result = StartupManager::setEnabled(item, enabled);
    if (!result.ok) {
        QMessageBox::warning(this, QStringLiteral("操作失败"), result.message);
        return;
    }
    reload();
}

void StartupPage::removeCurrent()
{
    const StartupItem item = currentItem();
    if (item.name.isEmpty())
        return;

    const auto answer = QMessageBox::warning(
        this, QStringLiteral("确认删除"),
        QStringLiteral("确定要删除启动项「%1」吗?\n\n位置:%2\n\n"
                       "注册表项会被直接删除(不可恢复);启动文件夹中的快捷方式会移入回收站。")
            .arg(item.name, item.location),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    const auto result = StartupManager::remove(item);
    if (!result.ok)
        QMessageBox::warning(this, QStringLiteral("删除失败"), result.message);
    else
        reload();
}

void StartupPage::openFileLocation()
{
    const StartupItem item = currentItem();
    if (item.filePath.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("无法定位"),
                                 QStringLiteral("这一项没有可定位的文件路径。"));
        return;
    }
    if (!revealInExplorer(item.filePath))
        QMessageBox::warning(this, QStringLiteral("无法打开"),
                             QStringLiteral("无法在资源管理器中打开:\n%1").arg(item.filePath));
}

void StartupPage::openRegistryLocation()
{
    const StartupItem item = currentItem();
    if (item.registryKey.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("不是注册表项"),
                                 QStringLiteral("「%1」来自%2,没有对应的注册表位置。")
                                     .arg(item.name, item.sourceLabel));
        return;
    }

    // regedit 支持用 /m 打开时定位到上次位置,这里直接启动并提示路径
    QApplication::clipboard()->setText(
        QStringLiteral("HKEY_CURRENT_USER\\%1").arg(item.registryKey));
    QMessageBox::information(
        this, QStringLiteral("注册表位置"),
        QStringLiteral("该启动项位于:\n\nHKCU 或 HKLM\\%1\n值名:%2\n\n"
                       "路径已复制到剪贴板,注册表编辑器即将打开。")
            .arg(item.registryKey, item.registryValue));
    runElevatedCommand(QStringLiteral("regedit.exe"), QString());
}

} // namespace ws
