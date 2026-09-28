#pragma once

// 侧边栏图标 + 几个界面里要用的小图形。
//
// 不用图片资源,全部用 QPainter 现画 —— 好处是可以跟着主题色走,
// 而且不用维护 .qrc 和一堆 png。

#include <QColor>
#include <QIcon>

class QPainter;
class QRectF;

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

// 应用图标 —— 主窗口、任务栏、系统托盘共用同一个图形。
//
// 颜色由调用方给(通常是 theme::accent()),这样 Icons 不用反过来依赖 Theme。
// 一次生成多个尺寸:托盘要 16/20,任务栏要 32/48,高分屏还会再取更大的那一档,
// 只给一个尺寸的话小尺寸那边会被缩放出锯齿。
QIcon appIcon(const QColor &color);

// 「窗口置顶」的图钉。
//
// 直接画在 box 里,颜色由调用方给 —— 按钮的悬停底板、选中底色属于控件自己的
// 事,留在控件里画,这里只负责那个钉子的形状。
//
// filled = 已经置顶(钉身填实),false = 没置顶(只描边)。两种状态差别要一眼能看出来,
// 否则用户点完不知道到底开没开。
void paintPin(QPainter &p, const QRectF &box, const QColor &color, bool filled);

} // namespace icons
} // namespace ws
