#pragma once

// 一行「名称 —— 数值」。
// 详情面板里几十处这种结构,单独抽出来省得每次手搭两个 QLabel。

#include <QColor>
#include <QWidget>

class QLabel;

namespace ws {

class StatRow : public QWidget
{
    Q_OBJECT

public:
    explicit StatRow(const QString &key, QWidget *parent = nullptr);

    void setKey(const QString &key);
    void setValue(const QString &value, const QColor &color = QColor());
    QString value() const;

    // 让多行左列对齐(传同一宽度)
    void setKeyWidth(int px);

private:
    QLabel *m_key = nullptr;
    QLabel *m_value = nullptr;
};

} // namespace ws
