// 生成 exe 用的 .ico 文件。
//
// 为什么需要它:界面上的图标是 QPainter 运行时画的(src/app/Icons.cpp),
// 资源管理器看不见 —— 它只读 PE 里的 RT_GROUP_ICON 资源。要让「文件夹里就有图标」,
// 必须先有一份真正的 .ico,再由 src/winres/WinScope.rc 在链接期嵌进 PE。
//
// 这里刻意复用 ws::icons::appIcon() 而不是另画一个图形:保证资源管理器里的图标
// 和窗口 / 任务栏 / 托盘上那个永远是同一个。改了 Icons.cpp 就重跑一次本工具。
//
// 用法:make_icon <输出路径>   (默认 ./WinScope.ico)

#include "app/Icons.h"
#include "app/Theme.h"

#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QPixmap>

#include <cstdio>
#include <vector>

namespace {

// 与 appIcon() 给的档位一致。托盘要 16/20、任务栏要 32/48、
// 资源管理器大图标要 96~128、超大图标要 256。
constexpr int kSizes[] = { 16, 20, 24, 32, 48, 64, 128, 256 };

// 从这一档起改用 PNG 压缩。32bpp 原始数据下 256x256 一张就 256KB,
// 而 .ico 允许内嵌 PNG(Vista 起支持),能压到几 KB。
constexpr int kPngFrom = 256;

struct Entry {
    int size = 0;
    QByteArray data;
};

void appendU16(QByteArray &out, quint16 v)
{
    out.append(char(v & 0xff));
    out.append(char((v >> 8) & 0xff));
}

void appendU32(QByteArray &out, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        out.append(char((v >> (8 * i)) & 0xff));
}

// ICO 里内嵌的位图 = BITMAPINFOHEADER + 自下而上的 BGRA 像素 + AND 掩码。
// 有 alpha 通道时 AND 掩码不参与显示,但结构上必须占位(全 0)。
QByteArray dibBytes(const QImage &img)
{
    const int w = img.width();
    const int h = img.height();
    const int maskStride = ((w + 31) / 32) * 4;   // 1bpp,每行按 4 字节对齐
    const int maskBytes = maskStride * h;

    QByteArray out;
    out.reserve(40 + w * h * 4 + maskBytes);

    appendU32(out, 40);                        // biSize
    appendU32(out, quint32(w));                // biWidth
    appendU32(out, quint32(h * 2));            // biHeight:XOR 图 + AND 掩码叠在一起
    appendU16(out, 1);                         // biPlanes
    appendU16(out, 32);                        // biBitCount
    appendU32(out, 0);                         // biCompression = BI_RGB
    appendU32(out, quint32(w * h * 4 + maskBytes));   // biSizeImage
    appendU32(out, 0);                         // biXPelsPerMeter
    appendU32(out, 0);                         // biYPelsPerMeter
    appendU32(out, 0);                         // biClrUsed
    appendU32(out, 0);                         // biClrImportant

    // QImage 的 Format_ARGB32 在内存里就是 B,G,R,A —— 正好是 ICO 要的通道顺序
    for (int y = h - 1; y >= 0; --y) {
        const uchar *line = img.constScanLine(y);
        out.append(reinterpret_cast<const char *>(line), w * 4);
    }
    out.append(maskBytes, '\0');
    return out;
}

QByteArray pngBytes(const QImage &img)
{
    QByteArray ba;
    QBuffer buf(&ba);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return ba;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    const QString target = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                    : QStringLiteral("WinScope.ico");

    const QIcon icon = ws::icons::appIcon(ws::theme::accent());

    std::vector<Entry> entries;
    for (int size : kSizes) {
        QImage img = icon.pixmap(QSize(size, size)).toImage();
        // appIcon() 内部是按 2 倍画的(高分屏用),这里缩到目标像素 ——
        // 顺带得到一次超采样,小尺寸比直接画干净
        if (img.width() != size) {
            img.setDevicePixelRatio(1.0);
            img = img.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        img = img.convertToFormat(QImage::Format_ARGB32);

        Entry e;
        e.size = size;
        e.data = (size >= kPngFrom) ? pngBytes(img) : dibBytes(img);
        entries.push_back(e);
    }

    QByteArray ico;
    appendU16(ico, 0);                              // reserved
    appendU16(ico, 1);                              // type = icon
    appendU16(ico, quint16(entries.size()));

    quint32 offset = quint32(6 + 16 * entries.size());
    for (const Entry &e : entries) {
        // 宽高各一字节,256 这一档用 0 表示
        ico.append(char(e.size >= 256 ? 0 : e.size));
        ico.append(char(e.size >= 256 ? 0 : e.size));
        ico.append(char(0));                        // 调色板颜色数(真彩写 0)
        ico.append(char(0));                        // reserved
        appendU16(ico, 1);                          // planes
        appendU16(ico, 32);                         // bitCount
        appendU32(ico, quint32(e.data.size()));
        appendU32(ico, offset);
        offset += quint32(e.data.size());
    }
    for (const Entry &e : entries)
        ico.append(e.data);

    QFile f(target);
    if (!f.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "无法写入 %s\n", qPrintable(target));
        return 1;
    }
    f.write(ico);
    f.close();

    std::printf("已生成 %s:%d 档尺寸,共 %d 字节\n",
                qPrintable(target), int(entries.size()), int(ico.size()));
    for (const Entry &e : entries)
        std::printf("  %3d x %3d   %8d 字节\n", e.size, e.size, int(e.data.size()));
    return 0;
}
