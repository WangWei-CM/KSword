#include "BootEditorTab.h"
#include "../../UI/AsyncOperation.h"
#include "../../Internationalization/LanguageManager.h"
#include "../../Framework/PrivilegeElevationPrompt.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QThreadPool>
#include <QTimer>
#include <QVariant>

#include <atomic>

namespace
{
    // 分离文件内重复常量：
    // - kColumnIdentifier 与主文件保持一致；
    // - kDefaultCommandTimeoutMs 与主文件保持一致。
    constexpr int kColumnIdentifier = 0;
    constexpr int kDefaultCommandTimeoutMs = 30000;

    // parseBoolText：
    // - 从字符串解释布尔值；
    // - 识别失败时回退 defaultValue。
    bool parseBoolText(const QString& rawText, const bool defaultValue)
    {
        const QString normalizedText = rawText.trimmed().toLower();
        if (normalizedText == QStringLiteral("yes")
            || normalizedText == QStringLiteral("on")
            || normalizedText == QStringLiteral("true")
            || normalizedText == QStringLiteral("1")
            || normalizedText == QStringLiteral("enabled"))
        {
            return true;
        }
        if (normalizedText == QStringLiteral("no")
            || normalizedText == QStringLiteral("off")
            || normalizedText == QStringLiteral("false")
            || normalizedText == QStringLiteral("0")
            || normalizedText == QStringLiteral("disabled"))
        {
            return false;
        }
        return defaultValue;
    }

    // ===================== 只读枚举异步化支撑 =====================
    // 背景：
    // - BootEditorTab 构造函数直接调用 refreshBcdEntries()，而构造函数与 refreshBcdEntries()
    //   都位于 BootEditorTab.cpp，不在本次可修改范围内，无法把首轮枚举整体挪出构造期；
    // - 因此改在 runBcdEdit 内部拦截“刷新专用只读枚举”：真正的 bcdedit 交给线程池执行，
    //   调用方立刻拿到 pending 状态并保留界面，后台结果回投 UI 线程后才提交一次；
    // - 跨调用状态用 QObject 动态属性承载，既避开不可修改的头文件，
    //   又保证控件析构时状态随对象一起消失，不会留下文件级残留。

    // kEnumPendingGenerationProperty：进行中的枚举请求代号，属性缺失或为 0 表示当前没有后台任务。
    constexpr char kEnumPendingGenerationProperty[] = "ks_bcdEnumPendingGeneration";
    // kEnumReadyResultProperty：后台回投后暂存的枚举结果，等待下一次同步调用取走。
    constexpr char kEnumReadyResultProperty[] = "ks_bcdEnumReadyResult";

    // 枚举结果 QVariantMap 字段名：
    // - 后台线程写入、UI 线程读取；
    // - 集中定义避免两侧键名拼写漂移。
    constexpr char kEnumResultStartSucceededKey[] = "startSucceeded";
    constexpr char kEnumResultTimeoutKey[] = "timeout";
    constexpr char kEnumResultExitCodeKey[] = "exitCode";
    constexpr char kEnumResultStandardOutputKey[] = "standardOutput";
    constexpr char kEnumResultStandardErrorKey[] = "standardError";

    // kProcessStartTimeoutMs：等待子进程创建完成的上限（毫秒）。
    constexpr int kProcessStartTimeoutMs = 3000;
    // kProcessKillWaitMs：强杀后回收进程对象的等待上限（毫秒）。
    constexpr int kProcessKillWaitMs = 1000;
    // kBackgroundWaitSliceMs：后台等待的分段粒度（毫秒），用于定期复查进程状态与应用退出标记。
    constexpr int kBackgroundWaitSliceMs = 200;

    // g_enumRequestGenerationCounter：
    // - 作用：全局单调递增的枚举请求代号，用于淘汰已被新请求取代的旧结果；
    // - 说明：目前只在 UI 线程自增，用原子类型是为了防止后续被误用到其它线程。
    std::atomic<quint64> g_enumRequestGenerationCounter{ 0 };

    // isBcdRefreshEnumRequest：
    // - 作用：判断本次调用是否为刷新流程专用的整表只读枚举；
    // - 入参 argumentList：bcdedit 参数列表；入参 commandDescription：调用方描述文本；
    // - 返回：参数与描述同时匹配刷新专用枚举时返回 true，用户自定义命令不会被误判。
    bool isBcdRefreshEnumRequest(const QStringList& argumentList, const QString& commandDescription)
    {
        if (commandDescription != QStringLiteral("枚举全部 BCD 条目"))
        {
            return false;
        }
        return argumentList.size() == 3
            && argumentList.at(0).compare(QStringLiteral("/enum"), Qt::CaseInsensitive) == 0
            && argumentList.at(1).compare(QStringLiteral("all"), Qt::CaseInsensitive) == 0
            && argumentList.at(2).compare(QStringLiteral("/v"), Qt::CaseInsensitive) == 0;
    }

