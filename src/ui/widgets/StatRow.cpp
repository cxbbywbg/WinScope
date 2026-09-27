#include "ui/widgets/StatRow.h"

#include "app/Theme.h"

#include <QHBoxLayout>
#include <QLabel>

namespace ws {

StatRow::StatRow(const QString &key, QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    m_key = new QLabel(key, this);
    m_key->setObjectName(QStringLiteral("StatKey"));

    m_value = new QLabel(QStringLiteral("—"), this);
    m_value->setObjectName(QStringLiteral("StatValue"));
    m_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_value->setFont(theme::monoFont(12));

    layout->addWidget(m_key);
    layout->addStretch(1);
    layout->addWidget(m_value);
}

void StatRow::setKey(const QString &key)
{
    m_key->setText(key);
}

void StatRow::setValue(const QString &value, const QColor &color)
{
    m_value->setText(value.isEmpty() ? QStringLiteral("—") : value);
    QColor c = color.isValid() ? color : theme::text();
    m_value->setStyleSheet(QStringLiteral("color: %1;").arg(c.name()));
}

QString StatRow::value() const
{
    return m_value->text();
}

void StatRow::setKeyWidth(int px)
{
    m_key->setMinimumWidth(px);
}

} // namespace ws
