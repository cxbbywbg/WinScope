#include "app/Icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <cmath>

namespace ws {
namespace icons {

namespace {

constexpr qreal kPi = 3.14159265358979323846;

void drawDashboard(QPainter &p, qreal s)
{
    const qreal r = s * 0.14;
    const qreal gap = s * 0.12;
    const qreal cell = (s - gap) / 2.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            // 右上角画成实心,让四个方块有主次
            if (x == 1 && y == 0)
                p.fillRect(QRectF(x * (cell + gap), y * (cell + gap), cell, cell), p.pen().color());
            else
                p.drawRoundedRect(QRectF(x * (cell + gap), y * (cell + gap), cell, cell), r, r);
        }
    }
}

void drawProcesses(QPainter &p, qreal s)
{
    const QColor c = p.pen().color();
    const qreal barH = s * 0.115;
    const qreal widths[3] = { 0.74, 0.52, 0.64 };

    for (int i = 0; i < 3; ++i) {
        const qreal y = s * (0.2 + i * 0.27);
        // 左侧圆点
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(s * 0.13, y), s * 0.065, s * 0.065);
        // 右侧横条
        p.setPen(QPen(c, barH, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(s * 0.3, y), QPointF(s * 0.3 + s * widths[i] * 0.8, y));
    }

    p.setPen(QPen(c, qMax(1.3, s * 0.075), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
}

void drawPerformance(QPainter &p, qreal s)
{
    QPainterPath path;
    path.moveTo(s * 0.08, s * 0.72);
    path.lineTo(s * 0.3, s * 0.72);
    path.lineTo(s * 0.42, s * 0.3);
    path.lineTo(s * 0.56, s * 0.86);
    path.lineTo(s * 0.7, s * 0.46);
    path.lineTo(s * 0.92, s * 0.46);
    p.drawPath(path);
}

void drawNetwork(QPainter &p, qreal s)
{
    const QColor c = p.pen().color();
    // 上箭头(上传)
    QPainterPath up;
    up.moveTo(s * 0.3, s * 0.16);
    up.lineTo(s * 0.5, s * 0.36);
    up.lineTo(s * 0.42, s * 0.36);
    up.lineTo(s * 0.42, s * 0.62);
    up.lineTo(s * 0.58, s * 0.62);
    up.lineTo(s * 0.58, s * 0.36);
    up.lineTo(s * 0.7, s * 0.36);
    up.closeSubpath();
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawPath(up);

    // 下箭头(下载),整体绕中心旋转 180°
    p.save();
    p.translate(s, s);
    p.rotate(180);
    p.drawPath(up);
    p.restore();
    p.setBrush(Qt::NoBrush);
}

void drawStartup(QPainter &p, qreal s)
{
    // 电源符号
    const qreal cx = s / 2, cy = s * 0.56, r = s * 0.31;
    QRectF box(cx - r, cy - r, r * 2, r * 2);
    p.drawArc(box, 60 * 16, 240 * 16);
    p.drawLine(QPointF(cx, s * 0.1), QPointF(cx, s * 0.46));
}

void drawServices(QPainter &p, qreal s)
{
    // 齿轮:圆 + 8 根齿
    const qreal cx = s / 2, cy = s / 2;
    const qreal outer = s * 0.42;
    const qreal inner = s * 0.27;
    for (int i = 0; i < 8; ++i) {
        const qreal a = i * kPi / 4.0;
        p.drawLine(QPointF(cx + std::cos(a) * inner, cy + std::sin(a) * inner),
                   QPointF(cx + std::cos(a) * outer, cy + std::sin(a) * outer));
    }
    p.drawEllipse(QPointF(cx, cy), inner, inner);
}

void drawSystem(QPainter &p, qreal s)
{
    // 显示器
    p.drawRoundedRect(QRectF(s * 0.1, s * 0.16, s * 0.8, s * 0.54), s * 0.08, s * 0.08);
    p.drawLine(QPointF(s * 0.5, s * 0.7), QPointF(s * 0.5, s * 0.85));
    p.drawLine(QPointF(s * 0.3, s * 0.88), QPointF(s * 0.7, s * 0.88));
}

void drawTools(QPainter &p, qreal s)
{
    // 扳手:斜线 + 两端半开口
    p.drawLine(QPointF(s * 0.32, s * 0.68), QPointF(s * 0.72, s * 0.28));
    p.drawArc(QRectF(s * 0.6, s * 0.08, s * 0.32, s * 0.32), 200 * 16, 250 * 16);
    p.drawArc(QRectF(s * 0.08, s * 0.6, s * 0.32, s * 0.32), 20 * 16, 250 * 16);
}

QPixmap renderNavPixmap(Nav id, const QColor &color, int size)
{
    const qreal dpr = 2.0;   // 直接按 2 倍画,高分屏不糊
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(dpr, dpr);
    p.setPen(QPen(color, qMax(1.3, size * 0.075), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);

    const qreal s = size;
    switch (id) {
    case Nav::Dashboard:
        drawDashboard(p, s);
        break;
    case Nav::Processes:
        drawProcesses(p, s);
        break;
    case Nav::Performance:
        drawPerformance(p, s);
        break;
    case Nav::Network:
        drawNetwork(p, s);
        break;
    case Nav::Startup:
        drawStartup(p, s);
        break;
    case Nav::Services:
        drawServices(p, s);
        break;
    case Nav::System:
        drawSystem(p, s);
        break;
    case Nav::Tools:
        drawTools(p, s);
        break;
    }
    p.end();

    return pm;
}

} // namespace

QIcon nav(Nav id, const QColor &color, int size)
{
    return QIcon(renderNavPixmap(id, color, size));
}

QIcon appIcon(const QColor &color)
{
    // 窗口图标、任务栏按钮、系统托盘都从这一份来。
    // 之前小窗没设图标,任务栏上显示的是 Qt 的默认图形,和主窗口对不上 —— 就是漏了这步
    QIcon icon;
    for (int size : { 16, 20, 24, 32, 48, 64, 128, 256 })
        icon.addPixmap(renderNavPixmap(Nav::Dashboard, color, size));
    return icon;
}

} // namespace icons
} // namespace ws
