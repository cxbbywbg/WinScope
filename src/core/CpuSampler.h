#pragma once

// CPU 采样。
//
// 总体占用走 GetSystemTimes,每核心占用走 NtQuerySystemInformation。
// 频率分两路:CallNtPowerInformation 给每个核心的实时主频,但在不少 AMD 机器上
// 它只会返回标称值(实测 4800H 恒为 2900MHz,曲线一条直线),所以优先用 PDH 的
// "\Processor Information(_Total)\% Processor Performance" 乘基准频率换算,
// 拿不到计数器再退回前者。最大睿频从 WMI Win32_Processor.MaxClockSpeed 取。
//
// 温度优先走 ACPI 热区(WMI MSAcpi_ThermalZoneTemperature),但很多笔记本固件
// 没实现它;拿不到就退回核显传感器 —— 核显和 CPU 核心在同一颗 die 上,可以当
// 参考值,但来源会记在 CpuInfo::temperatureSource 里由界面标出来。
// 功耗在多数机器上拿不到,拿不到就保持 -1,界面显示"不可用"。

#include "core/Types.h"

#include <QString>
#include <QVector>

#include <windows.h>

namespace ws {

class ThermalProvider;

class CpuSampler
{
public:
    CpuSampler();
    ~CpuSampler();

    // 载入不随时间变化的信息(型号、核心数、标称频率),只需调用一次
    void loadStaticInfo();

    // 采样一次。返回值的占用率是相对于"上次采样"的区间平均
    CpuInfo sample();

    const QString &modelName() const { return m_modelName; }

    // 核显温度的来源。核显和 CPU 核心在同一颗 die 上,拿不到 ACPI 热区时用它
    // 当 CPU 温度的参考值。不设也能跑,那就只报"不可用"
    void setThermalProvider(ThermalProvider *provider) { m_thermal = provider; }

private:
    struct Times {
        quint64 idle = 0;
        quint64 kernel = 0;
        quint64 user = 0;
    };

    void refreshAcpiTemperatureAndPower();
    void resolveTemperature();
    double currentTemperatureC() const;
    int physicalCoreCount() const;

    QString m_modelName;
    int m_physicalCores = 0;
    int m_logicalCores = 0;
    double m_baseMHz = 0.0;
    double m_maxClockMHz = 0.0;

    bool m_hasPrev = false;
    Times m_prevTotal;
    QVector<Times> m_prevCores;

    // ACPI 热区查询很慢(WMI),限频 10 秒;核显传感器每拍都取
    qint64 m_lastSlowQueryMs = 0;
    double m_acpiTemperatureC = -1.0;
    CpuTemperatureSource m_temperatureSource = CpuTemperatureSource::None;
    ThermalProvider *m_thermal = nullptr;
    double m_powerW = -1.0;

    void *m_powerQuery = nullptr;   // PdhQuery*
    void *m_freqQuery = nullptr;    // PdhQuery*,% Processor Performance
    int m_idxFreqPerf = -1;
};

} // namespace ws
