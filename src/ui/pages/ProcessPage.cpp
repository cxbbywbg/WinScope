#include "ui/pages/ProcessPage.h"

#include "app/Theme.h"
#include "core/ProcessController.h"
#include "core/Win32Utils.h"
#include "ui/widgets/Card.h"
#include "ui/widgets/FlowLayout.h"
#include "ui/widgets/StatRow.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ws {

namespace {

enum Action {
    ActClose = 0,
    ActTerminate,
    ActRestart,
    ActSuspend,
    ActResume,
    ActPriority,
    ActAffinity,
    ActReveal,
};

// 行上挂 PID 的专用 role。
//
// 千万别改用 Qt::UserRole:那一格被 updateRow() 的 setCell() 拿去存「排序用的数值」了,
// 名称列存的是 0.0。以前 currentProcess() 就是读第 0 列的 Qt::UserRole 当 PID,
// 结果永远拿到 0 → 详情面板永远显示「系统空闲进程」,右键菜单里每个操作
// (结束/挂起/优先级/打开文件所在位置…)都在 `info.pid == 0` 那关被拦下,
// 一律弹「请先在列表中选中一个进程」。单独占一个 role 就不会再撞。
constexpr int PidRole = Qt::UserRole + 2;

bool isNumericColumn(int column)
{
    return column != ProcessPage::ColName && column != ProcessPage::ColUser && column != ProcessPage::ColStatus;
}

// 列表行:自己实现比较,好让 CPU/内存这类列按数值排而不是按字符串排
// (否则 "9%" 会排在 "10%" 后面)
class ProcessTreeItem : public QTreeWidgetItem
{
public:
    explicit ProcessTreeItem(QTreeWidget *parent)
        : QTreeWidgetItem(parent)
    {
    }

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const QTreeWidget *tree = treeWidget();
        const int column = tree ? tree->sortColumn() : 0;
        const auto &rhs = static_cast<const ProcessTreeItem &>(other);

        if (isNumericColumn(column)) {
            const double a = data(column, Qt::UserRole).toDouble();
            const double b = rhs.data(column, Qt::UserRole).toDouble();
            if (a != b)
                return a < b;
            // 数值相同再按 PID 稳定排序,避免每秒刷新时行来回跳
            return data(ProcessPage::ColPid, Qt::UserRole).toDouble()
                < rhs.data(ProcessPage::ColPid, Qt::UserRole).toDouble();
        }

        const int cmp = data(column, Qt::UserRole + 1).toString().compare(
            rhs.data(column, Qt::UserRole + 1).toString(), Qt::CaseInsensitive);
        if (cmp != 0)
            return cmp < 0;
        return data(ProcessPage::ColPid, Qt::UserRole).toDouble()
            < rhs.data(ProcessPage::ColPid, Qt::UserRole).toDouble();
    }
};

ProcessTreeItem *asItem(QTreeWidgetItem *item)
{
    return static_cast<ProcessTreeItem *>(item);
}

} // namespace

ProcessPage::ProcessPage(QWidget *parent)
    : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 18);
    root->setSpacing(12);

    buildToolbar(root);
    buildListAndDetails(root);
}

QString ProcessPage::pageTitle() const
{
    return QStringLiteral("进程管理");
}

QString ProcessPage::pageSubtitle() const
{
    return QStringLiteral("查看进程占用、进程树与详细信息,并结束、挂起或调整优先级");
}

