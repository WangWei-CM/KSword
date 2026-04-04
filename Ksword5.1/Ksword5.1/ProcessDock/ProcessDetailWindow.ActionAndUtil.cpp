#include "ProcessDetailWindow.InternalCommon.h"

using namespace process_detail_window_internal;

// ============================================================
// ProcessDetailWindow.ActionAndUtil.cpp
// 作用：
// - 负责操作页动作（终止/挂起/优先级/注入）与通用工具函数。
// - 聚焦“执行动作 + 结果反馈 + 辅助格式化/查找”逻辑。
// ============================================================

void ProcessDetailWindow::executeTaskKillAction(const bool forceKill)
{
    // TaskKill 操作日志：同一动作只使用一个 kLogEvent，保证调用链可追踪。
    kLogEvent actionEvent;
    info << actionEvent
        << "[ProcessDetailWindow] executeTaskKillAction: pid="
        << m_baseRecord.pid
        << ", forceKill="
        << (forceKill ? "true" : "false")
        << eol;

    std::string detailText;
    const bool actionOk = ks::process::ExecuteTaskKill(m_baseRecord.pid, forceKill, &detailText);
    (actionOk ? info : err) << actionEvent
        << "[ProcessDetailWindow] executeTaskKillAction: actionOk="
        << (actionOk ? "true" : "false")
        << ", detail="
        << detailText
        << eol;
    showActionResultMessage(forceKill ? "Taskkill /f" : "Taskkill", actionOk, detailText, actionEvent);
}

void ProcessDetailWindow::executeTerminateProcessAction()
{
    // TerminateProcess 操作日志：同一动作只使用一个 kLogEvent，保证调用链可追踪。
    kLogEvent actionEvent;
    warn << actionEvent
        << "[ProcessDetailWindow] executeTerminateProcessAction: pid="
        << m_baseRecord.pid
        << eol;

    std::string detailText;
    const bool actionOk = ks::process::TerminateProcessByWin32(m_baseRecord.pid, &detailText);
    (actionOk ? info : err) << actionEvent
        << "[ProcessDetailWindow] executeTerminateProcessAction: actionOk="
        << (actionOk ? "true" : "false")
        << ", detail="
        << detailText
        << eol;
    showActionResultMessage("TerminateProcess", actionOk, detailText, actionEvent);
}

void ProcessDetailWindow::executeTerminateThreadsAction()
{
    // 全线程结束日志：同一动作只使用一个 kLogEvent，保证调用链可追踪。
    kLogEvent actionEvent;
    warn << actionEvent
        << "[ProcessDetailWindow] executeTerminateThreadsAction: pid="
        << m_baseRecord.pid
        << eol;

    std::string detailText;
    const bool actionOk = ks::process::TerminateAllThreadsByPid(m_baseRecord.pid, &detailText);
    (actionOk ? info : err) << actionEvent
        << "[ProcessDetailWindow] executeTerminateThreadsAction: actionOk="
        << (actionOk ? "true" : "false")
        << ", detail="
        << detailText
        << eol;
    showActionResultMessage("TerminateThread(全部线程)", actionOk, detailText, actionEvent);
}

void ProcessDetailWindow::executeSelectedTerminateAction()
{
    if (m_terminateActionCombo == nullptr)
    {
        kLogEvent terminateComboNullEvent;
        err << terminateComboNullEvent
            << "[ProcessDetailWindow] executeSelectedTerminateAction: m_terminateActionCombo 为空。"
            << eol;
        return;
    }

    // 结束方案调度：
    // - 下拉框只负责选择策略；
    // - 真正执行仍复用现有动作函数，确保日志链路和行为不变。
    const int actionId = m_terminateActionCombo->currentData().toInt();
    switch (actionId)
    {
    case 0:
        executeTaskKillAction(false);
        break;
    case 1:
        executeTaskKillAction(true);
        break;
    case 2:
        executeTerminateProcessAction();
        break;
    case 3:
        executeTerminateThreadsAction();
        break;
    default:
    {
        kLogEvent invalidTerminateActionEvent;
        warn << invalidTerminateActionEvent
            << "[ProcessDetailWindow] executeSelectedTerminateAction: 未知 actionId="
            << actionId
            << eol;
        break;
    }
    }
}

