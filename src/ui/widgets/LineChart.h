#pragma once

// 实时折线图。
//
// 自己画而不是用 QtCharts,原因有三:
//   1. 少一个模块依赖,打包时不用多带 Qt6Charts.dll
//   2. 深色主题下的网格/渐变/图例可以完全控制
//   3. 每帧只画几十个点,自己画反而更快
//
// 数据用环形缓冲:append 到容量上限后自动挤掉最旧的点。

#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

namespace ws {

class LineChart : public QWidget
{
    Q_OBJECT

public:
    explicit LineChart(QWidget *parent = nullptr);

    int addSeries(const QString &name, const QColor &color);
    void clearSeries();
    void clearData();

    void append(int seriesIndex, double value);
    void appendToAll(double value);

    void setCapacity(int points);
    int capacity() const { return m_capacity; }

    // 固定量程;传 lo >= hi 表示按数据自动缩放
    void setRange(double lo, double hi);
    void setAutoRange(bool enabled);

    // 右侧刻度的文本格式化,默认按数值直接显示
    void setFormatter(std::function<QString(double)> formatter);

    void setShowLegend(bool show);
    void setShowGrid(bool show);
    void setFillArea(bool fill);
    void setGridLines(int count);
    void setLineWidth(qreal width);

    // 当前纵轴上限(自动量程时是数据的峰值)。旁边放 BarRow 时用它对齐刻度,
    // 免得曲线和横条各说各话。
    double upperBound() const { return currentUpperBound(); }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct Series {
        QString name;
        QColor color;
        QVector<double> points;
    };

    double currentUpperBound() const;

    QVector<Series> m_series;
    int m_capacity = 60;
    double m_rangeLo = 0.0;
    double m_rangeHi = -1.0;   // lo >= hi 表示自动
    bool m_autoRange = true;
    std::function<QString(double)> m_formatter;
    bool m_showLegend = true;
    bool m_showGrid = true;
    bool m_fill = true;
    int m_gridLines = 4;
    qreal m_lineWidth = 1.8;
};

} // namespace ws
