#include "ui/widgets/MiniWindow.h"

#include "app/Icons.h"
#include "app/Theme.h"

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPair>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStringList>
#include <QVBoxLayout>

namespace ws {

namespace {

constexpr int kWidth = 288;
constexpr int kPadX = 14;
constexpr int kPadTop = 9;
constexpr int kPadBottom = 12;
constexpr int kHeaderH = 18;      // 顶部品牌行
constexpr int kRowH = 24;         // 一行的文字区 + 横条
constexpr int kRowGap = 7;
constexpr int kBarH = 3;
constexpr int kBarTop = 20;       // 横条相对行顶的偏移
constexpr int kLabelWidth = 70;   // 名称列,要放得下"CPU 频率"这种
constexpr int kGap = 8;

// 第一次运行时给一组默认值,就是用户最常看的那几个
QVector<MetricId> defaultMetrics()
{
    return { MetricId::CpuUsage,     MetricId::CpuFrequency, MetricId::CpuTemperature,
             MetricId::MemoryUsage,  MetricId::NetworkThroughput };
}

bool pointIsOnSomeScreen(const QPoint &p)
{
    for (const QScreen *screen : QGuiApplication::screens()) {
        if (screen->availableGeometry().adjusted(-40, -40, 40, 40).contains(p))
            return true;
    }
    return false;
}

// 参数选择对话框。
//
// 做成一个函数而不是一个类:它没有自己的状态,一次 exec 拿到结果就走,
// 而且不需要自定义信号,省得为一个模态小对话框再开一对 .h/.cpp
QVector<MetricId> runMetricPicker(QWidget *parent, const QVector<MetricId> &current)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("选择小窗显示的参数"));
    dialog.setMinimumWidth(400);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(8);

    auto *hint = new QLabel(
        QStringLiteral("小窗模式下只采集勾选的参数需要的数据,其余通道整拍跳过,后台开销更小。\n"
                       "一个都不勾的话小窗会只显示一行提示。"),
        &dialog);
    hint->setObjectName(QStringLiteral("CardHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    QVector<QPair<MetricId, QCheckBox *>> boxes;
    for (const MetricDef &def : metricDefs()) {
        auto *box = new QCheckBox(def.label, &dialog);
        box->setChecked(current.contains(def.id));
        box->setToolTip(def.hint);
        layout->addWidget(box);
        boxes.push_back({ def.id, box });
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("Primary"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return current;

    QVector<MetricId> result;
    for (const auto &pair : boxes) {
        if (pair.second->isChecked())
            result.push_back(pair.first);
    }
    return result;
}

} // namespace

MiniWindow::MiniWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("WinScope 小窗"));
    // Qt::Tool 在 Windows 上就是 WS_EX_TOOLWINDOW:任务栏不占按钮、不进 Alt+Tab。
    // 小窗模式改成靠右下角托盘图标进出,和微信那类常驻程序一致
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    setMouseTracking(false);

    // 必须显式设:不设的话任务栏/托盘会退回 Qt 的默认图形,和主窗口对不上
    setWindowIcon(icons::appIcon(theme::accent()));

    loadSettings();
    relayout();
    moveToDefaultCorner();

    // 置顶是设置项,建窗口时一起定掉,省得后面再 setWindowFlag 一次(那会重建窗口)
    if (m_alwaysOnTop)
        setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
}

MiniWindow::~MiniWindow()
{
    saveSettings();
}

bool MiniWindow::miniModeWasActive()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("miniWindow"));
    return settings.value(QStringLiteral("active"), false).toBool();
}

void MiniWindow::setMiniModeActive(bool active)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("miniWindow"));
    settings.setValue(QStringLiteral("active"), active);
}