void ProcessDetailWindow::executeSuspendProcessAction()
{
    // 挂起进程日志：同一动作只使用一个 kLogEvent，保证调用链可追踪。
    kLogEvent actionEvent;
    info << actionEvent
        << "[ProcessDetailWindow] executeSuspendProcessAction: pid="
        << m_baseRecord.pid
        << eol;

    std::string detailText;
    const bool actionOk = ks::process::SuspendProcess(m_baseRecord.pid, &detailText);
    (actionOk ? info : err) << actionEvent
        << "[ProcessDetailWindow] executeSuspendProcessAction: actionOk="
        << (actionOk ? "true" : "false")
        << ", detail="
        << detailText
        << eol;
    showActionResultMessage("挂起进程", actionOk, detailText, actionEvent);
}

void ProcessDetailWindow::executeResumeProcessAction()
{
    // 恢复进程日志：同一动作只使用一个 kLogEvent，保证调用链可追踪。
    kLogEvent actionEvent;
    info << actionEvent
        << "[ProcessDetailWindow] executeResumeProcessAction: pid="
        << m_baseRecord.pid
        << eol;

    std::string detailText;
    const bool actionOk = ks::process::ResumeProcess(m_baseRecord.pid, &detailText);
    (actionOk ? info : err) << actionEvent
        << "[ProcessDetailWindow] executeResumeProcessAction: actionOk="
        << (actionOk ? "true" : "false")
        << ", detail="
        << detailText
        << eol;
    showActionResultMessage("恢复进程", actionOk, detailText, actionEvent);
}

void ProcessDetailWindow::executeSetCriticalAction(const bool enableCritical)
{
    // 关键进程标记变更日志：同一动作只使用一个 kLogEvent，保证调用链可追踪。
    kLogEvent actionEvent;
    warn << actionEvent
        << "[ProcessDetailWindow] executeSetCriticalAction: pid="
        << m_baseRecord.pid
        << ", enableCritical="
        << (enableCritical ? "true" : "false")
        << eol;

    std::string detailText;
    const bool actionOk = ks::process::SetProcessCriticalFlag(m_baseRecord.pid, enableCritical, &detailText);
    (actionOk ? info : err) << actionEvent
        << "[ProcessDetailWindow] executeSetCriticalAction: actionOk="
        << (actionOk ? "true" : "false")
        << ", detail="
        << detailText
        << eol;
    showActionResultMessage(enableCritical ? "设为关键进程" : "取消关键进程", actionOk, detailText, actionEvent);
}

void ProcessDetailWindow::executeSetPriorityAction()
{
    if (m_priorityCombo == nullptr)
    {
        kLogEvent priorityComboNullEvent;
        err << priorityComboNullEvent
            << "[ProcessDetailWindow] executeSetPriorityAction: m_priorityCombo 为空。"
            << eol;
        return;
    }

    const int actionId = m_priorityCombo->currentData().toInt();
    ks::process::ProcessPriorityLevel priorityLevel = ks::process::ProcessPriorityLevel::Normal;
    switch (actionId)
    {
    case 0: priorityLevel = ks::process::ProcessPriorityLevel::Idle; break;
    case 1: priorityLevel = ks::process::ProcessPriorityLevel::BelowNormal; break;
    case 2: priorityLevel = ks::process::ProcessPriorityLevel::Normal; break;
    case 3: priorityLevel = ks::process::ProcessPriorityLevel::AboveNormal; break;
    case 4: priorityLevel = ks::process::ProcessPriorityLevel::High; break;
    case 5: priorityLevel = ks::process::ProcessPriorityLevel::Realtime; break;
    default: priorityLevel = ks::process::ProcessPriorityLevel::Normal; break;
    }

    // 优先级设置日志：同一动作只使用一个 kLogEvent，保证调用链可追踪。
    kLogEvent actionEvent;
    info << actionEvent
        << "[ProcessDetailWindow] executeSetPriorityAction: pid="
        << m_baseRecord.pid
        << ", actionId="
        << actionId
        << eol;

    std::string detailText;
    const bool actionOk = ks::process::SetProcessPriority(m_baseRecord.pid, priorityLevel, &detailText);
    (actionOk ? info : err) << actionEvent
        << "[ProcessDetailWindow] executeSetPriorityAction: actionOk="
        << (actionOk ? "true" : "false")
        << ", detail="
        << detailText
        << eol;
    showActionResultMessage("设置进程优先级", actionOk, detailText, actionEvent);
}

