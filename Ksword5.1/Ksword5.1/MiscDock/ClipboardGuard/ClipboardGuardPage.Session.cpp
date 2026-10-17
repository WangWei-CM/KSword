#include "ClipboardGuardPage.h"
#include "../../../../shared/ApiMonitorInjection.h"
#include "../../theme.h"
#include "../../MonitorDock/WinApiMonitorProtocol.h"
#include "../../Framework/PrivilegeElevationPrompt.h"

#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QStringConverter>
#include <QThreadPool>
#include <QUuid>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <iterator>

// ============================================================
// ClipboardGuardPage.Session.cpp
// 作用：
// 1) 规则表与驱动的双向同步（KSWORD_ARK_CLIPBOARD_POLICY_RULE 整表下发/回读）；
// 2) 按规则匹配当前运行进程，按目标位数注入 Agent 并写会话 INI；
// 3) 每个受管进程一条命名管道读取线程，事件解析后送进共享队列，
//    由 QTimer 批量刷进事件表（照抄 WinAPIDock 的 child-pipe 范式）。
// ============================================================

namespace ks::misc
{
    namespace
    {
        constexpr DWORD kPacketSize = static_cast<DWORD>(sizeof(ks::winapi_monitor::ApiMonitorEventPacket));

        template <std::size_t kCount>
        QString wideBufferToText(const wchar_t(&bufferValue)[kCount])
        {
            QString textValue = QString::fromWCharArray(bufferValue, static_cast<int>(kCount));
            const int nullIndex = textValue.indexOf(QChar(u'\0'));
            if (nullIndex >= 0)
            {
                textValue.truncate(nullIndex);
            }
            return textValue.trimmed();
        }

        // parseDetailField 作用：从 "key1=value1 key2=value2 ..." 形式的 detail 文本里
        // 取出某个 key 的值。ReportClipboardEvent（APIMonitor_x64 侧）用同样的分隔约定拼串，
        // 双方约定好格式即可，不需要一套共享的序列化库。
        QString parseDetailField(const QString& detailText, const QString& keyName)
        {
            const QString markerText = keyName + QStringLiteral("=");
            const int markerIndex = detailText.indexOf(markerText);
            if (markerIndex < 0)
            {
                return QString();
            }
            const int valueStart = markerIndex + markerText.size();
            int valueEnd = detailText.indexOf(QChar(u' '), valueStart);
            if (valueEnd < 0)
            {
                valueEnd = detailText.size();
            }
            return detailText.mid(valueStart, valueEnd - valueStart);
        }

        // packetToEventRow 作用：把 Agent 固定事件包解析成表格展示用的结构。
        ClipboardGuardEventRow packetToEventRow(const ks::winapi_monitor::ApiMonitorEventPacket& packetValue, const QString& processText)
        {
            ClipboardGuardEventRow rowValue;
            rowValue.timeText = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
            rowValue.pid = packetValue.pid;
            rowValue.tid = packetValue.tid;
            rowValue.processText = processText;

            const QString detailText = wideBufferToText(packetValue.detailText);
            rowValue.operationText = parseDetailField(detailText, QStringLiteral("op"));
            rowValue.formatText = parseDetailField(detailText, QStringLiteral("fmt"));
            rowValue.resultText = parseDetailField(detailText, QStringLiteral("action"));
            rowValue.sessionText = parseDetailField(detailText, QStringLiteral("session"));
            rowValue.integrityText = parseDetailField(detailText, QStringLiteral("integrity"));
            rowValue.ownerText = parseDetailField(detailText, QStringLiteral("owner"));
            rowValue.seq = parseDetailField(detailText, QStringLiteral("seq")).toULongLong();
            return rowValue;
        }
    }

