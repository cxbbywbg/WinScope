#pragma once

// Windows 服务管理。
// 走 SCM(服务控制管理器),枚举 / 启停 / 改启动类型。

#include "core/Types.h"
#include "core/ProcessController.h"   // ActionResult

#include <QVector>

namespace ws {

class ServiceManager
{
public:
    // 需要管理员权限才能改服务状态,枚举则不需要
    static QVector<ServiceInfo> enumerate();

    static ActionResult start(const QString &serviceName);
    static ActionResult stop(const QString &serviceName);
    static ActionResult restart(const QString &serviceName);
    static ActionResult pause(const QString &serviceName);
    static ActionResult resume(const QString &serviceName);
    // startType: "auto" / "auto-delayed" / "manual" / "disabled"
    static ActionResult setStartType(const QString &serviceName, const QString &startType);

    static QStringList startTypeOptions();
};

} // namespace ws
