#pragma once

// ============================================================
// CrashHistory.h
// 作用：
// - 读系统事件日志，把最近一段时间的崩溃事实按时间线汇总起来：
//   蓝屏记录（停止码 + 参数 + 转储路径）、非正常关机、以及文件系统筛选器
//   的加载记录（含映像时间戳）；
// - 回答两个单看一个转储永远答不出来的问题：
//   1) 这次到底是蓝屏还是「没有转储的硬挂死」——两者症状相似，
//      但前者有转储可查，后者连转储都不会产生，排查手段完全不同；
//   2) 崩溃当时系统里跑的到底是哪一次构建的驱动。
//
// 为什么需要它（实战教训）：
// - 修完一个驱动缺陷、重新编译加载之后又崩了，光看转储无法确定崩的是不是
//   修复版——直到从筛选器加载记录里读出映像时间戳，和构建时间对上，
//   才确认「修复确实没生效」而不是「装错了版本」，避免了一整轮误判。
// - 同一晚还发生过两次没有转储的硬挂死。事件日志里它们表现为
//   BugcheckCode=0 的非正常关机；如果不把这一类和蓝屏区分开，
//   就会把两件完全不同的事混在一起找原因。
//
// 局限：
// - 只读系统日志（普通用户可读），不碰安全日志；
// - 事件日志被清空或滚动覆盖后就查不到，时间窗口越长越可能不完整。
// ============================================================

#include "DumpCancellation.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

namespace ks::minidump
{
    // CrashEventKind：时间线上一条记录的性质。
    enum class CrashEventKind
    {
        BugCheck,      // BugCheck：一次蓝屏，有停止码也有转储文件。
        HardHang,      // HardHang：非正常关机但没有停止码——没有转储的硬挂死。
        BugCheckReboot,// BugCheckReboot：非正常关机且带停止码，与某次蓝屏对应。
        FilterLoad,    // FilterLoad：文件系统筛选器加载，带映像时间戳。
    };

    // CrashHistoryEntry：时间线上的一条记录。
    struct CrashHistoryEntry
    {
        QDateTime time;                             // time：事件时间（本地时区）。
        CrashEventKind kind = CrashEventKind::BugCheck; // kind：记录性质。
        QString kindText;                           // kindText：性质的中文短语。
        QString summary;                            // summary：一句话摘要。
        QString detail;                             // detail：关键字段明细。
        std::uint32_t bugCheckCode = 0;             // bugCheckCode：停止码，无则为 0。
        QString dumpPath;                           // dumpPath：转储文件路径，可为空。
    };

    // CollectCrashHistory 作用：汇总最近 maxDays 天的崩溃/挂死/筛选器加载记录。
    // 传入 maxDays 时间窗口天数（<=0 视为 30）、filterNameFilter 筛选器名过滤子串
    //（为空则不收集筛选器加载记录，避免刷出上百条无关驱动）、errorOut 失败原因输出；
    // 返回按时间倒序排列的记录；读日志失败时返回空表并写入 errorOut。
    std::vector<CrashHistoryEntry> CollectCrashHistory(
        int maxDays,
        const QString& filterNameFilter,
        QString* errorOut, const DumpCancellationToken& cancellation = {});

    // CrashEventKindText 作用：把记录性质转成中文短语。
    // 传入 kind；返回可直接进表格的文本。
    QString CrashEventKindText(CrashEventKind kind);
}