    void ClipboardGuardPage::refreshRuleTable()
    {
        if (m_ruleTable == nullptr)
        {
            return;
        }
        m_ruleTable->setRowCount(static_cast<int>(m_rules.size()));
        for (std::size_t rowIndex = 0; rowIndex < m_rules.size(); ++rowIndex)
        {
            const ClipboardGuardRule& ruleValue = m_rules[rowIndex];
            const int row = static_cast<int>(rowIndex);
            QString targetText;
            QString kindText;
            switch (ruleValue.targetKind)
            {
            case 1: targetText = QString::number(ruleValue.targetProcessId); kindText = QStringLiteral("PID"); break;
            case 2: targetText = ruleValue.targetImage; kindText = QStringLiteral("映像名"); break;
            case 3: targetText = ruleValue.targetImage; kindText = QStringLiteral("完整路径"); break;
            case 4: targetText = QStringLiteral("（全部进程）"); kindText = QStringLiteral("全局"); break;
            default: kindText = QStringLiteral("未设置"); break;
            }
            const auto actionText = [](const quint32 actionValue) {
                switch (actionValue)
                {
                case 1: return QStringLiteral("拦截");
                case 2: return QStringLiteral("仅记录");
                default: return QStringLiteral("允许");
                }
            };
            m_ruleTable->setItem(row, 0, createReadOnlyItem(targetText));
            m_ruleTable->setItem(row, 1, createReadOnlyItem(kindText));
            m_ruleTable->setItem(row, 2, createReadOnlyItem(ruleValue.ruleName));
            m_ruleTable->setItem(row, 3, createReadOnlyItem(actionText(ruleValue.readAction)));
            m_ruleTable->setItem(row, 4, createReadOnlyItem(actionText(ruleValue.writeAction)));
            m_ruleTable->setItem(row, 5, createReadOnlyItem(actionText(ruleValue.enumAction)));
            m_ruleTable->setItem(row, 6, createReadOnlyItem(ruleValue.enabled ? QStringLiteral("是") : QStringLiteral("否")));
        }

        // 复选框状态跟着规则表一起刷新，避免"手动删掉全局规则行"之后复选框
        // 还留在勾选状态这种两处状态各说各话的情况；阻塞信号防止这次同步
        // 反过来触发一次 toggleGlobalMonitor。
        if (m_globalMonitorCheck != nullptr)
        {
            const QSignalBlocker blocker(m_globalMonitorCheck);
            m_globalMonitorCheck->setChecked(hasEnabledGlobalRule());
        }
    }

    bool ClipboardGuardPage::hasEnabledGlobalRule() const
    {
        return std::any_of(m_rules.cbegin(), m_rules.cend(), [](const ClipboardGuardRule& ruleValue) {
            return ruleValue.enabled && ruleValue.targetKind == 4U;
        });
    }

    void ClipboardGuardPage::syncRulesToDriver()
    {
        std::vector<KSWORD_ARK_CLIPBOARD_POLICY_RULE> driverRules;
        driverRules.reserve(m_rules.size());
        for (const ClipboardGuardRule& ruleValue : m_rules)
        {
            KSWORD_ARK_CLIPBOARD_POLICY_RULE driverRule{};
            driverRule.ruleId = ruleValue.ruleId;
            driverRule.flags = ruleValue.enabled ? KSWORD_ARK_CLIPBOARD_POLICY_RULE_FLAG_ENABLED : 0UL;
            driverRule.targetKind = ruleValue.targetKind;
            driverRule.targetProcessId = ruleValue.targetProcessId;
            driverRule.readAction = ruleValue.readAction;
            driverRule.writeAction = ruleValue.writeAction;
            driverRule.enumAction = ruleValue.enumAction;
            const std::wstring targetImageText = ruleValue.targetImage.toStdWString();
            wcsncpy_s(driverRule.targetImage, targetImageText.c_str(), KSWORD_ARK_CLIPBOARD_POLICY_IMAGE_CHARS - 1U);
            const std::wstring ruleNameText = ruleValue.ruleName.toStdWString();
            wcsncpy_s(driverRule.ruleName, ruleNameText.c_str(), KSWORD_ARK_CLIPBOARD_POLICY_NAME_CHARS - 1U);
            driverRules.push_back(driverRule);
        }

        const ksword::ark::IoResult result = m_driverClient.setClipboardPolicy(
            KSWORD_ARK_CLIPBOARD_POLICY_FLAG_ENABLED, driverRules);
        if (!result.ok && m_statusLabel != nullptr)
        {
            m_statusLabel->setText(QStringLiteral("规则下发驱动失败：%1").arg(QString::fromStdString(result.message)));
        }
    }