void ProcessPage::buildToolbar(QVBoxLayout *root)
{
    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("搜索进程名 / PID / 路径…"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedWidth(260);
    connect(m_search, &QLineEdit::textChanged, this, [this]() { sortList(); });
    toolbar->addWidget(m_search);

    m_countLabel = new QLabel(this);
    m_countLabel->setObjectName(QStringLiteral("PageSubtitle"));
    toolbar->addWidget(m_countLabel);

    toolbar->addStretch(1);

    auto *endButton = new QPushButton(QStringLiteral("结束进程"), this);
    connect(endButton, &QPushButton::clicked, this, [this]() { runAction(ActClose); });
    toolbar->addWidget(endButton);

    auto *killButton = new QPushButton(QStringLiteral("强制结束"), this);
    killButton->setObjectName(QStringLiteral("Danger"));
    connect(killButton, &QPushButton::clicked, this, [this]() { runAction(ActTerminate); });
    toolbar->addWidget(killButton);

    m_pauseButton = new QPushButton(QStringLiteral("暂停"), this);
    connect(m_pauseButton, &QPushButton::clicked, this, [this]() { runAction(ActSuspend); });
    toolbar->addWidget(m_pauseButton);

    m_resumeButton = new QPushButton(QStringLiteral("恢复"), this);
    connect(m_resumeButton, &QPushButton::clicked, this, [this]() { runAction(ActResume); });
    toolbar->addWidget(m_resumeButton);

    m_priorityBox = new QComboBox(this);
    m_priorityBox->addItem(QStringLiteral("优先级:正常"), -1);
    const QStringList names = ProcessController::priorityNames();
    for (int i = 0; i < names.size(); ++i)
        m_priorityBox->addItem(QStringLiteral("优先级:%1").arg(names.at(i)), i);
    m_priorityBox->setFixedWidth(190);
    connect(m_priorityBox, &QComboBox::activated, this, [this](int index) {
        const int value = m_priorityBox->itemData(index).toInt();
        if (value >= 0)
            runAction(ActPriority);
        m_priorityBox->setCurrentIndex(0);
    });
    toolbar->addWidget(m_priorityBox);

    auto *affinityButton = new QPushButton(QStringLiteral("CPU 亲和性"), this);
    connect(affinityButton, &QPushButton::clicked, this, [this]() { runAction(ActAffinity); });
    toolbar->addWidget(affinityButton);

    root->addLayout(toolbar);
}

void ProcessPage::buildListAndDetails(QVBoxLayout *root)
{
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    auto *tabs = new QTabWidget(splitter);

    // ---------------- 列表
    m_list = new QTreeWidget(tabs);
    m_list->setColumnCount(ColumnCount);
    m_list->setHeaderLabels({ QStringLiteral("进程名称"), QStringLiteral("PID"), QStringLiteral("CPU"),
                              QStringLiteral("内存"), QStringLiteral("GPU"), QStringLiteral("显存"),
                              QStringLiteral("磁盘"), QStringLiteral("网络"), QStringLiteral("用户"),
                              QStringLiteral("状态") });
    m_list->setRootIsDecorated(false);
    m_list->setAlternatingRowColors(true);
    m_list->setUniformRowHeights(true);
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(ColCpu, Qt::DescendingOrder);
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    for (int c = ColPid; c < ColumnCount; ++c)
        m_list->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);

    connect(m_list->header(), &QHeaderView::sortIndicatorChanged, this, [this](int column, Qt::SortOrder order) {
        m_sortColumn = column;
        m_sortOrder = order;
    });
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this]() { refreshDetails(); });
    connect(m_list, &QTreeWidget::customContextMenuRequested, this, &ProcessPage::showContextMenu);

    tabs->addTab(m_list, QStringLiteral("进程列表"));

    // ---------------- 进程树
    m_tree = new QTreeWidget(tabs);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({ QStringLiteral("进程"), QStringLiteral("PID"), QStringLiteral("CPU"),
                              QStringLiteral("内存"), QStringLiteral("状态") });
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &ProcessPage::showContextMenu);
    tabs->addTab(m_tree, QStringLiteral("进程树"));

    splitter->addWidget(tabs);

    // ---------------- 详情
    auto *detailCard = new Card(QStringLiteral("进程详情"), splitter);
    detailCard->setMinimumWidth(330);
    buildDetailsPanel(detailCard->body());
    splitter->addWidget(detailCard);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({ 900, 360 });

    root->addWidget(splitter, 1);
}

