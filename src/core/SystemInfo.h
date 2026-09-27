#pragma once

// 系统信息:操作系统、CPU、GPU、内存条、主板、存储、网络。
// 硬件细节基本都靠 WMI,操作系统版本走注册表 + RtlGetVersion(GetVersionEx 会撒谎)。

#include "core/Types.h"

namespace ws {

class SystemInfo
{
public:
    // 采集完整信息,内部会新建 WMI 连接,耗时约几百毫秒,不要放在 UI 线程反复调
    static SystemInfoData collect();
};

} // namespace ws