void MiniWindow::loadSettings()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("miniWindow"));

    const QStringList keys = settings.value(QStringLiteral("metrics")).toStringList();
    if (keys.isEmpty()) {
        // 没配过(或者用户把配置清空了)就用默认那组。注意这里区分不了
        // "从没配过"和"用户明确一个都不要",后者极少见,按默认处理更友好
        m_metrics = defaultMetrics();
    } else {
        for (const QString &key : keys) {
            if (const MetricDef *def = metricDefByKey(key))
                m_metrics.push_back(def->id);
        }
    }

    m_alwaysOnTop = settings.value(QStringLiteral("alwaysOnTop"), true).toBool();
    m_savedPosition = settings.value(QStringLiteral("position")).toPoint();
    settings.endGroup();
}

void MiniWindow::saveSettings() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("miniWindow"));

    QStringList keys;
    for (MetricId id : m_metrics) {
        if (const MetricDef *def = metricDef(id))
            keys << def->key;
    }
    settings.setValue(QStringLiteral("metrics"), keys);
    settings.setValue(QStringLiteral("position"), pos());
    settings.setValue(QStringLiteral("alwaysOnTop"), m_alwaysOnTop);
    settings.endGroup();
}

void MiniWindow::moveToDefaultCorner()
{
    // 存的位置如果落在所有屏幕之外(换了显示器、拔了外接屏),就回到默认位置,
    // 否则窗口会出现在看不见的地方,用户只能靠删配置找回来
    if (!m_savedPosition.isNull() && pointIsOnSomeScreen(m_savedPosition)) {
        move(m_savedPosition);
        return;
    }
    const QScreen *primary = QGuiApplication::primaryScreen();
    if (!primary)
        return;
    const QRect area = primary->availableGeometry();
    move(area.right() - width() - 28, area.top() + 28);
}

void MiniWindow::relayout()
{
    // 没勾任何参数时也留一行位置,用来显示"右键选择参数"的提示
    const int rows = m_metrics.isEmpty() ? 1 : int(m_metrics.size());
    const int height = kPadTop + kHeaderH + rows * (kRowH + kRowGap) - kRowGap + kPadBottom;
    setFixedSize(kWidth, height);
    update();
}

void MiniWindow::setMetrics(const QVector<MetricId> &metrics)
{
    if (m_metrics == metrics)
        return;
    m_metrics = metrics;
    relayout();
    saveSettings();
    emit metricsChanged();
}

void MiniWindow::chooseMetrics()
{
    const QVector<MetricId> picked = runMetricPicker(this, m_metrics);
    setMetrics(picked);
}

QString MiniWindow::trayToolTip() const
{
    // 只取前两个有读数的,托盘提示长了会被 Windows 截断
    QStringList parts;
    for (MetricId id : m_metrics) {
        const MetricDef *def = metricDef(id);
        if (!def)
            continue;
        const MetricValue value = def->read(m_snapshot);
        if (value.text.isEmpty())
            continue;
        parts << QStringLiteral("%1 %2").arg(def->label, value.text);
        if (parts.size() >= 2)
            break;
    }

    if (parts.isEmpty())
        return QStringLiteral("WinScope 小窗");
    return QStringLiteral("WinScope 小窗\n%1").arg(parts.join(QStringLiteral(" · ")));
}

void MiniWindow::applyAlwaysOnTop(bool on)
{
    if (on == m_alwaysOnTop)
        return;
    m_alwaysOnTop = on;

    // setWindowFlag 会把窗口藏起来,所以改完要显式 show 回来,并且把位置补回去
    // (Windows 上重建窗口有时会把它挪回默认位置)
    const QPoint keep = pos();
    setWindowFlag(Qt::WindowStaysOnTopHint, on);
    if (!isVisible())
        return;
    show();
    move(keep);
    saveSettings();
}

void MiniWindow::onSystemSnapshot(const SystemSnapshot &snapshot)
{
    m_snapshot = snapshot;
    update();
}

void MiniWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 底板 + 边框
    p.setPen(QPen(theme::borderLight(), 1.0));
    p.setBrush(theme::surface());
    p.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));

    // 顶部品牌行。顺带提示怎么回到主界面 —— 无边框窗口没有标题栏,
    // 不写一句用户找不到回去的路
    p.setFont(theme::uiFont(11));
    p.setPen(theme::textFaint());
    p.drawText(QRect(kPadX, kPadTop, width() - 2 * kPadX, kHeaderH), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("WinScope 小窗"));
    p.drawText(QRect(kPadX, kPadTop, width() - 2 * kPadX, kHeaderH), Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("双击回主界面"));

    if (m_metrics.isEmpty()) {
        p.setFont(theme::uiFont(12));
        p.setPen(theme::textDim());
        p.drawText(QRect(kPadX, kPadTop + kHeaderH, width() - 2 * kPadX, kRowH),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("右键选择要显示的参数"));
        return;
    }

    int y = kPadTop + kHeaderH;
    for (MetricId id : m_metrics) {
        const MetricDef *def = metricDef(id);
        if (!def)
            continue;

        const MetricValue value = def->read(m_snapshot);
        // 读不到的指标颜色压暗、数值留空 —— 小窗地方小,写"不可用"三个字反而更挤,
        // 而且会让人以为是个正常读数
        const QColor color = value.text.isEmpty() ? theme::textFaint() : theme::forLevel(value.level);

        p.setFont(theme::uiFont(12));
        p.setPen(theme::textDim());
        p.drawText(QRect(kPadX, y, kLabelWidth, kRowH), Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(def->label, Qt::ElideRight, kLabelWidth));

        const int valueX = kPadX + kLabelWidth + kGap;
        const int valueW = width() - valueX - kPadX;
        p.setFont(theme::monoFont(12));
        p.setPen(color);
        p.drawText(QRect(valueX, y, valueW, kRowH), Qt::AlignRight | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(value.text, Qt::ElideRight, valueW));

        // 细横条:只有 0..1 的指标才画。主频、功耗、运行时间没有"满量程"的概念,
        // 画出来只能是编的,所以干脆不画
        if (value.ratio >= 0.0) {
            const QRectF track(kPadX, y + kBarTop, width() - 2 * kPadX, kBarH);
            p.setPen(Qt::NoPen);
            p.setBrush(theme::withAlpha(theme::borderLight(), 120));
            p.drawRoundedRect(track, kBarH / 2.0, kBarH / 2.0);

            const double ratio = qBound(0.0, value.ratio, 1.0);
            if (ratio > 0.0) {
                const qreal fillW = qMax(qreal(kBarH), qreal(track.width()) * ratio);
                p.setBrush(color);
                p.drawRoundedRect(QRectF(track.left(), track.top(), fillW, track.height()),
                                  kBarH / 2.0, kBarH / 2.0);
            }
        }

        y += kRowH + kRowGap;
    }
}

void MiniWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void MiniWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void MiniWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        // 松手时才存位置,拖动过程中每帧都写 QSettings 太浪费
        saveSettings();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void MiniWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        emit returnToMainRequested();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void MiniWindow::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    menu.addAction(QStringLiteral("选择要显示的参数…"), this, &MiniWindow::chooseMetrics);
    menu.addAction(QStringLiteral("返回主界面"), this, &MiniWindow::returnToMainRequested);

    QAction *onTop = menu.addAction(QStringLiteral("窗口置顶"));
    onTop->setCheckable(true);
    onTop->setChecked(m_alwaysOnTop);
    connect(onTop, &QAction::toggled, this, &MiniWindow::applyAlwaysOnTop);

    menu.addSeparator();
    menu.addAction(QStringLiteral("退出 WinScope"), this, [this] {
        m_quitting = true;
        emit quitRequested();
    });

    menu.exec(event->globalPos());
}

void MiniWindow::closeEvent(QCloseEvent *event)
{
    if (m_quitting) {
        event->accept();
        return;
    }
    // 无边框窗口照样能被 Alt+F4 关掉。这里不真的关,改成回主界面 ——
    // 真关掉会留下"程序还在跑但一个窗口都没有"的状态,只能去任务管理器结束
    event->ignore();
    hide();
    emit returnToMainRequested();
}

} // namespace ws
