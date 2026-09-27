#pragma once

// GPU 采样。
//
// 适配器型号与显存总量走 DXGI;实时占用走 PDH 的 "\GPU Engine(*)\Utilization Percentage",
// 适配器级显存走 "\GPU Adapter Memory(*)",进程级显存走 "\GPU Process Memory(*)"。
//
// 一台机器上可能同时有核显和独显,PDH 的实例名里带 luid,形如
//     pid_1234_luid_0x00000000_0x0000E7F7_phys_0_eng_0_engtype_3D
// 其中 luid 的两段分别对应 LUID 的 HighPart 和 LowPart(已实测确认),拿它和
// DXGI_ADAPTER_DESC1::AdapterLuid 对齐,就能把每个计数器实例归属到具体的适配器上。
//
// 温度/功耗/频率没有通用接口(NVAPI/ADL/NVML 都是厂商私有),温度交给
// ThermalProvider 按型号名匹配着读,读不到就保持 -1。

#include "core/Types.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace ws {

class ThermalProvider;

struct GpuAdapterDesc {
    QString name;
    quint64 dedicatedVideoMemory = 0;
    bool software = false;
    bool integrated = false;   // 专用显存不到 1GB,当成核显
};

class GpuSampler
{
public:
    GpuSampler();
    ~GpuSampler();

    // DXGI 枚举硬件适配器。WMI 的 AdapterRAM 是 32 位字段,超过 4GB 会溢出,
    // 显存总量只能信 DXGI
    static QVector<GpuAdapterDesc> adapters();

    // 载入型号/驱动版本(走 DXGI + WMI,较慢),只需一次
    void loadStaticInfo();

    // 采一轮,返回全部硬件适配器(核显 + 独显各一条)
    QVector<GpuInfo> sampleAll();

    // 主显卡:有独显就取独显(专用显存最大的那块),否则第一块
    const GpuInfo &primary() const { return m_primary; }

    // 温度来源。载入适配器清单时会顺手把型号名交给它,采样时按序号取温度
    void setThermalProvider(ThermalProvider *provider) { m_thermal = provider; }

    // sampleAll() 顺带产出的进程级数据(跨适配器合计),供进程列表使用
    const QHash<quint32, double> &perProcessUsage() const { return m_procUsage; }
    const QHash<quint32, quint64> &perProcessDedicated() const { return m_procDedicated; }
    const QHash<quint32, quint64> &perProcessShared() const { return m_procShared; }

private:
    // 一块硬件适配器的静态信息
    struct Adapter {
        QString name;
        QString driverVersion;
        QString driverDate;
        quint64 dedicatedVideoMemory = 0;
        quint32 luidHigh = 0;
        quint32 luidLow = 0;
        bool integrated = false;
    };

    void *m_query = nullptr;   // PdhQuery*
    int m_idxEngine = -1;
    int m_idxAdapterDedicated = -1;
    int m_idxAdapterShared = -1;
    int m_idxProcDedicated = -1;
    int m_idxProcShared = -1;

    bool m_available = false;
    QVector<Adapter> m_adapters;
    GpuInfo m_primary;
    ThermalProvider *m_thermal = nullptr;

    QHash<quint32, double> m_procUsage;
    QHash<quint32, quint64> m_procDedicated;
    QHash<quint32, quint64> m_procShared;
};

} // namespace ws
