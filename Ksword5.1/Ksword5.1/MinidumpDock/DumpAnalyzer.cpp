// ============================================================
// DumpAnalyzer.cpp
// 作用：
// - 实现 DumpAnalyzer.h 声明的综合诊断；
// - 归因流程：收集证据 → 按模块累加权重 → 系统模块降权 →
//   排序取头名 → 结合停止码/异常码生成结论与建议；
// - 所有结论都基于本次解析已经拿到的事实，不做外部查询，
//   也不对“该驱动是否真的有 bug”下断言，只给可信度与证据链。
// ============================================================

#include "DumpAnalyzer.h"

#include "DumpBugCheckText.h"
#include "MinidumpCodeText.h"

#include <QHash>

#include <algorithm>

namespace ks::minidump
{
    namespace
    {
        // kSystemModules：操作系统自带的核心模块名（小写比较）。
        // 这些模块几乎必然出现在任何一条内核/用户态调用栈上，
        // 直接按最高权重归因给它们只会得到“ntoskrnl.exe 导致蓝屏”这种废话，
        // 因此统一降权，让第三方驱动/组件浮出来。
        const char* const kSystemModules[] = {
            // 内核与 HAL
            "ntoskrnl.exe", "ntkrnlmp.exe", "ntkrnlpa.exe", "ntkrpamp.exe",
            "hal.dll", "halmacpi.dll", "halacpi.dll",
            // 内核基础设施
            "win32k.sys", "win32kbase.sys", "win32kfull.sys", "ci.dll",
            "clfs.sys", "cng.sys", "ksecdd.sys", "msrpc.sys", "tm.sys",
            "pshed.dll", "bootvid.dll", "kdcom.dll", "werkernel.sys",
            // 常见系统驱动栈
            "ntfs.sys", "fltmgr.sys", "volsnap.sys", "volmgr.sys", "partmgr.sys",
            "storport.sys", "storahci.sys", "disk.sys", "classpnp.sys",
            "acpi.sys", "pci.sys", "pcw.sys", "wdf01000.sys", "wdfldr.sys",
            "ndis.sys", "netio.sys", "tcpip.sys", "afd.sys", "http.sys",
            "dxgkrnl.sys", "dxgmms1.sys", "dxgmms2.sys", "watchdog.sys",
            "usbxhci.sys", "usbport.sys", "usbhub.sys", "ucx01000.sys",
            // 用户态核心
            "ntdll.dll", "kernel32.dll", "kernelbase.dll", "user32.dll",
            "gdi32.dll", "gdi32full.dll", "advapi32.dll", "rpcrt4.dll",
            "combase.dll", "ole32.dll", "oleaut32.dll", "sechost.dll",
            "msvcrt.dll", "ucrtbase.dll", "shcore.dll", "shell32.dll",
            "win32u.dll", "wow64.dll", "wow64cpu.dll", "wow64win.dll",
        };

        // kFaultingAddressWeight：崩溃指令地址命中的权重，这是最强证据。
        constexpr int kFaultingAddressWeight = 100;
        // kContextFrameWeight：CONTEXT 直接给出的栈顶帧权重，与崩溃地址同级。
        constexpr int kContextFrameWeight = 100;
        // kScannedFrameBaseWeight/kScannedFrameDecay：扫描所得栈帧的基础权重与逐帧衰减。
        constexpr int kScannedFrameBaseWeight = 60;
        constexpr int kScannedFrameDecay = 3;
        // kScannedFrameMinWeight：衰减后的下限，深处的帧仍保留少量票数。
        constexpr int kScannedFrameMinWeight = 8;
        // kParameterAddressWeight：BugCheck/异常参数里的地址命中权重。
        constexpr int kParameterAddressWeight = 40;
        // kUnloadedBonus：命中已卸载模块的额外加权——驱动卸载后仍被调用
        // 是极强的根因信号，几乎可以直接定案。
        constexpr int kUnloadedBonus = 70;
        // kSystemModuleDivisor：系统核心模块的权重除数。
        constexpr int kSystemModuleDivisor = 5;
        // kMaxBlameEntries：结论里最多保留的候选数。
        constexpr std::size_t kMaxBlameEntries = 6;
        // kMaxEvidencePerModule：单个候选最多记录的证据条数，防止刷屏。
        constexpr int kMaxEvidencePerModule = 6;

