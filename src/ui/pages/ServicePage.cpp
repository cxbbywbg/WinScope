#include "ui/pages/ServicePage.h"

#include "app/Theme.h"
#include "core/ServiceManager.h"
#include "core/Win32Utils.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ws {

namespace {

enum ServiceAction { StartService = 0, StopService, RestartService, PauseService, ResumeService };

QColor stateColor(const QString &state)
{
    if (state == QStringLiteral("正在运行"))
        return theme::ok();
    if (state == QStringLiteral("已停止"))
        return theme::textFaint();
    if (state.startsWith(QStringLiteral("正在")))
        return theme::warn();
    return theme::textDim();
}

} // namespace

ServicePage::ServicePage(QWidget *parent)
    : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 18);
    root->setSpacing(12);

    buildToolbar(root);

    m_table = new QTreeWidget(this);
    m_table->setColumnCount(7);
    m_table->setHeaderLabels({ QStringLiteral("服务名"), QStringLiteral("显示名称"), QStringLiteral("状态"),
                               QStringLiteral("启动类型"), QStringLiteral("PID"), QStringLiteral("账户"),
                               QStringLiteral("服务路径") });
    m_table->setRootIsDecorated(false);
    m_table->setAlternatingRowColors(true);
    m_table->setUniformRowHeights(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->header()->setStretchLastSection(true);
    m_table->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    for (int i = 2; i <= 5; ++i)
        m_table->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);

    connect(m_table, &QTreeWidget::currentItemChanged, this, [this]() { updateButtonStates(); });
    connect(m_table, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (!m_table->currentItem())
            return;
        QMenu menu(this);
        menu.addAction(QStringLiteral("启动"), this, [this]() { control(StartService); });
        menu.addAction(QStringLiteral("停止"), this, [this]() { control(StopService); });
        menu.addAction(QStringLiteral("重启"), this, [this]() { control(RestartService); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("暂停"), this, [this]() { control(PauseService); });
        menu.addAction(QStringLiteral("恢复"), this, [this]() { control(ResumeService); });
        menu.addSeparator();
        QMenu *typeMenu = menu.addMenu(QStringLiteral("设置启动类型"));
        typeMenu->addAction(QStringLiteral("自动"), this,
                            [this]() { changeStartType(QStringLiteral("auto")); });
        typeMenu->addAction(QStringLiteral("自动(延迟)"), this,
                            [this]() { changeStartType(QStringLiteral("auto-delayed")); });
        typeMenu->addAction(QStringLiteral("手动"), this,
                            [this]() { changeStartType(QStringLiteral("manual")); });
        typeMenu->addAction(QStringLiteral("已禁用"), this,
                            [this]() { changeStartType(QStringLiteral("disabled")); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("复制服务名"), this, [this]() {
            QApplication::clipboard()->setText(currentService().name);
        });
        menu.addAction(QStringLiteral("复制服务路径"), this, [this]() {
            QApplication::clipboard()->setText(currentService().binaryPath);
        });
        menu.exec(m_table->viewport()->mapToGlobal(pos));
    });

    root->addWidget(m_table, 1);
}

QString ServicePage::pageTitle() const
{
    return QStringLiteral("Windows 服务");
}

QString ServicePage::pageSubtitle() const
{
    return QStringLiteral("查看系统服务状态与启动类型,并执行启动、停止、重启等操作");
}

