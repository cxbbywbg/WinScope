#include "app/Icons.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/Win32Utils.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("WinScope"));
    QApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    QApplication::setOrganizationName(QStringLiteral("WinScope"));

    // 界面全是自绘控件,Fusion 是最不带平台私货的样式,表现最可预期
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    // 原生控件(下拉框弹出层、菜单阴影、文本选择色)也要跟着深色主题走
    QPalette palette;
    palette.setColor(QPalette::Window, ws::theme::background());
    palette.setColor(QPalette::WindowText, ws::theme::text());
    palette.setColor(QPalette::Base, ws::theme::surface());
    palette.setColor(QPalette::AlternateBase, ws::theme::surfaceAlt());
    palette.setColor(QPalette::Text, ws::theme::text());
    palette.setColor(QPalette::PlaceholderText, ws::theme::textFaint());
    palette.setColor(QPalette::Button, ws::theme::surfaceAlt());
    palette.setColor(QPalette::ButtonText, ws::theme::text());
    palette.setColor(QPalette::Highlight, ws::theme::accent());
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, ws::theme::surfaceAlt());
    palette.setColor(QPalette::ToolTipText, ws::theme::text());
    palette.setColor(QPalette::Disabled, QPalette::Text, ws::theme::textFaint());
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, ws::theme::textFaint());
    app.setPalette(palette);

    app.setFont(ws::theme::uiFont(13));
    app.setStyleSheet(ws::theme::styleSheet());

    // 尽力打开 SeDebugPrivilege:没有它拿不到其它用户进程的路径和命令行。
    // 失败也无所谓,进程列表照样能看,只是部分字段是「—」。
    ws::enableDebugPrivilege();

    ws::MainWindow window;
    window.setWindowIcon(ws::icons::nav(ws::icons::Nav::Dashboard, ws::theme::accent(), 64));
    window.show();

    return app.exec();
}
