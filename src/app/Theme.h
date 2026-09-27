#pragma once

// WinScope 的视觉规范。
//
// 全项目只在这里定义颜色和全局样式表,页面里不要再写死颜色值 ——
// 以后要换主题只改这一个文件。

#include "core/Types.h"

#include <QColor>
#include <QFont>
#include <QString>

namespace ws {
namespace theme {

// ---------------------------------------------------------------- 调色板

inline QColor background() { return QColor(0x0e, 0x10, 0x14); }
inline QColor sidebar() { return QColor(0x12, 0x15, 0x1a); }
inline QColor surface() { return QColor(0x17, 0x1b, 0x22); }
inline QColor surfaceAlt() { return QColor(0x1d, 0x22, 0x2b); }
inline QColor surfaceHover() { return QColor(0x24, 0x2a, 0x35); }
inline QColor border() { return QColor(0x27, 0x2d, 0x38); }
inline QColor borderLight() { return QColor(0x33, 0x3a, 0x47); }

inline QColor text() { return QColor(0xe8, 0xeb, 0xf1); }
inline QColor textDim() { return QColor(0x93, 0x9d, 0xad); }
inline QColor textFaint() { return QColor(0x5d, 0x66, 0x76); }

inline QColor accent() { return QColor(0x4d, 0x8d, 0xf7); }
inline QColor accentDim() { return QColor(0x2a, 0x4c, 0x86); }

inline QColor ok() { return QColor(0x2e, 0xc4, 0x6b); }
inline QColor warn() { return QColor(0xf2, 0xa0, 0x1d); }
inline QColor danger() { return QColor(0xef, 0x4d, 0x4d); }
inline QColor purple() { return QColor(0xa7, 0x8b, 0xfa); }
inline QColor cyan() { return QColor(0x2a, 0xcf, 0xe0); }
inline QColor pink() { return QColor(0xf0, 0x72, 0xb6); }

// 按负载等级取色(正常绿 / 警告橙 / 过载红)
QColor forLevel(LoadLevel level);
QString levelText(LoadLevel level);

// ---------------------------------------------------------------- 字体

// 数值一律用等宽字体,免得刷新时数字宽度变化导致整行抖动
QFont monoFont(int pixelSize, bool bold = false);
QFont uiFont(int pixelSize, bool bold = false);

// ---------------------------------------------------------------- 样式表

QString styleSheet();

// 生成一个带透明度的颜色(用于填充曲线下方的渐变)
QColor withAlpha(const QColor &c, int alpha);

} // namespace theme
} // namespace ws
