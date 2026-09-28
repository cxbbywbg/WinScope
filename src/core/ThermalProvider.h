#pragma once

// 显卡温度采样。
//
// Windows 没有通用的 GPU 温度接口,只能按厂商走私有 SDK,而且都得运行时动态加载
// (没装对应驱动就没有这个 DLL)。任何一个环节失败都必须静默降级成"不可用",
// 不能影响其它功能,也不能崩。
//
//   AMD     atiadlxx.dll   ADL2_OverdriveN_Temperature_Get(EDGE 传感器),
//                          拿不到再退回 ADL2_New_QueryPMLogData_Get 的 PMLOG sensor[8]
//   NVIDIA  nvml.dll       nvmlDeviceGetTemperature(优先,顺带能取到型号名)
//           nvapi64.dll    NvAPI_GPU_GetThermalSettings(退回)
//
// 一个必须绕开的坑:ADL 枚举出来的不是"显卡",而是"显示适配器"。
// 实测一块 AMD 核显会被拆成 3 条,同一个列表里还会混进 NVIDIA 的 4 条,
// 而且给 NVIDIA 的条目调温度接口一律返回 ADL_ERR_NOT_SUPPORTED。
// 所以 ADL 的 adapterIndex 和 DXGI 的适配器序号没有任何对应关系,
// 只能按 strAdapterName 匹配型号名(实测和 DXGI 的 Description 完全一致)。
//
// 读到的温度按 DXGI 适配器序号缓存一小段时间,CPU 和 GPU 两条采样路径共用一份,
// 不会对同一个传感器重复调用驱动。

#include "core/Types.h"

#include <QString>
#include <QVector>

namespace ws {

// 一块要读温度的显卡。name 用 DXGI 的型号名,integrated 决定它能不能当 CPU 温度的参考
struct ThermalTarget {
    QString name;
    bool integrated = false;
};

class ThermalProvider
{
public:
    ThermalProvider();
    ~ThermalProvider();

    ThermalProvider(const ThermalProvider &) = delete;
    ThermalProvider &operator=(const ThermalProvider &) = delete;

    // 加载厂商 SDK,只需调用一次。适配器清单由它自己枚举,返回是否至少有一个
    // 来源可用。加载失败只是温度显示"不可用",不影响其它功能
    bool load();

    // 按适配器序号取温度(摄氏度),取不到返回 -1
    double temperatureC(int index) const;

    // 能不能拿这块核显的温度当 CPU 温度的参考值。核显和 CPU 核心在同一颗 die 上,
    // 拿不到 ACPI 热区时它是唯一合理的替代 —— 但门槛卡得很死,见 .cpp 里的说明。
    // 取不到返回 -1,界面应当留空,绝不能退回用独显温度冒充
    double cpuProxyTemperatureC() const;

    // 诊断用:当前加载成功的来源,例如 "ADL + NVML"
    QString backendSummary() const;

    // 诊断/自检用:型号名看起来是不是 AMD APU 的核显。
    // 误判的代价不对称(漏判只是温度留空,误判会把独显温度当成 CPU 温度),
    // 所以这里宁可严。做成公开函数是为了让自检工具能用一组样本名回归验证
    static bool nameLooksLikeApuIntegrated(const QString &adapterName);

private:
    struct Impl;
    Impl *m_impl = nullptr;
};

} // namespace ws
