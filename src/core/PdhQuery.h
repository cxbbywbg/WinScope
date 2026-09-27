#pragma once

// PDH(Performance Data Helper)的轻量封装。
//
// 只做三件事:开查询、挂计数器、取当前值。
// 一律使用 PdhAddEnglishCounter,因为中文系统上计数器名是本地化的
// ("\Processor Information(_Total)\% Processor Time" 在中文版里叫别的名字),
// 英文名接口可以在任意语言版本上稳定工作。

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <windows.h>
#include <pdh.h>
// PDH_MORE_DATA / PDH_CSTATUS_* 这几个常量在 pdhmsg.h 里,pdh.h 不会自动带进来
#include <pdhmsg.h>

namespace ws {

class PdhQuery
{
public:
    PdhQuery() = default;
    ~PdhQuery();

    PdhQuery(const PdhQuery &) = delete;
    PdhQuery &operator=(const PdhQuery &) = delete;

    bool open();
    void close();
    bool isValid() const { return m_query != nullptr; }

    // 挂一个计数器,返回索引;失败返回 -1
    int add(const QString &counterPath);

    // 采样一次。速率类计数器首次采样返回 0,第二次起才有效
    bool collect();

    // 单实例计数器取值
    double single(int index, bool *ok = nullptr) const;

    // 多实例计数器:实例名 -> 值
    QHash<QString, double> instances(int index) const;
    QStringList instanceNames(int index) const;

private:
    void *counterHandle(int index) const;

    PDH_HQUERY m_query = nullptr;
    QVector<PDH_HCOUNTER> m_counters;
};

} // namespace ws