    // runBcdEditInCallingThread：
    // - 作用：在当前线程内完整执行一次 bcdedit，产出可跨线程搬运的纯值类型结果；
    // - 入参 argumentList：bcdedit 参数列表；入参 timeoutMs：整体执行超时上限（毫秒）；
    // - 返回：含启动状态、超时标记、退出码与标准输出/标准错误文本的 QVariantMap。
    QVariantMap runBcdEditInCallingThread(const QStringList& argumentList, const int timeoutMs)
    {
        QVariantMap resultMap;
        resultMap.insert(QString::fromLatin1(kEnumResultStartSucceededKey), false);
        resultMap.insert(QString::fromLatin1(kEnumResultTimeoutKey), false);
        resultMap.insert(QString::fromLatin1(kEnumResultExitCodeKey), -1);
        resultMap.insert(QString::fromLatin1(kEnumResultStandardOutputKey), QString());
        resultMap.insert(QString::fromLatin1(kEnumResultStandardErrorKey), QString());

        // 后台线程内刻意不设父对象：QProcess 生命周期完全由本函数栈决定，不跨线程共享。
        QProcess backgroundProcess;
        backgroundProcess.setProgram(QStringLiteral("bcdedit"));
        backgroundProcess.setArguments(argumentList);
        backgroundProcess.setProcessChannelMode(QProcess::SeparateChannels);
        backgroundProcess.start();

        if (!backgroundProcess.waitForStarted(kProcessStartTimeoutMs))
        {
            resultMap.insert(
                QString::fromLatin1(kEnumResultStandardErrorKey),
                backgroundProcess.errorString());
            return resultMap;
        }
        resultMap.insert(QString::fromLatin1(kEnumResultStartSucceededKey), true);

        // 分段等待：
        // - 后台线程没有事件循环，waitForFinished 不会重入 UI，也就不需要 processEvents；
        // - 切成小片是为了在应用退出时尽快松手，避免线程池收尾阶段被长命令拖住。
        const int effectiveTimeoutMs = timeoutMs > 0 ? timeoutMs : kDefaultCommandTimeoutMs;
        QElapsedTimer elapsedTimer;
        elapsedTimer.start();
        bool processFinished = false;
        while (true)
        {
            if (backgroundProcess.waitForFinished(kBackgroundWaitSliceMs)
                || backgroundProcess.state() == QProcess::NotRunning)
            {
                processFinished = true;
                break;
            }
            if (elapsedTimer.elapsed() >= effectiveTimeoutMs || QCoreApplication::closingDown())
            {
                break;
            }
        }

        if (processFinished)
        {
            resultMap.insert(QString::fromLatin1(kEnumResultExitCodeKey), backgroundProcess.exitCode());
        }
        else
        {
            backgroundProcess.kill();
            backgroundProcess.waitForFinished(kProcessKillWaitMs);
            resultMap.insert(QString::fromLatin1(kEnumResultTimeoutKey), true);
        }

        resultMap.insert(
            QString::fromLatin1(kEnumResultStandardOutputKey),
            QString::fromLocal8Bit(backgroundProcess.readAllStandardOutput()));
        resultMap.insert(
            QString::fromLatin1(kEnumResultStandardErrorKey),
            QString::fromLocal8Bit(backgroundProcess.readAllStandardError()));
        return resultMap;
    }
}

void BootEditorTab::applySelectedEntryChanges()
{
    if (m_commandPending) return;
    const BcdEntry* entry = currentEntry();
    if (!entry)
    {
        QMessageBox::information(this, QStringLiteral("引导编辑器"), QStringLiteral("请先选择一个引导条目。"));
        return;
    }
    const QString identifier = entry->identifierText.trimmed();
    if (identifier.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("引导编辑器"), QStringLiteral("当前条目标识符为空，无法写入。"));
        return;
    }
    BcdCommandPlan plan;
    const auto appendSet = [&plan, &identifier](const QString& key, const QString& value, const QString& title) {
        plan.commands.push_back({{QStringLiteral("/set"), identifier, key, value}, title, false});
    };
    const auto appendOptional = [&appendSet](const QString& key, const QLineEdit* edit, const QString& title) {
        const QString value = edit->text().trimmed();
        if (!value.isEmpty()) appendSet(key, value, title);
    };
    appendOptional(QStringLiteral("description"), m_descriptionEdit, QStringLiteral("写入 description"));
    appendOptional(QStringLiteral("device"), m_deviceEdit, QStringLiteral("写入 device"));
    appendOptional(QStringLiteral("osdevice"), m_osDeviceEdit, QStringLiteral("写入 osdevice"));
    appendOptional(QStringLiteral("path"), m_pathEdit, QStringLiteral("写入 path"));
    appendOptional(QStringLiteral("systemroot"), m_systemRootEdit, QStringLiteral("写入 systemroot"));
    appendOptional(QStringLiteral("locale"), m_localeEdit, QStringLiteral("写入 locale"));
    const QString policy = m_bootMenuPolicyCombo->currentData().toString().trimmed();
    if (!policy.isEmpty()) appendSet(QStringLiteral("bootmenupolicy"), policy, QStringLiteral("写入 bootmenupolicy"));
    appendSet(QStringLiteral("testsigning"), boolToBcdOnOff(m_testSigningCheck->isChecked()), QStringLiteral("写入 testsigning"));
    appendSet(QStringLiteral("nointegritychecks"), boolToBcdOnOff(m_noIntegrityCheck->isChecked()), QStringLiteral("写入 nointegritychecks"));
    appendSet(QStringLiteral("debug"), boolToBcdOnOff(m_debugCheck->isChecked()), QStringLiteral("写入 debug"));
    appendSet(QStringLiteral("bootlog"), boolToBcdYesNo(m_bootLogCheck->isChecked()), QStringLiteral("写入 bootlog"));
    appendSet(QStringLiteral("basevideo"), boolToBcdYesNo(m_baseVideoCheck->isChecked()), QStringLiteral("写入 basevideo"));
    appendSet(QStringLiteral("recoveryenabled"), boolToBcdYesNo(m_recoveryEnabledCheck->isChecked()), QStringLiteral("写入 recoveryenabled"));
    const auto appendDelete = [&plan, &identifier](const QString& key) {
        plan.commands.push_back({{QStringLiteral("/deletevalue"), identifier, key}, QStringLiteral("删除 %1").arg(key), true});
    };
    const QString mode = m_safeBootCombo->currentData().toString().trimmed().toLower();
    if (mode == QStringLiteral("off"))
    {
        appendDelete(QStringLiteral("safeboot"));
        appendDelete(QStringLiteral("safebootalternateshell"));
    }
    else if (mode == QStringLiteral("minimal") || mode == QStringLiteral("network"))
    {
        appendSet(QStringLiteral("safeboot"), mode, mode == QStringLiteral("minimal")
            ? QStringLiteral("写入 safeboot=minimal") : QStringLiteral("写入 safeboot=network"));
        appendDelete(QStringLiteral("safebootalternateshell"));
    }
    else if (mode == QStringLiteral("alternateshell"))
    {
        appendSet(QStringLiteral("safeboot"), QStringLiteral("minimal"), QStringLiteral("写入 safeboot=minimal"));
        appendSet(QStringLiteral("safebootalternateshell"), QStringLiteral("yes"), QStringLiteral("写入 safebootalternateshell=yes"));
    }
    plan.successTitle = QStringLiteral("引导编辑器");
    plan.successText = QStringLiteral("当前条目已写入完成。");
    queueBcdPlan(std::move(plan));
}

