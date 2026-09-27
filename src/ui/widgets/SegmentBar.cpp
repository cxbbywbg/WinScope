#include "ui/widgets/SegmentBar.h"

#include "app/Theme.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

namespace ws {

namespace {
constexpr int kDotSize = 9;
constexpr int kDotGap = 6;      // 色块与名称之间
constexpr int kNameGap = 8;     // 名称与数值之间
constexpr int kChipGap = 20;    // 图例项之间
constexpr int kLegendRow = 16;  // 图例每行高度
constexpr int kLegendTop = 8;   // 横条与图例之间的间距
} // namespace

SegmentBar::SegmentBar(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void SegmentBar::setSegments(const QVector<Segment> &segments)
{
    m_segments = segments;
    updateGeometry();
    update();
}

void SegmentBar::clear()
{
    m_segments.clear();
    updateGeometry();
    update();
}

void SegmentBar::setFormatter(std::function<QString(double)> formatter)
{
    m_formatter = std::move(formatter);
    updateGeometry();
    update();
}

void SegmentBar::setBarHeight(int px)
{
    m_barHeight = qBound(4, px, 24);
    updateGeometry();
    update();
}

void SegmentBar::setShowLegend(bool show)
{
    m_showLegend = show;
    updateGeometry();
    update();
}

void SegmentBar::setShowValues(bool show)
{
    m_showValues = show;
    updateGeometry();
    update();
}

void SegmentBar::setShowSeparators(bool show)
{
    m_showSeparators = show;
    update();
}

QString SegmentBar::formatValue(double value) const
{
    if (m_formatter)
        return m_formatter(value);
    return QString::number(value, 'f', value < 10.0 ? 1 : 0);
}

bool SegmentBar::hasHeightForWidth() const
{
    return m_showLegend && !m_segments.isEmpty();
}

int SegmentBar::heightForWidth(int w) const
{
    if (!m_showLegend || m_segments.isEmpty())
        return m_barHeight;
    return m_barHeight + kLegendTop + legendRows(w) * kLegendRow;
}

QSize SegmentBar::sizeHint() const
{
    const int legendH = (!m_showLegend || m_segments.isEmpty()) ? 0 : kLegendTop + kLegendRow;
    return QSize(280, m_barHeight + legendH);
}

QSize SegmentBar::minimumSizeHint() const
{
    return QSize(120, m_barHeight);
}

// 图例项按贪心换行排,返回行数
int SegmentBar::legendRows(int width) const
{
    if (m_segments.isEmpty() || width <= 0)
        return 0;

    const QFontMetrics nameFm(theme::uiFont(11));
    const QFontMetrics valueFm(theme::monoFont(11));

    int rows = 1;
    int x = 0;
    for (const Segment &s : m_segments) {
        int chipW = kDotSize + kDotGap + nameFm.horizontalAdvance(s.name);
        if (m_showValues)
            chipW += kNameGap + valueFm.horizontalAdvance(formatValue(s.value));
        if (x > 0 && x + chipW > width) {
            ++rows;
            x = 0;
        }
        x += chipW + kChipGap;
    }
    return rows;
}

QVector<SegmentBar::Chip> SegmentBar::layoutLegend(int width, int top) const
{
    QVector<Chip> chips;
    if (m_segments.isEmpty() || width <= 0)
        return chips;

    const QFontMetrics nameFm(theme::uiFont(11));
    const QFontMetrics valueFm(theme::monoFont(11));

    int x = 0;
    int row = 0;
    for (int i = 0; i < m_segments.size(); ++i) {
        const Segment &s = m_segments.at(i);
        int chipW = kDotSize + kDotGap + nameFm.horizontalAdvance(s.name);
        if (m_showValues)
            chipW += kNameGap + valueFm.horizontalAdvance(formatValue(s.value));

        if (x > 0 && x + chipW > width) {
            ++row;
            x = 0;
        }

        Chip chip;
        chip.index = i;
        chip.rect = QRect(x, top + row * kLegendRow, chipW, kLegendRow);
        chips.push_back(chip);
        x += chipW + kChipGap;
    }
    return chips;
}

void SegmentBar::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);

    const QRectF bar(0, 0, width(), m_barHeight);
    const qreal radius = m_barHeight / 2.0;

    // 底槽:任何分段都画不满时,剩下的部分看起来才不像"缺了一块"
    p.setBrush(theme::withAlpha(theme::borderLight(), 110));
    p.drawRoundedRect(bar, radius, radius);

    double total = 0.0;
    for (const Segment &s : m_segments)
        total += qMax(0.0, s.value);

    if (total > 0.0) {
        p.save();
        QPainterPath clip;
        clip.addRoundedRect(bar, radius, radius);
        p.setClipPath(clip);

        qreal x = bar.left();
        for (int i = 0; i < m_segments.size(); ++i) {
            const Segment &s = m_segments.at(i);
            const double v = qMax(0.0, s.value);
            if (v <= 0.0)
                continue;
            // 最后一段直接吃到右边界,避免逐段取整留下一条缝
            const qreal w = (i == m_segments.size() - 1) ? (bar.right() - x) : bar.width() * v / total;
            const QRectF piece(x, bar.top(), w, bar.height());
            p.setBrush(s.color.isValid() ? s.color : theme::accent());
            p.drawRect(piece);
            x += w;
        }

        if (m_showSeparators) {
            p.setPen(QPen(theme::withAlpha(theme::background(), 180), 1));
            x = bar.left();
            for (int i = 0; i < m_segments.size() - 1; ++i) {
                const double v = qMax(0.0, m_segments.at(i).value);
                if (v <= 0.0)
                    continue;
                x += bar.width() * v / total;
                if (x > bar.left() + 1 && x < bar.right() - 1)
                    p.drawLine(QPointF(x, bar.top()), QPointF(x, bar.bottom()));
            }
        }
        p.restore();
    }

    if (!m_showLegend || m_segments.isEmpty())
        return;

    // ---- 图例
    const auto chips = layoutLegend(width(), int(m_barHeight) + kLegendTop);
    for (const Chip &chip : chips) {
        const Segment &s = m_segments.at(chip.index);
        const QColor color = s.color.isValid() ? s.color : theme::accent();

        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(chip.rect.left(), chip.rect.center().y() - kDotSize / 2.0, kDotSize, kDotSize),
                          2.5, 2.5);

        const int nameX = chip.rect.left() + kDotSize + kDotGap;
        p.setFont(theme::uiFont(11));
        p.setPen(theme::textDim());
        const int nameW = p.fontMetrics().horizontalAdvance(s.name);
        p.drawText(QRect(nameX, chip.rect.top(), nameW, chip.rect.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, s.name);

        if (m_showValues) {
            p.setFont(theme::monoFont(11));
            p.setPen(theme::text());
            const int valueX = nameX + nameW + kNameGap;
            p.drawText(QRect(valueX, chip.rect.top(), chip.rect.right() - valueX + 1, chip.rect.height()),
                       Qt::AlignLeft | Qt::AlignVCenter, formatValue(s.value));
        }
    }
}

} // namespace ws
