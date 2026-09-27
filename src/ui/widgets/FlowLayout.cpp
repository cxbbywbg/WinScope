#include "ui/widgets/FlowLayout.h"

#include <QWidget>

namespace ws {

FlowLayout::FlowLayout(QWidget *parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent)
    , m_hSpace(hSpacing)
    , m_vSpace(vSpacing)
{
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem *item = takeAt(0))
        delete item;
}

void FlowLayout::addItem(QLayoutItem *item)
{
    m_items.append(item);
}

int FlowLayout::horizontalSpacing() const
{
    const int spacing = m_hSpace >= 0 ? m_hSpace : smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
    // 样式拿不到间距时给个稳妥的默认值,别让控件挤在一起
    return spacing >= 0 ? spacing : 8;
}

int FlowLayout::verticalSpacing() const
{
    const int spacing = m_vSpace >= 0 ? m_vSpace : smartSpacing(QStyle::PM_LayoutVerticalSpacing);
    return spacing >= 0 ? spacing : 8;
}

int FlowLayout::count() const
{
    return m_items.size();
}

QLayoutItem *FlowLayout::itemAt(int index) const
{
    return m_items.value(index);
}

QLayoutItem *FlowLayout::takeAt(int index)
{
    if (index >= 0 && index < m_items.size())
        return m_items.takeAt(index);
    return nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const
{
    return {};
}

bool FlowLayout::hasHeightForWidth() const
{
    return true;
}

int FlowLayout::heightForWidth(int width) const
{
    return doLayout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

QSize FlowLayout::sizeHint() const
{
    return minimumSize();
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (const QLayoutItem *item : m_items)
        size = size.expandedTo(item->minimumSize());
    const QMargins margins = contentsMargins();
    size += QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    return size;
}

int FlowLayout::doLayout(const QRect &rect, bool testOnly) const
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    getContentsMargins(&left, &top, &right, &bottom);

    const QRect effective = rect.adjusted(left, top, -right, -bottom);
    int x = effective.x();
    int y = effective.y();
    int lineHeight = 0;

    for (QLayoutItem *item : m_items) {
        const QSize hint = item->sizeHint();
        int nextX = x + hint.width() + horizontalSpacing();
        // 放不下就换行
        if (nextX - horizontalSpacing() > effective.right() && lineHeight > 0) {
            x = effective.x();
            y = y + lineHeight + verticalSpacing();
            nextX = x + hint.width() + horizontalSpacing();
            lineHeight = 0;
        }

        if (!testOnly)
            item->setGeometry(QRect(QPoint(x, y), hint));

        x = nextX;
        lineHeight = qMax(lineHeight, hint.height());
    }

    return y + lineHeight - rect.y() + bottom;
}

int FlowLayout::smartSpacing(QStyle::PixelMetric pm) const
{
    QObject *parent = this->parent();
    if (!parent)
        return -1;
    if (parent->isWidgetType()) {
        auto *widget = static_cast<QWidget *>(parent);
        return widget->style()->pixelMetric(pm, nullptr, widget);
    }
    return static_cast<QLayout *>(parent)->spacing();
}

} // namespace ws
