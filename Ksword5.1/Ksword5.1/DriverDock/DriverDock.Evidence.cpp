#include "DriverDock.Internal.h"

using namespace ksword::driver_dock_internal;

namespace
{
    // EvidenceModuleKey：模块证据聚合使用的小写模块名 key。
    // 输入：模块名或路径叶子名；处理：去空白、取文件名、小写；返回：稳定比较 key。
    QString evidenceModuleKey(QString moduleNameText)
    {
        moduleNameText = moduleNameText.trimmed();
        const int slashIndex = std::max(moduleNameText.lastIndexOf(QLatin1Char('\\')), moduleNameText.lastIndexOf(QLatin1Char('/')));
        if (slashIndex >= 0 && slashIndex + 1 < moduleNameText.size())
        {
            moduleNameText = moduleNameText.mid(slashIndex + 1);
        }
        return moduleNameText.toLower();
    }

    // evidenceModuleStem：从 xxx.sys 生成 xxx，用作 DriverObject 名称候选。
    // 输入：模块文件名；处理：提取叶子名并裁掉 .sys 后缀；返回：候选对象叶子名。
    QString evidenceModuleStem(const QString& moduleNameText)
    {
        QString stemText = evidenceModuleKey(moduleNameText);
        if (stemText.endsWith(QStringLiteral(".sys"), Qt::CaseInsensitive))
        {
            stemText.chop(4);
        }
        return stemText.trimmed();
    }

    // evidenceYesNo：把证据布尔值转为短中文文本。
    // 输入：flag 表示命中或未命中；处理：选择两个中文短语；返回：单元格显示文本。
    QString evidenceYesNo(const bool flag, const QString& yesText, const QString& noText)
    {
        return flag ? yesText : noText;
    }

    // evidenceHookStatusText：把内核 Hook 状态转为 DriverDock 证据文本。
    // 输入：R0 共享协议状态；处理：按共享常量映射；返回：中文状态文本。
    QString evidenceHookStatusText(const std::uint32_t statusValue)
    {
        switch (statusValue)
        {
        case KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN:
            return QStringLiteral("干净");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS:
            return QStringLiteral("可疑外跳");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_INTERNAL_BRANCH:
            return QStringLiteral("模块内跳转");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED:
            return QStringLiteral("读取失败");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_PARSE_FAILED:
            return QStringLiteral("解析失败");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_FORCE_REQUIRED:
            return QStringLiteral("需要强制确认");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_PATCHED:
            return QStringLiteral("已修复/摘除");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_PATCH_FAILED:
            return QStringLiteral("修复失败");
        default:
            return QStringLiteral("未知(%1)").arg(statusValue);
        }
    }

    // evidenceInlineHookTypeText：把 Inline Hook 类型转为简短文本。
    // 输入：共享协议 hookType；处理：映射常见跳转/补丁形态；返回：中文/汇编文本。
    QString evidenceInlineHookTypeText(const std::uint32_t hookType)
    {
        switch (hookType)
        {
        case KSWORD_ARK_INLINE_HOOK_TYPE_NONE:
            return QStringLiteral("无明显补丁");
        case KSWORD_ARK_INLINE_HOOK_TYPE_JMP_REL32:
            return QStringLiteral("JMP rel32");
        case KSWORD_ARK_INLINE_HOOK_TYPE_JMP_REL8:
            return QStringLiteral("JMP rel8");
        case KSWORD_ARK_INLINE_HOOK_TYPE_JMP_RIP_INDIRECT:
            return QStringLiteral("JMP [RIP+rel32]");
        case KSWORD_ARK_INLINE_HOOK_TYPE_MOV_RAX_JMP_RAX:
            return QStringLiteral("MOV RAX; JMP RAX");
        case KSWORD_ARK_INLINE_HOOK_TYPE_MOV_R11_JMP_R11:
            return QStringLiteral("MOV R11; JMP R11");
        case KSWORD_ARK_INLINE_HOOK_TYPE_RET_PATCH:
            return QStringLiteral("RET 补丁");
        case KSWORD_ARK_INLINE_HOOK_TYPE_INT3_PATCH:
            return QStringLiteral("INT3 补丁");
        case KSWORD_ARK_INLINE_HOOK_TYPE_UNKNOWN_PATCH:
            return QStringLiteral("未知补丁");
        default:
            return QStringLiteral("未知(%1)").arg(hookType);
        }
    }