void ServicePage::buildToolbar(QVBoxLayout *root)
{
    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("搜索服务名 / 显示名称…"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedWidth(240);
    connect(m_search, &QLineEdit::textChanged, this, [this]() { reload(); });
    toolbar->addWidget(m_search);

    m_stateFilter = new QComboBox(this);
    m_stateFilter->addItems({ QStringLiteral("全部状态"), QStringLiteral("正在运行"), QStringLiteral("已停止"),
                              QStringLiteral("自动启动"), QStringLiteral("已禁用") });
    m_stateFilter->setFixedWidth(126);
    connect(m_stateFilter, &QComboBox::currentIndexChanged, this, [this]() { reload(); });
    toolbar->addWidget(m_stateFilter);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("PageSubtitle"));
    toolbar->addWidget(m_summary);

    toolbar->addStretch(1);

    m_startButton = new QPushButton(QStringLiteral("启动"), this);
    connect(m_startButton, &QPushButton::clicked, this, [this]() { control(StartService); });
    toolbar->addWidget(m_startButton);

    m_stopButton = new QPushButton(QStringLiteral("停止"), this);
    connect(m_stopButton, &QPushButton::clicked, this, [this]() { control(StopService); });
    toolbar->addWidget(m_stopButton);

    auto *restartButton = new QPushButton(QStringLiteral("重启"), this);
    connect(restartButton, &QPushButton::clicked, this, [this]() { control(RestartService); });
    toolbar->addWidget(restartButton);

    m_pauseButton = new QPushButton(QStringLiteral("暂停"), this);
    connect(m_pauseButton, &QPushButton::clicked, this, [this]() { control(PauseService); });
    toolbar->addWidget(m_pauseButton);

    m_resumeButton = new QPushButton(QStringLiteral("恢复"), this);
    connect(m_resumeButton, &QPushButton::clicked, this, [this]() { control(ResumeService); });
    toolbar->addWidget(m_resumeButton);

    auto *typeBox = new QComboBox(this);
    typeBox->addItems(ServiceManager::startTypeOptions());
    typeBox->setFixedWidth(130);
    connect(typeBox, &QComboBox::activated, this, [this, typeBox](int index) {
        const QStringList keys = { QStringLiteral("auto"), QStringLiteral("auto-delayed"),
                                   QStringLiteral("manual"), QStringLiteral("disabled") };
        changeStartType(keys.value(index));
        typeBox->setCurrentIndex(-1);
    });
    toolbar->addWidget(typeBox);

    auto *refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    connect(refreshButton, &QPushButton::clicked, this, [this]() {
        m_loaded = false;
        reload();
    });
    toolbar->addWidget(refreshButton);

    root->addLayout(toolbar);
}

void ServicePage::onActivated()
{
    if (!m_loaded)
        reload();
}

void ServicePage::reload()
{
    if (!m_loaded) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        m_services = ServiceManager::enumerate();
        QApplication::restoreOverrideCursor();
        m_loaded = true;
    }

    const QString keyword = m_search->text().trimmed();
    const QString stateFilter = m_stateFilter->currentText();

    m_table->setUpdatesEnabled(false);
    m_table->clear();

    int shown = 0;
    int running = 0;
    for (int i = 0; i < m_services.size(); ++i) {
        const ServiceInfo &service = m_services.at(i);
        if (service.state == QStringLiteral("正在运行"))
            ++running;

        if (!keyword.isEmpty() && !service.name.contains(keyword, Qt::CaseInsensitive)
            && !service.displayName.contains(keyword, Qt::CaseInsensitive)) {
            continue;
        }
        if (stateFilter == QStringLiteral("正在运行") && service.state != stateFilter)
            continue;
        if (stateFilter == QStringLiteral("已停止") && service.state != stateFilter)
            continue;
        if (stateFilter == QStringLiteral("自动启动")
            && !service.startType.startsWith(QStringLiteral("自动")))
            continue;
        if (stateFilter == QStringLiteral("已禁用") && service.startType != QStringLiteral("已禁用"))
            continue;

        auto *row = new QTreeWidgetItem(m_table);
        row->setText(0, service.name);
        row->setText(1, service.displayName);
        row->setText(2, service.state);
        row->setForeground(2, stateColor(service.state));
        row->setText(3, service.startType);
        row->setForeground(3, service.startType == QStringLiteral("已禁用") ? theme::textFaint() : theme::text());
        row->setText(4, service.pid > 0 ? QString::number(service.pid) : QStringLiteral("—"));
        row->setText(5, service.account);
        row->setText(6, service.binaryPath);
        if (!service.description.isEmpty())
            row->setToolTip(1, service.description);
        row->setToolTip(6, service.binaryPath);
        row->setData(0, Qt::UserRole, i);
        ++shown;
    }

    m_table->setUpdatesEnabled(true);
    m_summary->setText(QStringLiteral("显示 %1 / %2 项,其中 %3 项正在运行")
                           .arg(shown)
                           .arg(m_services.size())
                           .arg(running));
    updateButtonStates();
}