void ProcessPage::buildDetailsPanel(QVBoxLayout *layout)
{
    m_detailName = new QLabel(this);
    m_detailName->setObjectName(QStringLiteral("CardTitle"));
    m_detailName->setWordWrap(true);
    layout->addWidget(m_detailName);

    m_detailPath = new QLabel(this);
    m_detailPath->setObjectName(QStringLiteral("CardHint"));
    m_detailPath->setWordWrap(true);
    m_detailPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_detailPath);

    layout->addSpacing(6);

    const QStringList keys = {
        QStringLiteral("PID"),        QStringLiteral("父进程"),   QStringLiteral("用户"),
        QStringLiteral("架构"),       QStringLiteral("启动时间"), QStringLiteral("CPU 时间"),
        QStringLiteral("CPU 占用"),   QStringLiteral("内存(工作集)"), QStringLiteral("提交大小"),
        QStringLiteral("虚拟大小"),   QStringLiteral("线程数"),   QStringLiteral("句柄数"),
        QStringLiteral("GPU 占用"),   QStringLiteral("显存"),     QStringLiteral("磁盘读 / 写"),
        QStringLiteral("网络 ↓ / ↑"), QStringLiteral("状态"),
    };
    for (const QString &key : keys) {
        auto *row = new StatRow(key, this);
        m_detailRows.push_back(row);
        layout->addWidget(row);
    }

    auto *commandKey = new QLabel(QStringLiteral("命令行"), this);
    commandKey->setObjectName(QStringLiteral("StatKey"));
    layout->addSpacing(4);
    layout->addWidget(commandKey);

    m_detailCommandLabel = new QLabel(this);
    m_detailCommandLabel->setObjectName(QStringLiteral("CardHint"));
    m_detailCommandLabel->setWordWrap(true);
    m_detailCommandLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detailCommandLabel->setMinimumHeight(44);
    m_detailCommandLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->addWidget(m_detailCommandLabel);

    layout->addStretch(1);
}

void ProcessPage::onProcessSnapshot(const ProcessSnapshot &snapshot)
{
    if (m_paused)
        return;

    m_processes = snapshot.processes;
    m_byPid.clear();
    for (const auto &p : m_processes)
        m_byPid.insert(p.pid, p);

    m_countLabel->setText(QStringLiteral("%1 个进程 · %2 个线程 · %3 个句柄")
                              .arg(snapshot.totalProcesses)
                              .arg(formatCount(snapshot.totalThreads), formatCount(snapshot.totalHandles)));

    updateList(snapshot);

    // 进程树只在它可见时才重建,省掉无谓的开销
    if (m_tree && m_tree->isVisible())
        updateTree(snapshot);

    refreshDetails();
}

void ProcessPage::onSystemSnapshot(const SystemSnapshot &snapshot)
{
    Q_UNUSED(snapshot);
}

void ProcessPage::onSamplingPaused(bool paused)
{
    m_paused = paused;
}

bool ProcessPage::passesFilter(const ProcessInfo &info) const
{
    const QString keyword = m_search->text().trimmed();
    if (keyword.isEmpty())
        return true;

    if (info.name.contains(keyword, Qt::CaseInsensitive))
        return true;
    if (QString::number(info.pid) == keyword)
        return true;
    if (info.path.contains(keyword, Qt::CaseInsensitive))
        return true;
    if (!info.description.isEmpty() && info.description.contains(keyword, Qt::CaseInsensitive))
        return true;
    return false;
}

void ProcessPage::updateList(const ProcessSnapshot &snapshot)
{
    Q_UNUSED(snapshot);

    QSet<quint32> alive;
    alive.reserve(m_processes.size());

    for (const ProcessInfo &info : m_processes) {
        alive.insert(info.pid);

        if (!passesFilter(info)) {
            // 被过滤掉的进程要从列表里撤掉,但保留在 m_byPid 里供详情使用
            if (auto *existing = m_items.take(info.pid))
                delete existing;
            continue;
        }

        QTreeWidgetItem *item = m_items.value(info.pid, nullptr);
        if (!item) {
            item = new ProcessTreeItem(m_list);
            m_items.insert(info.pid, item);
        }
        updateRow(item, info);
    }

    // 清理已退出的进程
    for (auto it = m_items.begin(); it != m_items.end();) {
        if (!alive.contains(it.key())) {
            delete it.value();
            it = m_items.erase(it);
        } else {
            ++it;
        }
    }

    m_list->sortItems(m_sortColumn, m_sortOrder);
}

