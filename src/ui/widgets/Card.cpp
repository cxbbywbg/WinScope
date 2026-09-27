#include "ui/widgets/Card.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace ws {

Card::Card(const QString &title, QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("Card"));

    m_root = new QVBoxLayout(this);
    m_root->setContentsMargins(16, 13, 16, 15);
    m_root->setSpacing(11);

    m_headerRow = new QHBoxLayout();
    m_headerRow->setContentsMargins(0, 0, 0, 0);
    m_headerRow->setSpacing(8);

    m_title = new QLabel(title, this);
    m_title->setObjectName(QStringLiteral("CardTitle"));
    m_title->setVisible(!title.isEmpty());
    m_headerRow->addWidget(m_title);

    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("CardHint"));
    m_hint->setVisible(false);
    m_headerRow->addWidget(m_hint);

    m_headerRow->addStretch(1);
    m_root->addLayout(m_headerRow);

    m_body = new QVBoxLayout();
    m_body->setContentsMargins(0, 0, 0, 0);
    m_body->setSpacing(8);
    m_root->addLayout(m_body, 1);
}

void Card::setTitle(const QString &title)
{
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty());
}

void Card::setHint(const QString &hint)
{
    m_hint->setText(hint);
    m_hint->setVisible(!hint.isEmpty());
}

void Card::addHeaderWidget(QWidget *widget)
{
    m_headerRow->addWidget(widget);
}

QHBoxLayout *Card::bodyHorizontal()
{
    if (m_bodyRow)
        return m_bodyRow;

    m_bodyRow = new QHBoxLayout();
    m_bodyRow->setContentsMargins(0, 0, 0, 0);
    m_bodyRow->setSpacing(16);
    m_body->addLayout(m_bodyRow, 1);
    return m_bodyRow;
}

void Card::setBodyMargins(int left, int top, int right, int bottom)
{
    m_body->setContentsMargins(left, top, right, bottom);
}

void Card::setBodySpacing(int spacing)
{
    m_body->setSpacing(spacing);
}

} // namespace ws