void ServicePage::updateButtonStates()
{
    const bool has = m_table && m_table->currentItem();
    const ServiceInfo service = has ? currentService() : ServiceInfo();
    const bool running = service.state == QStringLiteral("正在运行");
    const bool stopped = service.state == QStringLiteral("已停止");

    if (m_startButton)
        m_startButton->setEnabled(has && stopped);
    if (m_stopButton)
        m_stopButton->setEnabled(has && running && service.canStop);
    if (m_pauseButton)
        m_pauseButton->setEnabled(has && running && service.canPause);
    if (m_resumeButton)
        m_resumeButton->setEnabled(has && service.state == QStringLiteral("已暂停"));
}

ServiceInfo ServicePage::currentService() const
{
    if (!m_table || !m_table->currentItem())
        return ServiceInfo();
    const int index = m_table->currentItem()->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= m_services.size())
        return ServiceInfo();
    return m_services.at(index);
}

void ServicePage::control(int action)
{
    const ServiceInfo service = currentService();
    if (service.name.isEmpty())
        return;

    if (action == StopService || action == RestartService) {
        QString extra;
        if (service.name.contains(QLatin1String("Winmgmt")) || service.name.contains(QLatin1String("RpcSs"))
            || service.name.contains(QLatin1String("DcomLaunch"))) {
            extra = QStringLiteral("\n\n⚠ 这是系统核心服务,停止它可能导致系统异常。");
        }
        const auto answer = QMessageBox::warning(
            this, QStringLiteral("确认操作"),
            QStringLiteral("确定要%1服务「%2」吗?%3")
                .arg(action == StopService ? QStringLiteral("停止") : QStringLiteral("重启"), service.displayName,
                     extra),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    ActionResult result;
    switch (action) {
    case StartService:
        result = ServiceManager::start(service.name);
        break;
    case StopService:
        result = ServiceManager::stop(service.name);
        break;
    case RestartService:
        result = ServiceManager::restart(service.name);
        break;
    case PauseService:
        result = ServiceManager::pause(service.name);
        break;
    case ResumeService:
        result = ServiceManager::resume(service.name);
        break;
    default:
        return;
    }

    if (!result.ok) {
        QMessageBox::warning(this, QStringLiteral("操作失败"), result.message);
        return;
    }

    // 服务状态变化有延迟,给系统一点时间再重读
    QThread::msleep(400);
    m_loaded = false;
    reload();
}

void ServicePage::changeStartType(const QString &key)
{
    const ServiceInfo service = currentService();
    if (service.name.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("未选择服务"),
                                 QStringLiteral("请先在列表中选中一个服务。"));
        return;
    }

    QString target = key;
    if (target.isEmpty()) {
        const QStringList options = ServiceManager::startTypeOptions();
        bool ok = false;
        const QString choice = QInputDialog::getItem(this, QStringLiteral("设置启动类型"),
                                                     QStringLiteral("为「%1」选择启动类型:").arg(service.displayName),
                                                     options, 0, false, &ok);
        if (!ok)
            return;
        if (choice == QStringLiteral("自动"))
            target = QStringLiteral("auto");
        else if (choice == QStringLiteral("自动(延迟)"))
            target = QStringLiteral("auto-delayed");
        else if (choice == QStringLiteral("已禁用"))
            target = QStringLiteral("disabled");
        else
            target = QStringLiteral("manual");
    }

    const auto result = ServiceManager::setStartType(service.name, target);
    if (!result.ok) {
        QMessageBox::warning(this, QStringLiteral("设置失败"), result.message);
        return;
    }

    m_loaded = false;
    reload();
}

} // namespace ws
