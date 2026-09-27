#pragma once

// 工具箱页。
//
// 三组工具:
//   Windows 工具 —— 用 shell 打开系统自带的 .msc / 控制面板项
//   网络工具     —— 在本页内跑 ping / tracert / nslookup / netstat / ipconfig,
//                   输出直接显示在下方,不用切窗口
//   系统工具     —— 清理 DNS、重启 Explorer、重启网络、打开终端
//
// 网络命令统一走 QProcess 异步执行,同一时刻只允许一条在跑。

#include "ui/pages/PageBase.h"

#include <QByteArray>
#include <QStringList>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;

namespace ws {

class ToolsPage : public PageBase
{
    Q_OBJECT

public:
    explicit ToolsPage(QWidget *parent = nullptr);
    ~ToolsPage() override;

    QString pageTitle() const override;
    QString pageSubtitle() const override;

private:
    QWidget *buildWindowsToolsCard();
    QWidget *buildNetworkToolsCard();
    QWidget *buildSystemToolsCard();

    // 用 shell 打开一个系统工具(.msc / .cpl / .exe)
    void openSystemTool(const QString &target, const QString &displayName);
    // 在本页输出面板里跑一条命令
    void runCommand(const QString &displayName, const QStringList &programAndArgs);
    void stopRunningCommand();
    void refreshProcessOutput();
    void appendNote(const QString &text);

    QLineEdit *m_hostEdit = nullptr;
    QPlainTextEdit *m_output = nullptr;
    QLabel *m_runStatus = nullptr;
    QProcess *m_process = nullptr;

    QString m_header;      // 输出面板顶部那行「> 命令」
    QByteArray m_rawOutput;   // 累积原始字节,每次整体解码,避免多字节字符被切断
};

} // namespace ws
