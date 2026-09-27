#include "core/WmiQuery.h"

#include "core/Win32Utils.h"

#define _WIN32_DCOM
#include <windows.h>

#include <objbase.h>
#include <oleauto.h>
#include <wbemidl.h>

namespace ws {

namespace {

QString variantToString(VARIANT &v)
{
    switch (v.vt) {
    case VT_BSTR:
        return v.bstrVal ? fromWide(v.bstrVal) : QString();
    case VT_I1:
        return QString::number(v.cVal);
    case VT_I2:
        return QString::number(v.iVal);
    case VT_I4:
    case VT_INT:
        return QString::number(v.lVal);
    case VT_UI1:
        return QString::number(v.bVal);
    case VT_UI2:
        return QString::number(v.uiVal);
    case VT_UI4:
    case VT_UINT:
        return QString::number(v.ulVal);
    case VT_I8:
        return QString::number(v.llVal);
    case VT_UI8:
        return QString::number(v.ullVal);
    case VT_R4:
        return QString::number(double(v.fltVal), 'f', 2);
    case VT_R8:
        return QString::number(v.dblVal, 'f', 2);
    case VT_BOOL:
        return v.boolVal == VARIANT_FALSE ? QStringLiteral("False") : QStringLiteral("True");
    case VT_NULL:
    case VT_EMPTY:
        return QString();
    case VT_DATE: {
        SYSTEMTIME st{};
        if (VariantTimeToSystemTime(v.date, &st)) {
            return QStringLiteral("%1-%2-%3")
                .arg(st.wYear, 4, 10, QLatin1Char('0'))
                .arg(st.wMonth, 2, 10, QLatin1Char('0'))
                .arg(st.wDay, 2, 10, QLatin1Char('0'));
        }
        return QString();
    }
    case VT_ARRAY | VT_BSTR: {
        // 字符串数组拼接
        QStringList parts;
        SAFEARRAY *sa = v.parray;
        if (sa) {
            LONG lo = 0;
            LONG hi = -1;
            SafeArrayGetLBound(sa, 1, &lo);
            SafeArrayGetUBound(sa, 1, &hi);
            for (LONG i = lo; i <= hi; ++i) {
                BSTR b = nullptr;
                if (SUCCEEDED(SafeArrayGetElement(sa, &i, &b)) && b) {
                    parts << fromWide(b);
                    SysFreeString(b);
                }
            }
        }
        return parts.join(QStringLiteral(", "));
    }
    case VT_ARRAY | VT_I4:
    case VT_ARRAY | VT_UI4: {
        QStringList parts;
        SAFEARRAY *sa = v.parray;
        if (sa) {
            LONG lo = 0;
            LONG hi = -1;
            SafeArrayGetLBound(sa, 1, &lo);
            SafeArrayGetUBound(sa, 1, &hi);
            for (LONG i = lo; i <= hi; ++i) {
                LONG val = 0;
                if (SUCCEEDED(SafeArrayGetElement(sa, &i, &val)))
                    parts << QString::number(val);
            }
        }
        return parts.join(QStringLiteral(", "));
    }
    default:
        break;
    }
    return QString();
}

} // namespace

WmiQuery::WmiQuery()
{
    // 采样线程里 COM 未必初始化过。RPC_E_CHANGED_MODE 说明别人已用别的模型初始化,可直接用
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_comReady = SUCCEEDED(hr);
    m_comOwner = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE)
        m_comReady = true;
}

WmiQuery::~WmiQuery()
{
    if (m_services) {
        static_cast<IWbemServices *>(m_services)->Release();
        m_services = nullptr;
    }
    if (m_locator) {
        static_cast<IWbemLocator *>(m_locator)->Release();
        m_locator = nullptr;
    }
    if (m_comOwner)
        CoUninitialize();
}

bool WmiQuery::connect(const QString &ns)
{
    if (m_services)
        return true;
    if (!m_comReady)
        return false;

    // 进程内第一次调用时设置安全级别;返回 RPC_E_TOO_LATE 表示已被设置过,不算错
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         nullptr, EOAC_NONE, nullptr);

    IWbemLocator *locator = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                reinterpret_cast<void **>(&locator)))
        || !locator) {
        return false;
    }
    m_locator = locator;

    IWbemServices *services = nullptr;
    const std::wstring wns = toWide(ns);
    const HRESULT hr = locator->ConnectServer(BSTR(wns.c_str()), nullptr, nullptr, nullptr, 0, nullptr, nullptr,
                                              &services);
    if (FAILED(hr) || !services) {
        locator->Release();
        m_locator = nullptr;
        return false;
    }

    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    m_services = services;
    return true;
}

QVector<QHash<QString, QString>> WmiQuery::query(const QString &wql, const QStringList &properties)
{
    QVector<QHash<QString, QString>> rows;
    if (!connect())
        return rows;

    auto *services = static_cast<IWbemServices *>(m_services);
    IEnumWbemClassObject *enumerator = nullptr;
    const std::wstring wqlW = toWide(wql);
    const HRESULT hr = services->ExecQuery(BSTR(L"WQL"), BSTR(wqlW.c_str()),
                                           WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
                                           &enumerator);
    if (FAILED(hr) || !enumerator)
        return rows;

    while (true) {
        IWbemClassObject *obj = nullptr;
        ULONG returned = 0;
        const HRESULT next = enumerator->Next(WBEM_INFINITE, 1, &obj, &returned);
        if (FAILED(next) || returned == 0 || !obj) {
            if (obj)
                obj->Release();
            break;
        }

        QHash<QString, QString> row;
        for (const QString &prop : properties) {
            VARIANT v;
            VariantInit(&v);
            const std::wstring propW = toWide(prop);
            if (SUCCEEDED(obj->Get(BSTR(propW.c_str()), 0, &v, nullptr, nullptr)))
                row.insert(prop, variantToString(v));
            VariantClear(&v);
        }
        rows.push_back(row);
        obj->Release();
    }

    enumerator->Release();
    return rows;
}

QString WmiQuery::scalar(const QString &wql, const QString &property)
{
    const auto rows = query(wql, { property });
    if (rows.isEmpty())
        return QString();
    return rows.first().value(property);
}

QString wmiOsCaption()
{
    WmiQuery wmi;
    return wmi.scalar(QStringLiteral("SELECT Caption FROM Win32_OperatingSystem"), QStringLiteral("Caption"));
}

QString wmiThermalZoneCelsiusText()
{
    // 多数台式机主板不实现 ACPI 热区,这里失败是常态,调用方显示"—"即可
    WmiQuery wmi;
    WmiQuery rootWmi;
    if (!rootWmi.connect(QStringLiteral("ROOT\\WMI")))
        return QString();
    const auto rows = rootWmi.query(QStringLiteral("SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature"),
                                    { QStringLiteral("CurrentTemperature") });
    if (rows.isEmpty())
        return QString();
    bool ok = false;
    // 单位是开尔文的十分之一
    const double deciKelvin = rows.first().value(QStringLiteral("CurrentTemperature")).toDouble(&ok);
    if (!ok)
        return QString();
    const double celsius = deciKelvin / 10.0 - 273.15;
    if (celsius < -50.0 || celsius > 200.0)
        return QString();
    return QString::number(celsius, 'f', 1);
}

} // namespace ws