        // Hex 作用：把数值格式化成 0x 大写十六进制文本。
        QString Hex(const std::uint64_t value)
        {
            return QStringLiteral("0x%1").arg(QString::number(value, 16).toUpper());
        }

        // BlameAccumulator 作用：按模块名累加证据权重的中间容器。
        struct BlameAccumulator
        {
            // Candidate：一个候选模块的累加状态。
            // bestWeight 记录迄今最强证据的权重，用来决定谁有资格设定代表地址。
            struct Candidate
            {
                BlameEntry entry;   // entry：对外输出的候选条目。
                int bestWeight = 0; // bestWeight：已见过的最大单条证据权重。
            };

            // add 作用：登记一条命中证据。
            // 传入 note 地址解读结果、weight 本条证据权重与 evidence 证据描述。
            void add(const AddressNote& note, const int weight, const QString& evidence)
            {
                if (note.moduleName.isEmpty() || weight <= 0)
                {
                    return;
                }
                // key：模块名小写，避免同一模块因大小写不同被拆成两个候选。
                const QString key = note.moduleName.toLower();
                Candidate& candidate = entries[key];
                BlameEntry& entry = candidate.entry;
                if (entry.moduleName.isEmpty())
                {
                    entry.moduleName = note.moduleName;
                    entry.moduleBase = note.moduleBase;
                    entry.address = note.moduleBase + note.offset;
                    entry.offset = note.offset;
                    entry.unloadedModule = note.unloadedModule;
                }
                // 系统模块降权：它们出现在栈上是常态，不能据此定案。
                int effectiveWeight = IsSystemModule(note.moduleName)
                    ? weight / kSystemModuleDivisor
                    : weight;
                if (note.unloadedModule)
                {
                    // 加权要与证据强度挂钩：崩溃点落在已卸载模块里几乎可以定案，
                    // 值得满额加权；而它只是出现在某个扫描帧上时，很可能只是
                    // 栈上的残留值，满额加权会让它无条件顶到候选第一名。
                    effectiveWeight += weight >= kContextFrameWeight
                        ? kUnloadedBonus
                        : kUnloadedBonus / 4;
                    entry.unloadedModule = true;
                }
                entry.weight += effectiveWeight;
                if (entry.evidence.size() < kMaxEvidencePerModule && !evidence.isEmpty())
                {
                    entry.evidence.append(evidence);
                }
                // 代表地址只被“迄今最强的那一条证据”接管。
                // 若改成“达到某个阈值就覆盖”，后来的次强证据（例如调用栈第 3 帧）
                // 会把崩溃指令地址挤掉，候选里显示的偏移就不再是崩溃点。
                if (effectiveWeight > candidate.bestWeight)
                {
                    candidate.bestWeight = effectiveWeight;
                    entry.address = note.moduleBase + note.offset;
                    entry.offset = note.offset;
                }
            }

            // sorted 作用：按权重降序导出候选列表，最多 kMaxBlameEntries 条。
            // QHash 的遍历顺序不稳定，排序时用模块名做同分兜底，保证输出可复现。
            std::vector<BlameEntry> sorted() const
            {
                std::vector<BlameEntry> list;
                list.reserve(static_cast<std::size_t>(entries.size()));
                for (auto iterator = entries.cbegin(); iterator != entries.cend(); ++iterator)
                {
                    list.push_back(iterator.value().entry);
                }
                std::sort(
                    list.begin(),
                    list.end(),
                    [](const BlameEntry& left, const BlameEntry& right)
                    {
                        if (left.weight != right.weight)
                        {
                            return left.weight > right.weight;
                        }
                        return left.moduleName < right.moduleName;
                    });
                if (list.size() > kMaxBlameEntries)
                {
                    list.resize(kMaxBlameEntries);
                }
                return list;
            }

            QHash<QString, Candidate> entries; // entries：模块名小写 → 累加中的候选。
        };

