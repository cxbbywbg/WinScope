#pragma once

// 系统信息页。
//
// SystemInfo::collect() 要跑一串 WMI 查询,约 160~300ms,因此只在首次显示时采集,
// 之后靠手动刷新。这一点和任务管理器一致。

#include "ui/pages/PageBase.h"

class QLabel;

namespace ws {

class Card;

class SystemPage : public PageBase
{
    Q_OBJECT

public:
    explicit SystemPage(QWidget *parent = nullptr);

    QString pageTitle() const override;
    QString pageSubtitle() const override;

    void onActivated() override;

private:
    void reload();
    // 用一组键值对重新填充卡片内容
    void fillSection(Card *card, const QVector<KeyValue> &rows);
    // 把已采集到的数据拼成纯文本,方便贴到工单里
    void copyAll();

    Card *m_osCard = nullptr;
    Card *m_cpuCard = nullptr;
    Card *m_gpuCard = nullptr;
    Card *m_memoryCard = nullptr;
    Card *m_boardCard = nullptr;
    Card *m_storageCard = nullptr;
    Card *m_networkCard = nullptr;
    QLabel *m_statusLabel = nullptr;

    SystemInfoData m_data;
    bool m_loaded = false;
};

} // namespace ws