void ProcessDetailWindow::executeInjectInvalidShellcodeAction()
{
    // 无效 shellcode 注入日志：同一动作只使用一个 kLogEvent，保证调用链可追踪。
    kLogEvent actionEvent;
    warn << actionEvent
        << "[ProcessDetailWindow] executeInjectInvalidShellcodeAction: pid="
        << m_baseRecord.pid
        << eol;

    std::string detailText;
    const bool actionOk = ks::process::InjectInvalidShellcode(m_baseRecord.pid, &detailText);
    (actionOk ? info : err) << actionEvent
        << "[ProcessDetailWindow] executeInjectInvalidShellcodeAction: actionOk="
        << (actionOk ? "true" : "false")
        << ", detail="
        << detailText
        << eol;
    showActionResultMessage("注入无效shellcode", actionOk, detailText, actionEvent);
}

void ProcessDetailWindow::executeInjectDllAction()
{
    const QString dllPath = m_dllPathLineEdit->text().trimmed();
    if (dllPath.isEmpty())
    {
        kLogEvent injectDllEmptyPathEvent;
        warn << injectDllEmptyPathEvent
            << "[ProcessDetailWindow] executeInjectDllAction: DLL 路径为空。"
            << eol;
        QMessageBox::warning(this, "DLL 注入", "请先选择 DLL 文件。");
        return;
    }

    // DLL 注入日志：同一动作只使用一个 kLogEvent，保证调用链可追踪。
    kLogEvent actionEvent;
    info << actionEvent
        << "[ProcessDetailWindow] executeInjectDllAction: pid="
        << m_baseRecord.pid
        << ", dllPath="
        << dllPath.toStdString()
        << eol;

    std::string detailText;
    const bool actionOk = ks::process::InjectDllByPath(
        m_baseRecord.pid,
        dllPath.toStdString(),
        &detailText);
    (actionOk ? info : err) << actionEvent
        << "[ProcessDetailWindow] executeInjectDllAction: actionOk="
        << (actionOk ? "true" : "false")
        << ", detail="
        << detailText
        << eol;
    showActionResultMessage("DLL 注入", actionOk, detailText, actionEvent);
    if (actionOk)
    {
        requestAsyncModuleRefresh(true);
    }
}

