#include "app/Theme.h"

#include <QFontDatabase>

namespace ws {
namespace theme {

QColor forLevel(LoadLevel level)
{
    switch (level) {
    case LoadLevel::Critical:
        return danger();
    case LoadLevel::Warning:
        return warn();
    case LoadLevel::Normal:
    default:
        return ok();
    }
}

QString levelText(LoadLevel level)
{
    switch (level) {
    case LoadLevel::Critical:
        return QStringLiteral("高负载");
    case LoadLevel::Warning:
        return QStringLiteral("偏高");
    case LoadLevel::Normal:
    default:
        return QStringLiteral("正常");
    }
}

QFont monoFont(int pixelSize, bool bold)
{
    // 优先用系统自带的等宽字体,中文回退交给 Qt 自己处理
    static const QStringList preferred = {
        QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"), QStringLiteral("JetBrains Mono"),
        QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Courier New"),
    };

    QFont font;
    const QStringList families = QFontDatabase::families();
    for (const QString &name : preferred) {
        if (families.contains(name)) {
            font.setFamily(name);
            break;
        }
    }
    font.setPixelSize(pixelSize);
    font.setBold(bold);
    font.setStyleHint(QFont::Monospace);
    return font;
}

QFont uiFont(int pixelSize, bool bold)
{
    QFont font;
    // 微软雅黑在中文 Windows 上是最稳的界面字体
    static const QStringList preferred = { QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei"),
                                           QStringLiteral("Segoe UI"), QStringLiteral("Noto Sans CJK SC") };
    const QStringList families = QFontDatabase::families();
    for (const QString &name : preferred) {
        if (families.contains(name)) {
            font.setFamily(name);
            break;
        }
    }
    font.setPixelSize(pixelSize);
    font.setBold(bold);
    return font;
}

QColor withAlpha(const QColor &c, int alpha)
{
    QColor out = c;
    out.setAlpha(alpha);
    return out;
}

QString styleSheet()
{
    const QString bg = background().name();
    const QString sbg = sidebar().name();
    const QString surf = surface().name();
    const QString surfAlt = surfaceAlt().name();
    const QString hover = surfaceHover().name();
    const QString bd = border().name();
    const QString bdLight = borderLight().name();
    const QString fg = text().name();
    const QString fgDim = textDim().name();
    const QString fgFaint = textFaint().name();
    const QString acc = accent().name();
    const QString accDim = accentDim().name();

    // Qt 样式表对 QTableWidget 之类的复合控件要按 QTableView / QHeaderView 选
    return QStringLiteral(R"(
/* ---------------------------------------------------------------- 基础 */
QWidget {
    color: %FG%;
    font-size: 13px;
}
QMainWindow, QDialog {
    background: %BG%;
}
QToolTip {
    background: %SURF_ALT%;
    color: %FG%;
    border: 1px solid %BD_LIGHT%;
    padding: 6px 8px;
    border-radius: 4px;
}

/* ---------------------------------------------------------------- 侧边栏 */
#Sidebar {
    background: %SBG%;
    border-right: 1px solid %BD%;
}
#BrandTitle {
    font-size: 19px;
    font-weight: 600;
    color: %FG%;
    letter-spacing: 0.5px;
}
#BrandSubtitle {
    font-size: 11px;
    color: %FG_FAINT%;
}
#NavList {
    background: transparent;
    border: none;
    outline: none;
    padding: 6px 10px;
}
#NavList::item {
    height: 40px;
    padding-left: 12px;
    border-radius: 7px;
    color: %FG_DIM%;
    margin: 2px 0;
}
#NavList::item:hover {
    background: %HOVER%;
    color: %FG%;
}
#NavList::item:selected {
    background: %ACC_DIM%;
    color: #ffffff;
    font-weight: 600;
}
#SidebarFooter {
    color: %FG_FAINT%;
    font-size: 11px;
    padding: 12px 16px;
}