    // evidenceIatEatClassText：把 IAT/EAT 类型转成详情文本。
    // 输入：共享协议 hookClass；处理：映射 IAT 或 EAT；返回：中文类别文本。
    QString evidenceIatEatClassText(const std::uint32_t hookClass)
    {
        switch (hookClass)
        {
        case KSWORD_ARK_IAT_EAT_HOOK_CLASS_IAT:
            return QStringLiteral("IAT");
        case KSWORD_ARK_IAT_EAT_HOOK_CLASS_EAT:
            return QStringLiteral("EAT");
        default:
            return QStringLiteral("未知(%1)").arg(hookClass);
        }
    }

    // evidenceCallbackClassText：把 Callback 类别转成详情文本。
    // 输入：共享协议 callbackClass；处理：映射已知回调类型；返回：中文类别文本。
    QString evidenceCallbackClassText(const std::uint32_t callbackClass)
    {
        switch (callbackClass)
        {
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY:
            return QStringLiteral("注册表 CmCallback");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS:
            return QStringLiteral("进程 Notify");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_THREAD:
            return QStringLiteral("线程 Notify");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE:
            return QStringLiteral("镜像加载 Notify");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT:
            return QStringLiteral("Object Callback");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER:
            return QStringLiteral("Minifilter");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT:
            return QStringLiteral("WFP Callout");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER:
            return QStringLiteral("ETW Provider/Consumer");
        default:
            return QStringLiteral("未知(%1)").arg(callbackClass);
        }
    }

    // evidenceCallbackStatusText：把 Callback 枚举状态转成详情文本。
    // 输入：共享协议 status 和 NTSTATUS；处理：保留失败码；返回：中文状态文本。
    QString evidenceCallbackStatusText(const std::uint32_t statusValue, const long lastStatus)
    {
        switch (statusValue)
        {
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_OK:
            return QStringLiteral("可见/成功");
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_NOT_REGISTERED:
            return QStringLiteral("未注册");
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_UNSUPPORTED:
            return QStringLiteral("当前不支持");
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_QUERY_FAILED:
            return QStringLiteral("查询失败(%1)").arg(formatNtStatusText(lastStatus));
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_BUFFER_TRUNCATED:
            return QStringLiteral("缓冲截断");
        default:
            return QStringLiteral("未知(%1)").arg(statusValue);
        }
    }