    void ClipboardGuardPage::loadRulesFromDriver()
    {
        const ksword::ark::ClipboardPolicyStateResult result = m_driverClient.queryClipboardPolicyState();
        if (!result.io.ok)
        {
            if (m_statusLabel != nullptr)
            {
                m_statusLabel->setText(QStringLiteral("驱动策略回读失败：%1（可能驱动未加载，规则仍可在本页新增，下发时会再次报错）")
                    .arg(QString::fromStdString(result.io.message)));
            }
            return;
        }

        m_rules.clear();
        quint32 maxRuleId = 0;
        for (unsigned long ruleIndex = 0; ruleIndex < result.response.ruleCount; ++ruleIndex)
        {
            const KSWORD_ARK_CLIPBOARD_POLICY_RULE& driverRule = result.response.rules[ruleIndex];
            ClipboardGuardRule ruleValue;
            ruleValue.ruleId = driverRule.ruleId;
            ruleValue.enabled = (driverRule.flags & KSWORD_ARK_CLIPBOARD_POLICY_RULE_FLAG_ENABLED) != 0UL;
            ruleValue.targetKind = driverRule.targetKind;
            ruleValue.targetProcessId = driverRule.targetProcessId;
            ruleValue.targetImage = QString::fromWCharArray(driverRule.targetImage);
            ruleValue.ruleName = QString::fromWCharArray(driverRule.ruleName);
            ruleValue.readAction = driverRule.readAction;
            ruleValue.writeAction = driverRule.writeAction;
            ruleValue.enumAction = driverRule.enumAction;
            maxRuleId = std::max(maxRuleId, ruleValue.ruleId);
            m_rules.push_back(ruleValue);
        }
        m_nextLocalRuleId = maxRuleId + 1U;
        refreshRuleTable();
        if (m_statusLabel != nullptr)
        {
            m_statusLabel->setText(QStringLiteral("已从驱动加载 %1 条规则。").arg(m_rules.size()));
        }
    }

    const ClipboardGuardRule* ClipboardGuardPage::findMatchingRule(const ks::process::ProcessRecord& processRecord) const
    {
        return findMatchingRuleIn(m_rules, processRecord);
    }

    const ClipboardGuardRule* ClipboardGuardPage::findMatchingRuleIn(
        const std::vector<ClipboardGuardRule>& rules, const ks::process::ProcessRecord& processRecord)
    {
        const QString imagePath = QString::fromStdString(processRecord.imagePath);
        const QString imageName = imagePath.isEmpty()
            ? QString::fromStdString(processRecord.processName)
            : QFileInfo(imagePath).fileName();
        // globalRulePointer：命中的全局规则先记下来，不立即返回——具体规则
        // （PID/映像名/路径）必须优先于全局规则，哪怕它在规则表里排在全局规则
        // 后面；这样"全局仅记录 + 对某个进程单独设拦截"才能按预期生效。
        const ClipboardGuardRule* globalRulePointer = nullptr;
        for (const ClipboardGuardRule& ruleValue : rules)
        {
            if (!ruleValue.enabled)
            {
                continue;
            }
            switch (ruleValue.targetKind)
            {
            case 1: // PID
                if (ruleValue.targetProcessId == processRecord.pid)
                {
                    return &ruleValue;
                }
                break;
            case 2: // IMAGE_NAME
                if (!imageName.isEmpty() && imageName.compare(ruleValue.targetImage, Qt::CaseInsensitive) == 0)
                {
                    return &ruleValue;
                }
                break;
            case 3: // IMAGE_PATH：允许规则写完整路径或去盘符的路径尾部，大小写不敏感后缀匹配。
                if (!imagePath.isEmpty() && !ruleValue.targetImage.isEmpty()
                    && imagePath.endsWith(ruleValue.targetImage, Qt::CaseInsensitive))
                {
                    return &ruleValue;
                }
                break;
            case 4: // ALL：全局监控，先记下来，等具体规则都查完了再决定用不用它。
                globalRulePointer = &ruleValue;
                break;
            default:
                break;
            }
        }
        return globalRulePointer;
    }

