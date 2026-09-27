#include "ui/widgets/RingGauge.h"

#include "app/Theme.h"

#include <QPainter>
#include <QPainterPath>

namespace ws {

namespace {
// 从正下方偏左起,顺时针画 270°,缺口留在正下方
constexpr int kStartAngle = 225 * 16;
constexpr int kSpanAngle = -270 * 16;
} // namespace

RingGauge::RingGauge(QWidget *parent)
    : QWidget(parent)
    , m_accent(theme::accent())
{
    setMinimumSize(96, 96);
}

void RingGauge::setValue(double percent)
{
    m_value = percent;
    update();
}

void RingGauge::setDisplayText(const QString &text)
{
    m_display = text;
    update();
}

void RingGauge::setCaption(const QString &text)
{
    m_caption = text;
    update();
}

void RingGauge::setAccent(const QColor &color)
{
    m_accent = color;
    update();
}

void RingGauge::setAutoColor(bool enabled)
{
    m_autoColor = enabled;
    update();
}

void RingGauge::setThickness(int px)
{
    m_thickness = qMax(4, px);
    update();
}

void RingGauge::setValueFontSize(int px)
{
    m_valueFontSize = qMax(10, px);
    update();
}

QSize RingGauge::sizeHint() const
{
    return QSize(132, 132);
}

QSize RingGauge::minimumSizeHint() const
{
    return QSize(88, 88);
}

void RingGauge::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int side = qMin(width(), height());
    const qreal margin = m_thickness / 2.0 + 2.0;
    const QRectF box((width() - side) / 2.0 + margin, (height() - side) / 2.0 + margin, side - margin * 2,
                     side - margin * 2);

    // 轨道
    p.setPen(QPen(theme::border(), m_thickness, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(box, kStartAngle, kSpanAngle);

    const bool hasData = m_value >= 0.0;
    QColor color = m_accent;
    if (hasData && m_autoColor)
        color = theme::forLevel(levelFor(m_value));

    if (hasData) {
        const double ratio = qBound(0.0, m_value, 100.0) / 100.0;
        // 值很小时也画一小段,让用户看得出"有读数"
        const int span = int(kSpanAngle * qMax(ratio, 0.012));

        p.setPen(QPen(color, m_thickness, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(box, kStartAngle, span);
    }

    // ---- 中间文字
    const QString main = hasData ? (m_display.isEmpty() ? QStringLiteral("%1%").arg(m_value, 0, 'f', 0) : m_display)
                                 : QStringLiteral("—");

    QFont valueFont = theme::monoFont(m_valueFontSize, true);
    p.setFont(valueFont);
    p.setPen(hasData ? theme::text() : theme::textFaint());

    QRectF textRect = box;
    // 主数值略偏上,给下面的说明留位置
    if (!m_caption.isEmpty())
        textRect.translate(0, -side * 0.045);
    p.drawText(textRect, Qt::AlignCenter, main);

    if (!m_caption.isEmpty()) {
        p.setFont(theme::uiFont(11));
        p.setPen(theme::textDim());
        QRectF captionRect = box;
        captionRect.translate(0, side * 0.2);
        p.drawText(captionRect, Qt::AlignCenter, m_caption);
    }
}

} // namespace ws