    // evidenceAppendIoSummary：统一记录 ArkDriverClient 调用摘要。
    // 输入：标题、IoResult、输出列表；处理：追加一行可复制诊断；返回：无。
    void evidenceAppendIoSummary(
        QStringList& detailLines,
        const QString& titleText,
        const ksword::ark::IoResult& ioResult)
    {
        detailLines << QStringLiteral("[%1]").arg(titleText);
        detailLines << QStringLiteral("ok=%1 win32=%2 nt=%3 bytes=%4 message=%5")
            .arg(ioResult.ok ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(ioResult.win32Error)
            .arg(formatNtStatusText(ioResult.ntStatus))
            .arg(ioResult.bytesReturned)
            .arg(QString::fromStdString(ioResult.message));
    }

    // evidenceModuleNameMatches：判断 R0 返回模块名是否对应目标模块。
    // 输入：R0 模块名、目标模块名；处理：取叶子名小写比较；返回：true 表示同模块。
    bool evidenceModuleNameMatches(const QString& leftText, const QString& rightText)
    {
        const QString leftKey = evidenceModuleKey(leftText);
        const QString rightKey = evidenceModuleKey(rightText);
        return !leftKey.isEmpty() && !rightKey.isEmpty() && leftKey == rightKey;
    }

    // evidenceAddressLooksInsideModule：用基址/大小兜底判断地址是否落在模块内。
    // 输入：地址、模块基址、模块大小；处理：开区间范围判断；返回：true 表示落在模块范围内。
    bool evidenceAddressLooksInsideModule(
        const std::uint64_t addressValue,
        const std::uint64_t moduleBase,
        const std::uint32_t moduleSize)
    {
        if (addressValue == 0U || moduleBase == 0U || moduleSize == 0U)
        {
            return false;
        }
        return addressValue >= moduleBase && addressValue < (moduleBase + moduleSize);
    }
}


DriverDock::LoadedModuleEvidenceRecord DriverDock::buildPendingModuleEvidenceRecord(
    const LoadedKernelModuleRecord& moduleRecord)
{
    // 输入：模块记录；处理：填充各列等待文本；返回：占位证据记录。
    LoadedModuleEvidenceRecord evidence;
    evidence.moduleName = moduleRecord.moduleName;
    evidence.driverObjectStatusText = QStringLiteral("待扫描");
    evidence.driverStartMatchText = QStringLiteral("待扫描");
    evidence.majorFunctionStatusText = QStringLiteral("待扫描");
    evidence.iatEatStatusText = QStringLiteral("待扫描");
    evidence.inlineHookStatusText = QStringLiteral("待扫描");
    evidence.callbackStatusText = QStringLiteral("待扫描");
    evidence.detailText = QStringLiteral("模块 %1 尚未执行证据聚合。\n点击工具栏证据刷新按钮后，后台线程会只读查询 DriverObject / Hook / Callback。")
        .arg(moduleRecord.moduleName);
    return evidence;
}

QColor DriverDock::moduleEvidenceStatusColor(const LoadedModuleEvidenceRecord& evidence)
{
    // 输入：证据行；处理：错误/可疑/普通三档颜色；返回：QColor 前景色。
    if (!evidence.queryAttempted)
    {
        return KswordTheme::TextSecondaryColor();
    }
    if (evidence.hasMajorFunctionExternalJump ||
        evidence.hasIatEatSuspicious ||
        evidence.hasInlineHookSuspicious)
    {
        return QColor(QStringLiteral("#B23A3A"));
    }
    if (evidence.hasScanError ||
        !evidence.driverObjectResolved ||
        (evidence.driverStartKnown && !evidence.driverStartMatchesBase) ||
        evidence.hasCallbackReference)
    {
        return QColor(QStringLiteral("#D77A00"));
    }
    return QColor(QStringLiteral("#3A8F3A"));
}

bool DriverDock::queryDriverObjectForModuleEvidence(
    const LoadedKernelModuleRecord& moduleRecord,
    ksword::ark::DriverObjectQueryResult& resultOut,
    QString& attemptedNamesTextOut)
{
    // 输入：已加载模块行；处理：按常见 DriverObject 命名空间依次尝试查询；返回：是否解析成功。
    resultOut = ksword::ark::DriverObjectQueryResult{};
    attemptedNamesTextOut.clear();

    const QString stemText = evidenceModuleStem(moduleRecord.moduleName);
    if (stemText.isEmpty())
    {
        attemptedNamesTextOut = QStringLiteral("<模块名为空>");
        return false;
    }

    const QStringList candidateNames = {
        QStringLiteral("\\Driver\\%1").arg(stemText),
        QStringLiteral("\\FileSystem\\%1").arg(stemText),
        QStringLiteral("\\FileSystem\\Filters\\%1").arg(stemText),
        stemText
    };

    QStringList attemptedNames;
    const ksword::ark::DriverClient driverClient;
    for (const QString& candidateName : candidateNames)
    {
        if (attemptedNames.contains(candidateName, Qt::CaseInsensitive))
        {
            continue;
        }
        attemptedNames << candidateName;

        const ksword::ark::DriverObjectQueryResult queryResult = driverClient.queryDriverObject(
            candidateName.toStdWString(),
            KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_MAJOR_FUNCTIONS |
                KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_NAMES,
            1UL,
            1UL);

        resultOut = queryResult;
        if (queryResult.io.ok &&
            (queryResult.queryStatus == KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_OK ||
                queryResult.queryStatus == KSWORD_ARK_DRIVER_OBJECT_QUERY_STATUS_PARTIAL) &&
            queryResult.driverObjectAddress != 0U)
        {
            attemptedNamesTextOut = attemptedNames.join(QStringLiteral(", "));
            return true;
        }
    }

    attemptedNamesTextOut = attemptedNames.join(QStringLiteral(", "));
    return false;
}

std::vector<DriverDock::LoadedModuleEvidenceRecord> DriverDock::collectEvidenceForLoadedModules(
    const std::vector<LoadedKernelModuleRecord>& moduleRecords)
{
    // 输入：当前模块快照；处理：调用现有 DriverClient 能力聚合证据；返回：与输入等长的证据数组。
    std::vector<LoadedModuleEvidenceRecord> evidenceRecords;
    evidenceRecords.reserve(moduleRecords.size());

    const ksword::ark::DriverClient driverClient;
    const ksword::ark::KernelInlineHookScanResult inlineResult = driverClient.scanInlineHooks(
        0UL,
        KSWORD_ARK_KERNEL_HOOK_DEFAULT_MAX_ENTRIES,
        std::wstring());
    const ksword::ark::KernelIatEatHookScanResult iatEatResult = driverClient.enumerateIatEatHooks(
        KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_IMPORTS | KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_EXPORTS,
        KSWORD_ARK_KERNEL_HOOK_DEFAULT_MAX_ENTRIES,
        std::wstring());
    const ksword::ark::CallbackEnumResult callbackResult = driverClient.enumerateCallbacks(
        KSWORD_ARK_ENUM_CALLBACK_FLAG_INCLUDE_ALL);

    for (const LoadedKernelModuleRecord& moduleRecord : moduleRecords)
    {
        LoadedModuleEvidenceRecord evidence = buildPendingModuleEvidenceRecord(moduleRecord);
        evidence.queryAttempted = true;

        QStringList detailLines;
        detailLines << QStringLiteral("模块证据聚合")
                    << QStringLiteral("模块: %1").arg(moduleRecord.moduleName)
                    << QStringLiteral("基址: %1").arg(formatCompactAddress(moduleRecord.baseAddress))
                    << QStringLiteral("映像路径: %1").arg(moduleRecord.imagePath)
                    << QStringLiteral("说明: 本结果仅聚合证据，不执行卸载、移除或修复。")
                    << QString();

        ksword::ark::DriverObjectQueryResult objectResult;
        QString attemptedNamesText;
        evidence.driverObjectResolved = queryDriverObjectForModuleEvidence(
            moduleRecord,
            objectResult,
            attemptedNamesText);
        evidenceAppendIoSummary(detailLines, QStringLiteral("DriverObject 查询"), objectResult.io);
        detailLines << QStringLiteral("候选名称: %1").arg(attemptedNamesText);
        detailLines << QStringLiteral("QueryStatus: %1").arg(driverObjectQueryStatusText(objectResult.queryStatus));
        detailLines << QStringLiteral("DriverName: %1").arg(QString::fromStdWString(objectResult.driverName));
        detailLines << QStringLiteral("DriverObject: %1").arg(formatCompactAddress(objectResult.driverObjectAddress));
        detailLines << QStringLiteral("DriverStart: %1").arg(formatCompactAddress(objectResult.driverStart));
        detailLines << QStringLiteral("DriverSize: 0x%1").arg(static_cast<qulonglong>(objectResult.driverSize), 8, 16, QChar('0')).toUpper();
        detailLines << QStringLiteral("ImagePath: %1").arg(QString::fromStdWString(objectResult.imagePath));
        detailLines << QString();

        evidence.driverObjectName = QString::fromStdWString(objectResult.driverName);
        evidence.driverObjectStatusText = evidence.driverObjectResolved
            ? QStringLiteral("已解析")
            : QStringLiteral("未解析");
        evidence.driverStartKnown = objectResult.driverStart != 0U;
        evidence.driverStartMatchesBase = evidence.driverStartKnown &&
            objectResult.driverStart == moduleRecord.baseAddress;
        evidence.driverStartMatchText = !evidence.driverStartKnown
            ? QStringLiteral("未知")
            : evidenceYesNo(
                evidence.driverStartMatchesBase,
                QStringLiteral("匹配"),
                QStringLiteral("不匹配"));
        if (!objectResult.io.ok)
        {
            evidence.hasScanError = true;
        }

        detailLines << QStringLiteral("[MajorFunction]");
        if (objectResult.majorFunctions.empty())
        {
            detailLines << QStringLiteral("未返回 MajorFunction 表。") << QString();
        }
        else
        {
            for (const ksword::ark::DriverMajorFunctionEntry& entry : objectResult.majorFunctions)
            {
                const bool outsideOwnImage = (entry.flags & 0x00000002U) == 0U;
                if (outsideOwnImage)
                {
                    ++evidence.majorFunctionExternalCount;
                    detailLines << QStringLiteral("外跳: %1 dispatch=%2 module=%3 moduleBase=%4 location=%5")
                        .arg(driverMajorFunctionName(entry.majorFunction))
                        .arg(formatCompactAddress(entry.dispatchAddress))
                        .arg(QString::fromStdWString(entry.moduleName))
                        .arg(formatCompactAddress(entry.moduleBase))
                        .arg(driverDispatchLocationText(entry.flags));
                }
            }
            if (evidence.majorFunctionExternalCount == 0U)
            {
                detailLines << QStringLiteral("未发现 MajorFunction 外跳。") << QString();
            }
            else
            {
                detailLines << QString();
            }
        }
        evidence.hasMajorFunctionExternalJump = evidence.majorFunctionExternalCount != 0U;
        evidence.majorFunctionStatusText = evidence.hasMajorFunctionExternalJump
            ? QStringLiteral("外跳 %1").arg(evidence.majorFunctionExternalCount)
            : QStringLiteral("未见外跳");

        detailLines << QStringLiteral("[IAT/EAT]");
        if (!iatEatResult.io.ok)
        {
            evidence.hasScanError = true;
            detailLines << QStringLiteral("扫描失败: %1").arg(QString::fromStdString(iatEatResult.io.message));
        }
        else
        {
            for (const ksword::ark::KernelIatEatHookEntry& entry : iatEatResult.entries)
            {
                const bool sameModule = entry.moduleBase == moduleRecord.baseAddress ||
                    evidenceModuleNameMatches(QString::fromStdWString(entry.moduleName), moduleRecord.moduleName);
                if (!sameModule || entry.status != KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS)
                {
                    continue;
                }
                ++evidence.iatEatSuspiciousCount;
                detailLines << QStringLiteral("可疑: %1 module=%2 import=%3 func=%4 thunk=%5 current=%6 expected=%7 targetModule=%8 status=%9")
                    .arg(evidenceIatEatClassText(entry.hookClass))
                    .arg(QString::fromStdWString(entry.moduleName))
                    .arg(QString::fromStdWString(entry.importModuleName))
                    .arg(QString::fromLocal8Bit(entry.functionName.data(), static_cast<int>(entry.functionName.size())))
                    .arg(formatCompactAddress(entry.thunkAddress))
                    .arg(formatCompactAddress(entry.currentTarget))
                    .arg(formatCompactAddress(entry.expectedTarget))
                    .arg(QString::fromStdWString(entry.targetModuleName))
                    .arg(evidenceHookStatusText(entry.status));
            }
            if (evidence.iatEatSuspiciousCount == 0U)
            {
                detailLines << QStringLiteral("未发现该模块 IAT/EAT 可疑项。") << QString();
            }
            else
            {
                detailLines << QString();
            }
        }
        evidence.hasIatEatSuspicious = evidence.iatEatSuspiciousCount != 0U;
        evidence.iatEatStatusText = evidence.hasIatEatSuspicious
            ? QStringLiteral("可疑 %1").arg(evidence.iatEatSuspiciousCount)
            : (iatEatResult.io.ok ? QStringLiteral("未见可疑") : QStringLiteral("扫描失败"));

        detailLines << QStringLiteral("[Inline Hook]");
        if (!inlineResult.io.ok)
        {
            evidence.hasScanError = true;
            detailLines << QStringLiteral("扫描失败: %1").arg(QString::fromStdString(inlineResult.io.message));
        }
        else
        {
            for (const ksword::ark::KernelInlineHookEntry& entry : inlineResult.entries)
            {
                const bool sameModule = entry.moduleBase == moduleRecord.baseAddress ||
                    evidenceModuleNameMatches(QString::fromStdWString(entry.moduleName), moduleRecord.moduleName);
                if (!sameModule || entry.status != KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS)
                {
                    continue;
                }
                ++evidence.inlineHookSuspiciousCount;
                detailLines << QStringLiteral("可疑: module=%1 function=%2 address=%3 type=%4 target=%5 targetModule=%6 status=%7")
                    .arg(QString::fromStdWString(entry.moduleName))
                    .arg(QString::fromLocal8Bit(entry.functionName.data(), static_cast<int>(entry.functionName.size())))
                    .arg(formatCompactAddress(entry.functionAddress))
                    .arg(evidenceInlineHookTypeText(entry.hookType))
                    .arg(formatCompactAddress(entry.targetAddress))
                    .arg(QString::fromStdWString(entry.targetModuleName))
                    .arg(evidenceHookStatusText(entry.status));
            }
            if (evidence.inlineHookSuspiciousCount == 0U)
            {
                detailLines << QStringLiteral("未发现该模块 Inline Hook 可疑项。") << QString();
            }
            else
            {
                detailLines << QString();
            }
        }
        evidence.hasInlineHookSuspicious = evidence.inlineHookSuspiciousCount != 0U;
        evidence.inlineHookStatusText = evidence.hasInlineHookSuspicious
            ? QStringLiteral("可疑 %1").arg(evidence.inlineHookSuspiciousCount)
            : (inlineResult.io.ok ? QStringLiteral("未见可疑") : QStringLiteral("扫描失败"));

        detailLines << QStringLiteral("[Callback]");
        if (!callbackResult.io.ok)
        {
            evidence.hasScanError = true;
            detailLines << QStringLiteral("枚举失败: %1").arg(QString::fromStdString(callbackResult.io.message));
        }
        else
        {
            for (const ksword::ark::CallbackEnumEntry& entry : callbackResult.entries)
            {
                const bool sameModule = entry.moduleBase == moduleRecord.baseAddress ||
                    evidenceAddressLooksInsideModule(entry.callbackAddress, moduleRecord.baseAddress, entry.moduleSize) ||
                    evidenceModuleNameMatches(QString::fromStdWString(entry.modulePath), moduleRecord.moduleName) ||
                    QString::fromStdWString(entry.modulePath).contains(moduleRecord.moduleName, Qt::CaseInsensitive);
                if (!sameModule)
                {
                    continue;
                }
                ++evidence.callbackReferenceCount;
                detailLines << QStringLiteral("引用: class=%1 status=%2 callback=%3 context=%4 registration=%5 moduleBase=%6 modulePath=%7 name=%8 altitude=%9 detail=%10")
                    .arg(evidenceCallbackClassText(entry.callbackClass))
                    .arg(evidenceCallbackStatusText(entry.status, entry.lastStatus))
                    .arg(formatCompactAddress(entry.callbackAddress))
                    .arg(formatCompactAddress(entry.contextAddress))
                    .arg(formatCompactAddress(entry.registrationAddress))
                    .arg(formatCompactAddress(entry.moduleBase))
                    .arg(QString::fromStdWString(entry.modulePath))
                    .arg(QString::fromStdWString(entry.name))
                    .arg(QString::fromStdWString(entry.altitude))
                    .arg(QString::fromStdWString(entry.detail));
            }
            if (evidence.callbackReferenceCount == 0U)
            {
                detailLines << QStringLiteral("未发现 Callback 引用该模块。") << QString();
            }
            else
            {
                detailLines << QString();
            }
        }
        evidence.hasCallbackReference = evidence.callbackReferenceCount != 0U;
        evidence.callbackStatusText = evidence.hasCallbackReference
            ? QStringLiteral("引用 %1").arg(evidence.callbackReferenceCount)
            : (callbackResult.io.ok ? QStringLiteral("未见引用") : QStringLiteral("枚举失败"));

        detailLines << QStringLiteral("[全局扫描摘要]");
        evidenceAppendIoSummary(detailLines, QStringLiteral("Inline Hook"), inlineResult.io);
        detailLines << QStringLiteral("Inline returned=%1 total=%2 modules=%3 last=%4")
            .arg(inlineResult.entries.size())
            .arg(inlineResult.totalCount)
            .arg(inlineResult.moduleCount)
            .arg(formatNtStatusText(inlineResult.lastStatus));
        evidenceAppendIoSummary(detailLines, QStringLiteral("IAT/EAT"), iatEatResult.io);
        detailLines << QStringLiteral("IAT/EAT returned=%1 total=%2 modules=%3 last=%4")
            .arg(iatEatResult.entries.size())
            .arg(iatEatResult.totalCount)
            .arg(iatEatResult.moduleCount)
            .arg(formatNtStatusText(iatEatResult.lastStatus));
        evidenceAppendIoSummary(detailLines, QStringLiteral("Callback"), callbackResult.io);
        detailLines << QStringLiteral("Callback returned=%1 total=%2 last=%3")
            .arg(callbackResult.entries.size())
            .arg(callbackResult.totalCount)
            .arg(formatNtStatusText(callbackResult.lastStatus));

        evidence.detailText = detailLines.join('\n');
        evidenceRecords.push_back(std::move(evidence));
    }

    return evidenceRecords;
}

void DriverDock::refreshLoadedModuleEvidenceAsync()
{
    // 输入：当前已加载模块缓存；处理：后台聚合 R0 证据并回投 UI；返回：无。
    if (m_moduleEvidenceQuerying)
    {
        return;
    }
    if (m_loadedModuleCache.empty())
    {
        // 手动点击“证据刷新”但当前没有模块快照时，只提示用户先刷新模块：
        // - 输入：空 m_loadedModuleCache；
        // - 处理：不在这里反向调用 refreshLoadedKernelModuleRecords；
        // - 返回：无；避免空列表环境下模块刷新和证据刷新互相递归。
        if (m_moduleEvidenceStatusLabel != nullptr)
        {
            m_moduleEvidenceStatusLabel->setText(QStringLiteral("证据：没有可聚合的模块，请先刷新已加载模块。"));
        }
        return;
    }

    m_moduleEvidenceQuerying = true;
    const std::uint64_t ticketValue = ++m_moduleEvidenceQueryTicket;
    if (m_refreshModuleEvidenceButton != nullptr)
    {
        m_refreshModuleEvidenceButton->setEnabled(false);
    }
    if (m_moduleEvidenceStatusLabel != nullptr)
    {
        m_moduleEvidenceStatusLabel->setText(QStringLiteral("证据：后台聚合中，UI 不会阻塞..."));
    }

    const std::vector<LoadedKernelModuleRecord> moduleSnapshot = m_loadedModuleCache;
    QPointer<DriverDock> guardThis(this);
    auto* evidenceTask = QRunnable::create([guardThis, ticketValue, moduleSnapshot]()
        {
            auto resultRecords = DriverDock::collectEvidenceForLoadedModules(moduleSnapshot);

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, ticketValue, resultRecords = std::move(resultRecords)]() mutable
                {
                    if (guardThis == nullptr ||
                        guardThis->m_moduleEvidenceQueryTicket != ticketValue)
                    {
                        return;
                    }

                    guardThis->m_moduleEvidenceQuerying = false;
                    if (guardThis->m_refreshModuleEvidenceButton != nullptr)
                    {
                        guardThis->m_refreshModuleEvidenceButton->setEnabled(true);
                    }

                    std::size_t suspiciousCount = 0U;
                    std::size_t callbackCount = 0U;
                    std::size_t errorCount = 0U;
                    for (const auto& evidence : resultRecords)
                    {
                        if (evidence.hasMajorFunctionExternalJump ||
                            evidence.hasIatEatSuspicious ||
                            evidence.hasInlineHookSuspicious)
                        {
                            ++suspiciousCount;
                        }
                        if (evidence.hasCallbackReference)
                        {
                            ++callbackCount;
                        }
                        if (evidence.hasScanError)
                        {
                            ++errorCount;
                        }
                    }

                    guardThis->m_loadedModuleEvidenceCache = std::move(resultRecords);
                    guardThis->rebuildLoadedModuleEvidenceViews();
                    if (guardThis->m_moduleEvidenceStatusLabel != nullptr)
                    {
                        guardThis->m_moduleEvidenceStatusLabel->setText(
                            QStringLiteral("证据：已聚合 %1 个模块，可疑=%2，Callback引用=%3，错误=%4")
                            .arg(guardThis->m_loadedModuleEvidenceCache.size())
                            .arg(suspiciousCount)
                            .arg(callbackCount)
                            .arg(errorCount));
                    }
                },
                Qt::QueuedConnection);
        });
    evidenceTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(evidenceTask);
}

