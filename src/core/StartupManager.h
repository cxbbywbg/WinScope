#pragma once

// 开机启动项管理。
//
// 三类来源:
//   1. 注册表 Run / RunOnce(HKCU、HKLM、WOW6432Node)
//   2. 启动文件夹(%APPDATA% 与 %PROGRAMDATA% 的 Startup)
//   3. 计划任务里带"登录时/开机时"触发器的任务
//
// 启用/禁用沿用任务管理器的那套机制:写 Explorer\StartupApproved 下的二进制标记,
// 而不是去删注册表值 —— 这样用户随时能再打开,也不会丢配置。

#include "core/Types.h"
#include "core/ProcessController.h"   // ActionResult

#include <QVector>

namespace ws {

class StartupManager
{
public:
    static QVector<StartupItem> enumerate();

    // 注册表项与启动文件夹项支持启停;计划任务走任务计划自己的接口
    static ActionResult setEnabled(const StartupItem &item, bool enabled);

    // 从注册表里彻底删掉(不可恢复)
    static ActionResult remove(const StartupItem &item);
};

} // namespace ws
