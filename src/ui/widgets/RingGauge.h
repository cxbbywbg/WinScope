#pragma once

// 环形仪表:一个 270° 的圆弧进度盘,中间放大字数值。
// 总览页的 CPU / 内存 / GPU / 磁盘 都用它。

#include <QColor>
#include <QString>
#include <QWidget>

namespace ws {

class RingGauge : public QWidget
{
    Q_OBJECT

public:
    explicit RingGauge(QWidget *parent = nullptr);

    // percent < 0 表示无数据,画成灰色空盘
    void setValue(double percent);
    // 中间的大字,不设则显示百分比
    void setDisplayText(const QString &text);
    void setCaption(const QString &text);
    void setAccent(const QColor &color);
    // 打开后按占用率自动取色(绿/橙/红),关闭则固定用 accent
    void setAutoColor(bool enabled);
    void setThickness(int px);
    void setValueFontSize(int px);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    double m_value = -1.0;
    QString m_display;
    QString m_caption;
    QColor m_accent;
    bool m_autoColor = true;
    int m_thickness = 9;
    int m_valueFontSize = 22;
};

} // namespace ws