        // CategoryFromExceptionCode 作用：给用户态异常码归类，供结论使用。
        // 传入 code 异常码；返回最接近的故障归类。
        BugCheckCategory CategoryFromExceptionCode(const std::uint32_t code)
        {
            switch (code)
            {
            case 0xC0000005u: // EXCEPTION_ACCESS_VIOLATION
            case 0xC0000006u: // EXCEPTION_IN_PAGE_ERROR
            case 0xC00000FDu: // EXCEPTION_STACK_OVERFLOW
                return BugCheckCategory::Memory;
            case 0xC0000374u: // STATUS_HEAP_CORRUPTION
            case 0xC0000409u: // STATUS_STACK_BUFFER_OVERRUN
            case 0xC0000602u: // STATUS_FAIL_FAST_EXCEPTION
                return BugCheckCategory::Security;
            case 0xC0000135u: // STATUS_DLL_NOT_FOUND
            case 0xC0000138u: // STATUS_ORDINAL_NOT_FOUND
            case 0xC0000139u: // STATUS_ENTRYPOINT_NOT_FOUND
            case 0xC0000142u: // STATUS_DLL_INIT_FAILED
            case 0xC000007Bu: // STATUS_INVALID_IMAGE_FORMAT
                return BugCheckCategory::Boot;
            default:
                return BugCheckCategory::Software;
            }
        }

        // UserExceptionSuggestions 作用：按用户态异常码给出排查建议。
        // 传入 code 异常码；返回若干条中文建议。
        QStringList UserExceptionSuggestions(const std::uint32_t code)
        {
            switch (code)
            {
            case 0xC0000005u:
                return {
                    QStringLiteral("确认崩溃地址所属模块：若是自家模块，用同版本 PDB 在调试器里定位到具体函数。"),
                    QStringLiteral("访问地址落在 NULL 页时，检查该指针的来源函数是否漏判返回值失败。"),
                    QStringLiteral("访问地址是哨兵值（0xCCCC…/0xDDDD…/0xFEEEFEEE）时，按“未初始化”或“释放后使用”方向排查。"),
                    QStringLiteral("崩溃点在第三方模块（注入的钩子、杀软、输入法、录屏组件）时，先在干净环境复现以排除干扰。"),
                };
            case 0xC0000374u:
                return {
                    QStringLiteral("堆损坏的崩溃点通常离真正的越界写很远，必须开页堆定位：gflags /p /enable <exe> /full。"),
                    QStringLiteral("用 Application Verifier 打开堆检查，能在越界发生的那一刻断下来。"),
                    QStringLiteral("重点检查最近改动过的缓冲区拷贝、数组下标与 realloc 之后的旧指针使用。"),
                };
            case 0xC0000409u:
            case 0xC0000602u:
                return {
                    QStringLiteral("这是主动终止而非被动崩溃：参数 1 的 fast-fail 子码说明了触发的具体安全检查。"),
                    QStringLiteral("子码为栈 cookie 检查失败时，查栈上缓冲区的写入长度计算。"),
                    QStringLiteral("子码为 CFG/间接调用检查失败时，查函数指针是否被覆盖或指向非法目标。"),
                };
            case 0xC00000FDu:
                return {
                    QStringLiteral("检查是否存在无限递归：崩溃线程的调用栈里会出现同一模块的地址反复出现。"),
                    QStringLiteral("检查是否在栈上分配了超大对象或用了过大的 alloca/可变长数组。"),
                    QStringLiteral("线程栈保留大小不足时，可通过链接选项 /STACK 或 CreateThread 参数调大。"),
                };
            case 0xC0000135u:
            case 0xC0000138u:
            case 0xC0000139u:
            case 0xC0000142u:
            case 0xC000007Bu:
                return {
                    QStringLiteral("这是加载期失败而非运行期崩溃：核对依赖 DLL 的位数、版本与部署路径。"),
                    QStringLiteral("用依赖查看工具比对目标机上的实际加载结果，注意 SxS/清单与运行库版本。"),
                    QStringLiteral("查看模块列表里该 DLL 是否被加载到了非预期路径（DLL 劫持）。"),
                };
            case 0xE06D7363u:
                return {
                    QStringLiteral("这是一个未被接住的 C++ 异常：崩溃点是抛出点而不是缺陷点。"),
                    QStringLiteral("参数里的异常对象指针配合 PDB 可以还原异常类型与消息。"),
                    QStringLiteral("检查线程入口/回调边界是否缺少 catch，跨模块抛异常尤其容易漏接。"),
                };
            default:
                return {
                    QStringLiteral("用同版本 PDB 在调试器中打开本转储，可把崩溃点从“模块+偏移”还原到具体函数与行号。"),
                    QStringLiteral("对照模块列表确认崩溃模块的版本，与已知问题的修复版本比对。"),
                };
            }
        }
    }

