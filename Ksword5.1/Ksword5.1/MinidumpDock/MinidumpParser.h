#pragma once

// ============================================================
// MinidumpParser.h
// 作用：
// - 声明转储文件解析总入口 ParseDumpFile；
// - 入口内部自动判别文件类别：
//   MDMP（用户态 minidump）在本模块解析，
//   PAGEDU64/PAGEDUMP（内核转储）转交 KernelDumpParser 解析。
// 调用方式：
// - MinidumpDock 在线程池 worker 里调用 ParseDumpFile(filePath)；
// - 返回值 DumpParseResult 自包含全部展示数据，与文件映射无生命周期关联。
// ============================================================

#include "MinidumpFormat.h"

namespace ks::minidump
{
    // ParseDumpFile 作用：打开并解析一个转储文件。
    // 传入 filePath 转储文件完整路径；返回自包含的解析结果：
    // - recognized=false：签名不是受支持的转储格式；
    // - recognized=true 且 success=false：格式已识别但文件损坏/截断，errorText 给出原因；
    // - success=true：核心信息可展示，非致命问题记录在 diagnostics。
    // 本函数只读目标文件，可安全用于任意来源的样本；内部所有偏移访问均有边界检查。
    // Cancellation throws DumpWorkCancelled; partial results are never published.
    DumpParseResult ParseDumpFile(
        const QString& filePath, const DumpCancellationToken& cancellation = {});
}
