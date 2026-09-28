#pragma once

// 小窗能显示哪些指标,集中在这一张表里。
//
// 这张表同时被三处消费,所以只能有一份:
//   · 小窗自己(按用户勾选的顺序画行)
//   · 参数选择对话框(列出可选项)
//   · 采样范围计算(把勾中的指标各自需要哪几路采样并起来)
// 如果三处各写一份,加了指标之后迟早有一处忘了同步,而"忘了"的表现是小窗
// 显示空白、或者后台白采一路数据 —— 两种都不好查。
//
// 取值函数只做"从快照里读 + 格式化",不碰任何界面类型,所以自检工具也能用

#include "core/Types.h"

#include <QVector>

namespace ws {

// 指标的稳定标识。
//
// 写进 QSettings 的是 key(),所以**已有的 id 和 key 都不能改**,只能往后追加:
// 改了等于把用户攒下的配置清空
enum class MetricId {
    CpuUsage,
    CpuFrequency,
    CpuTemperature,
    CpuPower,
    MemoryUsage,
    GpuUsage,
    GpuTemperature,
    NetworkThroughput,
    DiskActivity,
    Uptime,
};

// 一个指标当前的值。text 已经格式化好,界面直接画
struct MetricValue {
    // 空字符串表示这台机器读不到,小窗会留空而不是编一个数出来
    QString text;
    // 0..1,给小窗里的细横条和配色用。-1 表示这个指标没有"满量程"的概念
    // (主频、功耗、运行时间都属于这种),这时只画文字不画横条
    double ratio = -1.0;
    LoadLevel level = LoadLevel::Normal;
};

struct MetricDef {
    MetricId id = MetricId::CpuUsage;
    QString key;            // QSettings 键名,ASCII
    QString label;          // 小窗里显示的名字
    QString hint;           // 选择对话框里的说明
    quint32 channels = 0;   // 需要哪几路采样(SampleScope::Bit 的位或)
    MetricValue (*read)(const SystemSnapshot &) = nullptr;
};

// 全部指标。顺序就是选择对话框里的顺序
const QVector<MetricDef> &metricDefs();

// 找不到返回 nullptr
const MetricDef *metricDef(MetricId id);

// 按 key 反查,用于读配置。找不到返回 nullptr
const MetricDef *metricDefByKey(const QString &key);

// 显示这些指标至少需要哪几路采样
SampleScope scopeForMetrics(const QVector<MetricId> &ids);

} // namespace ws