/* ---------------------------------------------------------------- 顶部栏 */
#HeaderBar {
    background: %BG%;
    border-bottom: 1px solid %BD%;
}
#PageTitle {
    font-size: 21px;
    font-weight: 600;
    color: %FG%;
}
#PageSubtitle {
    font-size: 12px;
    color: %FG_DIM%;
}

/* ---------------------------------------------------------------- 卡片 */
#Card {
    background: %SURF%;
    border: 1px solid %BD%;
    border-radius: 10px;
}
#CardTitle {
    font-size: 13px;
    font-weight: 600;
    color: %FG%;
}
#CardHint {
    font-size: 11px;
    color: %FG_FAINT%;
}
#StatKey {
    color: %FG_DIM%;
    font-size: 12px;
}
#StatValue {
    color: %FG%;
    font-size: 12px;
}

/* ---------------------------------------------------------------- 表格 */
QTableView, QTreeView {
    background: transparent;
    alternate-background-color: rgba(255, 255, 255, 0.018);
    border: none;
    gridline-color: transparent;
    outline: none;
    selection-background-color: %ACC_DIM%;
    selection-color: #ffffff;
}
QTableView::item, QTreeView::item {
    padding: 4px 6px;
    border: none;
}
QTableView::item:hover, QTreeView::item:hover {
    background: %HOVER%;
}
QHeaderView {
    background: transparent;
}
QHeaderView::section {
    background: %SURF_ALT%;
    color: %FG_DIM%;
    padding: 7px 8px;
    border: none;
    border-right: 1px solid %BD%;
    border-bottom: 1px solid %BD%;
    font-weight: 600;
}
QHeaderView::section:hover {
    color: %FG%;
    background: %HOVER%;
}
QTableCornerButton::section {
    background: %SURF_ALT%;
    border: none;
}
QTreeView::branch {
    background: transparent;
}

/* ---------------------------------------------------------------- 输入 */
QLineEdit, QComboBox, QSpinBox, QPlainTextEdit, QTextEdit {
    background: %SURF_ALT%;
    border: 1px solid %BD%;
    border-radius: 6px;
    padding: 6px 9px;
    color: %FG%;
    selection-background-color: %ACC%;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QPlainTextEdit:focus {
    border: 1px solid %ACC%;
}
QLineEdit::placeholder {
    color: %FG_FAINT%;
}
QComboBox::drop-down {
    border: none;
    width: 22px;
}
QComboBox::down-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid %FG_DIM%;
    margin-right: 8px;
}
QComboBox QAbstractItemView {
    background: %SURF_ALT%;
    border: 1px solid %BD_LIGHT%;
    border-radius: 6px;
    selection-background-color: %ACC_DIM%;
    outline: none;
}

/* ---------------------------------------------------------------- 按钮 */
QPushButton {
    background: %SURF_ALT%;
    border: 1px solid %BD%;
    border-radius: 6px;
    padding: 6px 14px;
    color: %FG%;
}
QPushButton:hover {
    background: %HOVER%;
    border-color: %BD_LIGHT%;
}
QPushButton:pressed {
    background: %SURF%;
}
QPushButton:disabled {
    color: %FG_FAINT%;
    background: %SURF%;
    border-color: %BD%;
}
QPushButton#Primary {
    background: %ACC%;
    border: 1px solid %ACC%;
    color: #ffffff;
    font-weight: 600;
}
QPushButton#Primary:hover {
    background: #5f9bfa;
}
QPushButton#Danger {
    background: transparent;
    border: 1px solid #5a2a2a;
    color: #f08a8a;
}
QPushButton#Danger:hover {
    background: #3a1c1c;
    border-color: %DANGER%;
    color: #ff9c9c;
}
QPushButton#Ghost {
    background: transparent;
    border: 1px solid transparent;
    color: %FG_DIM%;
    padding: 4px 8px;
}
QPushButton#Ghost:hover {
    background: %HOVER%;
    color: %FG%;
}

