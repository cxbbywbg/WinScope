"""生成安装向导用的两张位图,让向导和程序是同一套视觉。

NSIS MUI2 默认那条蓝色横幅是 2000 年代的味道,和 WinScope 的深色界面完全不搭。
这里按 src/app/Theme.h 里的调色板画两张:
  welcome.bmp  164x314  欢迎页/完成页左侧竖幅
  header.bmp   150x57   内页(选择位置、组件…)右上角小图

图形不重新设计 —— 直接从 src/winres/WinScope.ico 里取 256 那一档贴上去,
保证和 exe 图标、窗口图标、托盘图标是同一个。

NSIS 要的是 24bpp 未压缩 BMP,像素自下而上、通道顺序 BGR。
"""
import os
import struct
import sys
import zlib

# ---- 调色板,和 Theme.h 保持一致 ----
BG = (0x0e, 0x10, 0x14)          # background()
SURFACE = (0x17, 0x1b, 0x22)     # surface()
ACCENT = (0x4d, 0x8d, 0xf7)      # accent()
ACCENT_DIM = (0x2a, 0x4c, 0x86)  # accentDim()


def read_ico_png(path, want=256):
    """从 .ico 里取出指定尺寸那一档(PNG 或 DIB 都支持),返回 (w, h, RGBA 行)"""
    d = open(path, 'rb').read()
    _res, _typ, cnt = struct.unpack_from('<HHH', d, 0)
    off = 6
    for _ in range(cnt):
        w, h, _cc, _r, _pl, _bits, size, offset = struct.unpack_from('<BBBBHHII', d, off)
        off += 16
        if (w or 256) != want:
            continue
        blob = d[offset:offset + size]
        if blob[:8] == b'\x89PNG\r\n\x1a\n':
            return decode_png(blob)
        return decode_dib(blob)
    raise SystemExit('ico 里没有 %d 这一档' % want)


def decode_dib(b):
    hdr, = struct.unpack_from('<I', b, 0)
    w, h2 = struct.unpack_from('<ii', b, 4)
    h = h2 // 2
    rows = []
    for y in range(h):
        src = b[hdr + (h - 1 - y) * w * 4: hdr + (h - 1 - y) * w * 4 + w * 4]
        rows.append([(src[x * 4 + 2], src[x * 4 + 1], src[x * 4], src[x * 4 + 3])
                     for x in range(w)])
    return w, h, rows


def decode_png(b):
    pos, idat, w, h, ct = 8, b'', 0, 0, 6
    while pos < len(b):
        ln, typ = struct.unpack_from('>I4s', b, pos)
        pos += 8
        data = b[pos:pos + ln]
        pos += ln + 4
        if typ == b'IHDR':
            w, h, _dep, ct = struct.unpack_from('>IIBB', data, 0)
        elif typ == b'IDAT':
            idat += data
    raw = zlib.decompress(idat)
    bpp = 4 if ct == 6 else 3
    stride = w * bpp
    out, prev, p = [], bytearray(stride), 0
    for _y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            bb = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + bb) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + bb) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(bb - c), abs(a - c), abs(a + bb - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (bb if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out.append([(line[x * bpp], line[x * bpp + 1], line[x * bpp + 2],
                     255 if bpp == 3 else line[x * bpp + 3]) for x in range(w)])
        prev = line
    return w, h, out


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def write_bmp(path, w, h, px):
    """px[y][x] = (r,g,b);写成 24bpp BMP(自下而上 + 4 字节行对齐)"""
    stride = (w * 3 + 3) // 4 * 4
    body = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b = px[y][x]
            row += bytes((b, g, r))
        row += b'\x00' * (stride - len(row))
        body += row
    hdr = struct.pack('<2sIHHI', b'BM', 14 + 40 + len(body), 0, 0, 14 + 40)
    info = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(body), 2835, 2835, 0, 0)
    open(path, 'wb').write(hdr + info + bytes(body))


def blit(px, icon, ox, oy, scale):
    """把图标按最近邻缩放贴到画布上(图标本身已经带了抗锯齿)"""
    iw, ih, rows = icon
    for y in range(ih * scale):
        sy = y // scale
        for x in range(iw * scale):
            sx = x // scale
            r, g, b, a = rows[sy][sx]
            if a == 0:
                continue
            dx, dy = ox + x, oy + y
            if not (0 <= dx < len(px[0]) and 0 <= dy < len(px)):
                continue
            dr, dg, db = px[dy][dx]
            t = a / 255.0
            px[dy][dx] = (int(dr + (r - dr) * t),
                          int(dg + (g - dg) * t),
                          int(db + (b - db) * t))


def make_banner(icon, w=164, h=314):
    px = [[lerp(BG, SURFACE, (x + y) / float(w + h)) for x in range(w)] for y in range(h)]
    # 右下角一圈很淡的强调色光晕,和界面里的卡片一个味道
    for y in range(h):
        for x in range(w):
            d = ((x - w * 0.78) ** 2 + (y - h * 0.88) ** 2) ** 0.5
            if d < 150:
                t = (1 - d / 150.0) ** 2 * 0.28
                r, g, b = px[y][x]
                px[y][x] = (int(r + (ACCENT_DIM[0] - r) * t),
                            int(g + (ACCENT_DIM[1] - g) * t),
                            int(b + (ACCENT_DIM[2] - b) * t))
    s = 1  # 256 -> 256 太大,缩到 96 左右
    scaled = (icon[0], icon[1], icon[2])
    # 先把 256 缩到 96(整数比不好看,这里用双线性近似)
    tw = 96
    small = []
    for y in range(tw):
        row = []
        for x in range(tw):
            fx, fy = x * icon[0] // tw, y * icon[1] // tw
            row.append(icon[2][fy][fx])
        small.append(row)
    blit(px, (tw, tw, small), (w - tw) // 2, int(h * 0.22), 1)

    # 图标下面一条强调色短线,当作分隔
    for y in range(int(h * 0.22) + tw + 22, int(h * 0.22) + tw + 26):
        for x in range((w - 56) // 2, (w + 56) // 2):
            px[y][x] = ACCENT
    return w, h, px


def make_header(icon, w=150, h=57):
    px = [[lerp(BG, SURFACE, x / float(w)) for x in range(w)] for y in range(h)]
    tw = 38
    small = [[icon[2][y * icon[1] // tw][x * icon[0] // tw] for x in range(tw)]
             for y in range(tw)]
    blit(px, (tw, tw, small), w - tw - 12, (h - tw) // 2, 1)
    return w, h, px


if __name__ == '__main__':
    ico = sys.argv[1]
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    icon = read_ico_png(ico, 256)
    print('  图标来源 %s 的 256 档 -> %dx%d' % (os.path.basename(ico), icon[0], icon[1]))
    for name, fn in (('welcome.bmp', make_banner), ('header.bmp', make_header)):
        w, h, px = fn(icon)
        path = os.path.join(outdir, name)
        write_bmp(path, w, h, px)
        print('  %-12s %dx%d  %d 字节' % (name, w, h, os.path.getsize(path)))
