#pragma once

// 一行「名称 —— 水平进度条 —— 数值」。
//
// 性能页的明细原来是几十行纯文字,数字之间看不出轻重缓急;
// 加一条横条之后,哪项吃满了、哪项几乎没动,扫一眼就知道。
//
// 整个控件自绘(不像 StatRow 那样拼两个 QLabel),原因是横条要跟
// 左右两列文本严格对齐,交给布局管理器反而更难控。

#include <QColor>
#include <QString>
#include <QWidget>

namespace ws {

class BarRow : public QWidget
{
    Q_OBJECT

public:
    explicit BarRow(const QString &key, QWidget *parent = nullptr);

    void setKey(const QString &key);

    // ratio 取 0..1,超出会被夹住;color 留空则用主题强调色
    void setValue(double ratio, const QString &text, const QColor &color = QColor());
    void setRatio(double ratio);
    void setText(const QString &text);
    void setBarColor(const QColor &color);

    // 多行对齐:左列和右列各传同一个宽度
    void setKeyWidth(int px);
    void setValueWidth(int px);

    // 在条上画 25/50/75 三道淡刻度,便于横向比较各行
    void setShowTicks(bool show);

    // 警戒线(例如 90% 负载),ratio <= 0 表示不画
    void setMarker(double ratio, const QColor &color = QColor());

    void setBarHeight(int px);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_key;
    QString m_text;
    double m_ratio = 0.0;
    QColor m_barColor;
    double m_marker = -1.0;
    QColor m_markerColor;

    int m_keyWidth = 104;
    int m_valueWidth = 88;
    bool m_showTicks = true;
    int m_barHeight = 8;
};

} // namespace ws
