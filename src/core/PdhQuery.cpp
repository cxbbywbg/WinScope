#include "core/PdhQuery.h"

#include "core/Win32Utils.h"

#include <QVector>

#include <cstdlib>

namespace ws {

PdhQuery::~PdhQuery()
{
    close();
}

bool PdhQuery::open()
{
    if (m_query)
        return true;
    return PdhOpenQueryW(nullptr, 0, &m_query) == ERROR_SUCCESS;
}

void PdhQuery::close()
{
    if (m_query) {
        PdhCloseQuery(m_query);
        m_query = nullptr;
    }
    m_counters.clear();
}

int PdhQuery::add(const QString &counterPath)
{
    if (!m_query && !open())
        return -1;

    const std::wstring path = toWide(counterPath);
    PDH_HCOUNTER counter = nullptr;

    // 优先走英文名接口(中文系统上本地化名字会挂不上)
    PDH_STATUS st = PdhAddEnglishCounterW(m_query, path.c_str(), 0, &counter);
    if (st != ERROR_SUCCESS)
        st = PdhAddCounterW(m_query, path.c_str(), 0, &counter);
    if (st != ERROR_SUCCESS)
        return -1;

    m_counters.push_back(counter);
    return int(m_counters.size()) - 1;
}

bool PdhQuery::collect()
{
    if (!m_query)
        return false;
    return PdhCollectQueryData(m_query) == ERROR_SUCCESS;
}

void *PdhQuery::counterHandle(int index) const
{
    if (index < 0 || index >= m_counters.size())
        return nullptr;
    return m_counters.at(index);
}

double PdhQuery::single(int index, bool *ok) const
{
    if (ok)
        *ok = false;
    auto counter = static_cast<PDH_HCOUNTER>(counterHandle(index));
    if (!counter)
        return 0.0;

    PDH_FMT_COUNTERVALUE value{};
    if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &value) != ERROR_SUCCESS)
        return 0.0;
    if (value.CStatus != ERROR_SUCCESS && value.CStatus != PDH_CSTATUS_VALID_DATA
        && value.CStatus != PDH_CSTATUS_NEW_DATA)
        return 0.0;

    if (ok)
        *ok = true;
    return value.doubleValue;
}

namespace {

// PDH 的数组型取值要调两次:先问大小,再自己 malloc 缓冲
bool fetchArray(PDH_HCOUNTER counter, DWORD *itemCount, PDH_FMT_COUNTERVALUE_ITEM_W **items)
{
    DWORD bufSize = 0;
    DWORD count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bufSize, &count, nullptr);
    if (st != PDH_MORE_DATA || bufSize == 0)
        return false;

    auto *buf = static_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(std::malloc(bufSize));
    if (!buf)
        return false;

    st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bufSize, &count, buf);
    if (st != ERROR_SUCCESS) {
        std::free(buf);
        return false;
    }

    *itemCount = count;
    *items = buf;
    return true;
}

} // namespace

QHash<QString, double> PdhQuery::instances(int index) const
{
    QHash<QString, double> out;
    auto counter = static_cast<PDH_HCOUNTER>(counterHandle(index));
    if (!counter)
        return out;

    DWORD count = 0;
    PDH_FMT_COUNTERVALUE_ITEM_W *items = nullptr;
    if (!fetchArray(counter, &count, &items))
        return out;

    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus != ERROR_SUCCESS)
            continue;
        out.insert(fromWide(items[i].szName), items[i].FmtValue.doubleValue);
    }
    std::free(items);
    return out;
}

QStringList PdhQuery::instanceNames(int index) const
{
    QStringList names;
    auto counter = static_cast<PDH_HCOUNTER>(counterHandle(index));
    if (!counter)
        return names;

    DWORD count = 0;
    PDH_FMT_COUNTERVALUE_ITEM_W *items = nullptr;
    if (!fetchArray(counter, &count, &items))
        return names;

    names.reserve(int(count));
    for (DWORD i = 0; i < count; ++i)
        names << fromWide(items[i].szName);
    std::free(items);
    return names;
}

} // namespace ws
