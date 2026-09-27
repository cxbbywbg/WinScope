#pragma once

// 卡片容器:统一标题栏 + 内容区。
// 全项目的面板都用它,保证圆角、边框、内边距一致。

#include <QFrame>

class QHBoxLayout;
class QLabel;
class QVBoxLayout;

namespace ws {

class Card : public QFrame
{
    Q_OBJECT

public:
    explicit Card(const QString &title = QString(), QWidget *parent = nullptr);

    void setTitle(const QString &title);
    void setHint(const QString &hint);
    // 放到标题行右侧(通常是刷新按钮或下拉框)
    void addHeaderWidget(QWidget *widget);

    // 内容区布局,往里塞控件
    QVBoxLayout *body() const { return m_body; }

    // 把内容区换成一个横向布局并返回(常用:左边仪表 + 右边数据)
    QHBoxLayout *bodyHorizontal();

    void setBodyMargins(int left, int top, int right, int bottom);
    void setBodySpacing(int spacing);

private:
    QVBoxLayout *m_root = nullptr;
    QHBoxLayout *m_headerRow = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_hint = nullptr;
    QVBoxLayout *m_body = nullptr;
    QHBoxLayout *m_bodyRow = nullptr;   // bodyHorizontal() 惰性创建
};

} // namespace ws