void DriverDock::rebuildLoadedModuleEvidenceViews()
{
    // 输入：当前模块表和证据缓存；处理：逐行回填证据列颜色/文本；返回：无。
    if (m_moduleTable == nullptr)
    {
        return;
    }

    for (int rowIndex = 0; rowIndex < m_moduleTable->rowCount(); ++rowIndex)
    {
        QTableWidgetItem* moduleItem = m_moduleTable->item(rowIndex, 0);
        if (moduleItem == nullptr)
        {
            continue;
        }

        const std::size_t sourceIndex = static_cast<std::size_t>(
            moduleItem->data(ModuleRecordIndexRole).toULongLong());
        if (sourceIndex >= m_loadedModuleEvidenceCache.size())
        {
            continue;
        }

        const LoadedModuleEvidenceRecord& evidence = m_loadedModuleEvidenceCache[sourceIndex];
        const QStringList columnTexts = {
            evidence.driverObjectStatusText,
            evidence.driverStartMatchText,
            evidence.majorFunctionStatusText,
            evidence.iatEatStatusText,
            evidence.inlineHookStatusText,
            evidence.callbackStatusText
        };
        const QColor foregroundColor = moduleEvidenceStatusColor(evidence);
        for (int columnOffset = 0; columnOffset < columnTexts.size(); ++columnOffset)
        {
            QTableWidgetItem* cellItem = m_moduleTable->item(rowIndex, 2 + columnOffset);
            if (cellItem == nullptr)
            {
                cellItem = createReadOnlyItem(columnTexts[columnOffset]);
                m_moduleTable->setItem(rowIndex, 2 + columnOffset, cellItem);
            }
            else
            {
                cellItem->setText(columnTexts[columnOffset]);
            }
            cellItem->setForeground(QBrush(foregroundColor));
            cellItem->setToolTip(evidence.detailText.left(4000));
        }
    }

    showSelectedModuleEvidenceDetail();
}

