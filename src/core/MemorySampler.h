#pragma once

// 内存采样:物理内存 / 提交 / 分页池 / 页面文件。
// 全部来自 GlobalMemoryStatusEx + GetPerformanceInfo,无 PDH 开销。

#include "core/Types.h"

namespace ws {

class MemorySampler
{
public:
    MemoryInfo sample();
};

} // namespace ws
