#include "core/SystemInfo.h"

#include "core/GpuSampler.h"
#include "core/Win32Utils.h"
#include "core/WmiQuery.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <vector>

#include <windows.h>

namespace ws {

namespace {

QString registryString(HKEY root, const wchar_t *subKey, const wchar_t *name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return QString();

    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    QString result;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
        if (type == REG_SZ || type == REG_EXPAND_SZ)
            result = fromWide(buf);
        else if (type == REG_DWORD && size == sizeof(DWORD))
            result = QString::number(*reinterpret_cast<DWORD *>(buf));
    }
    RegCloseKey(key);
    return result;
}

quint32 registryDword(HKEY root, const wchar_t *subKey, const wchar_t *name, quint32 fallback = 0)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return fallback;

    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LONG rc = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return (rc == ERROR_SUCCESS && type == REG_DWORD) ? value : fallback;
}

QString windowsArchitecture()
{    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        return QStringLiteral("64 位 (x64)");
    case PROCESSOR_ARCHITECTURE_ARM64:
        return QStringLiteral("64 位 (ARM64)");
    case PROCESSOR_ARCHITECTURE_INTEL:
        return QStringLiteral("32 位 (x86)");
    case PROCESSOR_ARCHITECTURE_ARM:
        return QStringLiteral("32 位 (ARM)");
    default:
        return QStringLiteral("未知");
    }
}

QString cpuCacheSummary()
{
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
    if (len == 0)
        return QString();

    std::vector<BYTE> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationCache,
                                          reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buf.data()),
                                          &len)) {
        return QString();
    }

    // 同一级缓存可能有多个实例,按级别聚合去重
    QMap<int, quint32> sizeByLevel;
    DWORD offset = 0;
    while (offset < len) {
        auto *info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buf.data() + offset);
        if (info->Relationship == RelationCache) {
            const int level = info->Cache.Level;
            const quint32 size = info->Cache.CacheSize;
            sizeByLevel[level] = qMax(sizeByLevel.value(level, 0u), size);
        }
        if (info->Size == 0)
            break;
        offset += info->Size;
    }

    QStringList parts;
    for (auto it = sizeByLevel.constBegin(); it != sizeByLevel.constEnd(); ++it)
        parts << QStringLiteral("L%1 %2").arg(it.key()).arg(formatBytes(it.value()));
    return parts.join(QStringLiteral(" / "));
}

QString smbiosMemoryType(const QString &code)
{
    // SMBIOSMemoryType 的取值表,只列常见的
    static const QHash<int, QString> types = {
        { 20, QStringLiteral("DDR") },      { 21, QStringLiteral("DDR2") },
        { 24, QStringLiteral("DDR3") },     { 26, QStringLiteral("DDR4") },
        { 27, QStringLiteral("LPDDR") },    { 28, QStringLiteral("LPDDR2") },
        { 29, QStringLiteral("LPDDR3") },   { 30, QStringLiteral("LPDDR4") },
        { 34, QStringLiteral("DDR5") },     { 35, QStringLiteral("LPDDR5") },
    };
    bool ok = false;
    const int v = code.toInt(&ok);
    if (ok && types.contains(v))
        return types.value(v);
    return QStringLiteral("未知");
}

} // namespace