void ProcessPage::updateRow(QTreeWidgetItem *item, const ProcessInfo &info)
{
    auto setCell = [item](int column, const QString &text, double numeric, const QColor &color = QColor()) {
        item->setText(column, text);
        item->setData(column, Qt::UserRole, numeric);
        item->setData(column, Qt::UserRole + 1, text);
        if (color.isValid())
            item->setForeground(column, color);
    };

    setCell(ColName, info.name, 0.0);
    item->setData(ColName, PidRole, info.pid);
    if (!info.description.isEmpty())
        item->setToolTip(ColName, QStringLiteral("%1\n%2").arg(info.description, info.path));

    setCell(ColPid, QString::number(info.pid), double(info.pid));

    // CPU 占用高的标红,一眼能看出谁在吃 CPU
    const QColor cpuColor = info.cpuPercent >= 25.0 ? theme::warn()
        : (info.cpuPercent >= 50.0 ? theme::danger() : QColor());
    setCell(ColCpu, QStringLiteral("%1%").arg(info.cpuPercent, 0, 'f', 1), info.cpuPercent, cpuColor);

    setCell(ColMemory, formatBytes(info.workingSetBytes), double(info.workingSetBytes));
    setCell(ColGpu, info.gpuPercent > 0.0 ? QStringLiteral("%1%").arg(info.gpuPercent, 0, 'f', 1)
                                          : QStringLiteral("—"),
            info.gpuPercent);
    setCell(ColVram, info.gpuDedicatedBytes > 0 ? formatBytes(info.gpuDedicatedBytes) : QStringLiteral("—"),
            double(info.gpuDedicatedBytes));
    setCell(ColDisk, formatBytesPerSec(double(info.diskReadBytesPerSec + info.diskWriteBytesPerSec)),
            double(info.diskReadBytesPerSec + info.diskWriteBytesPerSec));
    setCell(ColNetwork, formatBytesPerSec(info.netRxBytesPerSec + info.netTxBytesPerSec),
            info.netRxBytesPerSec + info.netTxBytesPerSec);
    setCell(ColUser, info.user.isEmpty() ? QStringLiteral("—") : info.user, 0.0);

    QString status = QStringLiteral("运行中");
    if (info.critical)
        status = QStringLiteral("系统关键");
    else if (info.elevated)
        status = QStringLiteral("已提权");
    setCell(ColStatus, status, 0.0);

    item->setForeground(ColStatus, info.critical ? theme::warn() : theme::textDim());

    // 内存占用大的给个视觉提示
    if (info.workingSetBytes > (2ull << 30))
        item->setForeground(ColMemory, theme::warn());
}

void ProcessPage::updateTree(const ProcessSnapshot &snapshot)
{
    // 树是低频视图,直接整棵重建更简单,也不会像列表那样每秒抖动
    const quint32 selected = currentProcess().pid;

    m_tree->clear();

    QHash<quint32, QTreeWidgetItem *> nodes;
    QVector<ProcessInfo> pending = snapshot.processes;
    QHash<quint32, bool> placed;

    auto createNode = [&](const ProcessInfo &info) -> QTreeWidgetItem * {
        auto *item = new QTreeWidgetItem();
        item->setText(0, info.name);
        item->setText(1, QString::number(info.pid));
        item->setText(2, QStringLiteral("%1%").arg(info.cpuPercent, 0, 'f', 1));
        item->setText(3, formatBytes(info.workingSetBytes));
        item->setText(4, info.critical ? QStringLiteral("系统关键") : QStringLiteral("运行中"));
        item->setData(0, PidRole, info.pid);
        return item;
    };

    // 反复扫,直到所有能挂到父节点的进程都挂上;剩下的作为根
    bool progressed = true;
    while (progressed && placed.size() < pending.size()) {
        progressed = false;
        for (const ProcessInfo &info : pending) {
            if (placed.contains(info.pid))
                continue;

            if (info.parentPid != 0 && nodes.contains(info.parentPid)) {
                auto *node = createNode(info);
                nodes.value(info.parentPid)->addChild(node);
                nodes.insert(info.pid, node);
                placed.insert(info.pid, true);
                progressed = true;
            } else if (!nodes.contains(info.parentPid) && info.parentPid != 0) {
                // 父进程不在快照里(已退出或是别的会话),先留着看下一轮
                bool parentExists = false;
                for (const ProcessInfo &candidate : pending) {
                    if (candidate.pid == info.parentPid) {
                        parentExists = true;
                        break;
                    }
                }
                if (!parentExists) {
                    auto *node = createNode(info);
                    m_tree->addTopLevelItem(node);
                    nodes.insert(info.pid, node);
                    placed.insert(info.pid, true);
                    progressed = true;
                }
            } else {
                auto *node = createNode(info);
                m_tree->addTopLevelItem(node);
                nodes.insert(info.pid, node);
                placed.insert(info.pid, true);
                progressed = true;
            }
        }
    }

    // 兜底:环形父子关系导致的漏网之鱼
    for (const ProcessInfo &info : pending) {
        if (placed.contains(info.pid))
            continue;
        auto *node = createNode(info);
        m_tree->addTopLevelItem(node);
        nodes.insert(info.pid, node);
        placed.insert(info.pid, true);
    }

    m_tree->expandToDepth(0);
    m_tree->resizeColumnToContents(0);

    if (selected != 0 && nodes.contains(selected))
        m_tree->setCurrentItem(nodes.value(selected));
}

