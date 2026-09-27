#include "ui/widgets/BarRow.h"

#include "app/Theme.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

namespace ws {

namespace {
constexpr int kGap = 10;   // 文本与横条之间的间距
}

BarRow::BarRow(const QString &key, QWidget *parent)
    : QWidget(parent)
    , m_key(key)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void BarRow::setKey(const QString &key)
{
    m_key = key;
    update();
}

void BarRow::setValue(double ratio, const QString &text, const QColor &color)
{
    m_ratio = ratio;
    m_text = text;
    m_barColor = color;
    update();
}

void BarRow::setRatio(double ratio)
{
    m_ratio = ratio;
    update();
}

void BarRow::setText(const QString &text)
{
    m_text = text;
    update();
}

void BarRow::setBarColor(const QColor &color)
{
    m_barColor = color;
    update();
}

void BarRow::setKeyWidth(int px)
{
    m_keyWidth = qMax(0, px);
    updateGeometry();
    update();
}

void BarRow::setValueWidth(int px)
{
    m_valueWidth = qMax(0, px);
    updateGeometry();
    update();
}

void BarRow::setShowTicks(bool show)
{
    m_showTicks = show;
    update();
}

void BarRow::setMarker(double ratio, const QColor &color)
{
    m_marker = ratio;
    m_markerColor = color;
    update();
}

void BarRow::setBarHeight(int px)
{
    m_barHeight = qBound(3, px, 20);
    updateGeometry();
    update();
}

QSize BarRow::sizeHint() const
{
    return QSize(m_keyWidth + m_valueWidth + 2 * kGap + 120, qMax(20, m_barHeight + 12));
}

QSize BarRow::minimumSizeHint() const
{
    return QSize(m_keyWidth + m_valueWidth + 2 * kGap + 40, qMax(18, m_barHeight + 8));
}

void BarRow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int h = height();
    const QColor fill = m_barColor.isValid() ? m_barColor : theme::accent();

    // ---- 左列:名称
    p.setFont(theme::uiFont(12));
    p.setPen(theme::textDim());
    p.drawText(QRect(0, 0, m_keyWidth, h), Qt::AlignLeft | Qt::AlignVCenter,
               p.fontMetrics().elidedText(m_key, Qt::ElideRight, m_keyWidth));

    // ---- 右列:数值
    const QRect valueRect(width() - m_valueWidth, 0, m_valueWidth, h);
    p.setFont(theme::monoFont(12));
    p.setPen(fill);
    p.drawText(valueRect, Qt::AlignRight | Qt::AlignVCenter,
               p.fontMetrics().elidedText(m_text, Qt::ElideRight, m_valueWidth));

    // ---- 中间:横条
    const int trackX = m_keyWidth + kGap;
    const int trackW = width() - trackX - m_valueWidth - kGap;
    if (trackW < 8)
        return;

    const qreal radius = m_barHeight / 2.0;
    const QRectF track(trackX, (h - m_barHeight) / 2.0, trackW, m_barHeight);

    p.setPen(Qt::NoPen);
    p.setBrush(theme::withAlpha(theme::borderLight(), 110));
    p.drawRoundedRect(track, radius, radius);

    const double ratio = qBound(0.0, m_ratio, 1.0);
    if (ratio > 0.0) {
        // 极小占比也留一个圆点,免得看着像 0
        const qreal fillW = qMax(qreal(m_barHeight), qreal(trackW) * ratio);
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(track.left(), track.top(), fillW, track.height()), radius, radius);
    }

    // ---- 条上的装饰:刻度与警戒线。都画在条内,不溢出圆角
    p.save();
    QPainterPath clip;
    clip.addRoundedRect(track, radius, radius);
    p.setClipPath(clip);

    if (m_showTicks) {
        p.setPen(QPen(theme::withAlpha(theme::background(), 170), 1));
        for (int i = 1; i <= 3; ++i) {
            const qreal x = track.left() + track.width() * i / 4.0;
            p.drawLine(QPointF(x, track.top()), QPointF(x, track.bottom()));
        }
    }
    if (m_marker > 0.0 && m_marker <= 1.0) {
        const qreal x = track.left() + track.width() * m_marker;
        p.setPen(QPen(m_markerColor.isValid() ? m_markerColor : theme::warn(), 1));
        p.drawLine(QPointF(x, track.top()), QPointF(x, track.bottom()));
    }
    p.restore();
}

} // namespace ws
