#include "ui/widgets/Badge.h"

#include "app/Theme.h"

#include <QFontMetrics>
#include <QPainter>

namespace ws {

Badge::Badge(QWidget *parent)
    : QWidget(parent)
    , m_color(theme::ok())
{
    setFont(theme::uiFont(11, true));
}

void Badge::setState(const QString &text, const QColor &color)
{
    m_text = text;
    m_color = color;
    updateGeometry();
    update();
}

void Badge::setCompact(bool compact)
{
    m_compact = compact;
    updateGeometry();
    update();
}

QSize Badge::sizeHint() const
{
    const QFontMetrics fm(font());
    const int h = m_compact ? 18 : 22;
    return QSize(fm.horizontalAdvance(m_text) + (m_compact ? 16 : 22), h);
}

void Badge::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    if (m_text.isEmpty())
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = r.height() / 2.0;

    // 底色用状态色的低透明度版本,字用状态色本身 —— 深色背景上辨识度最好
    p.setPen(Qt::NoPen);
    p.setBrush(theme::withAlpha(m_color, 38));
    p.drawRoundedRect(r, radius, radius);

    p.setPen(theme::withAlpha(m_color, 110));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, radius, radius);

    p.setPen(m_color);
    p.setFont(font());
    p.drawText(rect(), Qt::AlignCenter, m_text);
}

} // namespace ws