    bool ClipboardGuardPage::writeSessionConfigForPid(
        const std::uint32_t pid, const ClipboardGuardRule& matchedRule,
        const QString& sessionId, QString* const errorTextOut) const
    {
        std::wstring agentDllPath, platformError;
        if (!ks::winapi_monitor::resolveAgentPath(pid,
                (QCoreApplication::applicationDirPath() + QStringLiteral("/APIMonitor_x64.dll")).toStdWString(),
                &agentDllPath, &platformError))
        {
            if (errorTextOut) *errorTextOut = QStringLiteral("无法选择匹配目标进程位数的 Agent DLL：%1")
                .arg(QString::fromStdWString(platformError));
            return false;
        }

        const QString configPath = QString::fromStdWString(ks::winapi_monitor::buildConfigPathForPid(pid));
        std::wstring sessionError;
        if (!ks::winapi_monitor::addSessionDirectoryReader(pid, &sessionError))
        {
            if (errorTextOut) *errorTextOut = QStringLiteral("无法准备跨账户会话目录：%1")
                .arg(QString::fromStdWString(sessionError));
            return false;
        }
        if (!QDir().mkpath(QFileInfo(configPath).absolutePath()))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("无法创建会话目录：%1").arg(QFileInfo(configPath).absolutePath());
            }
            return false;
        }

        QSaveFile configFile(configPath);
        if (!configFile.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("无法写入会话配置：%1").arg(configPath);
            }
            return false;
        }

        const auto actionKeyword = [](const quint32 actionValue) {
            switch (actionValue)
            {
            case 1: return QStringLiteral("block");
            case 2: return QStringLiteral("log_only");
            default: return QStringLiteral("allow");
            }
        };

        QTextStream outputStream(&configFile);
        outputStream.setEncoding(QStringConverter::Utf16LE);
        outputStream.setGenerateByteOrderMark(true);
        outputStream << "[monitor]\n";
        outputStream << "pipe_name=" << QString::fromStdWString(ks::winapi_monitor::buildPipeNameForPid(pid)) << '\n';
        outputStream << "stop_flag_path=" << QString::fromStdWString(ks::winapi_monitor::buildStopFlagPathForPid(pid)) << '\n';
        outputStream << "session_id=" << sessionId << '\n';
        std::wstring controllerSid;
        if (!ks::winapi_monitor::currentProcessUserSidString(&controllerSid))
        {
            if (errorTextOut) *errorTextOut = QStringLiteral("无法读取当前控制端 SID。");
            return false;
        }
        outputStream << "controller_sid=" << QString::fromStdWString(controllerSid) << '\n';
        outputStream << "agent_dll_path=" << QDir::toNativeSeparators(QString::fromStdWString(agentDllPath)) << '\n';
        // 只开剪贴板分类，不随手把通用文件/注册表/网络/进程/加载器监控一起打开——
        // "剪贴板保护"和通用 API 监控是两个不同的使用场景，这里只要剪贴板这一份。
        outputStream << "enable_file=0\n";
        outputStream << "enable_registry=0\n";
        outputStream << "enable_network=0\n";
        outputStream << "enable_process=0\n";
        outputStream << "enable_loader=0\n";
        outputStream << "enable_raw_fallback=0\n";
        outputStream << "fake_success_enabled=0\n";
        outputStream << "enable_clipboard=1\n";
        outputStream << "clipboard_read_action=" << actionKeyword(matchedRule.readAction) << '\n';
        outputStream << "clipboard_write_action=" << actionKeyword(matchedRule.writeAction) << '\n';
        outputStream << "clipboard_enum_action=" << actionKeyword(matchedRule.enumAction) << '\n';

        outputStream.flush();
        if (outputStream.status() != QTextStream::Ok || !configFile.commit())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("会话配置提交失败：%1").arg(configPath);
            }
            return false;
        }
        std::wstring aclError;
        if (!ks::winapi_monitor::grantSessionFileRead(configPath.toStdWString(), pid, &aclError))
        {
            if (errorTextOut) *errorTextOut = QStringLiteral("无法授予目标进程读取会话配置的权限：%1")
                .arg(QString::fromStdWString(aclError));
            return false;
        }
        // 新配置完整提交后再撤销上一次会话的停止标记；否则常驻 Agent
        // 在 WaitForNextSessionConfig 中会一直等待，无法重启同一 PID 的监控。
        const QString stopFlagPath = QString::fromStdWString(ks::winapi_monitor::buildStopFlagPathForPid(pid));
        if (QFile::exists(stopFlagPath) && !QFile::remove(stopFlagPath))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("会话配置已写入，但无法撤销停止标记：%1").arg(stopFlagPath);
            }
            return false;
        }
        return true;
    }

    bool ClipboardGuardPage::ensureProcessProtected(
        const ks::process::ProcessRecord& processRecord, const ClipboardGuardRule& matchedRule, QString* const errorTextOut)
    {
        QString existingSessionId;
        bool actionsUnchanged = false;
        bool pipeThreadExited = false;
        {
            std::lock_guard<std::mutex> lock(m_sessionsMutex);
            for (const std::unique_ptr<Session>& sessionPointer : m_sessions)
            {
                if (sessionPointer != nullptr && sessionPointer->pid == processRecord.pid)
                {
                    existingSessionId = sessionPointer->sessionId;
                    pipeThreadExited = sessionPointer->pipeThreadExited.load();
                    actionsUnchanged = sessionPointer->readAction == matchedRule.readAction
                        && sessionPointer->writeAction == matchedRule.writeAction
                        && sessionPointer->enumAction == matchedRule.enumAction;
                    break;
                }
            }
        }
        if (pipeThreadExited)
        {
            teardownSession(processRecord.pid);
            existingSessionId.clear();
        }
        if (!existingSessionId.isEmpty() && actionsUnchanged)
        {
            return true;
        }

        const bool newSession = existingSessionId.isEmpty();
        const QString sessionId = newSession
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : existingSessionId;

        ks::winapi_monitor::SessionLease lease;
        DWORD leaseError = 0;
        if (newSession && !lease.acquire(processRecord.pid, &leaseError))
        {
            if (errorTextOut) *errorTextOut = QStringLiteral("无法占用 PID=%1 的 Agent 会话（错误码 %2）；该进程可能已由 API 监控或剪贴板保护占用。")
                .arg(processRecord.pid).arg(leaseError);
            return false;
        }
        QString writeError;
        if (!writeSessionConfigForPid(processRecord.pid, matchedRule, sessionId, &writeError))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = writeError;
            }
            return false;
        }

        if (newSession)
        {
            // 先启动管道客户端，再注入 Agent；它建立服务端后即可连接。
            startPipeReadThreadForPid(processRecord.pid, sessionId, matchedRule, std::move(lease));
            const auto residentIt = m_residentAgentCreationTimes.find(processRecord.pid);
            const bool reuseResidentAgent = processRecord.creationTime100ns != 0
                && residentIt != m_residentAgentCreationTimes.cend()
                && residentIt->second == processRecord.creationTime100ns;
            if (!reuseResidentAgent)
            {
                std::wstring agentDllPath, injectError;
                if (!ks::winapi_monitor::resolveAgentPath(processRecord.pid,
                        (QCoreApplication::applicationDirPath() + QStringLiteral("/APIMonitor_x64.dll")).toStdWString(),
                        &agentDllPath, &injectError)
                    || !ks::winapi_monitor::injectAgent(processRecord.pid, agentDllPath, &injectError))
                {
                    teardownSession(processRecord.pid);
                    if (errorTextOut != nullptr)
                    {
                        *errorTextOut = QStringLiteral("注入 PID=%1 失败：%2")
                            .arg(processRecord.pid).arg(QString::fromStdWString(injectError));
                    }
                    return false;
                }
                if (processRecord.creationTime100ns != 0)
                {
                    m_residentAgentCreationTimes[processRecord.pid] = processRecord.creationTime100ns;
                }
            }
        }
        else
        {
            std::lock_guard<std::mutex> lock(m_sessionsMutex);
            for (const std::unique_ptr<Session>& sessionPointer : m_sessions)
            {
                if (sessionPointer != nullptr && sessionPointer->pid == processRecord.pid
                    && sessionPointer->sessionId == sessionId)
                {
                    sessionPointer->readAction = matchedRule.readAction;
                    sessionPointer->writeAction = matchedRule.writeAction;
                    sessionPointer->enumAction = matchedRule.enumAction;
                    break;
                }
            }
        }
        return true;
    }

    void ClipboardGuardPage::teardownSession(const std::uint32_t pid)
    {
        std::unique_ptr<Session> sessionPointer;
        {
            std::lock_guard<std::mutex> lock(m_sessionsMutex);
            const auto sessionIt = std::find_if(m_sessions.begin(), m_sessions.end(),
                [pid](const std::unique_ptr<Session>& candidate) { return candidate != nullptr && candidate->pid == pid; });
            if (sessionIt == m_sessions.end())
            {
                return;
            }
            sessionPointer = std::move(*sessionIt);
            m_sessions.erase(sessionIt);
        }

        // 写停止标记，让 Agent 自己的 250ms 轮询发现后主动卸载 hook、退出会话。
        const QString stopFlagPath = QString::fromStdWString(ks::winapi_monitor::buildStopFlagPathForPid(pid));
        QFile stopFile(stopFlagPath);
        if (stopFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            stopFile.write("stop");
            stopFile.close();
            std::wstring aclError;
            (void)ks::winapi_monitor::grantSessionFileRead(stopFlagPath.toStdWString(), pid, &aclError);
        }

        sessionPointer->stopFlag.store(true);
        if (sessionPointer->pipeThread != nullptr && sessionPointer->pipeThread->joinable())
        {
            // ReadFile 是由管道线程发起的同步 I/O；跨线程 CloseHandle 不能可靠
            // 取消它，还可能和读取线程的 CloseHandle 双重关闭同一个句柄。
            // 请求取消后由读取线程自己关闭管道句柄。
            ::CancelSynchronousIo(sessionPointer->pipeThread->native_handle());
            sessionPointer->pipeThread->join();
        }
    }

    void ClipboardGuardPage::startPipeReadThreadForPid(
        const std::uint32_t pid, const QString& sessionId, const ClipboardGuardRule& rule, ks::winapi_monitor::SessionLease lease)
    {
        auto sessionPointer = std::make_unique<Session>();
        sessionPointer->lease = std::move(lease);
        sessionPointer->pid = pid;
        sessionPointer->sessionId = sessionId;
        sessionPointer->readAction = rule.readAction;
        sessionPointer->writeAction = rule.writeAction;
        sessionPointer->enumAction = rule.enumAction;
        Session* const sessionRawPointer = sessionPointer.get();

        const QString pipeNameText = QString::fromStdWString(ks::winapi_monitor::buildPipeNameForPid(pid));
        QPointer<ClipboardGuardPage> guardThis(this);

        sessionPointer->pipeThread = std::make_unique<std::thread>([guardThis, sessionRawPointer, pipeNameText, pid]() {
            HANDLE pipeHandle = INVALID_HANDLE_VALUE;
            for (int attempt = 0; attempt < 120; ++attempt)
            {
                if (sessionRawPointer->stopFlag.load())
                {
                    sessionRawPointer->pipeThreadExited.store(true);
                    return;
                }
                pipeHandle = ::CreateFileW(
                    reinterpret_cast<LPCWSTR>(pipeNameText.utf16()), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (pipeHandle != INVALID_HANDLE_VALUE)
                {
                    break;
                }
                const DWORD lastError = ::GetLastError();
                if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PIPE_BUSY)
                {
                    sessionRawPointer->pipeThreadExited.store(true);
                    return;
                }
                ::WaitNamedPipeW(reinterpret_cast<LPCWSTR>(pipeNameText.utf16()), 250);
                ::Sleep(120);
            }
            if (pipeHandle == INVALID_HANDLE_VALUE)
            {
                sessionRawPointer->pipeThreadExited.store(true);
                return;
            }

            sessionRawPointer->pipeHandleValue.store(reinterpret_cast<std::uintptr_t>(pipeHandle));

            while (!sessionRawPointer->stopFlag.load())
            {
                ks::winapi_monitor::ApiMonitorEventPacket packetValue{};
                if (!ks::winapi_monitor::readEventPacket(pipeHandle, &packetValue, sessionRawPointer->stopFlag))
                {
                    break;
                }
                if (packetValue.size != sizeof(packetValue)
                    || packetValue.version != ks::winapi_monitor::kProtocolVersion)
                {
                    continue;
                }
                if (packetValue.eventKind != static_cast<std::uint32_t>(ks::winapi_monitor::EventKind::ApiCall)
                    || packetValue.sessionIdentity != ks::winapi_monitor::sessionIdentity(sessionRawPointer->sessionId.toStdWString())) continue;
                if (packetValue.category == static_cast<std::uint32_t>(ks::winapi_monitor::EventCategory::Internal))
                {
                    // 管道连上后 Agent 还要安装 Hook；只有收到确认事件，
                    // 页面才能把该进程计入“当前受保护进程”。
                    if (wideBufferToText(packetValue.apiName) == QStringLiteral("HooksInstalled"))
                    {
                        sessionRawPointer->hooksInstalled.store(true);
                    }
                    continue;
                }
                // 只关心剪贴板分类；本会话的 INI 已经只开了这一类，这里再过滤一层双保险。
                if (packetValue.category != static_cast<std::uint32_t>(ks::winapi_monitor::EventCategory::Clipboard))
                {
                    continue;
                }

                QMetaObject::invokeMethod(qApp, [guardThis, packetValue, pid]() {
                    if (guardThis.isNull())
                    {
                        return;
                    }
                    const QString processText = guardThis->resolveProcessNameForPid(pid);
                    guardThis->enqueuePendingRow(packetToEventRow(packetValue, processText));
                }, Qt::QueuedConnection);
            }

            const std::uintptr_t storedHandleValue = sessionRawPointer->pipeHandleValue.exchange(0);
            if (storedHandleValue != 0)
            {
                ::CloseHandle(reinterpret_cast<HANDLE>(storedHandleValue));
            }
            sessionRawPointer->hooksInstalled.store(false);
            sessionRawPointer->pipeThreadExited.store(true);
        });

        std::lock_guard<std::mutex> lock(m_sessionsMutex);
        m_sessions.push_back(std::move(sessionPointer));
    }

    void ClipboardGuardPage::enqueuePendingRow(ClipboardGuardEventRow rowValue)
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (m_pendingRows.size() >= kPendingRowCapacity)
        {
            m_pendingRows.pop_front();
        }
        m_pendingRows.push_back(std::move(rowValue));
    }

    void ClipboardGuardPage::flushPendingRows()
    {
        std::vector<ClipboardGuardEventRow> rowList;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            const std::size_t takeCount = std::min(kUiFlushRowLimit, m_pendingRows.size());
            rowList.reserve(takeCount);
            for (std::size_t index = 0; index < takeCount; ++index)
            {
                rowList.push_back(std::move(m_pendingRows.front()));
                m_pendingRows.pop_front();
            }
        }
        if (rowList.empty() || m_eventTable == nullptr)
        {
            return;
        }

        const bool updatesEnabled = m_eventTable->updatesEnabled();
        m_eventTable->setUpdatesEnabled(false);
        for (const ClipboardGuardEventRow& rowValue : rowList)
        {
            appendEventRow(rowValue);
        }
        const int removeCount = std::max(0, m_eventTable->rowCount() - 6000);
        if (removeCount > 0 && m_eventTable->model() != nullptr)
        {
            m_eventTable->model()->removeRows(0, removeCount);
        }
        m_eventTable->setUpdatesEnabled(updatesEnabled);
        m_eventTable->scrollToBottom();
    }

    void ClipboardGuardPage::appendEventRow(const ClipboardGuardEventRow& rowValue)
    {
        const int row = m_eventTable->rowCount();
        m_eventTable->insertRow(row);
        m_eventTable->setItem(row, ColumnTime, createReadOnlyItem(rowValue.timeText));
        m_eventTable->setItem(row, ColumnProcess, createReadOnlyItem(rowValue.processText));
        m_eventTable->setItem(row, ColumnPid, createReadOnlyItem(QString::number(rowValue.pid)));
        m_eventTable->setItem(row, ColumnOperation, createReadOnlyItem(rowValue.operationText));
        m_eventTable->setItem(row, ColumnFormat, createReadOnlyItem(rowValue.formatText));
        QTableWidgetItem* const resultItem = createReadOnlyItem(rowValue.resultText);
        if (rowValue.resultText == QStringLiteral("Blocked"))
        {
            resultItem->setForeground(QBrush(KswordTheme::ErrorColor()));
        }
        m_eventTable->setItem(row, ColumnResult, resultItem);
        m_eventTable->setItem(row, ColumnTid, createReadOnlyItem(QString::number(rowValue.tid)));
        m_eventTable->setItem(row, ColumnSession, createReadOnlyItem(rowValue.sessionText));
        m_eventTable->setItem(row, ColumnIntegrity, createReadOnlyItem(rowValue.integrityText));
        m_eventTable->setItem(row, ColumnOwner, createReadOnlyItem(rowValue.ownerText));
        QTableWidgetItem* const seqItem = createReadOnlyItem(QString::number(rowValue.seq));
        seqItem->setData(Qt::UserRole, static_cast<qulonglong>(rowValue.seq));
        m_eventTable->setItem(row, ColumnSeq, seqItem);
        m_eventTable->item(row, ColumnPid)->setData(Qt::UserRole, rowValue.pid);
    }

    QString ClipboardGuardPage::resolveProcessNameForPid(const quint32 pid)
    {
        std::lock_guard<std::mutex> lock(m_processNameCacheMutex);
        const auto cacheIt = m_processNameCache.find(pid);
        if (cacheIt != m_processNameCache.end())
        {
            return cacheIt->second;
        }
        return QStringLiteral("PID %1").arg(pid);
    }

    void ClipboardGuardPage::refreshProcessListAndSessionsAsync()
    {
        if (m_processScanInFlight.exchange(true))
        {
            // 上一轮扫描还没结束（进程数很多或磁盘慢），跳过本轮，避免任务在线程池里堆积。
            return;
        }

        // m_rules 只能在 UI 线程读写（规则编辑对话框、toggleGlobalMonitor 等都在 UI 线程改它），
        // 这里先拷一份快照带进后台线程，后台线程全程不碰 m_rules 本体，避免和规则编辑产生数据竞争。
        std::vector<ClipboardGuardRule> rulesSnapshot = m_rules;

        // 用本页面私有的 m_scanThreadPool 而不是全局线程池提交——见 .h 里
        // m_scanThreadPool 的注释和析构函数里 waitForDone() 的说明。
        QPointer<ClipboardGuardPage> guardThis(this);
        m_scanThreadPool.start([guardThis, rulesSnapshot = std::move(rulesSnapshot)]() {
            std::vector<ks::process::ProcessRecord> processList =
                ks::process::EnumerateProcesses(ks::process::ProcessEnumStrategy::Auto);

            std::unordered_map<quint32, QString> processNameCache;
            processNameCache.reserve(processList.size());
            for (const ks::process::ProcessRecord& processRecord : processList)
            {
                processNameCache.emplace(processRecord.pid, QString::fromStdString(processRecord.processName));
            }

            // 按当前规则逐一核对：命中就确保已保护，退出的进程回收会话。
            // PID 0/4（System Idle/System）不是可注入的普通进程；本进程自己
            // 也没必要监控自己——全局规则打开后这两类会被排除，避免每轮都
            // 白跑一次注入尝试。
            //
            // 这整段匹配 + 注入 + 会话回收必须留在这个后台线程里跑完：全局监控打开时
            // 命中的进程可能有一两百个，每个 ensureProcessProtected 都可能触发一次同步
            // 的 CreateRemoteThread 注入调用（单次最坏情况等到 InjectDllByPath 内部
            // 10 秒超时）；之前这段逻辑排回了 UI 线程执行，累计耗时超过系统判定无响应
            // 的阈值，2026-09-22 20:29:42 的 Windows Error Reporting AppHangB1 记录
            // （Ksword5.1.exe 被系统判定无响应并关闭）就是这里卡死的。
            const std::uint32_t selfPid = static_cast<std::uint32_t>(::GetCurrentProcessId());
            std::vector<std::uint32_t> matchedPids;
            int failureCount = 0;
            QString lastErrorText;
            for (const ks::process::ProcessRecord& processRecord : processList)
            {
                if (processRecord.pid == 0U || processRecord.pid == 4U || processRecord.pid == selfPid)
                {
                    continue;
                }
                const ClipboardGuardRule* const matchedRule =
                    ClipboardGuardPage::findMatchingRuleIn(rulesSnapshot, processRecord);
                if (matchedRule == nullptr)
                {
                    continue;
                }
                if (guardThis.isNull())
                {
                    return;
                }
                matchedPids.push_back(processRecord.pid);
                QString errorText;
                if (!guardThis->ensureProcessProtected(processRecord, *matchedRule, &errorText))
                {
                    ++failureCount;
                    lastErrorText = errorText;
                }
            }

            if (guardThis.isNull())
            {
                return;
            }

            std::vector<std::uint32_t> sessionPids;
            {
                std::lock_guard<std::mutex> lock(guardThis->m_sessionsMutex);
                for (const std::unique_ptr<Session>& sessionPointer : guardThis->m_sessions)
                {
                    if (sessionPointer != nullptr)
                    {
                        sessionPids.push_back(sessionPointer->pid);
                    }
                }
            }
            for (const std::uint32_t sessionPid : sessionPids)
            {
                if (std::find(matchedPids.cbegin(), matchedPids.cend(), sessionPid) == matchedPids.cend())
                {
                    // 进程退出或规则被删除/禁用：回收会话。写停止标记对已退出的进程是安全的空操作。
                    guardThis->teardownSession(sessionPid);
                }
            }
            for (auto residentIt = guardThis->m_residentAgentCreationTimes.begin();
                residentIt != guardThis->m_residentAgentCreationTimes.end();)
            {
                const bool stillRunning = std::any_of(processList.cbegin(), processList.cend(),
                    [residentIt](const ks::process::ProcessRecord& processRecord) {
                        return processRecord.pid == residentIt->first
                            && processRecord.creationTime100ns == residentIt->second;
                    });
                residentIt = stillRunning ? std::next(residentIt)
                    : guardThis->m_residentAgentCreationTimes.erase(residentIt);
            }

            // 到这里为止全部工作都已经做完；剩下的只是把结果排回 UI 线程展示，
            // 这一段本身很轻量，不会再造成卡顿。
            const std::size_t ruleCount = rulesSnapshot.size();
            std::size_t protectedCount = 0;
            {
                std::lock_guard<std::mutex> lock(guardThis->m_sessionsMutex);
                for (const std::unique_ptr<Session>& sessionPointer : guardThis->m_sessions)
                {
                    if (sessionPointer != nullptr && sessionPointer->hooksInstalled.load()
                        && !sessionPointer->pipeThreadExited.load())
                    {
                        ++protectedCount;
                    }
                }
            }
            QMetaObject::invokeMethod(qApp, [guardThis, processNameCache = std::move(processNameCache),
                ruleCount, protectedCount, failureCount, lastErrorText]() mutable {
                if (guardThis.isNull())
                {
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(guardThis->m_processNameCacheMutex);
                    guardThis->m_processNameCache = std::move(processNameCache);
                }

                if (guardThis->m_statusLabel != nullptr)
                {
                    QString statusText = QStringLiteral("规则 %1 条，当前受保护进程 %2 个。")
                        .arg(ruleCount).arg(protectedCount);
                    if (failureCount > 0)
                    {
                        statusText += QStringLiteral(" 有 %1 个进程处理失败，最后一次错误：%2")
                            .arg(failureCount).arg(lastErrorText);
                    }
                    guardThis->m_statusLabel->setText(statusText);
                }
                guardThis->m_processScanInFlight.store(false);
            }, Qt::QueuedConnection);
        });
    }
}
