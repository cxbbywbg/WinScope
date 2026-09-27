#include "ui/widgets/LineChart.h"

#include "app/Theme.h"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

namespace ws {

LineChart::LineChart(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

int LineChart::addSeries(const QString &name, const QColor &color)
{
    Series s;
    s.name = name;
    s.color = color;
    s.points.reserve(m_capacity);
    m_series.push_back(s);
    update();
    return m_series.size() - 1;
}

void LineChart::clearSeries()
{
    m_series.clear();
    update();
}

void LineChart::clearData()
{
    for (auto &s : m_series)
        s.points.clear();
    update();
}

void LineChart::append(int seriesIndex, double value)
{
    if (seriesIndex < 0 || seriesIndex >= m_series.size())
        return;

    auto &points = m_series[seriesIndex].points;
    points.push_back(value);
    // 超容量就丢掉最旧的,保持定长
    while (points.size() > m_capacity)
        points.removeFirst();
    update();
}

void LineChart::appendToAll(double value)
{
    for (int i = 0; i < m_series.size(); ++i)
        append(i, value);
}

void LineChart::setCapacity(int points)
{
    m_capacity = qMax(2, points);
    for (auto &s : m_series) {
        while (s.points.size() > m_capacity)
            s.points.removeFirst();
    }
    update();
}

void LineChart::setRange(double lo, double hi)
{
    m_rangeLo = lo;
    m_rangeHi = hi;
    m_autoRange = lo >= hi;
    update();
}

void LineChart::setAutoRange(bool enabled)
{
    m_autoRange = enabled;
    update();
}

void LineChart::setFormatter(std::function<QString(double)> formatter)
{
    m_formatter = std::move(formatter);
    update();
}

void LineChart::setShowLegend(bool show)
{
    m_showLegend = show;
    update();
}

void LineChart::setShowGrid(bool show)
{
    m_showGrid = show;
    update();
}

void LineChart::setFillArea(bool fill)
{
    m_fill = fill;
    update();
}

void LineChart::setGridLines(int count)
{
    m_gridLines = qBound(1, count, 10);
    update();
}

void LineChart::setLineWidth(qreal width)
{
    m_lineWidth = qMax(0.5, width);
    update();
}

QSize LineChart::sizeHint() const
{
    return QSize(360, 160);
}

QSize LineChart::minimumSizeHint() const
{
    return QSize(160, 90);
}

double LineChart::currentUpperBound() const
{
    if (!m_autoRange)
        return m_rangeHi;

    double peak = 0.0;
    for (const auto &s : m_series) {
        for (double v : s.points)
            peak = qMax(peak, v);
    }
    if (peak <= 0.0)
        return 1.0;
    // 留 15% 顶部空间,曲线不会贴边
    return peak * 1.15;
}

void LineChart::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int legendHeight = (m_showLegend && !m_series.isEmpty()) ? 18 : 0;
    const int labelWidth = 52;   // 右侧刻度文字
    const QRectF plot(2, legendHeight + 4, qMax(10, width() - labelWidth - 6),
                      qMax(10, height() - legendHeight - 8));

    const double lo = m_autoRange ? 0.0 : m_rangeLo;
    const double hi = currentUpperBound();
    const double span = (hi - lo) > 1e-9 ? (hi - lo) : 1.0;

    // ---- 网格 + 右侧刻度
    if (m_showGrid) {
        p.setPen(QPen(theme::withAlpha(theme::border(), 150), 1));
        for (int i = 0; i <= m_gridLines; ++i) {
            const qreal y = plot.bottom() - plot.height() * i / m_gridLines;
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));

            const double value = lo + span * i / m_gridLines;
            const QString label = m_formatter ? m_formatter(value) : QString::number(value, 'f', value < 10 ? 1 : 0);
            p.setFont(theme::monoFont(10));
            p.setPen(theme::textFaint());
            p.drawText(QRectF(plot.right() + 4, y - 7, labelWidth - 4, 14), Qt::AlignLeft | Qt::AlignVCenter, label);
            p.setPen(QPen(theme::withAlpha(theme::border(), 150), 1));
        }
    }

    // ---- 每条曲线
    for (const auto &s : m_series) {
        const int count = s.points.size();
        if (count < 2)
            continue;

        // x 轴按容量固定步长,数据不满时曲线从右往左长出来,而不是被拉伸
        const qreal step = plot.width() / qreal(qMax(1, m_capacity - 1));
        const qreal startX = plot.right() - step * (count - 1);

        QPainterPath line;
        QVector<QPointF> screenPoints;
        screenPoints.reserve(count);

        for (int i = 0; i < count; ++i) {
            const double v = qBound(lo, s.points[i], hi);
            const qreal x = startX + step * i;
            const qreal y = plot.bottom() - plot.height() * (v - lo) / span;
            screenPoints.push_back(QPointF(x, y));
            if (i == 0)
                line.moveTo(x, y);
            else
                line.lineTo(x, y);
        }

        if (m_fill) {
            QPainterPath area = line;
            area.lineTo(screenPoints.last().x(), plot.bottom());
            area.lineTo(screenPoints.first().x(), plot.bottom());
            area.closeSubpath();

            QLinearGradient gradient(0, plot.top(), 0, plot.bottom());
            gradient.setColorAt(0.0, theme::withAlpha(s.color, 72));
            gradient.setColorAt(1.0, theme::withAlpha(s.color, 0));
            p.setPen(Qt::NoPen);
            p.setBrush(gradient);
            p.drawPath(area);
        }

        p.setPen(QPen(s.color, m_lineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(line);

        // 最新一个点画个小圆点,提示"这是当前值"
        p.setPen(Qt::NoPen);
        p.setBrush(s.color);
        p.drawEllipse(screenPoints.last(), 2.4, 2.4);
    }

    // ---- 图例
    if (legendHeight > 0) {
        p.setFont(theme::uiFont(11));
        const QFontMetrics fm(p.font());
        qreal x = plot.left();
        for (const auto &s : m_series) {
            p.setPen(Qt::NoPen);
            p.setBrush(s.color);
            p.drawEllipse(QPointF(x + 4, legendHeight / 2.0 + 1), 3.5, 3.5);

            p.setPen(theme::textDim());
            const int textWidth = fm.horizontalAdvance(s.name);
            p.drawText(QRectF(x + 12, 0, textWidth + 4, legendHeight), Qt::AlignLeft | Qt::AlignVCenter, s.name);
            x += 12 + textWidth + 16;
            if (x > plot.right() - 60)
                break;
        }
    }
}

} // namespace ws
