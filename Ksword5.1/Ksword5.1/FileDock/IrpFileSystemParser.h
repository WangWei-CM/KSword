#pragma once

// ============================================================
// IrpFileSystemParser.h
// 作用：
// 1) 为 FileDock 提供"R0 自建 IRP 直发文件系统栈"的目录解析入口；
// 2) 同时取"绕过层"和"栈顶"两份视图，把差集作为疑似隐藏条目上报；
// 3) 只消费 ArkDriverClient 的结构化结果，不让 Dock UI 直接构造 IRP 协议包。
// ============================================================

#include "ManualFileSystemParser.h"

namespace ks::file
{
    // IrpScanDiagnostics 作用：
    // - 记录同一目录在两个栈层上的枚举差异；
    // - bypassOnlyNames 是核心产出：绕过过滤层才能看到的条目；
    // - layerBypassed 为 false 表示 R0 把请求回退到了栈顶，此时两份视图同源，
    //   差集恒为空，UI 不得把"无差异"解释为"确认没有隐藏项"。
    struct IrpScanDiagnostics
    {
        bool comparisonAvailable = false;   // 栈顶对照是否成功执行。
        bool layerBypassed = false;         // 本次是否真的投递到了更深的层。
        unsigned long requestedLayer = 0;   // 请求的栈层。
        unsigned long resolvedLayer = 0;    // R0 实际生效的栈层。
        int bypassEntryCount = 0;           // 绕过层视图条目数。
        int topLayerEntryCount = 0;         // 栈顶视图条目数。
        QStringList bypassOnlyNames;        // 只有绕过层能看到的条目。
        QStringList topLayerOnlyNames;      // 只有栈顶能看到的条目。
        QString bypassDriverName;           // 接收绕过层请求的驱动名。
        QString topLayerDriverName;         // 接收栈顶请求的驱动名。
    };

    // IrpFileSystemParser 作用：
    // - 通过 KswordARK 在内核自建 IRP 枚举目录，可选择投递到基础文件系统设备；
    // - 把固定协议行转换为 FileDock 已有的 ManualDirectoryEntry 模型。
    // 能力边界：
    // - 只有目录查询（IRP_MJ_DIRECTORY_CONTROL）绕过过滤层，打开（IRP_MJ_CREATE）
    //   仍走正常路径。因此"在目录查询结果里抹掉条目"这类隐藏能被发现，
    //   "只在打开时拦截"的则发现不了；差集为空不等于确认没有隐藏项。
    class IrpFileSystemParser final
    {
    public:
        // enumerateDirectory 作用：
        // - 用自建 IRP 枚举目录，默认投递到基础文件系统设备以绕过过滤层。
        // 调用方法：
        // - FileDock 选择"R0 IRP 解析"后在后台线程调用。
        // 入参 pathText：
        // - Windows 本地、卷 GUID 或 UNC 目录路径。
        // 出参 entriesOut：
        // - 绕过层视图的目录行。
        // 出参 fsTypeOut：
        // - 按 R0 返回的文件系统名映射，未知类型保留 Unknown。
        // 出参 errorTextOut：
        // - 失败时返回通信、协议或 NTSTATUS 诊断。
        // 出参 partialOut：
        // - true 表示驱动返回部分页、名称截断或达到 R3 总行预算。
        // 出参 sourceDetailOut：
        // - 返回用于状态栏的来源摘要（含实际生效栈层与接收驱动）。
        // 出参 diagnosticsOut：
        // - 可选，接收与栈顶视图的差异统计。
        // 返回值：
        // - 取得可信完整/部分目录结果时返回 true；完全失败返回 false。
        static bool enumerateDirectory(
            const QString& pathText,
            std::vector<ManualDirectoryEntry>& entriesOut,
            ManualFsType& fsTypeOut,
            QString& errorTextOut,
            bool* partialOut = nullptr,
            QString* sourceDetailOut = nullptr,
            IrpScanDiagnostics* diagnosticsOut = nullptr,
            const DirectoryCancellationCallback& canceled = {});

        // layerDisplayText 作用：
        // - 把 KSWORD_ARK_FILE_IRP_LAYER_* 映射为界面可读文案。
        static QString layerDisplayText(unsigned long layerValue);
    };
}
