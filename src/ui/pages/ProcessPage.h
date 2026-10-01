#pragma once

// 进程管理页。
//
// 列表用「增量更新」而不是每帧重建:
//   每次快照只对已有行改文本,新增/消失的进程才动节点。
//   否则 250 行 × 每秒重建一次,滚动位置和选中项都会被冲掉。

#include "ui/pages/PageBase.h"

#include <QHash>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

namespace ws {

class Card;
class StatRow;

class ProcessPage : public PageBase
{
    Q_OBJECT

public:
    explicit ProcessPage(QWidget *parent = nullptr);

    QString pageTitle() const override;
    QString pageSubtitle() const override;

    void onProcessSnapshot(const ProcessSnapshot &snapshot) override;
    void onSystemSnapshot(const SystemSnapshot &snapshot) override;
    void onSamplingPaused(bool paused) override;

    // 列表列定义。放在 public 是因为排序用的自定义 item 类要按列号判断
    // 该按数值还是按文本比较
    enum Column {
        ColName = 0,
        ColPid,
        ColCpu,
        ColMemory,
        ColGpu,
        ColVram,
        ColDisk,
        ColNetwork,
        ColUser,
        ColStatus,
        ColumnCount,
    };

private:
    void buildToolbar(QVBoxLayout *root);
    void buildListAndDetails(QVBoxLayout *root);
    void buildDetailsPanel(QVBoxLayout *layout);

    void updateList(const ProcessSnapshot &snapshot);
    void updateTree(const ProcessSnapshot &snapshot);
    void updateRow(QTreeWidgetItem *item, const ProcessInfo &info);
    void sortList();

    bool passesFilter(const ProcessInfo &info) const;

    // 当前视图(列表页签优先,否则进程树页签)里选中的那一行;没选中返回 nullptr
    QTreeWidgetItem *currentRow() const;
    // 取出选中行的进程信息。**别用 pid == 0 判断「没选中」** ——
    // 「系统空闲进程」本身就是 pid 0,是个能正常选中的行。
    bool selectedProcess(ProcessInfo *out) const;
    ProcessInfo currentProcess() const;

    void refreshDetails();
    void runAction(int action);
    void showContextMenu(const QPoint &pos);
    void chooseAffinity();
    // 在工具栏上闪一条临时提示(没有独立状态栏,借计数标签用)
    void statusBarMessage(const QString &text);

    QLineEdit *m_search = nullptr;
    QComboBox *m_priorityBox = nullptr;
    QTreeWidget *m_list = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_countLabel = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_resumeButton = nullptr;

    QHash<quint32, QTreeWidgetItem *> m_items;        // pid -> 列表行
    QVector<ProcessInfo> m_processes;
    QHash<quint32, ProcessInfo> m_byPid;

    int m_sortColumn = ColCpu;
    Qt::SortOrder m_sortOrder = Qt::DescendingOrder;
    bool m_paused = false;

    // 详情面板
    QVector<StatRow *> m_detailRows;
    QLabel *m_detailCommandLabel = nullptr;
    QLabel *m_detailName = nullptr;
    QLabel *m_detailPath = nullptr;
};

} // namespace ws