SystemInfoData SystemInfo::collect()
{
    SystemInfoData data;

    // ------------------------------------------------ 操作系统
    const wchar_t *osKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const QString productName = registryString(HKEY_LOCAL_MACHINE, osKey, L"ProductName");
    const QString displayVersion = registryString(HKEY_LOCAL_MACHINE, osKey, L"DisplayVersion");
    const QString build = registryString(HKEY_LOCAL_MACHINE, osKey, L"CurrentBuild");
    const quint32 ubr = registryDword(HKEY_LOCAL_MACHINE, osKey, L"UBR");
    const quint32 installDate = registryDword(HKEY_LOCAL_MACHINE, osKey, L"InstallDate");
    const QString owner = registryString(HKEY_LOCAL_MACHINE, osKey, L"RegisteredOwner");
    const QString edition = registryString(HKEY_LOCAL_MACHINE, osKey, L"EditionID");

    // Windows 11 的注册表 ProductName 至今仍写着 "Windows 10" —— 只能靠 build 号判断
    QString friendlyName = productName;
    if (build.toInt() >= 22000 && friendlyName.contains(QStringLiteral("Windows 10")))
        friendlyName.replace(QStringLiteral("Windows 10"), QStringLiteral("Windows 11"));

    data.os << KeyValue{ QStringLiteral("系统版本"),
                         displayVersion.isEmpty() ? friendlyName
                                                  : QStringLiteral("%1 %2").arg(friendlyName, displayVersion) };    data.os << KeyValue{ QStringLiteral("内部版本"), QStringLiteral("%1.%2").arg(build).arg(ubr) };
    data.os << KeyValue{ QStringLiteral("版本标识"), edition };
    data.os << KeyValue{ QStringLiteral("系统架构"), windowsArchitecture() };
    data.os << KeyValue{ QStringLiteral("系统目录"), QString::fromLocal8Bit(qgetenv("SystemRoot")) };
    data.os << KeyValue{ QStringLiteral("计算机名"), QString::fromLocal8Bit(qgetenv("COMPUTERNAME")) };
    data.os << KeyValue{ QStringLiteral("注册用户"), owner };
    if (installDate > 0) {
        data.os << KeyValue{ QStringLiteral("安装时间"),
                             QDateTime::fromSecsSinceEpoch(qint64(installDate)).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) };
    }
    data.os << KeyValue{ QStringLiteral("系统运行时间"), formatDuration(qint64(GetTickCount64() / 1000)) };

    // ------------------------------------------------ CPU
    const wchar_t *cpuKey = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    data.cpu << KeyValue{ QStringLiteral("型号"), registryString(HKEY_LOCAL_MACHINE, cpuKey, L"ProcessorNameString").trimmed() };
    data.cpu << KeyValue{ QStringLiteral("标识"), registryString(HKEY_LOCAL_MACHINE, cpuKey, L"Identifier").trimmed() };
    const quint32 baseMhz = registryDword(HKEY_LOCAL_MACHINE, cpuKey, L"~MHz");
    if (baseMhz > 0)
        data.cpu << KeyValue{ QStringLiteral("标称主频"), QStringLiteral("%1 MHz").arg(baseMhz) };

    {
        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        int physical = 0;
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
        if (len > 0) {
            std::vector<BYTE> buf(len);
            if (GetLogicalProcessorInformationEx(
                    RelationProcessorCore,
                    reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buf.data()), &len)) {
                DWORD offset = 0;
                while (offset < len) {
                    auto *info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buf.data() + offset);
                    if (info->Relationship == RelationProcessorCore)
                        ++physical;
                    if (info->Size == 0)
                        break;
                    offset += info->Size;
                }
            }
        }
        data.cpu << KeyValue{ QStringLiteral("物理核心"), QString::number(physical) };
        data.cpu << KeyValue{ QStringLiteral("逻辑处理器"), QString::number(si.dwNumberOfProcessors) };
    }
    data.cpu << KeyValue{ QStringLiteral("缓存"), cpuCacheSummary() };

    // ------------------------------------------------ GPU
    {
        // WMI 的 AdapterRAM 是 32 位字段,超过 4GB 会溢出(独显常见报 0),
        // 显存总量以 DXGI 为准
        const auto dxgi = GpuSampler::adapters();

        WmiQuery wmi;
        const auto rows = wmi.query(
            QStringLiteral("SELECT Name, AdapterRAM, DriverVersion, DriverDate, VideoProcessor, PNPDeviceID "
                           "FROM Win32_VideoController"),
            { QStringLiteral("Name"), QStringLiteral("AdapterRAM"), QStringLiteral("DriverVersion"),
              QStringLiteral("DriverDate"), QStringLiteral("VideoProcessor"), QStringLiteral("PNPDeviceID") });
        for (const auto &row : rows) {
            const QString name = row.value(QStringLiteral("Name"));
            if (name.contains(QStringLiteral("Microsoft Basic"), Qt::CaseInsensitive))
                continue;

            data.gpu << KeyValue{ QStringLiteral("型号"), name };

            // 按型号前缀匹配 DXGI 报出来的显存
            quint64 vramBytes = 0;
            for (const auto &d : dxgi) {
                if (d.software)
                    continue;
                if (name.startsWith(d.name.left(10), Qt::CaseInsensitive)
                    || d.name.startsWith(name.left(10), Qt::CaseInsensitive)) {
                    vramBytes = d.dedicatedVideoMemory;
                    break;
                }
            }
            if (vramBytes > 0) {
                data.gpu << KeyValue{ QStringLiteral("专用显存"), formatBytes(vramBytes) };
            } else {
                const QString vram = row.value(QStringLiteral("AdapterRAM"));
                if (!vram.isEmpty() && vram != QLatin1String("0"))
                    data.gpu << KeyValue{ QStringLiteral("显存(驱动报告,可能不准)"), formatBytes(vram.toULongLong()) };
            }

            data.gpu << KeyValue{ QStringLiteral("驱动版本"), row.value(QStringLiteral("DriverVersion")) };
            data.gpu << KeyValue{ QStringLiteral("驱动日期"), formatWmiDate(row.value(QStringLiteral("DriverDate"))) };
            data.gpu << KeyValue{ QStringLiteral("设备 ID"), row.value(QStringLiteral("PNPDeviceID")) };
        }
    }

    // ------------------------------------------------ 内存
    {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            data.memory << KeyValue{ QStringLiteral("总容量"), formatBytes(ms.ullTotalPhys) };
            data.memory << KeyValue{ QStringLiteral("可用"), formatBytes(ms.ullAvailPhys) };
        }

        WmiQuery wmi;
        const auto rows = wmi.query(
            QStringLiteral("SELECT Capacity, Speed, Manufacturer, PartNumber, DeviceLocator, SMBIOSMemoryType "
                           "FROM Win32_PhysicalMemory"),
            { QStringLiteral("Capacity"), QStringLiteral("Speed"), QStringLiteral("Manufacturer"),
              QStringLiteral("PartNumber"), QStringLiteral("DeviceLocator"), QStringLiteral("SMBIOSMemoryType") });

        data.memory << KeyValue{ QStringLiteral("插槽数量"), QString::number(rows.size()) };
        int slot = 1;
        for (const auto &row : rows) {
            const QString prefix = QStringLiteral("插槽 %1").arg(slot++);
            const QString cap = row.value(QStringLiteral("Capacity"));
            QStringList desc;
            if (!cap.isEmpty())
                desc << formatBytes(cap.toULongLong());
            const QString speed = row.value(QStringLiteral("Speed"));
            if (!speed.isEmpty() && speed != QLatin1String("0"))
                desc << QStringLiteral("%1 MHz").arg(speed);
            const QString type = smbiosMemoryType(row.value(QStringLiteral("SMBIOSMemoryType")));
            if (type != QStringLiteral("未知"))
                desc << type;
            const QString vendor = row.value(QStringLiteral("Manufacturer"));
            if (!vendor.isEmpty())
                desc << vendor;
            const QString part = row.value(QStringLiteral("PartNumber")).trimmed();
            if (!part.isEmpty())
                desc << part;

            data.memory << KeyValue{ prefix, desc.join(QStringLiteral(" · ")) };
        }
    }

    // ------------------------------------------------ 主板 / BIOS
    {
        WmiQuery wmi;
        const auto boards = wmi.query(
            QStringLiteral("SELECT Manufacturer, Product, Version, SerialNumber FROM Win32_BaseBoard"),
            { QStringLiteral("Manufacturer"), QStringLiteral("Product"), QStringLiteral("Version"),
              QStringLiteral("SerialNumber") });
        if (!boards.isEmpty()) {
            const auto &b = boards.first();
            data.motherboard << KeyValue{ QStringLiteral("主板厂商"), b.value(QStringLiteral("Manufacturer")) };
            data.motherboard << KeyValue{ QStringLiteral("主板型号"), b.value(QStringLiteral("Product")) };
            data.motherboard << KeyValue{ QStringLiteral("主板版本"), b.value(QStringLiteral("Version")) };
        }

        const auto bios = wmi.query(
            QStringLiteral("SELECT Manufacturer, SMBIOSBIOSVersion, ReleaseDate, Version FROM Win32_BIOS"),
            { QStringLiteral("Manufacturer"), QStringLiteral("SMBIOSBIOSVersion"), QStringLiteral("ReleaseDate"),
              QStringLiteral("Version") });
        if (!bios.isEmpty()) {
            const auto &b = bios.first();
            data.motherboard << KeyValue{ QStringLiteral("BIOS 厂商"), b.value(QStringLiteral("Manufacturer")) };
            data.motherboard << KeyValue{ QStringLiteral("BIOS 版本"), b.value(QStringLiteral("SMBIOSBIOSVersion")) };
            data.motherboard << KeyValue{ QStringLiteral("BIOS 日期"), formatWmiDate(b.value(QStringLiteral("ReleaseDate"))) };
        }

        const auto sys = wmi.query(QStringLiteral("SELECT Manufacturer, Model FROM Win32_ComputerSystem"),
                                   { QStringLiteral("Manufacturer"), QStringLiteral("Model") });
        if (!sys.isEmpty()) {
            data.motherboard << KeyValue{ QStringLiteral("整机厂商"), sys.first().value(QStringLiteral("Manufacturer")) };
            data.motherboard << KeyValue{ QStringLiteral("整机型号"), sys.first().value(QStringLiteral("Model")) };
        }
    }

    // ------------------------------------------------ 存储
    {
        WmiQuery wmi;
        const auto disks = wmi.query(
            QStringLiteral("SELECT Model, Size, InterfaceType, MediaType, SerialNumber, PNPDeviceID "
                           "FROM Win32_DiskDrive"),
            { QStringLiteral("Model"), QStringLiteral("Size"), QStringLiteral("InterfaceType"),
              QStringLiteral("MediaType"), QStringLiteral("SerialNumber"), QStringLiteral("PNPDeviceID") });
        int index = 1;
        for (const auto &row : disks) {
            const QString size = row.value(QStringLiteral("Size"));
            QStringList desc;
            if (!size.isEmpty())
                desc << formatBytes(size.toULongLong());
            desc << row.value(QStringLiteral("InterfaceType"));
            desc << row.value(QStringLiteral("MediaType"));
            const QString serial = row.value(QStringLiteral("SerialNumber")).trimmed().remove(QRegularExpression(
                QStringLiteral("[\\s.]+$")));
            if (!serial.isEmpty())
                desc << QStringLiteral("SN %1").arg(serial);

            data.storage << KeyValue{ QStringLiteral("磁盘 %1").arg(index++),
                                      QStringLiteral("%1 — %2")
                                          .arg(row.value(QStringLiteral("Model")),
                                               desc.join(QStringLiteral(" · "))) };
        }

        // 分区一览
        wchar_t drives[512] = {};
        if (GetLogicalDriveStringsW(DWORD(sizeof(drives) / sizeof(drives[0]) - 1), drives) > 0) {
            for (const wchar_t *p = drives; *p; p += wcslen(p) + 1) {
                if (GetDriveTypeW(p) != DRIVE_FIXED)
                    continue;
                ULARGE_INTEGER total{}, freeBytes{}, freeForUser{};
                if (!GetDiskFreeSpaceExW(p, &freeForUser, &total, &freeBytes))
                    continue;
                wchar_t label[256] = {};
                GetVolumeInformationW(p, label, 256, nullptr, nullptr, nullptr, nullptr, 0);
                data.storage << KeyValue{ QStringLiteral("分区 %1").arg(fromWide(p).left(2)),
                                          QStringLiteral("%1 可用 / %2 共 %3")
                                              .arg(formatBytes(freeBytes.QuadPart),
                                                   formatBytes(total.QuadPart),
                                                   fromWide(label).isEmpty() ? QStringLiteral("本地磁盘")
                                                                             : fromWide(label)) };
            }
        }
    }

    // ------------------------------------------------ 网络
    {
        WmiQuery wmi;
        const auto rows = wmi.query(
            QStringLiteral("SELECT Description, MACAddress, DefaultIPGateway, DNSServerSearchOrder, IPAddress "
                           "FROM Win32_NetworkAdapterConfiguration WHERE IPEnabled = True"),
            { QStringLiteral("Description"), QStringLiteral("MACAddress"), QStringLiteral("DefaultIPGateway"),
              QStringLiteral("DNSServerSearchOrder"), QStringLiteral("IPAddress") });
        for (const auto &row : rows) {
            const QString desc = row.value(QStringLiteral("Description"));
            QStringList parts;
            const QString ip = row.value(QStringLiteral("IPAddress"));
            if (!ip.isEmpty())
                parts << QStringLiteral("IP %1").arg(ip);
            const QString mac = row.value(QStringLiteral("MACAddress"));
            if (!mac.isEmpty())
                parts << QStringLiteral("MAC %1").arg(mac);
            const QString gw = row.value(QStringLiteral("DefaultIPGateway"));
            if (!gw.isEmpty())
                parts << QStringLiteral("网关 %1").arg(gw);
            const QString dns = row.value(QStringLiteral("DNSServerSearchOrder"));
            if (!dns.isEmpty())
                parts << QStringLiteral("DNS %1").arg(dns);

            data.network << KeyValue{ desc, parts.join(QStringLiteral(" · ")) };
        }
    }

    return data;
}

} // namespace ws