void BootEditorTab::applyBootManagerChanges()
{
    queueBcdCommand({QStringLiteral("/timeout"), QString::number(m_timeoutSpin->value())}, QStringLiteral("写入 timeout"));
}

void BootEditorTab::setLegacyBootForSelectedEntry()
{
    const BcdEntry* entry = currentEntry();
    if (!entry) { QMessageBox::information(this, QStringLiteral("传统引导"), QStringLiteral("请先选择一个引导条目。")); return; }
    queueBootMenuPolicy(entry->identifierText.trimmed(), QStringLiteral("Legacy"), QStringLiteral("当前项启用传统引导"));
}

void BootEditorTab::setLegacyBootForDefaultEntry()
{
    const QString identifier = m_defaultIdentifierText.trimmed();
    if (identifier.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("传统引导"), QStringLiteral("当前未识别到默认启动项，请先刷新 BCD。"));
        return;
    }
    queueBootMenuPolicy(identifier, QStringLiteral("Legacy"), QStringLiteral("默认项启用传统引导"));
}

void BootEditorTab::setStandardBootForSelectedEntry()
{
    const BcdEntry* entry = currentEntry();
    if (!entry) { QMessageBox::information(this, QStringLiteral("传统引导"), QStringLiteral("请先选择一个引导条目。")); return; }
    queueBootMenuPolicy(entry->identifierText.trimmed(), QStringLiteral("Standard"), QStringLiteral("当前项恢复标准引导"));
}

void BootEditorTab::setSelectedAsDefaultEntry()
{
    const BcdEntry* entry = currentEntry();
    if (!entry) { QMessageBox::information(this, QStringLiteral("引导编辑器"), QStringLiteral("请先选择一个引导条目。")); return; }
    queueBcdCommand({QStringLiteral("/default"), entry->identifierText.trimmed()}, QStringLiteral("设置默认启动项"));
}

void BootEditorTab::addSelectedToBootSequence()
{
    const BcdEntry* entry = currentEntry();
    if (!entry) { QMessageBox::information(this, QStringLiteral("引导编辑器"), QStringLiteral("请先选择一个引导条目。")); return; }
    queueBcdCommand({QStringLiteral("/bootsequence"), entry->identifierText.trimmed(), QStringLiteral("/addfirst")}, QStringLiteral("设置下一次启动项"));
}

void BootEditorTab::createCopyFromSelectedEntry()
{
    if (m_commandPending) return;
    const BcdEntry* entry = currentEntry();
    if (!entry) { QMessageBox::information(this, QStringLiteral("引导编辑器"), QStringLiteral("请先选择一个引导条目。")); return; }
    const QString identifier = entry->identifierText.trimmed();
    const QString description = readElementValue(*entry, {QStringLiteral("description"), QStringLiteral("描述")});
    const QString initial = description.trimmed().isEmpty() ? QStringLiteral("新建引导项") : QStringLiteral("%1 - 副本").arg(description.trimmed());
    const QPointer<BootEditorTab> alive(this);
    bool ok = false;
    const QString newDescription = QInputDialog::getText(this, QStringLiteral("复制引导项"), QStringLiteral("新引导项描述："), QLineEdit::Normal, initial, &ok).trimmed();
    if (!alive || !ok || newDescription.isEmpty()) return;
    BcdCommandPlan plan;
    plan.commands.push_back({{QStringLiteral("/copy"), identifier, QStringLiteral("/d"), newDescription}, QStringLiteral("复制引导项"), false});
    plan.copyAndAppend = true;
    plan.errorStyle = BcdErrorStyle::Copy;
    plan.successTitle = QStringLiteral("复制引导项");
    plan.successText = QStringLiteral("复制成功。");
    queueBcdPlan(std::move(plan));
}

