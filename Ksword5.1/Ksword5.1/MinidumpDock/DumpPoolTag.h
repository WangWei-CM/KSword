#pragma once

// ============================================================
// DumpPoolTag.h
// 作用：
// - 从停止码参数与崩溃点寄存器里识别出「池标记」（4 个可打印字符），
//   并回答最关键的一个问题：这块出问题的内存是谁分配的；
// - 归属手段有两条，都不靠猜：
//   1) 读 WDK 调试器自带的 triage\pooltag.txt（存在时）；
//   2) 在已加载模块的磁盘映像里搜这 4 个字节——驱动调用
//      ExAllocatePoolWithTag 时标记是立即数，必然出现在自己的映像里。
//
// 为什么这件事值得单独做（实战教训）：
// - 一次 0x13A KERNEL_MODE_HEAP_CORRUPTION 的崩溃栈上全是 ndis/NETIO，
//   下一次换成了 conhost，两次都与真正的肇事者毫无关系——池损坏的“发现者”
//   只是碰巧下一个来分配内存的人。真正把矛头指向肇事驱动的，是被判定损坏
//   的那个池块的归属标记。少了这一步，栈上那些名字只会把人往错的方向带。
// - 0x50 的现场同理：崩在 ExFreePoolWithTag 时，标记就在参数寄存器里，
//   它直接说明“这次释放的是谁的内存”。
// 局限：
// - 三元组转储（本机蓝屏产生的 Minidump）通常不含池块所在的内存页，
//   因此拿不到池头里的标记，只能从参数与寄存器里取；
// - 映像搜索只能证明“某模块的代码里出现过这 4 个字节”，是强线索而非铁证，
//   命中多个模块时一律全部列出，不替使用者做选择。
// ============================================================

#include "MinidumpFormat.h"

namespace ks::minidump
{
    // LooksLikePoolTag 作用：判断一个 32 位值是否像池标记。
    // 传入 value；返回是否 4 个字节都落在池标记常用字符集内。
    // 判据从严：池标记按约定是可打印 ASCII，通常为字母数字，末位允许空格。
    bool LooksLikePoolTag(std::uint32_t value);

    // PoolTagText 作用：把 32 位值按小端还原成 4 字符文本。
    // 传入 value；返回如 "KsFi"。不做合法性检查，调用前先过 LooksLikePoolTag。
    QString PoolTagText(std::uint32_t value);

    // ApplyPoolTagAttribution 作用：识别池标记候选并就地写入 result.poolTags。
    // 传入 result 解析结果（就地修改）；命中时同时向 analysis.findings 追加一条结论。
    // 只在停止码或寄存器里确实出现了像池标记的值时才产出，不制造噪声。
    void ApplyPoolTagAttribution(
        DumpParseResult& result, const DumpCancellationToken& cancellation = {});
}