void ProcessDetailWindow::executeInjectShellcodeAction()
{
    const QString shellcodePath = m_shellcodePathLineEdit->text().trimmed();
    if (shellcodePath.isEmpty())
    {
        kLogEvent injectShellcodeEmptyPathEvent;
        warn << injectShellcodeEmptyPathEvent
            << "[ProcessDetailWindow] executeInjectShellcodeAction: shellcode 路径为空。"
            << eol;
        QMessageBox::warning(this, "Shellcode 注入", "请先选择 shellcode 文件。");
        return;
    }

    // Shellcode 注入日志：同一动作只使用一个 kLogEvent，保证调用链可追踪。
    kLogEvent actionEvent;
    info << actionEvent
        << "[ProcessDetailWindow] executeInjectShellcodeAction: pid="
        << m_baseRecord.pid
        << ", filePath="
        << shellcodePath.toStdString()
        << eol;

    std::vector<std::uint8_t> shellcodeBuffer;
    std::string readErrorText;
    if (!readBinaryFile(shellcodePath, shellcodeBuffer, readErrorText, actionEvent))
    {
        err << actionEvent
            << "[ProcessDetailWindow] executeInjectShellcodeAction: 读取文件失败, error="
            << readErrorText
            << eol;
        showActionResultMessage("Shellcode 注入", false, readErrorText, actionEvent);
        return;
    }

    std::string detailText;
    const bool actionOk = ks::process::InjectShellcodeBuffer(
        m_baseRecord.pid,
        shellcodeBuffer,
        &detailText);
    (actionOk ? info : err) << actionEvent
        << "[ProcessDetailWindow] executeInjectShellcodeAction: actionOk="
        << (actionOk ? "true" : "false")
        << ", shellcodeSize="
        << shellcodeBuffer.size()
        << ", detail="
        << detailText
        << eol;
    showActionResultMessage("Shellcode 注入", actionOk, detailText, actionEvent);
}

QIcon ProcessDetailWindow::resolveProcessIcon(const std::string& processPath, const int iconPixelSize)
{
    Q_UNUSED(iconPixelSize);

    // 优先使用传入路径；为空时按当前 PID 兜底查询一次。
    QString pathText = QString::fromStdString(processPath);
    if (pathText.trimmed().isEmpty() && m_baseRecord.pid != 0)
    {
        pathText = QString::fromStdString(ks::process::QueryProcessPathByPid(m_baseRecord.pid));
    }
    if (pathText.isEmpty())
    {
        kLogEvent resolveIconFallbackEvent;
        dbg << resolveIconFallbackEvent
            << "[ProcessDetailWindow] resolveProcessIcon: 路径为空，返回默认图标。"
            << eol;
        return QIcon(":/Icon/process_main.svg");
    }

    auto iconIt = m_iconCacheByPath.find(pathText);
    if (iconIt != m_iconCacheByPath.end())
    {
        kLogEvent resolveIconCacheHitEvent;
        dbg << resolveIconCacheHitEvent
            << "[ProcessDetailWindow] resolveProcessIcon: 命中图标缓存, path="
            << pathText.toStdString()
            << eol;
        return iconIt.value();
    }

    // 先尝试直接按 EXE 路径加载图标；失败再回退 QFileIconProvider。
    QIcon processIcon(pathText);
    if (processIcon.isNull())
    {
        QFileIconProvider iconProvider;
        processIcon = iconProvider.icon(QFileInfo(pathText));
    }
    if (processIcon.isNull())
    {
        processIcon = QIcon(":/Icon/process_main.svg");
    }
    m_iconCacheByPath.insert(pathText, processIcon);
    kLogEvent resolveIconCacheStoreEvent;
    dbg << resolveIconCacheStoreEvent
        << "[ProcessDetailWindow] resolveProcessIcon: 缓存图标, path="
        << pathText.toStdString()
        << eol;
    return processIcon;
}

QString ProcessDetailWindow::formatModuleSizeText(const std::uint32_t moduleSizeBytes) const
{
    const double sizeKb = static_cast<double>(moduleSizeBytes) / 1024.0;
    if (sizeKb < 1024.0)
    {
        return QString("%1 KB").arg(QString::number(sizeKb, 'f', 1));
    }
    const double sizeMb = sizeKb / 1024.0;
    return QString("%1 MB").arg(QString::number(sizeMb, 'f', 2));
}

QString ProcessDetailWindow::formatHexText(const std::uint64_t value) const
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << value;
    return QString::fromStdString(stream.str());
}

