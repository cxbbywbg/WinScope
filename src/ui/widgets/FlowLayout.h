#pragma once

// 自动换行布局。
// 用途:每核心占用的小方块、工具箱里的按钮墙 —— 这些控件数量不定,
// 用 QGridLayout 得自己算列数,用这个只要往里加就行。

#include <QLayout>
#include <QList>
#include <QRect>
#include <QStyle>

namespace ws {

class FlowLayout : public QLayout
{
    Q_OBJECT

public:
    explicit FlowLayout(QWidget *parent = nullptr, int margin = 0, int hSpacing = 8, int vSpacing = 8);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int horizontalSpacing() const;
    int verticalSpacing() const;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect &rect) override;
    QSize sizeHint() const override;
    QLayoutItem *takeAt(int index) override;

private:
    int doLayout(const QRect &rect, bool testOnly) const;
    int smartSpacing(QStyle::PixelMetric pm) const;

    QList<QLayoutItem *> m_items;
    int m_hSpace = -1;
    int m_vSpace = -1;
};

} // namespace ws
