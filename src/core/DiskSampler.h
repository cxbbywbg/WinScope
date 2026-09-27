#pragma once

// 磁盘采样:整机读写速率/活动时间/IOPS/响应时间 + 各分区容量。
// 速率类指标全部来自 PDH 的 PhysicalDisk 计数器。

#include "core/Types.h"

namespace ws {

class DiskSampler
{
public:
    DiskSampler();
    ~DiskSampler();

    void loadStaticInfo();   // 挂计数器 + 枚举分区(分区只在需要时重扫)
    void refreshVolumes();

    DiskInfo sample();

    const QVector<DiskVolume> &volumes() const { return m_volumes; }

private:
    void *m_query = nullptr;   // PdhQuery*
    int m_idxActive = -1;
    int m_idxRead = -1;
    int m_idxWrite = -1;
    int m_idxIops = -1;
    int m_idxResponse = -1;
    int m_idxQueue = -1;

    QVector<DiskVolume> m_volumes;
};

} // namespace ws