void ProcessPage::sortList()
{
    updateList(ProcessSnapshot());
}

QTreeWidgetItem *ProcessPage::currentRow() const
{
    if (m_list && m_list->isVisible())
        return m_list->currentItem();
    if (m_tree && m_tree->isVisible())
        return m_tree->currentItem();
    return nullptr;
}

bool ProcessPage::selectedProcess(ProcessInfo *out) const
{
    QTreeWidgetItem *item = currentRow();
    if (!item)
        return false;

    const quint32 pid = item->data(0, PidRole).toUInt();
    const auto it = m_byPid.constFind(pid);
    if (it == m_byPid.constEnd())
        return false;

    if (out)
        *out = it.value();
    return true;
}

ProcessInfo ProcessPage::currentProcess() const
{
    ProcessInfo info;
    selectedProcess(&info);
    return info;
}

void ProcessPage::refreshDetails()
{
    ProcessInfo info;
    // 别用「pid == 0 且名字为空」猜有没有选中:「系统空闲进程」就是 pid 0 的真实一行
    if (!selectedProcess(&info)) {
        m_detailName->setText(QStringLiteral("未选择进程"));
        m_detailPath->setText(QStringLiteral("在左侧列表中选中一个进程查看详情"));
        for (auto *row : m_detailRows)
            row->setValue(QString());
        return;
    }

    m_detailName->setText(info.description.isEmpty() ? info.name
                                                     : QStringLiteral("%1  ·  %2").arg(info.name, info.description));
    m_detailPath->setText(info.path.isEmpty() ? QStringLiteral("(路径不可见,可能受保护或需要管理员权限)")
                                              : info.path);

    m_detailRows[0]->setValue(QString::number(info.pid));
    m_detailRows[1]->setValue(QString::number(info.parentPid));
    m_detailRows[2]->setValue(info.user.isEmpty() ? QStringLiteral("—") : info.user);
    m_detailRows[3]->setValue(info.architecture.isEmpty() ? QStringLiteral("—") : info.architecture);
    m_detailRows[4]->setValue(formatDateTime(info.startTimeMs));

    const qint64 totalSeconds = qint64(info.cpuTimeMs / 1000);
    m_detailRows[5]->setValue(QStringLiteral("%1 小时 %2 分 %3 秒")
                                  .arg(totalSeconds / 3600)
                                  .arg((totalSeconds % 3600) / 60)
                                  .arg(totalSeconds % 60));
    m_detailRows[6]->setValue(QStringLiteral("%1%").arg(info.cpuPercent, 0, 'f', 2));
    m_detailRows[7]->setValue(formatBytes(info.workingSetBytes));
    m_detailRows[8]->setValue(formatBytes(info.privateBytes));
    m_detailRows[9]->setValue(formatBytes(info.virtualBytes));
    m_detailRows[10]->setValue(formatCount(info.threadCount));
    m_detailRows[11]->setValue(formatCount(info.handleCount));
    m_detailRows[12]->setValue(QStringLiteral("%1%").arg(info.gpuPercent, 0, 'f', 1));
    m_detailRows[13]->setValue(formatBytes(info.gpuDedicatedBytes));
    m_detailRows[14]->setValue(QStringLiteral("%1 / %2")
                                   .arg(formatBytesPerSec(double(info.diskReadBytesPerSec)),
                                        formatBytesPerSec(double(info.diskWriteBytesPerSec))));
    m_detailRows[15]->setValue(QStringLiteral("%1 / %2")
                                   .arg(formatBytesPerSec(info.netRxBytesPerSec),
                                        formatBytesPerSec(info.netTxBytesPerSec)));
    m_detailRows[16]->setValue(info.critical ? QStringLiteral("系统关键进程")
                                             : (info.elevated ? QStringLiteral("以管理员身份运行")
                                                              : QStringLiteral("普通权限")));

    // 命令行
    if (m_detailCommandLabel) {
        m_detailCommandLabel->setText(info.commandLine.isEmpty() ? QStringLiteral("(需要管理员权限才能读取命令行)")
                                                                : info.commandLine);
    }
}

