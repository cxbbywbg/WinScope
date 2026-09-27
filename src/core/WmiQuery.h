#pragma once

// WMI 的极简封装。
//
// WMI 是拿硬件信息最省事的途径(主板/BIOS/内存条/显存/温度),
// 但 COM 样板代码很长,这里统一收口。
// 所有失败路径都返回空结果,调用方不需要 try/catch,拿不到就显示"—"。

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ws {

class WmiQuery
{
public:
    WmiQuery();
    ~WmiQuery();

    WmiQuery(const WmiQuery &) = delete;
    WmiQuery &operator=(const WmiQuery &) = delete;

    // 连接命名空间,默认 ROOT\CIMV2
    bool connect(const QString &ns = QStringLiteral("ROOT\\CIMV2"));
    bool isConnected() const { return m_services != nullptr; }

    // 执行 WQL,按 properties 顺序取列,每行一个哈希
    QVector<QHash<QString, QString>> query(const QString &wql, const QStringList &properties);

    // 只取第一行的某列
    QString scalar(const QString &wql, const QString &property);

    // 当前线程是否已经初始化过 COM(诊断用)
    bool comReady() const { return m_comReady; }

private:
    void *m_locator = nullptr;
    void *m_services = nullptr;
    bool m_comReady = false;
    bool m_comOwner = false;   // 本次是否由我们调用 CoInitializeEx
};

// 常用便捷查询(内部自带连接,失败返回空)
QString wmiOsCaption();
QString wmiThermalZoneCelsiusText();   // 返回空表示不可用

} // namespace ws
