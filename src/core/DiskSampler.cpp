#include "core/DiskSampler.h"

#include "core/PdhQuery.h"
#include "core/Win32Utils.h"

#include <QVector>

#include <windows.h>
#include <winioctl.h>

namespace ws {

namespace {

// 分区 -> 物理盘号。拿不到就留空,不影响其它指标
QString physicalDriveFor(const QString &letter)
{
    const QString device = QStringLiteral("\\\\.\\%1").arg(letter);
    HANDLE h = CreateFileW(toWide(device).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return QString();

    // VOLUME_DISK_EXTENTS 尾部带一个可变长数组,缓冲区留够
    BYTE buf[sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 8] = {};
    DWORD returned = 0;
    QString result;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf, sizeof(buf), &returned, nullptr)) {
        auto *ext = reinterpret_cast<VOLUME_DISK_EXTENTS *>(buf);
        if (ext->NumberOfDiskExtents > 0)
            result = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(ext->Extents[0].DiskNumber);
    }
    CloseHandle(h);
    return result;
}

} // namespace

DiskSampler::DiskSampler() = default;

DiskSampler::~DiskSampler()
{
    delete static_cast<PdhQuery *>(m_query);
    m_query = nullptr;
}

void DiskSampler::loadStaticInfo()
{
    auto *pdh = new PdhQuery();
    if (pdh->open()) {
        // _Total 聚合实例在所有语言版本上都叫 _Total
        m_idxActive = pdh->add(QStringLiteral("\\PhysicalDisk(_Total)\\% Disk Time"));
        m_idxRead = pdh->add(QStringLiteral("\\PhysicalDisk(_Total)\\Disk Read Bytes/sec"));
        m_idxWrite = pdh->add(QStringLiteral("\\PhysicalDisk(_Total)\\Disk Write Bytes/sec"));
        m_idxIops = pdh->add(QStringLiteral("\\PhysicalDisk(_Total)\\Disk Transfers/sec"));
        m_idxResponse = pdh->add(QStringLiteral("\\PhysicalDisk(_Total)\\Avg. Disk sec/Transfer"));
        m_idxQueue = pdh->add(QStringLiteral("\\PhysicalDisk(_Total)\\Current Disk Queue Length"));
        pdh->collect();
        m_query = pdh;
    } else {
        delete pdh;
    }

    refreshVolumes();
}

void DiskSampler::refreshVolumes()
{
    m_volumes.clear();

    wchar_t drives[512] = {};
    const DWORD len = GetLogicalDriveStringsW(DWORD(sizeof(drives) / sizeof(drives[0]) - 1), drives);
    if (len == 0)
        return;

    for (const wchar_t *p = drives; *p; p += wcslen(p) + 1) {
        const QString root = fromWide(p);
        const UINT type = GetDriveTypeW(p);

        DiskVolume vol;
        vol.letter = root.left(2);
        vol.isRemovable = (type == DRIVE_REMOVABLE);
        vol.isNetwork = (type == DRIVE_REMOTE);

        // 只统计本地固定盘与可移动盘,网络盘/光驱不计入容量统计
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
            continue;

        wchar_t label[256] = {};
        wchar_t fs[64] = {};
        if (GetVolumeInformationW(p, label, 256, nullptr, nullptr, nullptr, fs, 64)) {
            vol.label = fromWide(label);
            vol.fileSystem = fromWide(fs);
        }

        ULARGE_INTEGER freeForUser{}, total{}, totalFree{};
        if (GetDiskFreeSpaceExW(p, &freeForUser, &total, &totalFree)) {
            vol.totalBytes = total.QuadPart;
            vol.freeBytes = totalFree.QuadPart;
        }

        if (type == DRIVE_FIXED)
            vol.devicePath = physicalDriveFor(vol.letter);

        m_volumes.push_back(vol);
    }
}

DiskInfo DiskSampler::sample()
{
    DiskInfo info;
    info.volumes = m_volumes;

    if (!m_query)
        return info;

    auto *pdh = static_cast<PdhQuery *>(m_query);
    if (!pdh->collect())
        return info;

    info.activePercent = qBound(0.0, pdh->single(m_idxActive), 100.0);
    info.readBytesPerSec = qMax(0.0, pdh->single(m_idxRead));
    info.writeBytesPerSec = qMax(0.0, pdh->single(m_idxWrite));
    info.iops = qMax(0.0, pdh->single(m_idxIops));
    // 计数器单位是秒,换算成毫秒
    info.responseMs = qMax(0.0, pdh->single(m_idxResponse) * 1000.0);
    info.queueLength = qMax(0.0, pdh->single(m_idxQueue));

    return info;
}

} // namespace ws
