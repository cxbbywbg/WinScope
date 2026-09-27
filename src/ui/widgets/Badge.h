#pragma once

// 状态胶囊:一个带底色的小圆角标签,用来显示「正常 / 偏高 / 高负载 / 已停止」这类状态。

#include <QColor>
#include <QWidget>

namespace ws {

class Badge : public QWidget
{
    Q_OBJECT

public:
    explicit Badge(QWidget *parent = nullptr);

    void setState(const QString &text, const QColor &color);
    void setCompact(bool compact);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_text;
    QColor m_color;
    bool m_compact = false;
};

} // namespace ws
