#pragma once

// 侧边栏图标。
//
// 不用图片资源,全部用 QPainter 现画 —— 好处是可以跟着主题色走,
// 而且不用维护 .qrc 和一堆 png。

#include <QColor>
#include <QIcon>

namespace ws {
namespace icons {

enum class Nav {
    Dashboard,
    Processes,
    Performance,
    Network,
    Startup,
    Services,
    System,
    Tools,
};

QIcon nav(Nav id, const QColor &color, int size = 20);

} // namespace icons
} // namespace ws