void DriverDock::showSelectedModuleEvidenceDetail()
{
    // 输入：当前模块表选择；处理：读取缓存并显示详情；返回：无。
    if (m_moduleEvidenceDetailEditor == nullptr)
    {
        return;
    }
    if (m_moduleTable == nullptr || m_moduleTable->selectionModel() == nullptr)
    {
        m_moduleEvidenceDetailEditor->setText(QStringLiteral("模块表不可用。"));
        return;
    }

    const QModelIndexList selectedRows = m_moduleTable->selectionModel()->selectedRows(0);
    if (selectedRows.isEmpty())
    {
        m_moduleEvidenceDetailEditor->setText(QStringLiteral("请选择一条已加载模块查看聚合证据。"));
        return;
    }

    const int rowIndex = selectedRows.front().row();
    QTableWidgetItem* moduleItem = m_moduleTable->item(rowIndex, 0);
    if (moduleItem == nullptr)
    {
        m_moduleEvidenceDetailEditor->setText(QStringLiteral("当前行没有模块名。"));
        return;
    }

    const std::size_t sourceIndex = static_cast<std::size_t>(
        moduleItem->data(ModuleRecordIndexRole).toULongLong());
    if (sourceIndex >= m_loadedModuleEvidenceCache.size())
    {
        m_moduleEvidenceDetailEditor->setText(
            QStringLiteral("模块 %1 尚未生成证据详情。")
            .arg(moduleItem->text()));
        return;
    }

    m_moduleEvidenceDetailEditor->setText(m_loadedModuleEvidenceCache[sourceIndex].detailText);
}
