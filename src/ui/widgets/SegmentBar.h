#pragma once

// 构成条:一条按比例切开的横条 + 下面自动换行的图例。
//
// 内存的「已使用 / 缓存 / 可用」、CPU 时间的「用户 / 内核 / 空闲」这类
// 构成关系,用几个并排的数字说不清楚,一条堆叠横条一眼就能看出比例。

#include <QColor>
#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

namespace ws {

struct Segment {
    QString name;
    double value = 0.0;
    QColor color;
};

class SegmentBar : public QWidget
{
    Q_OBJECT

public:
    explicit SegmentBar(QWidget *parent = nullptr);

    void setSegments(const QVector<Segment> &segments);
    const QVector<Segment> &segments() const { return m_segments; }
    void clear();

    // 图例里的数值怎么显示(字节 / 百分比 / 计数),默认按数值直接显示
    void setFormatter(std::function<QString(double)> formatter);

    void setBarHeight(int px);
    void setShowLegend(bool show);
    void setShowValues(bool show);
    // 分段之间画不画 1px 分隔线
    void setShowSeparators(bool show);

    bool hasHeightForWidth() const override;
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct Chip {
        QRect rect;
        int index = 0;
    };

    int legendRows(int width) const;
    QVector<Chip> layoutLegend(int width, int top) const;
    QString formatValue(double value) const;

    QVector<Segment> m_segments;
    std::function<QString(double)> m_formatter;
    int m_barHeight = 10;
    bool m_showLegend = true;
    bool m_showValues = true;
    bool m_showSeparators = true;
};

} // namespace ws