bool ProcessDetailWindow::readBinaryFile(
    const QString& filePath,
    std::vector<std::uint8_t>& bufferOut,
    std::string& errorTextOut,
    const kLogEvent& actionEvent) const
{
    // 文件读取入口日志：沿用调用方的 actionEvent，确保动作链路 GUID 连续。
    info << actionEvent
        << "[ProcessDetailWindow] readBinaryFile: filePath="
        << filePath.toStdString()
        << eol;

    bufferOut.clear();
    errorTextOut.clear();

    QFile fileObject(filePath);
    if (!fileObject.open(QIODevice::ReadOnly))
    {
        errorTextOut = "Open file failed: " + fileObject.errorString().toStdString();
        err << actionEvent
            << "[ProcessDetailWindow] readBinaryFile: 打开失败, error="
            << errorTextOut
            << eol;
        return false;
    }

    const QByteArray rawBytes = fileObject.readAll();
    if (rawBytes.isEmpty())
    {
        errorTextOut = "File is empty.";
        warn << actionEvent
            << "[ProcessDetailWindow] readBinaryFile: 文件为空。"
            << eol;
        return false;
    }

    bufferOut.resize(static_cast<std::size_t>(rawBytes.size()));
    std::copy(
        reinterpret_cast<const std::uint8_t*>(rawBytes.constData()),
        reinterpret_cast<const std::uint8_t*>(rawBytes.constData()) + rawBytes.size(),
        bufferOut.begin());
    info << actionEvent
        << "[ProcessDetailWindow] readBinaryFile: 读取成功, size="
        << bufferOut.size()
        << eol;
    return true;
}

void ProcessDetailWindow::showActionResultMessage(
    const QString& title,
    const bool actionOk,
    const std::string& detailText,
    const kLogEvent& actionEvent)
{
    // 动作反馈日志：按照规范不再弹窗，只输出日志，避免打断用户流程。
    const std::string normalizedDetailText = detailText.empty() ? "无附加信息" : detailText;
    (actionOk ? info : err) << actionEvent
        << "[ProcessDetailWindow] showActionResultMessage: title="
        << title.toStdString()
        << ", actionOk="
        << (actionOk ? "true" : "false")
        << ", detail="
        << normalizedDetailText
        << eol;
}

ks::process::ProcessModuleRecord* ProcessDetailWindow::selectedModuleRecord()
{
    QTreeWidgetItem* currentItem = m_moduleTable->currentItem();
    if (currentItem == nullptr)
    {
        kLogEvent selectedModuleNullEvent;
        warn << selectedModuleNullEvent
            << "[ProcessDetailWindow] selectedModuleRecord: 当前无选中行。"
            << eol;
        return nullptr;
    }

    const std::string pathText = currentItem->data(
        toModuleColumnIndex(ModuleColumn::Path),
        Qt::UserRole).toString().toStdString();
    const std::uint64_t baseAddress = currentItem->data(
        toModuleColumnIndex(ModuleColumn::Path),
        Qt::UserRole + 1).toULongLong();

    auto foundIt = std::find_if(
        m_moduleRecords.begin(),
        m_moduleRecords.end(),
        [baseAddress, &pathText](const ks::process::ProcessModuleRecord& moduleRecord)
        {
            return moduleRecord.moduleBaseAddress == baseAddress && moduleRecord.modulePath == pathText;
        });
    if (foundIt == m_moduleRecords.end())
    {
        kLogEvent selectedModuleNotFoundEvent;
        warn << selectedModuleNotFoundEvent
            << "[ProcessDetailWindow] selectedModuleRecord: 缓存中未找到对应模块记录。"
            << eol;
        return nullptr;
    }
    kLogEvent selectedModuleFoundEvent;
    dbg << selectedModuleFoundEvent
        << "[ProcessDetailWindow] selectedModuleRecord: 命中模块记录, path="
        << foundIt->modulePath
        << eol;
    return &(*foundIt);
}