    bool IsSystemModule(const QString& moduleName)
    {
        if (moduleName.isEmpty())
        {
            return false;
        }
        // baseName：只比较文件名部分，且统一小写。
        const QString baseName = BaseModuleName(moduleName).toLower();
        for (const char* const name : kSystemModules)
        {
            if (baseName == QLatin1String(name))
            {
                return true;
            }
        }
        return false;
    }

    QString AnalysisConfidenceText(const AnalysisConfidence confidence)
    {
        switch (confidence)
        {
        case AnalysisConfidence::High:   return QStringLiteral("高（崩溃指令直接落在该模块内）");
        case AnalysisConfidence::Medium: return QStringLiteral("中（有地址级证据，但来自栈扫描或指向系统模块）");
        case AnalysisConfidence::Low:    return QStringLiteral("低（仅按停止码分类推断，无地址级证据）");
        default:                         return QStringLiteral("无结论");
        }
    }

    void BuildAnalysis(const ModuleIndex& modules, DumpParseResult& result)
    {
        CheckDumpCancellation();
        DumpAnalysis analysis{};
        BlameAccumulator accumulator;
        // strongEvidenceModules：拿到过“崩溃指令级证据”的模块名（小写）。
        // 只记一个布尔量是不够的：可信度文案宣称“崩溃指令直接落在该模块内”，
        // 而候选第一名未必就是承载那条强证据的模块——A 模块出现在崩溃点、
        // B 模块靠一堆扫描帧刷到了更高票，此时给 B 打 High 就是撒谎。
        QStringList strongEvidenceModules;

        // ---------- 证据 1：崩溃指令地址 ----------
        if (result.faultingAddress != 0)
        {
            const AddressNote note = modules.resolve(result.faultingAddress);
            if (!note.moduleName.isEmpty())
            {
                strongEvidenceModules.append(note.moduleName.toLower());
                accumulator.add(
                    note,
                    kFaultingAddressWeight,
                    QStringLiteral("崩溃指令地址 %1 位于 %2")
                        .arg(Hex(result.faultingAddress), note.symbolText));
            }
        }

        // ---------- 证据 2：崩溃线程的调用栈 ----------
        // 只有崩溃线程的栈才参与归因。转储里通常有上百个线程，绝大多数停在
        // 各自的等待点上，把它们的栈帧也计入会让票数被无关模块淹没，
        // 结论最后总会指向 ntdll/kernel32 这类到处都在的模块。
        // 内核转储没有线程 ID，全部帧的 threadId 都是 0，正好等同于“崩溃线程”。
        const std::uint32_t blameThreadId =
            result.kind == DumpKind::UserMinidump ? result.faultingThreadId : 0u;
        for (const StackFrameEntry& frame : result.stackFrames)
        {
            CheckDumpCancellation();
            if (frame.moduleName.isEmpty() || frame.threadId != blameThreadId)
            {
                continue;
            }
            // 重新解析一次拿到准确的 base/offset，不依赖帧里已格式化好的文本。
            const AddressNote note = modules.resolve(frame.address);
            if (note.moduleName.isEmpty())
            {
                continue;
            }
            if (frame.fromContext)
            {
                strongEvidenceModules.append(note.moduleName.toLower());
                accumulator.add(
                    note,
                    kContextFrameWeight,
                    QStringLiteral("崩溃线程栈顶为 %1").arg(note.symbolText));
            }
            else
            {
                // 越靠近栈顶的帧越可能属于真实调用链，权重逐帧衰减。
                const int weight = std::max(
                    kScannedFrameMinWeight,
                    kScannedFrameBaseWeight - frame.index * kScannedFrameDecay);
                accumulator.add(
                    note,
                    weight,
                    QStringLiteral("疑似调用栈第 %1 帧为 %2")
                        .arg(frame.index)
                        .arg(note.symbolText));
            }
        }

        // ---------- 证据 3：停止码 / 异常参数里的地址 ----------
        if (result.kind != DumpKind::UserMinidump && result.bugCheckCode != 0)
        {
            for (int index = 0; index < 4; ++index)
            {
                const std::uint64_t parameter = result.bugCheckParameters[index];
                if (parameter == 0)
                {
                    continue;
                }
                const AddressNote note = modules.resolve(parameter);
                if (note.moduleName.isEmpty())
                {
                    continue;
                }
                accumulator.add(
                    note,
                    kParameterAddressWeight,
                    QStringLiteral("停止码参数 %1（%2）落在 %3")
                        .arg(index + 1)
                        .arg(Hex(parameter), note.symbolText));
            }
        }

        analysis.blame = accumulator.sorted();

        // ---------- 组装结论 ----------
        // topBlame：权重最高的候选；为空说明没有任何地址级证据。
        const BlameEntry* topBlame = analysis.blame.empty() ? nullptr : &analysis.blame.front();
        // suspectThirdParty：头名是不是第三方模块，决定结论口径。
        const bool suspectThirdParty =
            topBlame != nullptr && !IsSystemModule(topBlame->moduleName);

        if (result.kind == DumpKind::UserMinidump)
        {
            // ===== 用户态：以异常码为主线 =====
            const std::uint32_t code = result.exceptionCode;
            const QString codeName = ExceptionCodeName(code);
            const QString meaning = ExceptionCodeMeaning(code);

            if (code == 0)
            {
                // 没有异常记录不等于没话可说：这句结论恰恰是使用者最需要看到的，
                // 否则他会对着一堆线程和模块表找一个根本不存在的崩溃点。
                // 故障归类留空——快照转储没有“故障”可归类，硬套一个只会误导。
                analysis.headline = QStringLiteral(
                    "本转储不含异常记录：多半是主动生成的快照（任务管理器/ProcDump），而不是崩溃现场；"
                    "下面的线程、模块与内存信息仍然有效，但没有崩溃点可供归因。");
                analysis.confidence = AnalysisConfidence::None;
            }
            else
            {
                analysis.category = BugCheckCategoryText(CategoryFromExceptionCode(code));
                // headline：模块 + 异常码名 + 含义，一句话交代清楚“谁在哪里因为什么崩了”。
                // 位置优先取崩溃指令地址本身；它归不到模块时才退回权重最高的候选。
                const QString faultSymbol = result.faultingAddress != 0
                    ? modules.symbolText(result.faultingAddress)
                    : QString();
                QString where = faultSymbol;
                if (where.isEmpty())
                {
                    // 崩溃地址归不到模块时，不能改用候选第一名的偏移来填这句话——
                    // 那个偏移来自栈扫描帧，与崩溃点无关，而同一份报告下面还写着
                    // “崩溃指令地址未落在任何已知模块区间内”，两句话直接打架。
                    where = result.faultingAddress != 0
                        ? QStringLiteral("%1（不属于任何已加载模块）")
                            .arg(Hex(result.faultingAddress))
                        : QStringLiteral("未知位置");
                }
                analysis.headline = QStringLiteral("进程崩溃于 %1，异常 %2%3。")
                    .arg(where)
                    .arg(Hex(code))
                    .arg(codeName.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(codeName));
                if (!meaning.isEmpty())
                {
                    analysis.findings.append(meaning);
                }
            }
            analysis.suggestions = UserExceptionSuggestions(code);
        }
        else
        {
            // ===== 内核态：以停止码为主线 =====
            const std::uint32_t code = result.bugCheckCode;
            const QString codeName = BugCheckCodeNameEx(code);
            const QString meaning = BugCheckMeaning(code);
            const BugCheckCategory category = BugCheckCategoryOf(code);
            analysis.category = BugCheckCategoryText(category);

            // codeText：停止码数值 + 官方名称，结论与证据里反复用到。
            const QString codeText = codeName.isEmpty()
                ? Hex(code)
                : QStringLiteral("%1 (%2)").arg(Hex(code), codeName);
            if (suspectThirdParty)
            {
                analysis.headline = QStringLiteral("疑似由 %1 引起的 %2。")
                    .arg(topBlame->moduleName, codeText);
            }
            else if (topBlame != nullptr)
            {
                analysis.headline = QStringLiteral("%1；崩溃点位于系统模块 %2，通常说明真正的触发者在调用链更上游。")
                    .arg(codeText, topBlame->moduleName);
            }
            else
            {
                analysis.headline = QStringLiteral("%1；本转储未能把崩溃地址归属到任何模块。")
                    .arg(codeText);
            }
            if (!meaning.isEmpty())
            {
                analysis.findings.append(meaning);
            }
            analysis.suggestions = BugCheckSuggestions(code);

            // 硬件类停止码额外提醒：这类问题换驱动无效。
            if (category == BugCheckCategory::Hardware)
            {
                analysis.findings.append(QStringLiteral(
                    "该停止码由硬件错误报告机制产生，通常不是某个驱动的代码缺陷，"
                    "优先按内存/CPU/供电/超频方向排查。"));
            }
        }

        // ---------- 证据链补充 ----------
        if (result.faultingAddress != 0)
        {
            const QString symbol = modules.symbolText(result.faultingAddress);
            analysis.findings.append(
                symbol.isEmpty()
                    ? QStringLiteral("崩溃指令地址 %1（未落在任何已知模块区间内，可能是已释放的代码页或动态生成的代码）。")
                        .arg(Hex(result.faultingAddress))
                    : QStringLiteral("崩溃指令地址 %1 位于 %2。")
                        .arg(Hex(result.faultingAddress), symbol));
        }
        // 已卸载模块命中值得单独提示，但措辞不能写成断言：
        // 栈上到处是历史残留值，一个旧模块的地址出现在栈里，可能是真的还在被调用，
        // 也可能只是没被覆盖掉的垃圾。只有当它同时是崩溃指令级证据时才敢下结论。
        for (const BlameEntry& blame : analysis.blame)
        {
            if (!blame.unloadedModule)
            {
                continue;
            }
            const bool fromCrashPoint =
                strongEvidenceModules.contains(blame.moduleName.toLower());
            analysis.findings.append(
                fromCrashPoint
                    ? QStringLiteral("崩溃点落在已卸载的模块 %1 内：驱动卸载后仍有代码在执行，"
                        "这是典型的“卸载时未取消挂起操作”缺陷。")
                        .arg(blame.moduleName)
                    : QStringLiteral("栈上出现了已卸载模块 %1 的地址。这可能说明它卸载后仍被回调，"
                        "也可能只是栈上尚未被覆盖的残留值——需要结合是否反复出现来判断。")
                        .arg(blame.moduleName));
            break;
        }
        if (!result.stackFrames.empty())
        {
            // faultingFrames：崩溃线程自己的帧数——归因只看这些，报告里要说清楚。
            std::size_t faultingFrames = 0;
            for (const StackFrameEntry& frame : result.stackFrames)
            {
                if (frame.threadId == blameThreadId)
                {
                    ++faultingFrames;
                }
            }
            analysis.findings.append(
                QStringLiteral("调用栈由栈内存扫描重建（无符号），崩溃线程 %1 帧、全部线程共 %2 帧；"
                    "顺序为近似值且可能含残留帧，归因只采用崩溃线程的帧。")
                    .arg(faultingFrames)
                    .arg(result.stackFrames.size()));
        }

        // ---------- 可信度 ----------
        if (topBlame == nullptr)
        {
            analysis.confidence = result.bugCheckCode != 0 || result.exceptionCode != 0
                ? AnalysisConfidence::Low
                : AnalysisConfidence::None;
        }
        else if (suspectThirdParty &&
                 strongEvidenceModules.contains(topBlame->moduleName.toLower()))
        {
            analysis.confidence = AnalysisConfidence::High;
        }
        else
        {
            analysis.confidence = AnalysisConfidence::Medium;
        }
        // 用户态无异常记录的快照不给可信度，前面已置为 None，这里不覆盖。
        if (result.kind == DumpKind::UserMinidump && result.exceptionCode == 0)
        {
            analysis.confidence = AnalysisConfidence::None;
        }

        result.analysis = std::move(analysis);
    }
}