void ProcessPage::runAction(int action)
{
    ProcessInfo info;
    if (!selectedProcess(&info)) {
        QMessageBox::information(this, QStringLiteral("未选择进程"),
                                 QStringLiteral("请先在列表中选中一个进程。"));
        return;
    }

    if (action == ActReveal) {
        if (info.path.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("无法定位"),
                                 QStringLiteral("读不到「%1」(PID %2) 的可执行文件路径。\n"
                                                "该进程可能受系统保护,或需要管理员权限。")
                                     .arg(info.name)
                                     .arg(info.pid));
        } else if (!revealInExplorer(info.path)) {
            QMessageBox::warning(this, QStringLiteral("无法打开"),
                                 QStringLiteral("无法在资源管理器中打开:\n%1").arg(info.path));
        }
        return;
    }

    if (action == ActAffinity) {
        chooseAffinity();
        return;
    }

    // 结束/强制结束/暂停这类破坏性操作先确认
    if (action == ActClose || action == ActTerminate || action == ActSuspend) {
        const QString verb = action == ActClose ? QStringLiteral("结束")
            : (action == ActTerminate ? QStringLiteral("强制结束") : QStringLiteral("挂起"));
        QString extra;
        if (info.critical)
            extra = QStringLiteral("\n\n⚠ 这是系统关键进程,结束它可能导致系统不稳定甚至蓝屏。");
        else if (info.elevated)
            extra = QStringLiteral("\n\n该进程以管理员身份运行,操作可能被拒绝。");

        const auto answer = QMessageBox::warning(
            this, QStringLiteral("确认操作"),
            QStringLiteral("确定要%1 %2 (PID %3) 吗?%4").arg(verb, info.name).arg(info.pid).arg(extra),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    ActionResult result;
    switch (action) {
    case ActClose:
        result = ProcessController::closeGracefully(info.pid);
        break;
    case ActTerminate:
        result = ProcessController::terminate(info.pid);
        break;
    case ActRestart:
        result = ProcessController::restart(info.pid, info.path, info.commandLine);
        break;
    case ActSuspend:
        result = ProcessController::suspend(info.pid);
        break;
    case ActResume:
        result = ProcessController::resume(info.pid);
        break;
    case ActPriority: {
        const int index = m_priorityBox->currentData().toInt();
        result = ProcessController::setPriority(info.pid, ProcessController::priorityValueAt(index));
        break;
    }
    default:
        return;
    }

    if (!result.ok) {
        QMessageBox::warning(this, QStringLiteral("操作失败"), result.message);
    } else if (!result.message.isEmpty()) {
        // 成功也提示一下,因为"已发送关闭请求"和"已结束"是两回事
        statusBarMessage(result.message);
    }
}

void ProcessPage::statusBarMessage(const QString &text)
{
    // 页面没有自己的状态栏,复用标题区下方的计数标签做一次短暂提示
    if (!m_countLabel)
        return;
    const QString original = m_countLabel->text();
    m_countLabel->setText(text);
    m_countLabel->setStyleSheet(QStringLiteral("color: %1;").arg(theme::ok().name()));
    QTimer::singleShot(3000, this, [this, original]() {
        m_countLabel->setText(original);
        m_countLabel->setStyleSheet(QString());
    });
}

void ProcessPage::showContextMenu(const QPoint &pos)
{
    QTreeWidget *source = qobject_cast<QTreeWidget *>(sender());
    if (!source)
        return;

    // 右键点在哪一行就作用于哪一行。
    //
    // 以前这里写的是「currentItem() 为空就直接 return」,两个问题:
    //   1) 用户的心理模型就是「我右键的是这一行」。没先左键点一下时,
    //      菜单要么不弹、要么作用于上一次选中的那一行 —— 两种都很费解;
    //   2) 列表每秒刷新一次(updateList 里 sortItems 会把行取出来重排),
    //      选中项未必还在用户以为的那一行上。
    // 先按 pos 找到光标下那一行并设为当前行,后面的 runAction()/selectedProcess()
    // 一律读当前行,整条链路就只有一个真相。
    QTreeWidgetItem *item = source->itemAt(pos);
    if (!item)
        return;                     // 点在空白处:不弹菜单
    if (source->currentItem() != item)
        source->setCurrentItem(item);

    QMenu menu(this);
    menu.addAction(QStringLiteral("结束进程"), this, [this]() { runAction(ActClose); });
    menu.addAction(QStringLiteral("强制结束"), this, [this]() { runAction(ActTerminate); });
    menu.addAction(QStringLiteral("重启进程"), this, [this]() { runAction(ActRestart); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("挂起"), this, [this]() { runAction(ActSuspend); });
    menu.addAction(QStringLiteral("恢复"), this, [this]() { runAction(ActResume); });
    menu.addSeparator();

    QMenu *priorityMenu = menu.addMenu(QStringLiteral("设置优先级"));
    const QStringList names = ProcessController::priorityNames();
    for (int i = 0; i < names.size(); ++i) {
        priorityMenu->addAction(names.at(i), this, [this, i]() {
            ProcessInfo info;
            if (!selectedProcess(&info))
                return;
            const auto result = ProcessController::setPriority(info.pid, ProcessController::priorityValueAt(i));
            if (!result.ok)
                QMessageBox::warning(this, QStringLiteral("操作失败"), result.message);
            else
                statusBarMessage(result.message);
        });
    }

    menu.addAction(QStringLiteral("设置 CPU 亲和性"), this, [this]() { runAction(ActAffinity); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("打开文件所在位置"), this, [this]() { runAction(ActReveal); });
    menu.addAction(QStringLiteral("复制 PID"), this, [this]() {
        QApplication::clipboard()->setText(QString::number(currentProcess().pid));
    });
    menu.addAction(QStringLiteral("复制路径"), this, [this]() {
        const ProcessInfo info = currentProcess();
        if (!info.path.isEmpty())
            QApplication::clipboard()->setText(info.path);
    });

    menu.exec(source->viewport()->mapToGlobal(pos));
}

void ProcessPage::chooseAffinity()
{
    ProcessInfo info;
    if (!selectedProcess(&info))
        return;

    quint64 mask = 0;
    if (!ProcessController::queryAffinity(info.pid, &mask)) {
        QMessageBox::warning(this, QStringLiteral("无法读取"),
                             QStringLiteral("读取该进程的 CPU 亲和性失败,可能需要管理员权限。"));
        return;
    }

    const int cores = int(sizeof(quint64) * 8);
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("CPU 亲和性 — %1 (PID %2)").arg(info.name).arg(info.pid));

    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(QStringLiteral("勾选允许该进程运行的逻辑处理器。取消全部勾选会失败,至少保留一个。"),
                            &dialog);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("CardHint"));
    layout->addWidget(hint);

    QVector<QCheckBox *> boxes;
    auto *grid = new QWidget(&dialog);
    auto *flow = new FlowLayout(grid, 0, 10, 8);
    for (int i = 0; i < cores; ++i) {
        auto *box = new QCheckBox(QStringLiteral("CPU %1").arg(i), grid);
        box->setChecked((mask >> i) & 1ull);
        boxes.push_back(box);
        flow->addWidget(box);
    }
    layout->addWidget(grid);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;

    quint64 newMask = 0;
    for (int i = 0; i < cores; ++i) {
        if (boxes[i]->isChecked())
            newMask |= (1ull << i);
    }

    const auto result = ProcessController::setAffinity(info.pid, newMask);
    if (!result.ok)
        QMessageBox::warning(this, QStringLiteral("操作失败"), result.message);
    else
        statusBarMessage(result.message);
}

} // namespace ws