void BootEditorTab::deleteSelectedEntry()
{
    if (m_commandPending) return;
    const BcdEntry* entry = currentEntry();
    if (!entry) { QMessageBox::information(this, QStringLiteral("引导编辑器"), QStringLiteral("请先选择一个引导条目。")); return; }
    const QString identifier = entry->identifierText.trimmed();
    if (identifier.compare(QStringLiteral("{bootmgr}"), Qt::CaseInsensitive) == 0)
    { QMessageBox::warning(this, QStringLiteral("删除引导项"), QStringLiteral("不允许删除 {bootmgr}。")); return; }
    if (identifier.compare(QStringLiteral("{current}"), Qt::CaseInsensitive) == 0)
    { QMessageBox::warning(this, QStringLiteral("删除引导项"), QStringLiteral("不允许删除 {current}。")); return; }
    const QPointer<BootEditorTab> alive(this);
    const auto confirmed = QMessageBox::warning(this, QStringLiteral("删除引导项"),
        QStringLiteral("即将删除条目：%1\n该操作不可撤销，是否继续？").arg(identifier), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (!alive || confirmed != QMessageBox::Yes) return;
    queueBcdCommand({QStringLiteral("/delete"), identifier}, QStringLiteral("删除引导项"));
}

void BootEditorTab::exportBcdStore()
{
    if (m_commandPending) return;
    const QPointer<BootEditorTab> alive(this);
    const QString name = QStringLiteral("bcd_backup_%1.bcd").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出 BCD"), name, QStringLiteral("BCD 文件 (*.bcd);;所有文件 (*.*)"));
    if (!alive || path.trimmed().isEmpty()) return;
    queueBcdCommand({QStringLiteral("/export"), path}, QStringLiteral("导出 BCD"), false);
}

void BootEditorTab::importBcdStore()
{
    if (m_commandPending) return;
    const QPointer<BootEditorTab> alive(this);
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入 BCD"), QString(), QStringLiteral("BCD 文件 (*.bcd);;所有文件 (*.*)"));
    if (!alive || path.trimmed().isEmpty()) return;
    const auto confirmed = QMessageBox::warning(this, QStringLiteral("导入 BCD"),
        QStringLiteral("导入会覆盖当前 BCD 存储，可能影响系统启动。\n确认继续吗？"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (!alive || confirmed != QMessageBox::Yes) return;
    queueBcdCommand({QStringLiteral("/import"), path}, QStringLiteral("导入 BCD"));
}

void BootEditorTab::executeCustomCommand()
{
    if (m_commandPending) return;
    const QString raw = m_customCommandEdit->text().trimmed();
    if (raw.isEmpty()) { QMessageBox::information(this, QStringLiteral("自定义命令"), QStringLiteral("请输入命令参数。")); return; }
    QStringList args = QProcess::splitCommand(raw);
    if (!args.isEmpty() && args.front().compare(QStringLiteral("bcdedit"), Qt::CaseInsensitive) == 0) args.removeFirst();
    if (args.isEmpty()) { QMessageBox::information(this, QStringLiteral("自定义命令"), QStringLiteral("未解析到有效参数。")); return; }
    BcdCommandPlan plan;
    plan.commands.push_back({args, QStringLiteral("执行自定义 bcdedit"), false});
    plan.errorStyle = BcdErrorStyle::Custom;
    plan.successTitle = QStringLiteral("自定义命令");
    plan.successText = QStringLiteral("命令执行成功。");
    plan.refreshAfter = !raw.toLower().contains(QStringLiteral("/enum"));
    queueBcdPlan(std::move(plan));
}

void BootEditorTab::queueBcdCommand(const QStringList& args, const QString& operation, const bool refreshAfter)
{
    BcdCommandPlan plan;
    plan.commands.push_back({args, operation, false});
    plan.successTitle = QStringLiteral("引导编辑器");
    plan.successText = QStringLiteral("%1成功。").arg(operation);
    plan.refreshAfter = refreshAfter;
    queueBcdPlan(std::move(plan));
}

void BootEditorTab::queueBootMenuPolicy(const QString& identifierText, const QString& policyText, const QString& operation)
{
    const QString identifier = identifierText.trimmed();
    const QString policy = policyText.trimmed();
    if (identifier.isEmpty() || policy.isEmpty())
    { QMessageBox::warning(this, QStringLiteral("传统引导"), QStringLiteral("参数无效：标识符或策略为空。")); return; }
    BcdCommandPlan plan;
    plan.commands.push_back({{QStringLiteral("/set"), identifier, QStringLiteral("bootmenupolicy"), policy}, operation, false});
    plan.successTitle = QStringLiteral("传统引导");
    plan.successText = QStringLiteral("%1成功。\n标识符：%2\n策略：%3").arg(operation, identifier, policy);
    queueBcdPlan(std::move(plan));
}

void BootEditorTab::setBcdCommandBusy(const bool busy)
{
    m_commandPending = busy;
    for (QWidget* widget : {m_toolbarWidget, m_editorPane, static_cast<QWidget*>(m_customCommandEdit),
        static_cast<QWidget*>(m_runCustomCommandButton), static_cast<QWidget*>(m_applyEntryButton), static_cast<QWidget*>(m_applyBootMgrButton)})
    { if (widget) widget->setEnabled(!busy); }
}

void BootEditorTab::queueBcdPlan(BcdCommandPlan plan)
{
    if (m_commandPending || plan.commands.empty()) return;
    bool needsAdministrator = false;
    for (const auto& command : plan.commands)
    {
        const QString verb = command.args.value(0).trimmed().toLower();
        if (verb != QStringLiteral("/enum") && verb != QStringLiteral("/enumall") && verb != QStringLiteral("/v")) needsAdministrator = true;
    }
    if (needsAdministrator && !ks::ui::isCurrentProcessElevated())
    {
        (void)ks::ui::requestAdministratorRestartForFeature(this, QStringLiteral("修改 BCD 启动配置"));
        return;
    }
    setProperty(kEnumPendingGenerationProperty, QVariant());
    setProperty(kEnumReadyResultProperty, QVariant());
    setBcdCommandBusy(true);
    struct StepReceipt { BcdPlannedCommand command; BcdCommandResult result; bool success = false; bool tolerated = false; bool optional = false; };
    struct Receipt { std::vector<StepReceipt> steps; BcdCommandResult finalEnum; bool success = true; bool hasEnum = false; };
    const auto accepted = m_commandOperation->submitTransaction<BcdCommandPlan, Receipt>(plan,
        [](const BcdCommandPlan& request) {
            Receipt receipt;
            const auto execute = [](const BcdPlannedCommand& command) {
                const QVariantMap map = runBcdEditInCallingThread(command.args, kDefaultCommandTimeoutMs);
                BcdCommandResult result;
                result.startSucceeded = map.value(QString::fromLatin1(kEnumResultStartSucceededKey)).toBool();
                result.timeout = map.value(QString::fromLatin1(kEnumResultTimeoutKey)).toBool();
                result.exitCode = map.value(QString::fromLatin1(kEnumResultExitCodeKey)).toInt();
                result.standardOutputText = map.value(QString::fromLatin1(kEnumResultStandardOutputKey)).toString();
                result.standardErrorText = map.value(QString::fromLatin1(kEnumResultStandardErrorKey)).toString();
                result.mergedOutputText = result.standardOutputText;
                if (!result.standardErrorText.trimmed().isEmpty())
                {
                    if (!result.mergedOutputText.trimmed().isEmpty()) result.mergedOutputText += QStringLiteral("\n");
                    result.mergedOutputText += result.standardErrorText;
                }
                kLogEvent event;
                if (!result.startSucceeded) err << event << "[BootEditor] 命令启动失败: " << command.operation.toStdString() << ", error=" << result.standardErrorText.toStdString() << eol;
                else if (result.timeout) err << event << "[BootEditor] 命令执行超时: " << command.operation.toStdString() << ", timeoutMs=" << kDefaultCommandTimeoutMs << eol;
                else if (result.exitCode == 0) info << event << "[BootEditor] 命令执行完成: " << command.operation.toStdString() << ", exitCode=" << result.exitCode << eol;
                else warn << event << "[BootEditor] 命令返回非零: " << command.operation.toStdString() << ", exitCode=" << result.exitCode << eol;
                return result;
            };
            for (const auto& command : request.commands)
            {
                StepReceipt step{command, execute(command)};
                step.success = step.result.startSucceeded && !step.result.timeout && step.result.exitCode == 0;
                const QString lower = step.result.mergedOutputText.toLower();
                step.tolerated = !step.success && command.missingOkay &&
                    (lower.contains(QStringLiteral("not found")) || lower.contains(QStringLiteral("cannot find")) || lower.contains(QStringLiteral("does not exist"))
                     || lower.contains(QStringLiteral("找不到")) || lower.contains(QStringLiteral("不存在")) || lower.contains(QStringLiteral("未找到")));
                if (step.tolerated)
                {
                    kLogEvent warning;
                    warn << warning << "[BootEditor] deletevalue 字段不存在，按目标状态继续: element="
                        << command.args.value(2).toStdString() << ", identifier=" << command.args.value(1).toStdString() << eol;
                }
                receipt.steps.push_back(std::move(step));
                if (!receipt.steps.back().success && !receipt.steps.back().tolerated) { receipt.success = false; break; }
            }
            if (request.copyAndAppend && receipt.success)
            {
                const QRegularExpression regex(QStringLiteral("\\{[^\\}\\s]+\\}"));
                auto matches = regex.globalMatch(receipt.steps.front().result.mergedOutputText);
                QString identifier;
                while (matches.hasNext()) identifier = matches.next().captured(0);
                if (!identifier.trimmed().isEmpty())
                {
                    BcdPlannedCommand command{{QStringLiteral("/displayorder"), identifier.trimmed(), QStringLiteral("/addlast")}, QStringLiteral("追加到 displayorder"), false};
                    StepReceipt step{command, execute(command)};
                    step.success = step.result.startSucceeded && !step.result.timeout && step.result.exitCode == 0;
                    step.optional = true; // Copy success remains true even if the optional displayorder update failed.
                    receipt.steps.push_back(std::move(step));
                }
            }
            if (request.refreshAfter)
            {
                receipt.finalEnum = execute({{QStringLiteral("/enum"), QStringLiteral("all"), QStringLiteral("/v")}, QStringLiteral("枚举全部 BCD 条目"), false});
                receipt.hasEnum = true;
            }
            return receipt;
        },
        [this, plan](const Receipt& receipt, std::uint64_t) {
            const QPointer<BootEditorTab> alive(this);
            for (const auto& step : receipt.steps)
            {
                appendCommandLog(QStringLiteral("bcdedit %1").arg(step.command.args.join(' ')), step.result);
                if (step.success || step.tolerated) continue;
                const auto& result = step.result;
                const QString operation = step.command.operation;
                if (!step.optional && plan.errorStyle == BcdErrorStyle::Copy)
                    QMessageBox::warning(this, QStringLiteral("复制引导项"), QStringLiteral("复制失败：\n%1").arg(result.mergedOutputText.trimmed()));
                else if (!step.optional && plan.errorStyle == BcdErrorStyle::Custom)
                    QMessageBox::warning(this, QStringLiteral("自定义命令"), QStringLiteral("命令执行失败：\n%1").arg(result.mergedOutputText.trimmed()));
                else if (!result.startSucceeded)
                    QMessageBox::warning(this, QStringLiteral("引导编辑器"), QStringLiteral("%1失败：命令未能启动。\n%2").arg(operation, result.mergedOutputText.trimmed()));
                else if (result.timeout)
                    QMessageBox::warning(this, QStringLiteral("引导编辑器"), QStringLiteral("%1失败：命令执行超时。").arg(operation));
                else if (!ks::ui::promptForPrivilegeFailure(this, operation, result.mergedOutputText))
                    QMessageBox::warning(this, QStringLiteral("引导编辑器"), QStringLiteral("%1失败：\n%2").arg(operation, result.mergedOutputText.trimmed()));
                if (!alive) return true;
            }
            if (receipt.success && !plan.successText.isEmpty())
            {
                QMessageBox::information(this, plan.successTitle, plan.successText);
                if (!alive) return true;
            }
            if (receipt.hasEnum)
            {
                appendCommandLog(QStringLiteral("bcdedit /enum all /v"), receipt.finalEnum);
                applyBcdEnumResult(receipt.finalEnum);
            }
            setBcdCommandBusy(false);
            return true;
        },
        [this](std::exception_ptr) {
            setBcdCommandBusy(false);
            m_statusLabel->setText(ks::i18n::text(QStringLiteral("misc.async.receipt_missing"), QStringLiteral("后台任务未返回有效回执，请刷新状态。")));
        });
    if (accepted == 0)
    {
        setBcdCommandBusy(false);
        m_statusLabel->setText(ks::i18n::text(QStringLiteral("misc.async.receipt_missing"), QStringLiteral("后台任务未返回有效回执，请刷新状态。")));
    }
}

void BootEditorTab::copySelectedRowToClipboard()
{
    const int rowIndex = m_entryTable->currentRow();
    if (rowIndex < 0)
    {
        return;
    }

    QStringList cellTextList;
    for (int colIndex = 0; colIndex < m_entryTable->columnCount(); ++colIndex)
    {
        QTableWidgetItem* item = m_entryTable->item(rowIndex, colIndex);
        cellTextList.push_back(item != nullptr ? item->text() : QString());
    }
    QApplication::clipboard()->setText(cellTextList.join('\t'));
}

int BootEditorTab::currentEntryIndex() const
{
    const int rowIndex = m_entryTable->currentRow();
    if (rowIndex < 0)
    {
        return -1;
    }
    QTableWidgetItem* identifierItem = m_entryTable->item(rowIndex, kColumnIdentifier);
    if (identifierItem == nullptr)
    {
        return -1;
    }
    return identifierItem->data(Qt::UserRole).toInt();
}

const BootEditorTab::BcdEntry* BootEditorTab::currentEntry() const
{
    const int index = currentEntryIndex();
    if (index < 0 || index >= static_cast<int>(m_entryList.size()))
    {
        return nullptr;
    }
    return &m_entryList[static_cast<std::size_t>(index)];
}

bool BootEditorTab::entryMatchesFilter(const BcdEntry& entry) const
{
    const QString keywordText = m_filterEdit->text().trimmed();
    if (keywordText.isEmpty())
    {
        return true;
    }
    const QString lowerKeyword = keywordText.toLower();
    const QString descriptionText = readElementValue(entry, QStringList{
        QStringLiteral("description"),
        QStringLiteral("描述")
        });
    const QString pathText = readElementValue(entry, QStringList{
        QStringLiteral("path"),
        QStringLiteral("路径")
        });
    const QString deviceText = readElementValue(entry, QStringList{
        QStringLiteral("device"),
        QStringLiteral("设备")
        });

    return entry.identifierText.toLower().contains(lowerKeyword)
        || entry.objectTypeText.toLower().contains(lowerKeyword)
        || descriptionText.toLower().contains(lowerKeyword)
        || pathText.toLower().contains(lowerKeyword)
        || deviceText.toLower().contains(lowerKeyword);
}

QString BootEditorTab::readElementValue(
    const BcdEntry& entry,
    const QStringList& candidateKeyList) const
{
    // 只做精确键匹配：
    // - 避免 device/osdevice 等相似字段被 contains 误命中；
    // - 若字段缺失则返回空，交给调用方决定后续行为。
    for (const QString& candidateRawKey : candidateKeyList)
    {
        const QString normalizedCandidateKey = normalizeElementKey(candidateRawKey);
        if (entry.elementMap.contains(normalizedCandidateKey))
        {
            return entry.elementMap.value(normalizedCandidateKey);
        }
    }
    return QString();
}

bool BootEditorTab::readElementBool(
    const BcdEntry& entry,
    const QStringList& candidateKeyList,
    const bool defaultValue) const
{
    const QString valueText = readElementValue(entry, candidateKeyList);
    return parseBoolText(valueText, defaultValue);
}

void BootEditorTab::appendCommandLog(
    const QString& commandTitle,
    const BcdCommandResult& commandResult)
{
    // Only genuine command/enum receipts reach this canonical output document.
    const QString timestampText = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    QString statusText = QStringLiteral("start=%1, timeout=%2, exit=%3")
        .arg(commandResult.startSucceeded ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(commandResult.timeout ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(commandResult.exitCode);
    QString logText = QStringLiteral("[%1] %2\n%3\n[%4]\n\n")
        .arg(timestampText)
        .arg(commandTitle)
        .arg(commandResult.mergedOutputText.trimmed())
        .arg(statusText);

    m_rawOutputEdit->moveCursor(QTextCursor::End);
    m_rawOutputEdit->insertPlainText(logText);
    m_rawOutputEdit->moveCursor(QTextCursor::End);
}

BootEditorTab::BcdCommandResult BootEditorTab::runBcdEdit(
    const QStringList& argumentList,
    const int timeoutMs,
    const QString& commandDescription)
{
    // Only the dedicated read-only refresh path uses this compatibility entry.
    BcdCommandResult result;
    const QString commandVerb = argumentList.value(0).trimmed().toLower();
    const bool readOnlyCommand = commandVerb == QStringLiteral("/enum") ||
        commandVerb == QStringLiteral("/enumall") ||
        commandVerb == QStringLiteral("/v");
    if (!readOnlyCommand && !ks::ui::isCurrentProcessElevated())
    {
        (void)ks::ui::requestAdministratorRestartForFeature(
            this,
            QStringLiteral("修改 BCD 启动配置"));
        result.startSucceeded = false;
        result.standardErrorText = QStringLiteral("需要管理员权限。请在管理员实例中重试。");
        result.mergedOutputText = result.standardErrorText;
        return result;
    }

    // ============ 刷新专用只读枚举：整段交给线程池，UI 线程零阻塞 ============
    if (isBcdRefreshEnumRequest(argumentList, commandDescription))
    {
        // 第一步：后台结果已经回投时直接取走，按同步语义返回，让刷新流程正常填表并写日志。
        const QVariant readyResultVariant = property(kEnumReadyResultProperty);
        if (readyResultVariant.isValid())
        {
            setProperty(kEnumReadyResultProperty, QVariant());

            const QVariantMap readyResultMap = readyResultVariant.toMap();
            result.startSucceeded =
                readyResultMap.value(QString::fromLatin1(kEnumResultStartSucceededKey)).toBool();
            result.timeout =
                readyResultMap.value(QString::fromLatin1(kEnumResultTimeoutKey)).toBool();
            result.exitCode =
                readyResultMap.value(QString::fromLatin1(kEnumResultExitCodeKey)).toInt();
            result.standardOutputText =
                readyResultMap.value(QString::fromLatin1(kEnumResultStandardOutputKey)).toString();
            result.standardErrorText =
                readyResultMap.value(QString::fromLatin1(kEnumResultStandardErrorKey)).toString();
            result.mergedOutputText = result.standardOutputText;
            if (!result.standardErrorText.trimmed().isEmpty())
            {
                if (!result.mergedOutputText.trimmed().isEmpty())
                {
                    result.mergedOutputText += QStringLiteral("\n");
                }
                result.mergedOutputText += result.standardErrorText;
            }

            kLogEvent asyncResultEvent;
            if (!result.startSucceeded)
            {
                err << asyncResultEvent
                    << "[BootEditor] 命令启动失败: "
                    << commandDescription.toStdString()
                    << ", error="
                    << result.standardErrorText.toStdString()
                    << eol;
            }
            else if (result.timeout)
            {
                err << asyncResultEvent
                    << "[BootEditor] 命令执行超时: "
                    << commandDescription.toStdString()
                    << ", timeoutMs="
                    << timeoutMs
                    << eol;
            }
            else if (result.exitCode == 0)
            {
                info << asyncResultEvent
                    << "[BootEditor] 命令执行完成: "
                    << commandDescription.toStdString()
                    << ", exitCode="
                    << result.exitCode
                    << eol;
            }
            else
            {
                warn << asyncResultEvent
                    << "[BootEditor] 命令返回非零: "
                    << commandDescription.toStdString()
                    << ", exitCode="
                    << result.exitCode
                    << eol;
            }
            return result;
        }

        // 第二步：没有现成结果就派发后台任务；已有任务在跑时直接复用，避免重复拉起 bcdedit。
        if (property(kEnumPendingGenerationProperty).toULongLong() == 0)
        {
            // requestGeneration 用途：淘汰已被新请求取代的旧结果。
            const quint64 requestGeneration = ++g_enumRequestGenerationCounter;
            setProperty(
                kEnumPendingGenerationProperty,
                QVariant::fromValue<qulonglong>(requestGeneration));

            // guardedSelf 用途：后台任务可能晚于控件销毁，回投前必须验证生命周期。
            const QPointer<BootEditorTab> guardedSelf(this);
            const QStringList requestArgumentList = argumentList;
            const int requestTimeoutMs = timeoutMs;
            QThreadPool::globalInstance()->start(
                [guardedSelf, requestGeneration, requestArgumentList, requestTimeoutMs]()
                {
                    // 后台线程只做纯数据采集，产出可跨线程搬运的值类型。
                    const QVariantMap collectedResultMap =
                        runBcdEditInCallingThread(requestArgumentList, requestTimeoutMs);

                    QCoreApplication* const appInstance = QCoreApplication::instance();
                    if (appInstance == nullptr)
                    {
                        return;
                    }
                    QMetaObject::invokeMethod(
                        appInstance,
                        [guardedSelf, requestGeneration, collectedResultMap]()
                        {
                            if (guardedSelf == nullptr)
                            {
                                return;
                            }
                            const quint64 currentPendingGeneration =
                                guardedSelf->property(kEnumPendingGenerationProperty).toULongLong();
                            if (currentPendingGeneration != requestGeneration)
                            {
                                return;
                            }
                            guardedSelf->setProperty(kEnumPendingGenerationProperty, QVariant());
                            if (guardedSelf->m_commandPending)
                            {
                                // A pre-transaction query cannot replace the accepted transaction/readback.
                                return;
                            }
                            guardedSelf->setProperty(kEnumReadyResultProperty, collectedResultMap);
                            guardedSelf->refreshBcdEntries();
                        });
                });
        }

        // 尚未收到真实回执时不把旧文本伪装成新快照；调用方保留表格、选择与编辑参数。
        result.pending = true;
        if (m_lastEnumRawText.trimmed().isEmpty() && m_statusLabel != nullptr)
        {
            m_statusLabel->setText(QStringLiteral("状态：尚未加载 BCD 数据"));
        }
        return result;
    }

    return result;
}

QString BootEditorTab::normalizeElementKey(const QString& rawKeyText)
{
    // 规范化规则：
    // - 转小写；
    // - 去除空格、连字符、下划线与制表符；
    // - 保留英数字与常见中文字段名。
    QString normalizedText = rawKeyText.trimmed().toLower();
    normalizedText.remove(QRegularExpression(QStringLiteral("[\\s\\-_:]")));
    return normalizedText;
}

std::vector<BootEditorTab::BcdEntry> BootEditorTab::parseBcdEnumOutput(const QString& enumOutputText)
{
    std::vector<BcdEntry> entryList;

    BcdEntry currentEntry;
    QStringList currentRawLineList;
    bool insideBlock = false;
    bool nextNonEmptyLineIsType = false;

    // flushCurrent：
    // - 将当前构建中的对象推入结果；
    // - 清理状态以便开始下一对象。
    const auto flushCurrent = [&entryList, &currentEntry, &currentRawLineList]()
        {
            if (currentEntry.identifierText.trimmed().isEmpty()
                && currentEntry.elementMap.isEmpty()
                && currentEntry.objectTypeText.trimmed().isEmpty())
            {
                currentRawLineList.clear();
                currentEntry = BcdEntry();
                return;
            }

            const QString idLowerText = currentEntry.identifierText.trimmed().toLower();
            currentEntry.isBootManager = (idLowerText == QStringLiteral("{bootmgr}"));
            currentEntry.isCurrent = (idLowerText == QStringLiteral("{current}"));
            currentEntry.rawBlockText = currentRawLineList.join('\n');
            entryList.push_back(currentEntry);

            currentEntry = BcdEntry();
            currentRawLineList.clear();
        };

    const QStringList allLineList = enumOutputText.split('\n');
    const QRegularExpression separatorRegex(QStringLiteral("^-{3,}$"));
    const QRegularExpression pairRegex(QStringLiteral("^(.+?)\\s{2,}(.+)$"));

    for (const QString& rawLine : allLineList)
    {
        const QString lineText = rawLine;
        const QString trimmedLineText = lineText.trimmed();

        if (trimmedLineText.isEmpty())
        {
            if (insideBlock)
            {
                currentRawLineList.push_back(lineText);
            }
            continue;
        }

        if (separatorRegex.match(trimmedLineText).hasMatch())
        {
            flushCurrent();
            insideBlock = true;
            nextNonEmptyLineIsType = true;
            currentRawLineList.push_back(lineText);
            continue;
        }

        if (!insideBlock)
        {
            continue;
        }

        currentRawLineList.push_back(lineText);

        if (nextNonEmptyLineIsType)
        {
            currentEntry.objectTypeText = trimmedLineText;
            nextNonEmptyLineIsType = false;
            continue;
        }

        const QRegularExpressionMatch pairMatch = pairRegex.match(lineText);
        if (!pairMatch.hasMatch())
        {
            continue;
        }

        const QString keyText = pairMatch.captured(1).trimmed();
        const QString valueText = pairMatch.captured(2).trimmed();
        const QString normalizedKeyText = normalizeElementKey(keyText);

        if (!normalizedKeyText.isEmpty())
        {
            currentEntry.elementMap.insert(normalizedKeyText, valueText);
        }

        const bool maybeIdentifierKey =
            normalizedKeyText.contains(QStringLiteral("identifier"))
            || normalizedKeyText.contains(QStringLiteral("标识符"))
            || normalizedKeyText.contains(QStringLiteral("识别符"));
        if (currentEntry.identifierText.trimmed().isEmpty())
        {
            if (maybeIdentifierKey)
            {
                currentEntry.identifierText = valueText;
            }
            else if (valueText.startsWith('{') && valueText.endsWith('}'))
            {
                currentEntry.identifierText = valueText;
            }
        }
    }

    flushCurrent();
    return entryList;
}

QString BootEditorTab::boolToBcdOnOff(const bool enabled)
{
    return enabled ? QStringLiteral("on") : QStringLiteral("off");
}

QString BootEditorTab::boolToBcdYesNo(const bool enabled)
{
    return enabled ? QStringLiteral("yes") : QStringLiteral("no");
}