/* ---------------------------------------------------------------- 勾选 */
QCheckBox, QRadioButton {
    spacing: 7px;
    color: %FG%;
}
QCheckBox::indicator, QRadioButton::indicator {
    width: 15px;
    height: 15px;
    border: 1px solid %BD_LIGHT%;
    border-radius: 4px;
    background: %SURF_ALT%;
}
QCheckBox::indicator:checked {
    background: %ACC%;
    border-color: %ACC%;
}
QRadioButton::indicator {
    border-radius: 8px;
}
QRadioButton::indicator:checked {
    background: %ACC%;
    border-color: %ACC%;
}

/* ---------------------------------------------------------------- 滚动条 */
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: #39414f;
    min-height: 28px;
    border-radius: 5px;
}
QScrollBar::handle:vertical:hover {
    background: #4a5464;
}
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
    margin: 0;
}
QScrollBar::handle:horizontal {
    background: #39414f;
    min-width: 28px;
    border-radius: 5px;
}
QScrollBar::handle:horizontal:hover {
    background: #4a5464;
}
QScrollBar::add-line, QScrollBar::sub-line {
    height: 0;
    width: 0;
}
QScrollBar::add-page, QScrollBar::sub-page {
    background: transparent;
}
QScrollArea {
    background: transparent;
    border: none;
}
QScrollArea > QWidget > QWidget {
    background: transparent;
}

/* ---------------------------------------------------------------- 页签 */
QTabWidget::pane {
    border: 1px solid %BD%;
    border-radius: 8px;
    background: %SURF%;
    top: -1px;
}
QTabBar::tab {
    background: transparent;
    color: %FG_DIM%;
    padding: 7px 16px;
    border: 1px solid transparent;
    border-top-left-radius: 7px;
    border-top-right-radius: 7px;
    margin-right: 2px;
}
QTabBar::tab:hover {
    color: %FG%;
}
QTabBar::tab:selected {
    background: %SURF%;
    color: %FG%;
    border-color: %BD%;
    border-bottom-color: %SURF%;
    font-weight: 600;
}

/* ---------------------------------------------------------------- 其它 */
QSplitter::handle {
    background: %BD%;
}
QSplitter::handle:hover {
    background: %ACC%;
}
QProgressBar {
    background: %SURF_ALT%;
    border: none;
    border-radius: 4px;
    height: 6px;
    text-align: center;
    color: transparent;
}
QProgressBar::chunk {
    background: %ACC%;
    border-radius: 4px;
}
QMenu {
    background: %SURF_ALT%;
    border: 1px solid %BD_LIGHT%;
    border-radius: 7px;
    padding: 5px;
}
QMenu::item {
    padding: 6px 26px 6px 14px;
    border-radius: 5px;
    color: %FG%;
}
QMenu::item:selected {
    background: %ACC_DIM%;
}
QMenu::item:disabled {
    color: %FG_FAINT%;
}
QMenu::separator {
    height: 1px;
    background: %BD%;
    margin: 5px 8px;
}
QListWidget {
    background: transparent;
    border: none;
    outline: none;
}
QListWidget::item {
    padding: 6px 8px;
    border-radius: 5px;
    color: %FG%;
}
QListWidget::item:hover {
    background: %HOVER%;
}
QListWidget::item:selected {
    background: %ACC_DIM%;
    color: #ffffff;
}
)")
        .replace(QStringLiteral("%BG%"), bg)
        .replace(QStringLiteral("%SBG%"), sbg)
        .replace(QStringLiteral("%SURF_ALT%"), surfAlt)
        .replace(QStringLiteral("%SURF%"), surf)
        .replace(QStringLiteral("%HOVER%"), hover)
        .replace(QStringLiteral("%BD_LIGHT%"), bdLight)
        .replace(QStringLiteral("%BD%"), bd)
        .replace(QStringLiteral("%FG_FAINT%"), fgFaint)
        .replace(QStringLiteral("%FG_DIM%"), fgDim)
        .replace(QStringLiteral("%FG%"), fg)
        .replace(QStringLiteral("%ACC_DIM%"), accDim)
        .replace(QStringLiteral("%ACC%"), acc)
        .replace(QStringLiteral("%DANGER%"), danger().name());
}

} // namespace theme
} // namespace ws
