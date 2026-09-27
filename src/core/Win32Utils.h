#pragma once

// WinScope 的 Win32 公共工具。
// 集中放编码转换、格式化、权限、进程查询这类到处都要用的零碎逻辑,
// 避免每个采样器各写一份。

#include <QString>
#include <QtGlobal>
#include <windows.h>

namespace ws {

// ------------------------------------------------------------ 编码

QString fromWide(const wchar_t *s, int len = -1);
QString fromWide(const std::wstring &s);
std::wstring toWide(const QString &s);

// 内核返回的 UNICODE_STRING 是定长缓冲,不一定以 \0 结尾
QString fromUnicodeBuffer(const wchar_t *buf, int capacity);

// ------------------------------------------------------------ 格式化

QString formatBytes(quint64 bytes);              // "1.23 GB"
QString formatBytesPerSec(double bytesPerSec);   // "1.23 MB/s"
QString formatCount(quint64 n);                  // 1234 -> "1,234"
QString formatDuration(qint64 seconds);          // "3 天 4 小时 12 分"
QString formatDateTime(qint64 epochMs);          // "2026-09-27 19:02:30"
QString formatPercent(double v, int decimals = 0);
// WMI 的日期是 "20200826000000.000000+000" 这种紧凑格式,转成 "2020-08-26"
QString formatWmiDate(const QString &raw);

// ------------------------------------------------------------ 权限

bool isProcessElevated();
// 打开当前进程的 SeDebugPrivilege,没有它拿不到其它用户进程的路径/命令行
bool enableDebugPrivilege();
// 用 runas 重新拉起自己,成功则调用方应立即退出
bool relaunchElevated();

// ------------------------------------------------------------ 时间

// 系统启动时刻(epoch 毫秒),用于换算进程启动时间
qint64 systemBootTimeMs();

// ------------------------------------------------------------ 进程查询

QString queryProcessPath(quint32 pid);
QString queryProcessUser(quint32 pid);
QString queryProcessArchitecture(HANDLE process);
QString queryProcessDescription(const QString &filePath);   // 版本资源里的文件说明
QString queryProcessPublisher(const QString &filePath);

// 判断进程是否"正在响应":发一个 0 超时的消息探测
bool isProcessResponding(HANDLE process);

// ------------------------------------------------------------ COM

// RAII 包装:保证当前线程初始化过 COM。
// 任务计划(ITaskService)这类接口在没初始化 COM 的线程里会直接创建失败,
// 而且失败得很安静,所以调用前先拿一个这个对象。
class ComInitializer
{
public:
    ComInitializer();
    ~ComInitializer();
    bool ready() const { return m_ready; }

    ComInitializer(const ComInitializer &) = delete;
    ComInitializer &operator=(const ComInitializer &) = delete;

private:
    bool m_ready = false;
    bool m_owner = false;   // 本次是否由我们调用了 CoInitializeEx
};

// ------------------------------------------------------------ 其它

// 打开文件/文件夹所在位置(资源管理器选中)
void revealInExplorer(const QString &path);
// 以管理员身份打开一个 shell 命令(用于工具箱)
bool runElevatedCommand(const QString &exe, const QString &args);
// 用 ShellExecute 启动目标。.msc / 控制面板项这类东西不能直接 CreateProcess,
// 必须走 shell。elevated=true 时用 runas 动词提权。
bool shellOpen(const QString &target, const QString &args = QString(), bool elevated = false);

} // namespace ws
