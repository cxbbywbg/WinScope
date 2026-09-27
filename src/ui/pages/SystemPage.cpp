#include "ui/pages/SystemPage.h"

#include "app/Theme.h"
#include "core/SystemInfo.h"
#include "ui/widgets/Card.h"
#include "ui/widgets/StatRow.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace ws {

namespace {

// 系统信息页的每个分区都是「卡片 + 若干键值行」
Card *makeSectionCard(const QString &title, QWidget *parent)
{
    auto *card = new Card(title, parent);
    card->setBodySpacing(6);
    return card;
}

} // namespace

SystemPage::SystemPage(QWidget *parent)
    : PageBase(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    // ---------------- 顶部工具条
    auto *toolbar = new QWidget(this);
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(20, 14, 20, 0);
    toolbarLayout->setSpacing(10);

    m_statusLabel = new QLabel(QStringLiteral("尚未加载"), toolbar);
    m_statusLabel->setObjectName(QStringLiteral("PageSubtitle"));
    toolbarLayout->addWidget(m_statusLabel);
    toolbarLayout->addStretch(1);

    auto *copyButton = new QPushButton(QStringLiteral("复制全部信息"), toolbar);
    connect(copyButton, &QPushButton::clicked, this, &SystemPage::copyAll);
    toolbarLayout->addWidget(copyButton);

    auto *refreshButton = new QPushButton(QStringLiteral("重新采集"), toolbar);
    connect(refreshButton, &QPushButton::clicked, this, [this]() {
        m_loaded = false;
        reload();
    });
    toolbarLayout->addWidget(refreshButton);

    outer->addWidget(toolbar);

    // ---------------- 可滚动内容区
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll, 1);

    auto *content = new QWidget(scroll);
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(20, 14, 20, 20);
    root->setSpacing(14);

    // 两列布局,信息密度高一些
    auto *row1 = new QHBoxLayout();
    row1->setSpacing(14);
    m_osCard = makeSectionCard(QStringLiteral("操作系统"), content);
    m_cpuCard = makeSectionCard(QStringLiteral("处理器"), content);
    row1->addWidget(m_osCard, 1);
    row1->addWidget(m_cpuCard, 1);
    root->addLayout(row1);

    auto *row2 = new QHBoxLayout();
    row2->setSpacing(14);
    m_gpuCard = makeSectionCard(QStringLiteral("显卡"), content);
    m_memoryCard = makeSectionCard(QStringLiteral("内存"), content);
    row2->addWidget(m_gpuCard, 1);
    row2->addWidget(m_memoryCard, 1);
    root->addLayout(row2);

    auto *row3 = new QHBoxLayout();
    row3->setSpacing(14);
    m_boardCard = makeSectionCard(QStringLiteral("主板与 BIOS"), content);
    m_networkCard = makeSectionCard(QStringLiteral("网络"), content);
    row3->addWidget(m_boardCard, 1);
    row3->addWidget(m_networkCard, 1);
    root->addLayout(row3);

    m_storageCard = makeSectionCard(QStringLiteral("存储"), content);
    root->addWidget(m_storageCard);

    root->addStretch(1);
    scroll->setWidget(content);

    // 采集一次很慢,先放个占位提示,切到本页时再真正查
    m_statusLabel->setText(QStringLiteral("切换到本页时采集(约 0.2 秒)"));
}

QString SystemPage::pageTitle() const
{
    return QStringLiteral("系统信息");
}

QString SystemPage::pageSubtitle() const
{
    return QStringLiteral("操作系统、CPU、显卡、内存条、主板 BIOS、存储与网络的硬件明细");
}

void SystemPage::onActivated()
{
    if (!m_loaded)
        reload();
}

void SystemPage::fillSection(Card *card, const QVector<KeyValue> &rows)
{
    if (!card)
        return;

    QVBoxLayout *body = card->body();

    // 清掉旧行(只动 body 里的,Card 自己的标题行在 root 布局里,不受影响)
    while (body->count() > 0) {
        QLayoutItem *item = body->takeAt(0);
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    if (rows.isEmpty()) {
        auto *empty = new QLabel(QStringLiteral("未采集到数据"), card);
        empty->setObjectName(QStringLiteral("CardHint"));
        body->addWidget(empty);
        return;
    }

    for (const KeyValue &kv : rows) {
        auto *row = new StatRow(kv.key, card);
        row->setValue(kv.value);
        row->setToolTip(QStringLiteral("%1: %2").arg(kv.key, kv.value));
        body->addWidget(row);
    }
}

void SystemPage::reload()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_data = SystemInfo::collect();
    QApplication::restoreOverrideCursor();
    m_loaded = true;

    fillSection(m_osCard, m_data.os);
    fillSection(m_cpuCard, m_data.cpu);
    fillSection(m_gpuCard, m_data.gpu);
    fillSection(m_memoryCard, m_data.memory);
    fillSection(m_boardCard, m_data.motherboard);
    fillSection(m_storageCard, m_data.storage);
    fillSection(m_networkCard, m_data.network);

    int rowCount = 0;
    for (const QVector<KeyValue> *section :
         { &m_data.os, &m_data.cpu, &m_data.gpu, &m_data.memory, &m_data.motherboard, &m_data.storage,
           &m_data.network }) {
        rowCount += section->size();
    }

    m_statusLabel->setText(QStringLiteral("采集时间 %1 · %2 项")
                               .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                               .arg(rowCount));
}

void SystemPage::copyAll()
{
    if (!m_loaded) {
        m_statusLabel->setText(QStringLiteral("还没有采集,先点「重新采集」"));
        return;
    }

    struct Section {
        const char *title;
        const QVector<KeyValue> *rows;
    };
    const Section sections[] = {
        { "操作系统", &m_data.os },        { "处理器", &m_data.cpu },      { "显卡", &m_data.gpu },
        { "内存", &m_data.memory },        { "主板与 BIOS", &m_data.motherboard },
        { "存储", &m_data.storage },       { "网络", &m_data.network },
    };

    QStringList lines;
    for (const Section &section : sections) {
        if (section.rows->isEmpty())
            continue;
        if (!lines.isEmpty())
            lines << QString();
        lines << QStringLiteral("[%1]").arg(QString::fromUtf8(section.title));
        for (const KeyValue &kv : *section.rows)
            lines << QStringLiteral("%1\t%2").arg(kv.key, kv.value);
    }

    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    m_statusLabel->setText(QStringLiteral("已复制 %1 行到剪贴板").arg(lines.size()));
}

} // namespace ws
