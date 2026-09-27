#pragma once

// 所有功能页的基类。
//
// 主窗口只认这一套接口:标题、副标题、三种快照回调、首次激活回调。
// 这样主窗口不需要知道任何具体页面的类型,加新页面只改 MainWindow 的一处列表。

#include "core/NetConnectionSampler.h"
#include "core/Types.h"

#include <QWidget>

namespace ws {

class PageBase : public QWidget
{
    Q_OBJECT

public:
    explicit PageBase(QWidget *parent = nullptr)
        : QWidget(parent)
    {
    }

    virtual QString pageTitle() const = 0;
    virtual QString pageSubtitle() const = 0;

    // 三种快照,页面按需重写
    virtual void onSystemSnapshot(const SystemSnapshot &snapshot) { Q_UNUSED(snapshot); }
    virtual void onProcessSnapshot(const ProcessSnapshot &snapshot) { Q_UNUSED(snapshot); }
    virtual void onConnections(const QVector<NetConnection> &connections) { Q_UNUSED(connections); }

    // 每次切到本页时调用。昂贵的一次性加载(SystemInfo / 服务枚举 / 启动项枚举)放这里,
    // 避免程序一启动就把所有东西查一遍。
    virtual void onActivated() {}

    // 采样暂停/继续时通知页面,暂停期间不要再刷表格
    virtual void onSamplingPaused(bool paused) { Q_UNUSED(paused); }
};

} // namespace ws
