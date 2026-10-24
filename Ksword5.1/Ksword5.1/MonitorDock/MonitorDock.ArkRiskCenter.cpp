#include "../UI/CompleteExport.h"
#include "MonitorNavigationSchema.h"
#include "../UI/StructuredFieldView.h"
#include "../UI/TableContextMenu.h"
#include "MonitorDock.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/PageControlStyle.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/FlatButtonTheme.h"
#include "../Internationalization/LanguageManager.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/CodeEditorWidget.h"
#include "../UI/TableColumnAutoFit.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/DetailLayoutRegistry.h"
#include "../ksword/process/process.h"
#include "../theme.h"

// F-09 / F-12：本文件是把这两条判据接进生产调用点的第一处。
// 判据本身留在 shared/evidence 里（Qt-free / Win32-free），这边只做值转换和提示文案，
// 不在 Qt 侧另造一套“像不像同一个进程”的土办法。
#include "../../../shared/evidence/LiveNavigation.h"
#include "../../../shared/evidence/ObjectIdentity.h"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QItemSelectionModel>
#include <QEvent>
#include <QTableView>
#include <QTimer>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaMethod>
#include <QMetaObject>
#include <QModelIndex>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThreadPool>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// evidence：分析层判据的短别名。生产侧只往里传值、只读它的枚举结论。
namespace evidence = Ksword::Evidence;

namespace
{
    ks::ui::FieldNode nativeJsonNode(const QString& name, const QJsonValue& value)
    {
        ks::ui::FieldNode node;
        node.name = name;
        if (value.isObject()) {
            node.kind = ks::ui::FieldNode::Kind::Section;
            const auto object = value.toObject();
            for (auto it = object.constBegin(); it != object.constEnd(); ++it) node.children.append(nativeJsonNode(it.key(), it.value()));
        } else if (value.isArray()) {
            node.kind = ks::ui::FieldNode::Kind::Section;
            const auto array = value.toArray();
            for (qsizetype i = 0; i < array.size(); ++i) node.children.append(nativeJsonNode(QString::number(i), array[i]));
        } else if (value.isBool()) node.value = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        else if (value.isDouble()) node.value = QString::number(value.toDouble(), 'g', 17);
        else if (value.isString()) node.value = value.toString();
        else node.value = QStringLiteral("null");
        return node;
    }

    enum class RiskColumn : int { Score = 0, Source, Category, Title, Detail, Count };

    constexpr int kRiskEntryIndexRole = Qt::UserRole + 1;
    constexpr int kRiskProcessIdRole = Qt::UserRole + 2;
    // kRiskEvidenceIdRole：F-12 的证据 id 随表格行走。
    // 为什么不复用 kRiskEntryIndexRole 去查缓存：下标在快照被换掉之后仍然“有效”，
    // 会静默指向另一条记录；证据 id 查不到就是查不到，能把这种情况显式暴露出来。
    constexpr int kRiskEvidenceIdRole = Qt::UserRole + 3;

    // ERROR_ACCESS_DENIED / ERROR_INVALID_PARAMETER。
    // 这里不引 Windows.h：本文件只需要区分“读不到”和“不存在”两种失败，
    // 用两个常量比拖进整个 Win32 头更清楚。
    constexpr std::uint64_t kWin32ErrorAccessDenied = 5U;
    constexpr std::uint64_t kWin32ErrorInvalidParameter = 87U;

    int riskColumnIndex(const RiskColumn column)
    {
        // Input: risk-center logical column. Processing: cast to Qt table column. Return: integer index.
        return static_cast<int>(column);
    }

    QString hex64(const std::uint64_t value)
    {
        // Input: 64-bit address/hash/mask. Processing: render fixed-width uppercase hex. Return: display text.
        return QStringLiteral("0x%1").arg(static_cast<qulonglong>(value), 16, 16, QChar('0')).toUpper();
    }

    QString hex32(const std::uint32_t value)
    {
        // Input: 32-bit flags/status. Processing: render fixed-width uppercase hex. Return: display text.
        return QStringLiteral("0x%1").arg(value, 8, 16, QChar('0')).toUpper();
    }

    QString wideText(const std::wstring& value)
    {
        // Input: ArkDriverClient wide string. Processing: convert to QString. Return: empty or display text.
        return value.empty() ? QString() : QString::fromStdWString(value);
    }

    QString narrowText(const std::string& value)
    {
        // Input: ArkDriverClient narrow string. Processing: convert to QString. Return: empty or display text.
        return value.empty() ? QString() : QString::fromStdString(value);
    }

    QString friendlyIoMessage(const std::string& value)
    {
        // 输入：ArkDriverClient io.message 原始文本。
        // 处理：把 DeviceIoControl/unsupported/空消息转换为适合状态栏和风险明细的中文说明。
        // 返回：可直接拼入 ARK 风险中心摘要行的短文本。
        const QString rawText = narrowText(value).trimmed();
        if (rawText.isEmpty())
        {
            return QStringLiteral("无额外驱动消息");
        }
        if (rawText.contains(QStringLiteral("DeviceIoControl"), Qt::CaseInsensitive))
        {
            return QStringLiteral("驱动接口调用失败或当前驱动版本不支持该审计入口");
        }
        if (rawText.contains(QStringLiteral("unsupported"), Qt::CaseInsensitive) ||
            rawText.contains(QStringLiteral("not implemented"), Qt::CaseInsensitive))
        {
            return QStringLiteral("当前驱动版本尚未提供该审计入口");
        }
        return rawText;
    }

    double clampScore(const double score)
    {
        // Input: arbitrary risk score. Processing: clamp to the risk-center range. Return: 0..100 score.
        return std::max(0.0, std::min(100.0, score));
    }

    QString scoreText(const double score)
    {
        // Input: normalized risk score. Processing: keep one decimal place. Return: sortable display text.
        return QString::number(clampScore(score), 'f', 1);
    }

    QString ioSummary(const ksword::ark::IoResult& io)
    {
        // Input: ArkDriverClient I/O result. Processing: compact transport/protocol diagnostics. Return: one line.
        return QStringLiteral("ok=%1 win32=%2 nt=%3 bytes=%4 说明=%5")
            .arg(io.ok ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(io.win32Error)
            .arg(hex32(static_cast<std::uint32_t>(io.ntStatus)))
            .arg(io.bytesReturned)
            .arg(friendlyIoMessage(io.message));
    }

    QString arkRiskTableMenuStyle()
    {
        // 输入：无。
        // 处理：生成不透明 QMenu 样式，避免深色主题下右键菜单透明或文字不可读。
        // 返回：可直接 setStyleSheet 到表格右键菜单的样式文本。
        return QStringLiteral(
            "QMenu{background:%1;color:%2;border:1px solid %3;}"
            "QMenu::item{padding:5px 24px 5px 24px;background:transparent;}"
            "QMenu::item:selected{background:%4;color:%6;}"
            "QMenu::item:disabled{color:%5;}")
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::AccentHex(KswordTheme::AccentRole::Blue))
            .arg(KswordTheme::TextSecondaryHex())
            .arg(KswordTheme::OnAccentDynamicHex());
    }

    void copyArkRiskTableCurrentRow(QTableView* table)
    {
        // 输入：风险中心表格。
        // 处理：读取当前显示行的所有列，按 TSV 写入剪贴板。
        // 返回：无返回值；无当前行或剪贴板不可用时静默返回。
        if (table == nullptr || QApplication::clipboard() == nullptr)
        {
            return;
        }

        const int rowIndex = table->currentIndex().row();
        if (rowIndex < 0 || rowIndex >= table->model()->rowCount())
        {
            return;
        }

        QStringList fields;
        fields.reserve(table->model()->columnCount());
        for (int columnIndex = 0; columnIndex < table->model()->columnCount(); ++columnIndex)
        {
            fields.push_back(table->model()->index(rowIndex, columnIndex).data().toString());
        }
        QApplication::clipboard()->setText(fields.join(QChar('\t')));
    }

    void installArkRiskTableCopyMenu(MonitorDock* dock, QTableView* table)
    {
        // 输入：风险中心所属 Dock 与结果表格。
        // 处理：安装“复制当前行”和“转到进程详细信息”的右键菜单；右键点击行时同步当前单元格。
        //       跳转本身交给 MonitorDock::navigateToProcessDetailFromArkRiskRow，因为
        //       F-09/F-12 的判据需要读 Dock 的快照缓存（证据 id 反查），菜单这层拿不到。
        // 返回：无返回值；不触发任何审计动作或系统修改。
        if (dock == nullptr || table == nullptr)
        {
            return;
        }

        table->setContextMenuPolicy(Qt::CustomContextMenu);
        QPointer<MonitorDock> guardDock(dock);
        QObject::connect(table, &QTableView::customContextMenuRequested, table, [guardDock, table](const QPoint& localPosition) {
            const QModelIndex clickedIndex = table->indexAt(localPosition);
            if (clickedIndex.isValid())
            {
                table->setCurrentIndex(clickedIndex);
            }

            QMenu menu(table);
            menu.setStyleSheet(arkRiskTableMenuStyle());
            QAction* copyRowAction = menu.addAction(QStringLiteral("复制当前行"));
            // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
            ks::ui::SetTableContextActionRole(copyRowAction, ks::ui::TableContextActionRole::CopyRows);
            copyRowAction->setEnabled(table->currentIndex().row() >= 0);

            quint32 processId = 0U;
            const QModelIndex scoreItem = table->model()->index(table->currentIndex().row(), riskColumnIndex(RiskColumn::Score));
            bool processIdOk = false;
            const qulonglong storedProcessId = scoreItem.isValid()
                ? scoreItem.data(kRiskProcessIdRole).toULongLong(&processIdOk)
                : 0ULL;
            if (processIdOk && storedProcessId > 0ULL &&
                storedProcessId <= static_cast<qulonglong>(std::numeric_limits<quint32>::max()))
            {
                processId = static_cast<quint32>(storedProcessId);
            }

            QAction* openProcessAction = menu.addAction(
                QIcon(QStringLiteral(":/Icon/process_details.svg")),
                QStringLiteral("转到进程详细信息"));
            // 只要这行指向某个 PID 就允许点击：身份够不够由判据说了算，
            // 说明也由判据给。菜单这层提前灰掉只会让用户不知道为什么不能跳。
            openProcessAction->setEnabled(processId != 0U);

            const int currentRow = table->currentIndex().row();
            const QAction* selectedAction = menu.exec(table->viewport()->mapToGlobal(localPosition));
            if (selectedAction == copyRowAction)
            {
                copyArkRiskTableCurrentRow(table);
            }
            else if (selectedAction == openProcessAction && guardDock != nullptr)
            {
                guardDock->navigateToProcessDetailFromArkRiskRow(currentRow);
            }
        });
    }

    quint32 payloadProcessId(const QJsonObject& payload)
    {
        const QJsonValue processIdValue = payload.value(QStringLiteral("processId"));
        bool processIdOk = false;
        const qulonglong processId = processIdValue.toVariant().toULongLong(&processIdOk);
        return processIdOk && processId > 0ULL &&
            processId <= static_cast<qulonglong>(std::numeric_limits<quint32>::max())
            ? static_cast<quint32>(processId)
            : 0U;
    }

    QString arkRiskSessionBootId()
    {
        // 输入：无。
        // 处理：返回本次 KSword 运行的会话标识，用作 ProcessInstanceId::bootId 的保守替身。
        //       为什么可以这么用（F-03/F-09）：
        //         1) ObjectIdentity.h 要求两侧 bootId 非空且相等才可能判 Confirmed，这一位
        //            是用来挡“跨启动的同 PID”的；
        //         2) 风险中心快照只存在于内存，采集与点击必然发生在同一次进程运行内，而一
        //            次进程运行不可能跨越重启，所以“同一运行 ⇒ 同一启动周期”不是过度声称；
        //         3) 反向的偏差是安全的：两次运行即使同属一次系统启动也会拿到不同 token，
        //            结果只会退化成 Candidate 并拒绝跳转，不会放行错误目标。
        // 返回：进程内恒定的会话标识文本。
        static const QString sessionBootId = QStringLiteral("ksword-run-%1-%2")
            .arg(static_cast<qulonglong>(QCoreApplication::applicationPid()))
            .arg(static_cast<qulonglong>(QDateTime::currentMSecsSinceEpoch()));
        return sessionBootId;
    }

    bool parseWin32CodeFromDetail(const std::string& detailText, std::uint64_t& codeOut)
    {
        // 输入：ks::process::QueryProcessCreationTimeByPid 的诊断原文，形如 "OpenProcess failed(5)"。
        // 处理：取最后一对括号里的十进制数。解析失败绝不编一个 0 出来——那会把“错误码未知”
        //       伪装成“错误码是 0（成功）”。
        // 返回：解析成功为 true 且写 codeOut，否则 false 且不改 codeOut。
        const std::size_t openIndex = detailText.rfind('(');
        if (openIndex == std::string::npos)
        {
            return false;
        }
        const std::size_t closeIndex = detailText.find(')', openIndex + 1U);
        if (closeIndex == std::string::npos || closeIndex <= openIndex + 1U)
        {
            return false;
        }
        const std::string_view codeText =
            std::string_view(detailText).substr(openIndex + 1U, closeIndex - openIndex - 1U);
        return evidence::ParseU64(codeText, codeOut);
    }

    struct LiveProcessProbe final
    {
        // present：是否有正面证据表明该 PID 当前确实绑着一个进程对象。
        // 注意它不等于 createTimeKnown：权限不足读不到创建时间，恰恰说明对象存在。
        bool present = false;
        bool createTimeKnown = false;
        std::uint64_t createTime100ns = 0U;
        evidence::CollectionOutcome outcome;
    };

    LiveProcessProbe probeProcessCreateTime(const std::uint32_t processId)
    {
        // 输入：目标 PID。
        // 处理：读一次当前进程实例的创建时间，并把失败语义拆开（F-05）：
        //         * 成功                       -> present + createTimeKnown
        //         * ERROR_INVALID_PARAMETER(87) -> 这个 PID 上没有进程，present=false
        //         * ERROR_ACCESS_DENIED(5) 及其它 -> 对象在，但创建时间读不到，present=true
        //       为什么默认落在“读不到”而不是“已退出”：ResolveProcessNavigation 把
        //       !found 直接判成 RejectObjectExited；如果把权限失败也算作 !found，就会
        //       对一个还活着的进程宣称“已退出”，那是凭空捏造的结论。
        //       两条路径都会拒绝跳转，差别只在给用户的说明是否属实。
        // 返回：探测结果；原始错误码与原文一并保留在 outcome 里。
        LiveProcessProbe probe;
        if (processId == 0U)
        {
            probe.outcome = evidence::CollectionOutcome::notCollected();
            return probe;
        }

        std::uint64_t createTime100ns = 0U;
        std::string detailText;
        if (ks::process::QueryProcessCreationTimeByPid(processId, &createTime100ns, &detailText))
        {
            probe.present = true;
            probe.createTimeKnown = true;
            probe.createTime100ns = createTime100ns;
            probe.outcome = evidence::CollectionOutcome::success();
            return probe;
        }

        std::uint64_t win32Code = 0U;
        const bool codeKnown = parseWin32CodeFromDetail(detailText, win32Code);
        if (!codeKnown)
        {
            // 错误码解析不出来：状态记 Error，nativeCode 保持 unset（“未知”不是 0）。
            probe.present = true;
            probe.outcome.status = evidence::CollectionStatus::Error;
            probe.outcome.message = detailText;
            return probe;
        }

        probe.present = win32Code != kWin32ErrorInvalidParameter;
        probe.outcome = evidence::CollectionOutcome::failure(
            win32Code == kWin32ErrorAccessDenied
                ? evidence::CollectionStatus::AccessDenied
                : evidence::CollectionStatus::Error,
            std::string("WIN32"),
            win32Code,
            detailText);
        return probe;
    }

    evidence::ProcessInstanceId makeSavedProcessIdentity(const MonitorDock::ArkRiskCenterEntry& entry)
    {
        // 输入：一行风险记录。
        // 处理：把采集期固定下来的字段翻译成分析层的 ProcessInstanceId。只做值转换，
        //       不把 Qt 类型塞进 shared/evidence。
        //       pid / createTime 缺失时保持 OptionalU64 的 unset 状态：那会让 strength()
        //       落到 Weak/Unusable，crossSessionKey() 返回空串，导航随即被判 IdentityUnusable。
        //       这正是我们想要的——身份不足就不许跳。
        // 返回：采集期身份。
        evidence::ProcessInstanceId identity;
        identity.bootId = entry.identityBootId.toStdString();
        if (entry.processId != 0U)
        {
            identity.pid = evidence::OptionalU64::of(static_cast<std::uint64_t>(entry.processId));
        }
        if (entry.processCreateTimeKnown)
        {
            identity.createTime100ns = evidence::OptionalU64::of(entry.processCreateTime100ns);
        }
        identity.imageName = entry.payload.value(QStringLiteral("imageName")).toString().toStdString();
        return identity;
    }

    QJsonValue optionalU64Json(const evidence::OptionalU64& value, const evidence::U64Format format)
    {
        // 输入：可能未知的 64 位值与持久化格式。
        // 处理：未知写 JSON null，已知写十进制/十六进制字符串。
        //       F-08：全程走字符串，不经过 double；“未知”也绝不退化成 "0"。
        // 返回：可直接插入 payload 的 JSON 值。
        const std::string text = evidence::FormatOptionalU64(value, format);
        return text.empty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QString::fromStdString(text));
    }

    QJsonObject collectionOutcomeJson(const evidence::CollectionOutcome& outcome)
    {
        // 输入：一次采集的结果。
        // 处理：把状态、原始错误码域、原始错误码和原文一起写进导出，
        //       让“权限不足 / 不存在 / 其它失败”在 JSON/CSV 里也保持可区分（F-05）。
        // 返回：payload 子对象。
        QJsonObject json;
        json.insert(QStringLiteral("status"),
            QString::fromLatin1(evidence::CollectionStatusName(outcome.status)));
        json.insert(QStringLiteral("nativeCodeDomain"),
            outcome.nativeCodeDomain.empty() ? QJsonValue(QJsonValue::Null)
                                             : QJsonValue(QString::fromStdString(outcome.nativeCodeDomain)));
        json.insert(QStringLiteral("nativeCode"),
            optionalU64Json(outcome.nativeCode, evidence::U64Format::Decimal));
        json.insert(QStringLiteral("message"),
            outcome.message.empty() ? QJsonValue(QJsonValue::Null)
                                    : QJsonValue(QString::fromStdString(outcome.message)));
        return json;
    }

    void attachArkRiskNavigationIdentity(
        std::vector<MonitorDock::ArkRiskCenterEntry>& entries,
        const QString& sessionBootId,
        const std::uint64_t refreshTicket)
    {
        // 输入：已排好序的风险行、本次运行的会话标识、本轮刷新票据。
        // 处理：F-12 给每行发一个证据 id；F-09 在**采集瞬间**就把该行指向的进程实例
        //       身份（PID + 创建时间）固定下来。
        //       为什么创建时间必须在这里读：等用户点击时再读，读到的是“此刻占着这个
        //       PID 的进程”，那就没有任何东西能证明它和风险行说的是同一个实例——PID
        //       复用会被完全漏掉。
        //       读不到时保持 processCreateTimeKnown=false，绝不写 0 顶替。
        // 返回：无返回值。本函数在工作线程运行，只写源文本与数值，不调用 LanguageManager。
        std::unordered_map<std::uint32_t, LiveProcessProbe> probeCache;
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            MonitorDock::ArkRiskCenterEntry& entry = entries[index];
            // 证据 id 带上票据：旧快照的行不会和新快照的行撞号，
            // 于是“菜单还开着但快照已换”能被查出来而不是静默指向别人。
            entry.evidenceId = QStringLiteral("ark-risk/%1/%2")
                .arg(static_cast<qulonglong>(refreshTicket))
                .arg(static_cast<qulonglong>(index));
            entry.payload.insert(QStringLiteral("evidenceId"), entry.evidenceId);

            entry.identityBootId = sessionBootId;
            entry.processId = static_cast<std::uint32_t>(payloadProcessId(entry.payload));
            if (entry.processId == 0U)
            {
                // 这行本来就不指向进程（驱动 / CPU / Hook 类）。
                // 保持 NotCollected：不是“采集失败”，也不是“没有身份问题”。
                entry.processIdentityOutcome = evidence::CollectionOutcome::notCollected();
                continue;
            }

            auto cached = probeCache.find(entry.processId);
            if (cached == probeCache.end())
            {
                cached = probeCache.emplace(entry.processId, probeProcessCreateTime(entry.processId)).first;
            }
            const LiveProcessProbe& probe = cached->second;
            entry.processCreateTimeKnown = probe.createTimeKnown;
            entry.processCreateTime100ns = probe.createTime100ns;
            entry.processIdentityOutcome = probe.outcome;

            evidence::OptionalU64 createTime;
            if (probe.createTimeKnown)
            {
                createTime = evidence::OptionalU64::of(probe.createTime100ns);
            }
            QJsonObject identityJson;
            identityJson.insert(QStringLiteral("bootId"), sessionBootId);
            identityJson.insert(QStringLiteral("pid"),
                QString::fromStdString(evidence::FormatU64(entry.processId, evidence::U64Format::Decimal)));
            identityJson.insert(QStringLiteral("createTime100ns"),
                optionalU64Json(createTime, evidence::U64Format::Decimal));
            identityJson.insert(QStringLiteral("createTimeCollection"), collectionOutcomeJson(probe.outcome));
            entry.payload.insert(QStringLiteral("processIdentity"), identityJson);
        }
    }

    bool processDetailRouteAvailable()
    {
        // 输入：无。
        // 处理：只读探测是否存在能接收带身份进程详情导航的顶层窗口。
        //       为什么不用 invokeMethod 试探：那会真的把导航发出去，无法先判断再决定；
        //       F-12 要求“目标缺失时准确解释”，就必须把“目标页不在”和“对象不在页面里”
        //       分成两个可区分的结论。
        // 返回：存在可接收该导航的窗口为 true。
        const QWidgetList topLevelWidgetList = QApplication::topLevelWidgets();
        for (const QWidget* topLevelWidget : topLevelWidgetList)
        {
            if (topLevelWidget == nullptr)
            {
                continue;
            }
            const QMetaObject* metaObject = topLevelWidget->metaObject();
            for (int methodIndex = 0; methodIndex < metaObject->methodCount(); ++methodIndex)
            {
                const QMetaMethod method = metaObject->method(methodIndex);
                // 按方法名 + 形参个数匹配，不拼签名字符串：moc 记录的是头文件里写的
                // 类型名（quint32/quint64），拼签名一旦和它对不上就会误判成“目标页不在”。
                if (method.parameterCount() == 2 &&
                    method.name() == QByteArrayLiteral("openProcessDetailByIdentity"))
                {
                    return true;
                }
            }
        }
        return false;
    }

    QString arkRiskNavigationRejectionText(
        const evidence::NavigationOutcome outcome,
        const evidence::LiveNavigationDecision liveDecision,
        const bool liveDecisionEvaluated,
        const bool noProcessBinding)
    {
        // 输入：F-12 的请求判定、F-09 的现场复核判定、后者是否真的跑过，
        //       以及这行是否压根不指向任何进程。
        // 处理：把两条判据的结论翻译成用户能读懂的一句话。只在 GUI 线程调用，
        //       所以这里可以走 LanguageManager。
        // 返回：拒绝原因文本。
        if (noProcessBinding)
        {
            // 判据同样是 IdentityUnusable，但原因是"没有对象"而不是"创建时间没取到"。
            // 复用后者的文案会让用户去查一个根本不存在的权限问题。
            return ks::i18n::contextText(
                QStringLiteral("monitor.ark_risk.nav.no_process"),
                QStringLiteral("该风险记录没有关联到任何进程实例，无法转到进程详情。"));
        }
        if (!liveDecisionEvaluated)
        {
            switch (outcome)
            {
            case evidence::NavigationOutcome::IdentityUnusable:
                return ks::i18n::contextText(
                    QStringLiteral("monitor.ark_risk.nav.identity_unusable"),
                    QStringLiteral(
                        "该风险记录在采集时没能取到进程创建时间，身份不足以确认是同一个进程实例。"
                        "为避免 PID 被复用后打开无关进程，本次跳转已取消。"));
            case evidence::NavigationOutcome::EvidenceIdMissing:
                return ks::i18n::contextText(
                    QStringLiteral("monitor.ark_risk.nav.evidence_id_missing"),
                    QStringLiteral("该行没有携带证据 id，跳转后无法回到原始证据，本次跳转已取消。"));
            case evidence::NavigationOutcome::EvidenceNotSaved:
                return ks::i18n::contextText(
                    QStringLiteral("monitor.ark_risk.nav.evidence_not_saved"),
                    QStringLiteral(
                        "这条证据已不在当前风险快照中（快照可能已被重新刷新）。请重新刷新风险后再试。"));
            case evidence::NavigationOutcome::TargetPageMissing:
                return ks::i18n::contextText(
                    QStringLiteral("monitor.ark_risk.nav.target_page_missing"),
                    QStringLiteral("找不到可接收进程详情导航的主窗口，目标页可能已关闭。"));
            case evidence::NavigationOutcome::ObjectNotPresent:
                return ks::i18n::contextText(
                    QStringLiteral("monitor.ark_risk.nav.object_not_present"),
                    QStringLiteral("该 PID 对应的进程当前已不存在，无法转到进程详情。"));
            case evidence::NavigationOutcome::Delivered:
                // Delivered 一定会进入现场复核，走不到这里；留着是为了穷尽枚举。
                break;
            }
            // 兜底只说"无法确认"，不替判据编一个更确定的结论。
            return ks::i18n::contextText(
                QStringLiteral("monitor.ark_risk.nav.identity_unverifiable"),
                QStringLiteral(
                    "现场无法确认该 PID 仍是采集时的那个进程实例（例如权限不足读不到创建时间）。"
                    "本次跳转已取消。"));
        }

        switch (liveDecision)
        {
        case evidence::LiveNavigationDecision::RejectObjectExited:
            return ks::i18n::contextText(
                QStringLiteral("monitor.ark_risk.nav.object_exited"),
                QStringLiteral("该风险记录对应的进程实例已退出，本次跳转已取消。"));
        case evidence::LiveNavigationDecision::RejectIdentityMismatch:
            return ks::i18n::contextText(
                QStringLiteral("monitor.ark_risk.nav.identity_mismatch"),
                QStringLiteral(
                    "该 PID 现在属于另一个进程实例（创建时间不一致，PID 已被复用）。"
                    "本次跳转已取消，不会把操作交给新进程。"));
        case evidence::LiveNavigationDecision::RejectIdentityUnverifiable:
            return ks::i18n::contextText(
                QStringLiteral("monitor.ark_risk.nav.identity_unverifiable"),
                QStringLiteral(
                    "现场无法确认该 PID 仍是采集时的那个进程实例（例如权限不足读不到创建时间）。"
                    "本次跳转已取消。"));
        case evidence::LiveNavigationDecision::Allow:
            // Allow 时调用方已经跳走，不会来要拒绝文案；同样只为穷尽枚举。
            break;
        }
        return ks::i18n::contextText(
            QStringLiteral("monitor.ark_risk.nav.identity_unverifiable"),
            QStringLiteral(
                "现场无法确认该 PID 仍是采集时的那个进程实例（例如权限不足读不到创建时间）。"
                "本次跳转已取消。"));
    }

    QJsonObject payloadBase(const QString& source, const QString& category, const QString& title, const QString& detail, const double score)
    {
        // 输入：风险来源、分类、标题、详情和分数。
        // 处理：构造导出和详情区复用的 JSON 根对象。
        // 返回：包含风险基础字段的 payload 对象。
        QJsonObject payload;
        payload.insert(QStringLiteral("source"), source);
        payload.insert(QStringLiteral("category"), category);
        payload.insert(QStringLiteral("title"), title);
        payload.insert(QStringLiteral("detail"), detail);
        payload.insert(QStringLiteral("riskScore"), clampScore(score));
        return payload;
    }

    MonitorDock::ArkRiskCenterEntry makeEntry(
        const QString& source,
        const QString& category,
        const QString& title,
        const QString& detail,
        const double score,
        QJsonObject payload)
    {
        // Input: display fields and structured payload. Processing: fill cache row. Return: risk-center entry.
        MonitorDock::ArkRiskCenterEntry entry;
        entry.sourceName = source;
        entry.category = category;
        entry.title = title;
        entry.detail = detail;
        entry.riskScore = clampScore(score);
        entry.riskScoreText = scoreText(entry.riskScore);
        payload.insert(QStringLiteral("source"), source);
        payload.insert(QStringLiteral("category"), category);
        payload.insert(QStringLiteral("title"), title);
        payload.insert(QStringLiteral("detail"), detail);
        payload.insert(QStringLiteral("riskScore"), entry.riskScore);
        entry.payload = std::move(payload);
        return entry;
    }

    QString csvEscape(const QString& value)
    {
        // Input: CSV field. Processing: quote when needed and double embedded quotes. Return: CSV-safe text.
        QString escaped = value;
        escaped.replace(QChar('"'), QStringLiteral("\"\""));
        if (escaped.contains(QChar(',')) || escaped.contains(QChar('"')) || escaped.contains(QChar('\n')) || escaped.contains(QChar('\r')))
        {
            escaped = QStringLiteral("\"%1\"").arg(escaped);
        }
        return escaped;
    }

    struct ArkRiskExportSnapshot
    {
        std::shared_ptr<const std::vector<MonitorDock::ArkRiskCenterEntry>> entries;
        bool asCsv = false;
    };

    template<class Sink>
    bool formatArkRiskExport(const ArkRiskExportSnapshot& snapshot, Sink& sink)
    {
        if (!snapshot.entries) return false;
        if (!snapshot.asCsv)
        {
            QJsonArray rows;
            for (const auto& entry : *snapshot.entries) rows.append(entry.payload);
            return sink.writeUtf8(QJsonDocument(rows).toJson(QJsonDocument::Indented))
                && sink.rowsCompleted(static_cast<std::uint64_t>(snapshot.entries->size()));
        }
        if (!sink.writeUtf8(QByteArrayView("riskScore,source,category,title,detail,json\n"))) return false;
        for (const auto& entry : *snapshot.entries)
        {
            const QString jsonText = QString::fromUtf8(QJsonDocument(entry.payload).toJson(QJsonDocument::Compact));
            const QString line = QStringLiteral("%1,%2,%3,%4,%5,%6\n")
                .arg(csvEscape(entry.riskScoreText), csvEscape(entry.sourceName), csvEscape(entry.category),
                    csvEscape(entry.title), csvEscape(entry.detail), csvEscape(jsonText));
            if (!sink.writeUtf8(line.toUtf8()) || !sink.rowsCompleted()) return false;
        }
        return true;
    }

    bool matchesFilter(const MonitorDock::ArkRiskCenterEntry& entry, const QString& filter)
    {
        // Input: cache row and filter text. Processing: case-insensitive scan across display fields and JSON. Return: match flag.
        if (filter.isEmpty())
        {
            return true;
        }
        for (const QString& field : entry.searchFields)
        {
            if (field.contains(filter, Qt::CaseInsensitive))
            {
                return true;
            }
        }
        return false;
    }

    QString arkRiskColumnText(const MonitorDock::ArkRiskCenterEntry& entry, const int column)
    {
        switch (column)
        {
        case 0: return entry.riskScoreText;
        case 1: return entry.sourceName;
        case 2: return entry.category;
        case 3: return entry.title;
        case 4: return entry.detail;
        default: return {};
        }
    }

    struct ArkRiskProjection
    {
        // 模型只保留索引根，不持有本次准备所借用的旧快照。
        std::shared_ptr<const std::vector<std::size_t>> indexes;
        std::vector<int> rowsBySource;
        std::shared_ptr<const std::vector<MonitorDock::ArkRiskCenterEntry>> previousEntries;
        std::shared_ptr<const std::vector<std::size_t>> previousIndexes;
        bool sameMembers = false;
    };

    ArkRiskProjection buildArkRiskProjection(
        const std::shared_ptr<const std::vector<MonitorDock::ArkRiskCenterEntry>>& entriesRoot,
        const QString& filter, const bool highOnly, const int sortColumn, const Qt::SortOrder sortOrder,
        std::shared_ptr<const std::vector<MonitorDock::ArkRiskCenterEntry>> previousEntries,
        std::shared_ptr<const std::vector<std::size_t>> previousIndexes)
    {
        const auto& entries = *entriesRoot;
        std::vector<std::size_t> indexes;
        indexes.reserve(entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i)
            if ((!highOnly || entries[i].riskScore >= 50.0) && matchesFilter(entries[i], filter)) indexes.push_back(i);
        if (sortColumn >= 0 && sortColumn < 5)
            std::stable_sort(indexes.begin(), indexes.end(), [&entries, sortColumn, sortOrder](const auto left, const auto right) {
                const int comparison = sortColumn == 0
                    ? (entries[left].riskScore < entries[right].riskScore ? -1 : entries[left].riskScore > entries[right].riskScore ? 1 : 0)
                    : QString::localeAwareCompare(arkRiskColumnText(entries[left], sortColumn), arkRiskColumnText(entries[right], sortColumn));
                return sortOrder == Qt::AscendingOrder ? comparison < 0 : comparison > 0;
            });
        ArkRiskProjection projection;
        projection.previousEntries = std::move(previousEntries);
        projection.previousIndexes = std::move(previousIndexes);
        projection.rowsBySource.assign(entries.size(), -1);
        for (std::size_t row = 0; row < indexes.size(); ++row)
            projection.rowsBySource[indexes[row]] = static_cast<int>(row);
        projection.sameMembers = projection.previousEntries == entriesRoot && projection.previousIndexes
            && projection.previousIndexes->size() == indexes.size();
        if (projection.sameMembers)
            for (const auto source : *projection.previousIndexes)
                if (source >= projection.rowsBySource.size() || projection.rowsBySource[source] < 0)
                {
                    projection.sameMembers = false;
                    break;
                }
        // 已发布索引根与临时旧快照保持独立，避免反复排序保留所有历史准备结果。
        projection.indexes = ks::ui::RetainCompleteExportSnapshot<std::vector<std::size_t>>(
            std::make_shared<const std::vector<std::size_t>>(std::move(indexes)));
        return projection;
    }

    class ArkRiskSnapshotModel final : public QAbstractTableModel
    {
    public:
        ArkRiskSnapshotModel(QObject* table, std::function<void()> request)
            : QAbstractTableModel(table), m_request(std::move(request)) { table->installEventFilter(this); }
        int rowCount(const QModelIndex& parent = {}) const override
        { return parent.isValid() || !m_indexes ? 0 : static_cast<int>(m_indexes->size()); }
        int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 5; }
        const MonitorDock::ArkRiskCenterEntry* entryAt(const int row) const
        {
            if (!m_entries || row < 0 || row >= rowCount()) return nullptr;
            const auto source = (*m_indexes)[static_cast<std::size_t>(row)];
            return source < m_entries->size() ? &(*m_entries)[source] : nullptr;
        }
        std::shared_ptr<const MonitorDock::ArkRiskCenterEntry> snapshotEntryAt(const int row) const
        {
            const auto* entry = entryAt(row);
            return entry ? std::shared_ptr<const MonitorDock::ArkRiskCenterEntry>(m_entries, entry)
                : std::shared_ptr<const MonitorDock::ArkRiskCenterEntry>{};
        }
        std::shared_ptr<const std::vector<MonitorDock::ArkRiskCenterEntry>> entriesSnapshot() const
        { return m_entries; }
        std::shared_ptr<const std::vector<std::size_t>> indexesSnapshot() const
        { return m_indexes; }
        std::size_t sourceIndexAt(const int row) const
        {
            return m_indexes && row >= 0 && row < rowCount()
                ? (*m_indexes)[static_cast<std::size_t>(row)] : std::numeric_limits<std::size_t>::max();
        }
        QVariant data(const QModelIndex& index, const int role) const override
        {
            const auto* entry = index.isValid() ? entryAt(index.row()) : nullptr;
            if (!entry || index.column() < 0 || index.column() >= 5) return {};
            if (role == Qt::DisplayRole || role == Qt::ToolTipRole) return ks::i18n::sourceText(arkRiskColumnText(*entry, index.column()));
            if (role == Qt::TextAlignmentRole) return int(Qt::AlignVCenter | (index.column() == 0 ? Qt::AlignRight : Qt::AlignLeft));
            if (index.column() == 0)
            {
                if (role == Qt::UserRole || role == ks::ui::NumericSortRole) return entry->riskScore;
                if (role == kRiskEntryIndexRole) return QVariant::fromValue<qulonglong>((*m_indexes)[static_cast<std::size_t>(index.row())]);
                if (role == kRiskProcessIdRole) return QVariant::fromValue<qulonglong>(entry->processId);
                if (role == kRiskEvidenceIdRole) return entry->evidenceId;
            }
            return {};
        }
        QVariant headerData(int section, Qt::Orientation orientation, int role) const override
        {
            if (role != Qt::DisplayRole) return {};
            if (orientation == Qt::Vertical) return section + 1;
            static const QStringList headers{QStringLiteral("riskScore"), QStringLiteral("来源"), QStringLiteral("分类"), QStringLiteral("标题"), QStringLiteral("详情")};
            return section >= 0 && section < headers.size() ? QVariant(ks::i18n::sourceText(headers[section])) : QVariant();
        }
        void sort(int, Qt::SortOrder) override { m_request(); }
        bool replace(std::shared_ptr<const std::vector<MonitorDock::ArkRiskCenterEntry>> entries,
            const std::shared_ptr<const ArkRiskProjection>& projection)
        {
            // 结构通知必须成对完成，不能嵌套；同步观察者重入时，调用方复用既有合并准备路径。
            if (m_publishing || !projection || !projection->indexes) return false;
            m_publishing = true;
            const QPointer<ArkRiskSnapshotModel> alive(this);
            const bool permutation = projection->sameMembers
                && m_entries == projection->previousEntries && m_indexes == projection->previousIndexes;
            if (permutation) emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
            else beginResetModel();
            if (!alive) return false;
            // 选择模型会在上述信号中创建持久索引，必须等信号返回后再取列表。
            const auto before = permutation ? persistentIndexList() : QModelIndexList{};
            auto previousEntries = std::move(m_entries); auto previousIndexes = std::move(m_indexes);
            m_entries = std::move(entries); m_indexes = projection->indexes;
            if (permutation)
            {
                QModelIndexList after;
                after.reserve(before.size());
                for (const auto& index : before)
                {
                    int row = -1;
                    if (index.isValid() && index.row() >= 0
                        && static_cast<std::size_t>(index.row()) < previousIndexes->size())
                    {
                        const auto source = (*previousIndexes)[static_cast<std::size_t>(index.row())];
                        if (source < projection->rowsBySource.size()) row = projection->rowsBySource[source];
                    }
                    after.push_back(row >= 0 ? this->index(row, index.column()) : QModelIndex{});
                }
                changePersistentIndexList(before, after);
                emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
            }
            else endResetModel();
            if (!alive) return false;
            m_publishing = false;
            // Both publication roots already own retirement aliases. Releasing
            // these small handles cannot destroy the full archive on this thread.
            return true;
        }
    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event->type() == QEvent::Show) QTimer::singleShot(0, this, m_request);
            return QAbstractTableModel::eventFilter(watched, event);
        }
    private:
        std::function<void()> m_request;
        std::shared_ptr<const std::vector<MonitorDock::ArkRiskCenterEntry>> m_entries;
        std::shared_ptr<const std::vector<std::size_t>> m_indexes;
        bool m_publishing = false;
    };

    void addStatusEntry(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, QStringList& statusLines, const QString& source, const QString& category, const bool ok, const bool unsupported, const ksword::ark::IoResult& io)
    {
        // Input: one read-only query status. Processing: append status text and optional low-risk failure row. Return: no value.
        const QString summary = unsupported ? QStringLiteral("%1: 未集成/驱动过旧/能力缺失").arg(source) : QStringLiteral("%1: %2").arg(source, ioSummary(io));
        statusLines << summary;
        if (ok)
        {
            return;
        }
        const double risk = unsupported ? 0.0 : 5.0;
        QJsonObject payload = payloadBase(source, category, unsupported ? QStringLiteral("等待 R0 支持") : QStringLiteral("查询失败"), summary, risk);
        payload.insert(QStringLiteral("unsupported"), unsupported);
        payload.insert(QStringLiteral("win32Error"), static_cast<int>(io.win32Error));
        payload.insert(QStringLiteral("ntStatus"), hex32(static_cast<std::uint32_t>(io.ntStatus)));
        entries.push_back(makeEntry(source, category, unsupported ? QStringLiteral("未集成/驱动过旧") : QStringLiteral("查询失败"), summary, risk, payload));
    }

    double memoryScore(const ksword::ark::KernelMemoryEvidenceEntry& row)
    {
        // Input: memory evidence row. Processing: score RWX/non-module/pool/text risks and confidence. Return: risk score.
        double score = 0.0;
        if (row.riskFlags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_RWX) score += 35.0;
        if (row.riskFlags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_NONMODULE_EXECUTABLE) score += 35.0;
        if (row.riskFlags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_MODULE_NON_TEXT_EXECUTABLE) score += 25.0;
        if (row.riskFlags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_EXECUTABLE_POOL) score += 25.0;
        if (row.riskFlags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_LARGE_EXECUTABLE) score += 12.0;
        if (row.riskFlags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_OWNER_MISSING) score += 12.0;
        score += std::min<double>(15.0, static_cast<double>(row.confidence) / 10.0);
        return clampScore(score);
    }

    QString memoryRiskText(const std::uint32_t flags)
    {
        // Input: memory evidence risk bits. Processing: map known bits. Return: compact text.
        QStringList parts;
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_RWX) parts << QStringLiteral("RWX");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_NONMODULE_EXECUTABLE) parts << QStringLiteral("非模块执行");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_MODULE_NON_TEXT_EXECUTABLE) parts << QStringLiteral("模块非text执行");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_EXECUTABLE_POOL) parts << QStringLiteral("执行池");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_LARGE_EXECUTABLE) parts << QStringLiteral("大页执行");
        if (flags & KSWORD_ARK_MEMORY_EVIDENCE_RISK_OWNER_MISSING) parts << QStringLiteral("Owner缺失");
        return parts.isEmpty() ? QStringLiteral("正常") : parts.join(QStringLiteral(" | "));
    }

    double crossViewScore(const std::uint32_t flags, const std::uint32_t confidence)
    {
        // Input: cross-view anomaly bits and confidence. Processing: score DKOM-style mismatches. Return: risk score.
        double score = 0.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_ACTIVE_LIST) score += 40.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_CID_TABLE) score += 35.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_ORPHAN) score += 35.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_CID_ONLY) score += 30.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_START_ADDRESS_OUTSIDE_MODULE) score += 30.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_DANGLING_OBJECT) score += 30.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_NOT_IN_PROCESS_LIST) score += 28.0;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_ACTIVE_ONLY) score += 20.0;
        score += std::min<double>(15.0, static_cast<double>(confidence) / 10.0);
        return clampScore(score);
    }

    QString crossViewText(const std::uint32_t flags)
    {
        // Input: cross-view anomaly bits. Processing: map known DKOM indicators. Return: compact text.
        QStringList parts;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_CID_ONLY) parts << QStringLiteral("CID-only");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_ACTIVE_ONLY) parts << QStringLiteral("Active-only");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_ACTIVE_LIST) parts << QStringLiteral("缺ActiveList");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_CID_TABLE) parts << QStringLiteral("缺CID");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_ORPHAN) parts << QStringLiteral("孤儿线程");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_NOT_IN_PROCESS_LIST) parts << QStringLiteral("线程进程缺失");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_START_ADDRESS_OUTSIDE_MODULE) parts << QStringLiteral("入口出模块");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_DANGLING_OBJECT) parts << QStringLiteral("悬空对象");
        return parts.isEmpty() ? QStringLiteral("正常") : parts.join(QStringLiteral(" | "));
    }

    double driverScore(const std::uint32_t flags, const std::uint32_t confidence)
    {
        // Input: driver integrity risk bits and confidence. Processing: score CPU/IDT/owner/dispatch risks. Return: risk score.
        double score = 0.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_NON_CORE_OWNER) score += 40.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_OWNER_MISMATCH) score += 35.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_OUTSIDE_DRIVER_IMAGE) score += 35.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_WP_DISABLED) score += 35.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_DESCRIPTOR_INVALID) score += 25.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_NXE_DISABLED) score += 25.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_SMEP_DISABLED) score += 22.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_SECTION_MISMATCH) score += 18.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CROSS_DRIVER_ATTACH) score += 18.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_SMAP_DISABLED) score += 18.0;
        // 以下几位此前没有计分：分值与 driver_integrity.c::KswordARKDriverIntegrityScoreRisk 保持一致，
        // 两处各自维护一份评分（没有共用实现），漏掉任何一处新位都会让风险中心的汇总分与驱动侧证据
        // 行自带的 riskScore 对不上——尤其是 HIDDEN_HOOK：驱动侧单独一位就顶到 100，这里必须一样。
        if (flags & (KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_BASELINE_CHANGED |
            KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_TABLE_DIVERGED |
            KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_TABLE_RELOCATED)) score += 50.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_TARGET_NON_EXEC) score += 45.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_OBJTYPE_PROC_NON_CORE) score += 50.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_PROC_DETOUR) score += 60.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_HIDDEN_HOOK) score += 100.0;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_LAYOUT_UNVERIFIED) score += 5.0;
        score += std::min<double>(15.0, static_cast<double>(confidence) / 10.0);
        return clampScore(score);
    }

    QString driverRiskText(const std::uint32_t flags)
    {
        // Input: driver integrity risk bits. Processing: map high-value known bits. Return: compact text.
        QStringList parts;
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_OWNER_MISMATCH) parts << QStringLiteral("Owner不匹配");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_OUTSIDE_DRIVER_IMAGE) parts << QStringLiteral("外跳");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_NON_CORE_OWNER) parts << QStringLiteral("IDT外部Owner");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_WP_DISABLED) parts << QStringLiteral("WP关闭");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_NXE_DISABLED) parts << QStringLiteral("NXE关闭");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_SMEP_DISABLED) parts << QStringLiteral("SMEP关闭");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CPU_SMAP_DISABLED) parts << QStringLiteral("SMAP关闭");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_DESCRIPTOR_INVALID) parts << QStringLiteral("描述符异常");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_SECTION_MISMATCH) parts << QStringLiteral("Section不匹配");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_CROSS_DRIVER_ATTACH) parts << QStringLiteral("跨驱动挂接");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_MODULE_UNRESOLVED) parts << QStringLiteral("模块未解析");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_DYNDATA_UNAVAILABLE) parts << QStringLiteral("DynData缺失");
        if (flags & (KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_BASELINE_CHANGED |
            KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_TABLE_DIVERGED |
            KSWORD_ARK_DRIVER_INTEGRITY_RISK_IDT_TABLE_RELOCATED)) parts << QStringLiteral("IDT基线偏离");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_TARGET_NON_EXEC) parts << QStringLiteral("目标非可执行节");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_OBJTYPE_PROC_NON_CORE) parts << QStringLiteral("方法指针非核心归属");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_PROC_DETOUR) parts << QStringLiteral("入口跳板");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_HIDDEN_HOOK) parts << QStringLiteral("存在隐藏行为");
        if (flags & KSWORD_ARK_DRIVER_INTEGRITY_RISK_LAYOUT_UNVERIFIED) parts << QStringLiteral("布局未验证");
        return parts.isEmpty() ? hex32(flags) : parts.join(QStringLiteral(" | "));
    }

    QString hookStatusText(const std::uint32_t status)
    {
        // Input: kernel hook status. Processing: map known status codes. Return: display text.
        switch (status)
        {
        case KSWORD_ARK_KERNEL_HOOK_STATUS_CLEAN: return QStringLiteral("Clean");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS: return QStringLiteral("Suspicious");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_INTERNAL_BRANCH: return QStringLiteral("InternalBranch");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED: return QStringLiteral("ReadFailed");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_PARSE_FAILED: return QStringLiteral("ParseFailed");
        case KSWORD_ARK_KERNEL_HOOK_STATUS_FORCE_REQUIRED: return QStringLiteral("ForceRequired");
        default: return QStringLiteral("Status(%1)").arg(status);
        }
    }

    double hookScore(const std::uint32_t status, const bool hasPatchOrDiff)
    {
        // Input: hook status and patch/diff presence. Processing: score suspicious hook evidence. Return: risk score.
        double score = hasPatchOrDiff ? 25.0 : 0.0;
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_SUSPICIOUS) score += 60.0;
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_FORCE_REQUIRED) score += 55.0;
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_READ_FAILED) score += 15.0;
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_PARSE_FAILED) score += 15.0;
        if (status == KSWORD_ARK_KERNEL_HOOK_STATUS_INTERNAL_BRANCH) score += 8.0;
        return clampScore(score);
    }

    QString callbackClassText(const std::uint32_t callbackClass)
    {
        // Input: callback class id. Processing: map shared protocol constants. Return: display text.
        switch (callbackClass)
        {
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY: return QStringLiteral("Registry");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS: return QStringLiteral("Process");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_THREAD: return QStringLiteral("Thread");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE: return QStringLiteral("Image");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT: return QStringLiteral("Object");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER: return QStringLiteral("Minifilter");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT: return QStringLiteral("WFP");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER: return QStringLiteral("ETW");
        default: return QStringLiteral("Callback(%1)").arg(callbackClass);
        }
    }

    double callbackScore(const ksword::ark::CallbackEnumEntry& row)
    {
        // Input: callback enumeration row. Processing: score private/unresolved/untrusted rows. Return: risk score.
        double score = 0.0;
        if (row.status == KSWORD_ARK_CALLBACK_ENUM_STATUS_QUERY_FAILED) score += 20.0;
        if (row.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_UNSUPPORTED ||
            row.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_PATTERN_SCAN ||
            row.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_NOTIFY_ARRAY ||
            row.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_REGISTRY_LIST ||
            row.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_OBJECT_TYPE_LIST)
        {
            score += 25.0;
        }
        if (row.moduleBase == 0U && row.callbackAddress != 0U) score += 30.0;
        if ((row.fieldFlags & KSWORD_ARK_CALLBACK_ENUM_FIELD_MODULE) == 0U) score += 15.0;
        if ((row.trustFlags & (KSWORD_ARK_CALLBACK_TRUST_REVALIDATED | KSWORD_ARK_CALLBACK_TRUST_PUBLIC_API | KSWORD_ARK_CALLBACK_TRUST_PDB_PROFILE)) == 0U) score += 12.0;
        return clampScore(score);
    }

    QString mutationOperationText(const std::uint32_t operation)
    {
        // Input: mutation operation id. Processing: map known audit operation. Return: display text.
        switch (operation)
        {
        case KSWORD_ARK_MUTATION_OPERATION_PREPARE: return QStringLiteral("Prepare");
        case KSWORD_ARK_MUTATION_OPERATION_COMMIT: return QStringLiteral("Commit");
        case KSWORD_ARK_MUTATION_OPERATION_ROLLBACK: return QStringLiteral("Rollback");
        case KSWORD_ARK_MUTATION_OPERATION_QUERY_AUDIT: return QStringLiteral("QueryAudit");
        default: return QStringLiteral("Operation(%1)").arg(operation);
        }
    }

    QString mutationStatusText(const std::uint32_t status)
    {
        // Input: mutation status id. Processing: map common audit status. Return: display text.
        switch (status)
        {
        case KSWORD_ARK_MUTATION_STATUS_PREPARED: return QStringLiteral("Prepared");
        case KSWORD_ARK_MUTATION_STATUS_DRY_RUN: return QStringLiteral("DryRun");
        case KSWORD_ARK_MUTATION_STATUS_COMMITTED: return QStringLiteral("Committed");
        case KSWORD_ARK_MUTATION_STATUS_ROLLED_BACK: return QStringLiteral("RolledBack");
        case KSWORD_ARK_MUTATION_STATUS_REJECTED_SAFETY_POLICY: return QStringLiteral("RejectedSafetyPolicy");
        case KSWORD_ARK_MUTATION_STATUS_REJECTED_BEFORE_MISMATCH: return QStringLiteral("RejectedBeforeMismatch");
        case KSWORD_ARK_MUTATION_STATUS_REJECTED_TARGET_CHANGED: return QStringLiteral("RejectedTargetChanged");
        case KSWORD_ARK_MUTATION_STATUS_WRITE_FAILED: return QStringLiteral("WriteFailed");
        case KSWORD_ARK_MUTATION_STATUS_READ_FAILED: return QStringLiteral("ReadFailed");
        default: return QStringLiteral("Status(%1)").arg(status);
        }
    }

    double mutationScore(const ksword::ark::MutationAuditEntry& row)
    {
        // Input: mutation audit row. Processing: score commit/write-fail/rejected changes; dry-run lowers score. Return: risk score.
        double score = 0.0;
        if (row.operation == KSWORD_ARK_MUTATION_OPERATION_COMMIT) score += 45.0;
        if (row.status == KSWORD_ARK_MUTATION_STATUS_COMMITTED) score += 30.0;
        if (row.status == KSWORD_ARK_MUTATION_STATUS_WRITE_FAILED) score += 25.0;
        if (row.status == KSWORD_ARK_MUTATION_STATUS_REJECTED_TARGET_CHANGED) score += 18.0;
        if (row.status == KSWORD_ARK_MUTATION_STATUS_REJECTED_BEFORE_MISMATCH) score += 16.0;
        if (row.operation == KSWORD_ARK_MUTATION_OPERATION_ROLLBACK) score += 12.0;
        if (row.riskFlags != 0U) score += 20.0;
        if ((row.flags & KSWORD_ARK_MUTATION_FLAG_DRY_RUN) != 0U) score -= 20.0;
        return clampScore(score);
    }

    void appendMemory(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const ksword::ark::KernelMemoryEvidenceResult& result)
    {
        // Input: memory evidence result. Processing: append only non-zero risk rows. Return: no value.
        for (const auto& row : result.entries)
        {
            if (row.riskFlags == 0U) continue;
            const double risk = memoryScore(row);
            const QString title = QStringLiteral("%1 %2").arg(memoryRiskText(row.riskFlags), hex64(row.virtualAddress));
            const QString detail = QStringLiteral("owner=%1 size=%2 confidence=%3 %4")
                .arg(wideText(row.ownerName), hex64(row.regionSize)).arg(row.confidence).arg(wideText(row.detail));
            QJsonObject payload = payloadBase(QStringLiteral("Memory Evidence"), QStringLiteral("Memory"), title, detail, risk);
            payload.insert(QStringLiteral("virtualAddress"), hex64(row.virtualAddress));
            payload.insert(QStringLiteral("regionSize"), hex64(row.regionSize));
            payload.insert(QStringLiteral("riskFlags"), hex32(row.riskFlags));
            payload.insert(QStringLiteral("permissionFlags"), hex32(row.permissionFlags));
            payload.insert(QStringLiteral("ownerName"), wideText(row.ownerName));
            payload.insert(QStringLiteral("contentHash"), hex64(row.contentHash));
            entries.push_back(makeEntry(QStringLiteral("Memory Evidence"), QStringLiteral("Memory"), title, detail, risk, payload));
        }
    }

    void appendProcessCrossView(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const ksword::ark::ProcessCrossViewResult& result)
    {
        // Input: process cross-view result. Processing: append anomalous process rows. Return: no value.
        for (const auto& row : result.entries)
        {
            if (row.anomalyFlags == 0U) continue;
            const double risk = crossViewScore(row.anomalyFlags, row.confidence);
            const QString title = QStringLiteral("PID %1 %2").arg(row.processId).arg(crossViewText(row.anomalyFlags));
            const QString detail = QStringLiteral("image=%1 object=%2 source=%3 %4")
                .arg(narrowText(row.imageName), hex64(row.objectAddress), hex32(row.sourceMask), narrowText(row.detail));
            QJsonObject payload = payloadBase(QStringLiteral("Process Cross-View"), QStringLiteral("Process"), title, detail, risk);
            payload.insert(QStringLiteral("processId"), static_cast<int>(row.processId));
            payload.insert(QStringLiteral("imageName"), narrowText(row.imageName));
            payload.insert(QStringLiteral("objectAddress"), hex64(row.objectAddress));
            payload.insert(QStringLiteral("anomalyFlags"), hex32(row.anomalyFlags));
            payload.insert(QStringLiteral("sourceMask"), hex32(row.sourceMask));
            entries.push_back(makeEntry(QStringLiteral("Process Cross-View"), QStringLiteral("Process"), title, detail, risk, payload));
        }
    }

    void appendThreadCrossView(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const ksword::ark::ThreadCrossViewResult& result)
    {
        // Input: thread cross-view result. Processing: append anomalous thread rows. Return: no value.
        for (const auto& row : result.entries)
        {
            if (row.anomalyFlags == 0U) continue;
            const double risk = crossViewScore(row.anomalyFlags, row.confidence);
            const QString title = QStringLiteral("TID %1/PID %2 %3").arg(row.threadId).arg(row.processId).arg(crossViewText(row.anomalyFlags));
            const QString detail = QStringLiteral("thread=%1 process=%2 start=%3 %4")
                .arg(hex64(row.objectAddress), hex64(row.processObjectAddress), hex64(row.startAddress), narrowText(row.detail));
            QJsonObject payload = payloadBase(QStringLiteral("Thread Cross-View"), QStringLiteral("Thread"), title, detail, risk);
            payload.insert(QStringLiteral("threadId"), static_cast<int>(row.threadId));
            payload.insert(QStringLiteral("processId"), static_cast<int>(row.processId));
            payload.insert(QStringLiteral("objectAddress"), hex64(row.objectAddress));
            payload.insert(QStringLiteral("anomalyFlags"), hex32(row.anomalyFlags));
            payload.insert(QStringLiteral("sourceMask"), hex32(row.sourceMask));
            entries.push_back(makeEntry(QStringLiteral("Thread Cross-View"), QStringLiteral("Thread"), title, detail, risk, payload));
        }
    }

    void appendDriverIntegrity(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const QString& source, const ksword::ark::DriverIntegrityResult& result)
    {
        // Input: driver or CPU integrity result. Processing: append rows with risk flags. Return: no value.
        for (const auto& row : result.entries)
        {
            if (row.riskFlags == 0U) continue;
            const double risk = driverScore(row.riskFlags, row.confidence);
            const QString title = QStringLiteral("%1 %2").arg(source, driverRiskText(row.riskFlags));
            const QString detail = QStringLiteral("owner=%1 object=%2 target=%3 cpu=G%4/%5/V%6 %7")
                .arg(wideText(row.ownerModule), hex64(row.objectAddress), hex64(row.targetAddress))
                .arg(row.processorGroup).arg(row.processorNumber).arg(row.vector).arg(wideText(row.detail));
            QJsonObject payload = payloadBase(source, QStringLiteral("Driver"), title, detail, risk);
            payload.insert(QStringLiteral("riskFlags"), hex32(row.riskFlags));
            payload.insert(QStringLiteral("objectAddress"), hex64(row.objectAddress));
            payload.insert(QStringLiteral("targetAddress"), hex64(row.targetAddress));
            payload.insert(QStringLiteral("ownerModule"), wideText(row.ownerModule));
            payload.insert(QStringLiteral("processorGroup"), static_cast<int>(row.processorGroup));
            payload.insert(QStringLiteral("processorNumber"), static_cast<int>(row.processorNumber));
            payload.insert(QStringLiteral("vector"), static_cast<int>(row.vector));
            entries.push_back(makeEntry(source, QStringLiteral("Driver"), title, detail, risk, payload));
        }
    }

    void appendInlineHooks(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const ksword::ark::KernelInlineHookScanResult& result)
    {
        // Input: inline hook scan result. Processing: append suspicious or patched rows. Return: no value.
        for (const auto& row : result.entries)
        {
            const bool hasPatch = row.hookType != KSWORD_ARK_INLINE_HOOK_TYPE_NONE;
            const double risk = hookScore(row.status, hasPatch);
            if (risk <= 0.0) continue;
            const QString functionName = narrowText(row.functionName).trimmed();
            const QString title = QStringLiteral("%1 %2").arg(hookStatusText(row.status), functionName.isEmpty() ? hex64(row.functionAddress) : functionName);
            const QString detail = QStringLiteral("module=%1 targetModule=%2 function=%3 target=%4")
                .arg(wideText(row.moduleName), wideText(row.targetModuleName), hex64(row.functionAddress), hex64(row.targetAddress));
            QJsonObject payload = payloadBase(QStringLiteral("Inline Hook"), QStringLiteral("Hook"), title, detail, risk);
            payload.insert(QStringLiteral("status"), hookStatusText(row.status));
            payload.insert(QStringLiteral("hookType"), static_cast<int>(row.hookType));
            payload.insert(QStringLiteral("functionAddress"), hex64(row.functionAddress));
            payload.insert(QStringLiteral("targetAddress"), hex64(row.targetAddress));
            payload.insert(QStringLiteral("moduleName"), wideText(row.moduleName));
            entries.push_back(makeEntry(QStringLiteral("Inline Hook"), QStringLiteral("Hook"), title, detail, risk, payload));
        }
    }

    void appendIatEatHooks(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const ksword::ark::KernelIatEatHookScanResult& result)
    {
        // Input: IAT/EAT hook scan result. Processing: append suspicious or pointer-diff rows. Return: no value.
        for (const auto& row : result.entries)
        {
            const bool differs = row.currentTarget != 0U && row.expectedTarget != 0U && row.currentTarget != row.expectedTarget;
            const double risk = hookScore(row.status, differs);
            if (risk <= 0.0) continue;
            const QString hookClass = row.hookClass == KSWORD_ARK_IAT_EAT_HOOK_CLASS_EAT ? QStringLiteral("EAT") : QStringLiteral("IAT");
            const QString title = QStringLiteral("%1 %2 %3").arg(hookClass, hookStatusText(row.status), narrowText(row.functionName));
            const QString detail = QStringLiteral("module=%1 import=%2 current=%3 expected=%4")
                .arg(wideText(row.moduleName), wideText(row.importModuleName), hex64(row.currentTarget), hex64(row.expectedTarget));
            QJsonObject payload = payloadBase(QStringLiteral("IAT/EAT Hook"), QStringLiteral("Hook"), title, detail, risk);
            payload.insert(QStringLiteral("hookClass"), hookClass);
            payload.insert(QStringLiteral("status"), hookStatusText(row.status));
            payload.insert(QStringLiteral("thunkAddress"), hex64(row.thunkAddress));
            payload.insert(QStringLiteral("currentTarget"), hex64(row.currentTarget));
            payload.insert(QStringLiteral("expectedTarget"), hex64(row.expectedTarget));
            payload.insert(QStringLiteral("moduleName"), wideText(row.moduleName));
            entries.push_back(makeEntry(QStringLiteral("IAT/EAT Hook"), QStringLiteral("Hook"), title, detail, risk, payload));
        }
    }

    void appendCallbacks(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const ksword::ark::CallbackEnumResult& result)
    {
        // Input: callback enumeration result. Processing: append private/unresolved/untrusted callback rows. Return: no value.
        for (const auto& row : result.entries)
        {
            const double risk = callbackScore(row);
            if (risk < 20.0) continue;
            const QString callbackClass = callbackClassText(row.callbackClass);
            const QString title = QStringLiteral("%1 %2").arg(callbackClass, hex64(row.callbackAddress));
            const QString detail = QStringLiteral("module=%1 name=%2 altitude=%3 source=%4 trust=%5")
                .arg(wideText(row.modulePath), wideText(row.name), wideText(row.altitude)).arg(row.source).arg(hex32(row.trustFlags));
            QJsonObject payload = payloadBase(QStringLiteral("Callback"), QStringLiteral("Callback"), title, detail, risk);
            payload.insert(QStringLiteral("callbackClass"), callbackClass);
            payload.insert(QStringLiteral("source"), static_cast<int>(row.source));
            payload.insert(QStringLiteral("fieldFlags"), hex32(row.fieldFlags));
            payload.insert(QStringLiteral("trustFlags"), hex32(row.trustFlags));
            payload.insert(QStringLiteral("callbackAddress"), hex64(row.callbackAddress));
            payload.insert(QStringLiteral("modulePath"), wideText(row.modulePath));
            payload.insert(QStringLiteral("name"), wideText(row.name));
            payload.insert(QStringLiteral("altitude"), wideText(row.altitude));
            entries.push_back(makeEntry(QStringLiteral("Callback"), QStringLiteral("Callback"), title, detail, risk, payload));
        }
    }

    void appendMutationAudit(std::vector<MonitorDock::ArkRiskCenterEntry>& entries, const ksword::ark::MutationAuditResult& result)
    {
        // Input: read-only mutation audit result. Processing: append audit rows without exposing write controls. Return: no value.
        for (const auto& row : result.entries)
        {
            const double risk = mutationScore(row);
            if (risk <= 0.0) continue;
            const QString operation = mutationOperationText(row.operation);
            const QString status = mutationStatusText(row.status);
            const QString title = QStringLiteral("TX %1 %2 %3").arg(static_cast<qulonglong>(row.transactionId)).arg(operation, status);
            const QString detail = QStringLiteral("target=%1 bytes=%2 pid=%3 flags=%4 risk=%5")
                .arg(hex64(row.targetAddress)).arg(row.bytes).arg(row.processId).arg(hex32(row.flags), hex32(row.riskFlags));
            QJsonObject payload = payloadBase(QStringLiteral("Mutation Audit"), QStringLiteral("Mutation"), title, detail, risk);
            payload.insert(QStringLiteral("operation"), operation);
            payload.insert(QStringLiteral("status"), status);
            payload.insert(QStringLiteral("transactionId"), QString::number(static_cast<qulonglong>(row.transactionId)));
            payload.insert(QStringLiteral("sequence"), QString::number(static_cast<qulonglong>(row.sequence)));
            payload.insert(QStringLiteral("targetAddress"), hex64(row.targetAddress));
            payload.insert(QStringLiteral("bytes"), static_cast<int>(row.bytes));
            payload.insert(QStringLiteral("processId"), static_cast<int>(row.processId));
            payload.insert(QStringLiteral("flags"), hex32(row.flags));
            payload.insert(QStringLiteral("riskFlags"), hex32(row.riskFlags));
            entries.push_back(makeEntry(QStringLiteral("Mutation Audit"), QStringLiteral("Mutation"), title, detail, risk, payload));
        }
    }
}

void MonitorDock::initializeArkRiskCenterTab()
{
    // 输入：无，由 initializeUi 调用。
    // 处理：创建只读 ARK 风险中心页；所有驱动访问都在 ArkDriverClient 中完成。
    // 返回：无返回值，控件由 Qt 父子树释放。
    m_arkRiskCenterPage = new QWidget(m_sideTabWidget);
    QVBoxLayout* pageLayout = new QVBoxLayout(m_arkRiskCenterPage);
    pageLayout->setContentsMargins(6, 6, 6, 6);
    pageLayout->setSpacing(6);

    // 只读风险查询与导出属于同一工具面，状态与证据颜色继续由风险逻辑管理。
    QWidget* toolbar = new QWidget(m_arkRiskCenterPage);
    ks::ui::StylePrimaryToolbar(toolbar);
    QHBoxLayout* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(8, 6, 8, 6);
    toolbarLayout->setSpacing(8);

    m_arkRiskRefreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QStringLiteral("刷新风险"), m_arkRiskCenterPage);
    m_arkRiskRefreshButton->setToolTip(QStringLiteral("只读聚合 Memory / Process / Driver / Callback / Hook / Mutation 发现"));
    m_arkRiskHighOnlyCheck = new QCheckBox(QStringLiteral("仅高风险"), m_arkRiskCenterPage);
    m_arkRiskHighOnlyCheck->setChecked(true);
    m_arkRiskHighOnlyCheck->setToolTip(QStringLiteral("仅显示 riskScore >= 50 的记录"));
    m_arkRiskFilterEdit = new QLineEdit(m_arkRiskCenterPage);
    ks::ui::StyleSearchField(m_arkRiskFilterEdit);
    m_arkRiskFilterEdit->setClearButtonEnabled(true);
    m_arkRiskFilterEdit->setPlaceholderText(QStringLiteral("过滤来源/分类/标题/详情/JSON"));
    m_arkRiskExportJsonButton = new QPushButton(QStringLiteral("导出 JSON"), m_arkRiskCenterPage);
    m_arkRiskExportCsvButton = new QPushButton(QStringLiteral("导出 CSV"), m_arkRiskCenterPage);
    for (QPushButton* button : {m_arkRiskRefreshButton, m_arkRiskExportJsonButton, m_arkRiskExportCsvButton})
    {
        ks::ui::ApplyFlatButtonTheme(button, ks::ui::FlatButtonTone::Neutral,
            ks::ui::FlatButtonAppearance::Solid);
    }
    m_arkRiskStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_arkRiskCenterPage);
    m_arkRiskStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_arkRiskStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    toolbarLayout->addWidget(m_arkRiskRefreshButton);
    toolbarLayout->addWidget(m_arkRiskHighOnlyCheck);
    toolbarLayout->addWidget(m_arkRiskFilterEdit, 1);
    toolbarLayout->addWidget(m_arkRiskExportJsonButton);
    toolbarLayout->addWidget(m_arkRiskExportCsvButton);
    toolbarLayout->addWidget(m_arkRiskStatusLabel);
    ks::ui::NormalizeToolbarRow(toolbarLayout);
    pageLayout->addWidget(toolbar);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_arkRiskCenterPage);
    pageLayout->addWidget(splitter, 1);

    m_arkRiskTable = new ks::ui::TableActionTableView(splitter);
    m_arkRiskDispatcher = std::make_shared<ks::ui::AsyncUiDispatcher>(this);
    m_arkRiskTable->setModel(new ArkRiskSnapshotModel(m_arkRiskTable, [this]() { rebuildArkRiskCenterTable(); }));
    // 风险聚合自带 CSV/JSON 导出，紧凑模式避免再占一行对比入口。
    ks::ui::SetTableActionBarMode(m_arkRiskTable, ks::ui::TableActionBarMode::Compact);
    m_arkRiskTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_arkRiskTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_arkRiskTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_arkRiskTable->setAlternatingRowColors(true);
    m_arkRiskTable->setSortingEnabled(true);
    m_arkRiskTable->verticalHeader()->setVisible(false);
    m_arkRiskTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_arkRiskTable->horizontalHeader()->setSectionResizeMode(riskColumnIndex(RiskColumn::Detail), QHeaderView::Stretch);
    installArkRiskTableCopyMenu(this, m_arkRiskTable);
    splitter->addWidget(m_arkRiskTable);

    m_arkRiskDetailEdit = new ks::ui::StructuredFieldView(splitter);

    m_arkRiskDetailEdit->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("ARK 风险中心为只读聚合页。\n不提供任意写、修复、提交 mutation 或驱动卸载按钮；Mutation 仅展示 dry-run/audit/rollback 状态。")));
    splitter->addWidget(m_arkRiskDetailEdit);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    ks::ui::DetailLayoutRegistry::registerStructuredHost(
        m_arkRiskTable, m_arkRiskDetailEdit, m_arkRiskCenterPage);

    connect(m_arkRiskRefreshButton, &QPushButton::clicked, this, [this]() { refreshArkRiskCenterAsync(); });
    connect(m_arkRiskFilterEdit, &QLineEdit::textChanged, this, [this]() { rebuildArkRiskCenterTable(); });
    connect(m_arkRiskHighOnlyCheck, &QCheckBox::toggled, this, [this]() { rebuildArkRiskCenterTable(); });
    connect(m_arkRiskTable->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex&, const QModelIndex&) { showArkRiskCenterDetailForCurrentRow(); });
    connect(m_arkRiskExportJsonButton, &QPushButton::clicked, this, [this]() { exportArkRiskCenterAsJson(); });
    connect(m_arkRiskExportCsvButton, &QPushButton::clicked, this, [this]() { exportArkRiskCenterAsCsv(); });

    ks::monitor::AddMonitorNavigationTab(m_sideTabWidget, m_arkRiskCenterPage, QStringLiteral("ark-risk"));
}

void MonitorDock::refreshArkRiskCenterAsync()
{
    // 输入：刷新按钮或首次进入页面触发。
    // 处理：后台通过 ArkDriverClient 只读查询多路证据，主线程更新缓存和表格。
    // 返回：无返回值。
    if (m_arkRiskRefreshInProgress)
    {
        return;
    }

    const QPointer<MonitorDock> guardThis(this);
    const auto dispatcher = m_arkRiskDispatcher;
    m_arkRiskRefreshInProgress = true;
    const std::uint64_t ticket = ++m_arkRiskRefreshTicket;
    if (m_arkRiskRefreshButton != nullptr)
    {
        m_arkRiskRefreshButton->setEnabled(false);
    }
    if (!guardThis || guardThis->m_arkRiskRefreshTicket != ticket) return;
    if (m_arkRiskStatusLabel != nullptr)
    {
        m_arkRiskStatusLabel->setText(QStringLiteral("状态：聚合查询中..."));
        if (!guardThis || guardThis->m_arkRiskRefreshTicket != ticket) return;
        m_arkRiskStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:700;").arg(KswordTheme::PrimaryBlueHex));
        if (!guardThis || guardThis->m_arkRiskRefreshTicket != ticket) return;
    }

    // sessionBootId 在 GUI 线程先取一次：进程内恒定，工作线程直接用值，
    // 免得静态初始化和采集在不同线程上竞争。
    const QString sessionBootId = arkRiskSessionBootId();
    QRunnable* task = QRunnable::create([guardThis, dispatcher, ticket, sessionBootId]() {
        std::vector<MonitorDock::ArkRiskCenterEntry> entries;
        QStringList statusLines;
        const ksword::ark::DriverClient client;

        const unsigned long memoryFlags =
            KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_LOADED_MODULE_EXECUTABLE |
            KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_BIGPOOL |
            KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_TEXT_SECTION_SAMPLES |
            KSWORD_ARK_MEMORY_EVIDENCE_FLAG_INCLUDE_SUSPECTED_BIGPOOL;
        const auto memory = client.queryKernelMemoryEvidence(memoryFlags);
        addStatusEntry(entries, statusLines, QStringLiteral("Memory Evidence"), QStringLiteral("Memory"), memory.io.ok, memory.unsupported, memory.io);
        if (memory.io.ok) appendMemory(entries, memory);

        const auto process = client.queryProcessCrossView();
        addStatusEntry(entries, statusLines, QStringLiteral("Process Cross-View"), QStringLiteral("Process"), process.io.ok, process.unsupported, process.io);
        if (process.io.ok) appendProcessCrossView(entries, process);

        const auto thread = client.queryThreadCrossView();
        addStatusEntry(entries, statusLines, QStringLiteral("Thread Cross-View"), QStringLiteral("Thread"), thread.io.ok, thread.unsupported, thread.io);
        if (thread.io.ok) appendThreadCrossView(entries, thread);

        const auto driver = client.queryDriverIntegrity();
        addStatusEntry(entries, statusLines, QStringLiteral("Driver Integrity"), QStringLiteral("Driver"), driver.io.ok, driver.unsupported, driver.io);
        if (driver.io.ok) appendDriverIntegrity(entries, QStringLiteral("Driver Integrity"), driver);

        const auto cpu = client.queryKernelCpuIntegrity();
        addStatusEntry(entries, statusLines, QStringLiteral("CPU Integrity"), QStringLiteral("Driver"), cpu.io.ok, cpu.unsupported, cpu.io);
        if (cpu.io.ok) appendDriverIntegrity(entries, QStringLiteral("CPU Integrity"), cpu);

        const auto inlineHooks = client.scanInlineHooks();
        addStatusEntry(entries, statusLines, QStringLiteral("Inline Hook"), QStringLiteral("Hook"), inlineHooks.io.ok, false, inlineHooks.io);
        if (inlineHooks.io.ok) appendInlineHooks(entries, inlineHooks);

        const auto iatEatHooks = client.enumerateIatEatHooks();
        addStatusEntry(entries, statusLines, QStringLiteral("IAT/EAT Hook"), QStringLiteral("Hook"), iatEatHooks.io.ok, false, iatEatHooks.io);
        if (iatEatHooks.io.ok) appendIatEatHooks(entries, iatEatHooks);

        const auto callbacks = client.enumerateCallbacks();
        addStatusEntry(entries, statusLines, QStringLiteral("Callback"), QStringLiteral("Callback"), callbacks.io.ok, false, callbacks.io);
        if (callbacks.io.ok) appendCallbacks(entries, callbacks);

        const auto mutation = client.queryMutationAudit();
        addStatusEntry(entries, statusLines, QStringLiteral("Mutation Audit"), QStringLiteral("Mutation"), mutation.io.ok, mutation.unsupported, mutation.io);
        if (mutation.io.ok) appendMutationAudit(entries, mutation);

        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            return left.riskScore > right.riskScore;
        });

        // F-09/F-12：排序定稿后再发证据 id 并固定进程身份，让 id 与最终顺序一一对应。
        attachArkRiskNavigationIdentity(entries, sessionBootId, ticket);
        for (auto& entry : entries)
            entry.searchFields = {entry.sourceName, entry.category, entry.title, entry.detail, entry.riskScoreText,
                QString::fromUtf8(QJsonDocument(entry.payload).toJson(QJsonDocument::Compact))};

        // Create the retirement owner on this value worker before queuing a
        // callback. A closed page or rejected ticket can release only aliases.
        const auto deferredEntries = ks::ui::RetainCompleteExportSnapshot<std::vector<MonitorDock::ArkRiskCenterEntry>>(
            std::make_shared<const std::vector<MonitorDock::ArkRiskCenterEntry>>(std::move(entries)));
        const auto deferredStatusLines = ks::ui::RetainCompleteExportSnapshot<QStringList>(
            std::make_shared<const QStringList>(std::move(statusLines)));
        // 复用既有投递器与析构同步；后台线程不再检查后借用可能已销毁的 QObject。
        dispatcher->post(
            [guardThis, ticket, deferredEntries, deferredStatusLines]() mutable {
                if (guardThis == nullptr || guardThis->m_arkRiskRefreshTicket != ticket)
                {
                    return;
                }

                auto commit = [guardThis, ticket, deferredEntries, deferredStatusLines]() mutable
                {
                    if (guardThis == nullptr || guardThis->m_arkRiskRefreshTicket != ticket)
                    {
                        return;
                    }

                    guardThis->m_arkRiskRefreshInProgress = false;
                    if (guardThis->m_arkRiskRefreshButton != nullptr)
                    {
                        guardThis->m_arkRiskRefreshButton->setEnabled(true);
                    }
                    if (!guardThis || guardThis->m_arkRiskRefreshTicket != ticket) return;
                    auto previousEntries = std::move(guardThis->m_arkRiskCenterEntries);
                    guardThis->m_arkRiskCenterEntries = deferredEntries;
                    guardThis->m_arkRiskEntriesVersion = ticket;
                    // The old complete root is retired by its publication owner.
                    guardThis->rebuildArkRiskCenterTable();
                    if (!guardThis || guardThis->m_arkRiskRefreshTicket != ticket) return;
                    guardThis->showArkRiskCenterDetailForCurrentRow();
                    if (!guardThis || guardThis->m_arkRiskRefreshTicket != ticket) return;
                    if (guardThis->m_arkRiskStatusLabel != nullptr)
                    {
                        guardThis->m_arkRiskStatusLabel->setText(
                            QStringLiteral("状态：%1 项；%2")
                            .arg(static_cast<qulonglong>(guardThis->m_arkRiskCenterEntries->size()))
                            .arg(deferredStatusLines->join(QStringLiteral("；"))));
                        if (!guardThis || guardThis->m_arkRiskRefreshTicket != ticket) return;
                        guardThis->m_arkRiskStatusLabel->setStyleSheet(
                            QStringLiteral("color:%1; font-weight:700;")
                                .arg(KswordTheme::TextPrimaryHex()));
                    }
                };
                if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
                        guardThis.data(),
                        QStringLiteral("monitor-ark-risk-center-snapshot-apply"),
                        {guardThis->m_arkRiskTable},
                        commit))
                {
                    return;
                }
                commit();
            });
    });
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void MonitorDock::rebuildArkRiskCenterTable()
{
    if (!m_arkRiskTable || !m_arkRiskDispatcher) return;
    const auto ticket = ++m_arkRiskProjectionTicket;
    if (!m_arkRiskTable->isVisible()) return;
    if (!m_arkRiskProjectionDue)
    {
        if (!m_arkRiskProjectionScheduled)
        {
            m_arkRiskProjectionScheduled = true;
            QTimer::singleShot(120, this, [this]() {
                m_arkRiskProjectionScheduled = false;
                m_arkRiskProjectionDue = true;
                rebuildArkRiskCenterTable();
            });
        }
        return;
    }
    m_arkRiskProjectionDue = false;
    if (m_arkRiskProjectionRunning) return;
    m_arkRiskProjectionRunning = true;
    const auto entries = m_arkRiskCenterEntries;
    const QString filter = m_arkRiskFilterEdit ? m_arkRiskFilterEdit->text().trimmed() : QString();
    const bool highOnly = m_arkRiskHighOnlyCheck && m_arkRiskHighOnlyCheck->isChecked();
    const int sortColumn = m_arkRiskTable->horizontalHeader()->sortIndicatorSection();
    const auto sortOrder = m_arkRiskTable->horizontalHeader()->sortIndicatorOrder();
    const auto dispatcher = m_arkRiskDispatcher;
    const auto model = static_cast<ArkRiskSnapshotModel*>(m_arkRiskTable->model());
    const auto previousEntries = model->entriesSnapshot();
    const auto previousIndexes = model->indexesSnapshot();
    QThreadPool::globalInstance()->start(QRunnable::create([this, entries, previousEntries, previousIndexes,
        filter, highOnly, sortColumn, sortOrder, ticket, dispatcher]() {
        const auto projection = ks::ui::RetainCompleteExportSnapshot<ArkRiskProjection>(
            std::make_shared<const ArkRiskProjection>(
                buildArkRiskProjection(entries, filter, highOnly, sortColumn, sortOrder, previousEntries, previousIndexes)));
        dispatcher->post([this, entries, projection, ticket]() {
            m_arkRiskProjectionRunning = false;
            if (ticket != m_arkRiskProjectionTicket) { rebuildArkRiskCenterTable(); return; }
            if (!m_arkRiskTable->isVisible()) return;
            auto commit = [this, entries, projection, ticket]() {
                if (ticket != m_arkRiskProjectionTicket || !m_arkRiskTable->isVisible()) return;
                const QPointer<MonitorDock> alive(this);
                ks::ui::DetailLayoutRegistry::prepareDataRebuild(m_arkRiskDetailEdit);
                if (!alive || ticket != m_arkRiskProjectionTicket) return;
                auto* model = static_cast<ArkRiskSnapshotModel*>(m_arkRiskTable->model());
                const QPointer<ArkRiskSnapshotModel> modelOwner(model);
                // 在后台准备和菜单等待结束后的提交时读取当前选择。
                const auto current = m_arkRiskTable->currentIndex();
                const auto selectedEntries = model->entriesSnapshot();
                const auto selectedSource = model->sourceIndexAt(current.row());
                const int selectedColumn = current.isValid() ? current.column() : riskColumnIndex(RiskColumn::Score);
                if (!model->replace(entries, projection))
                {
                    if (alive && modelOwner && ticket == m_arkRiskProjectionTicket) rebuildArkRiskCenterTable();
                    return;
                }
                if (!alive || !modelOwner || ticket != m_arkRiskProjectionTicket) return;
                if (!m_arkRiskTable->currentIndex().isValid())
                {
                    // 成员变化使用重置；以 O(1) 恢复仍可见的选中源项，保留重置观察者的新选择。
                    int row = selectedEntries == entries && selectedSource < projection->rowsBySource.size()
                        ? projection->rowsBySource[selectedSource] : -1;
                    if (row < 0 && model->rowCount() > 0) row = 0;
                    if (row >= 0) m_arkRiskTable->setCurrentIndex(model->index(row, selectedColumn));
                }
                if (!alive || !modelOwner || ticket != m_arkRiskProjectionTicket) return;
                showArkRiskCenterDetailForCurrentRow();
            };
            if (!ks::ui::DeferTableUiCommitIfContextMenuOpen(this,
                QStringLiteral("monitor-ark-risk-filter-apply"), {m_arkRiskTable}, commit)) commit();
        });
    }));
}

void MonitorDock::showArkRiskCenterDetailForCurrentRow() const
{
    // 输入：无，读取当前表格选择。
    // 处理：通过缓存索引展开摘要和 JSON payload。
    // 返回：无返回值。
    if (m_arkRiskDetailEdit == nullptr || m_arkRiskTable == nullptr)
    {
        return;
    }
    const auto* source = static_cast<ArkRiskSnapshotModel*>(m_arkRiskTable->model())->entryAt(m_arkRiskTable->currentIndex().row());
    if (source == nullptr)
    {
        m_arkRiskDetailEdit->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("请选择一条风险记录。")));
        return;
    }
    const auto& entry = *source;
    ks::ui::FieldDocument detail;
    detail.section(QStringLiteral("ARK 风险详情"));
    detail.field(QStringLiteral("riskScore"), entry.riskScoreText);
    detail.field(QStringLiteral("source"), entry.sourceName);
    detail.field(QStringLiteral("category"), entry.category);
    detail.field(QStringLiteral("title"), entry.title);
    detail.field(QStringLiteral("detail"), entry.detail);
    detail.nodes.append(nativeJsonNode(QStringLiteral("Evidence"), entry.payload));
    m_arkRiskDetailEdit->setDocument(detail);
}

void MonitorDock::navigateToProcessDetailFromArkRiskRow(const int tableRow)
{
    // 输入：风险中心表格的显示行号。
    // 处理：F-12 + F-09 的双重判据，本轮第一个把这两条判据接进生产的调用点。
    //       原实现是 ks::ui::OpenProcessDetailByPid(processId) —— 裸 PID 跳转，
    //       风险行是异步采集的，从采集到点击之间目标随时可能退出并让出 PID，
    //       裸 PID 会把操作交给恰好占用该号码的新进程。
    // 返回：无返回值。判据不过时只弹说明，绝不退回裸 PID 跳转。
    if (m_arkRiskTable == nullptr || tableRow < 0 || tableRow >= m_arkRiskTable->model()->rowCount())
    {
        return;
    }

    // 采集完成后的规范根可能已经前进，异步投影仍在显示上一版完整快照。
    // 从可见模型的源索引 O(1) 取得该行，并强持它所属的不可变根直至导航结束。
    // 证据 id、PID 和创建身份始终来自同一份已发布证据，不能反查较新的规范根。
    const auto entry = static_cast<ArkRiskSnapshotModel*>(m_arkRiskTable->model())->snapshotEntryAt(tableRow);
    const QString evidenceId = entry ? entry->evidenceId : QString();

    // outcome / liveDecision：两条判据各自的原始结论，一路带进提示文本，便于对照日志排障。
    evidence::NavigationOutcome outcome = evidence::NavigationOutcome::EvidenceIdMissing;
    evidence::LiveNavigationDecision liveDecision = evidence::LiveNavigationDecision::Allow;
    bool liveDecisionEvaluated = false;
    // identityDiagnosticParts：采集期与现场复核各自的原始诊断都留着。
    // 两者说的是不同时刻的不同失败，塌成一条就分不清是“当初没取到”还是“现在读不到”。
    QStringList identityDiagnosticParts;
    quint32 rejectedProcessId = 0U;
    bool noProcessBinding = false;

    if (evidenceId.isEmpty())
    {
        // 请求根本没带证据 id：DecideNavigation 对这种请求也是判 EvidenceIdMissing，
        // 这里没有可构造的请求对象，所以直接用同一个枚举值表达，不另造语义。
        outcome = evidence::NavigationOutcome::EvidenceIdMissing;
    }
    else if (entry == nullptr)
    {
        // 带了 id 但当前快照里查不到：这条证据不在会话中，不能现场“猜”一条补上。
        outcome = evidence::NavigationOutcome::EvidenceNotSaved;
    }
    else if (entry->processId == 0U)
    {
        // 这行本来就不指向进程（驱动 / CPU / Hook 类）。身份判定同样是 IdentityUnusable
        // ——ProcessInstanceId 没有 pid 就是 Unusable——但拒绝理由要说清是"没有对象"。
        outcome = evidence::NavigationOutcome::IdentityUnusable;
        noProcessBinding = true;
    }
    else
    {
        rejectedProcessId = entry->processId;
        if (!entry->processIdentityOutcome.message.empty())
        {
            identityDiagnosticParts << QStringLiteral("capture=%1(%2)")
                .arg(QString::fromStdString(entry->processIdentityOutcome.message),
                    QString::fromLatin1(
                        evidence::CollectionStatusName(entry->processIdentityOutcome.status)));
        }

        // 采集期身份（离线侧）。
        const evidence::ProcessInstanceId savedIdentity = makeSavedProcessIdentity(*entry);

        // 现场复核（F-09）：跳转瞬间重新读一次，绝不复用采集期结果——
        // 复用等于没有复核。
        const LiveProcessProbe liveProbe = probeProcessCreateTime(entry->processId);
        evidence::LiveResolution liveResolution;
        liveResolution.found = liveProbe.present;
        if (liveProbe.present)
        {
            // 现场这一侧填**当前**运行会话，而不是抄采集行里的那个。
            // 抄过来等于无条件宣称“两边同属一个启动周期”；一旦以后支持从文件载入
            // 旧快照，那个假设就会悄悄放行跨启动的同 PID。分开填之后，两个 bootId
            // 不同就会被 MatchProcessInstance 判 NoMatch，正是我们要的拒绝（F-03）。
            liveResolution.liveProcess.bootId = arkRiskSessionBootId().toStdString();
            liveResolution.liveProcess.pid =
                evidence::OptionalU64::of(static_cast<std::uint64_t>(entry->processId));
            if (liveProbe.createTimeKnown)
            {
                liveResolution.liveProcess.createTime100ns =
                    evidence::OptionalU64::of(liveProbe.createTime100ns);
            }
            liveResolution.liveProcess.imageName = savedIdentity.imageName;
        }
        if (!liveProbe.outcome.message.empty())
        {
            identityDiagnosticParts << QStringLiteral("live=%1(%2)")
                .arg(QString::fromStdString(liveProbe.outcome.message),
                    QString::fromLatin1(evidence::CollectionStatusName(liveProbe.outcome.status)));
        }

        evidence::NavigationRequest request;
        request.page = evidence::NavigationPage::Process;
        // MakeProcessRef 的 key 来自 crossSessionKey()：身份不到 Strong 就是空串，
        // ObjectRef::navigable() 随即为假，DecideNavigation 判 IdentityUnusable。
        // 这正是“创建时间取不到就不许跳”的落点。
        request.object = evidence::MakeProcessRef(savedIdentity, evidenceId.toStdString());
        request.evidenceId = evidenceId.toStdString();
        request.anchor = evidence::FormatU64(entry->processId, evidence::U64Format::Decimal);
        request.requireExactMatch = true;

        outcome = evidence::DecideNavigation(
            request,
            processDetailRouteAvailable(),
            // 对象是否还在：以现场探测为准。权限不足读不到创建时间时对象仍然在，
            // 那种情况要落到下面的身份复核去解释，而不是谎称“对象不在”。
            liveProbe.present,
            // 当前可见模型的不可变根仍持有这段证据，整次导航都保留其所有权。
            true);

        if (outcome == evidence::NavigationOutcome::Delivered)
        {
            liveDecision = evidence::ResolveProcessNavigation(savedIdentity, liveResolution);
            liveDecisionEvaluated = true;
            if (liveDecision == evidence::LiveNavigationDecision::Allow)
            {
                // Allow 蕴含 MatchProcessInstance == Confirmed，而 Confirmed 要求两侧
                // 创建时间都在场且相等，所以这里的 processCreateTime100ns 必然是真值。
                // 即便未来有人破坏了这个前提，OpenProcessDetailByIdentity 对
                // creationTime==0 也会直接拒绝，不会退化成裸 PID 跳转。
                ks::ui::OpenProcessDetailByIdentity(entry->processId, entry->processCreateTime100ns);
                return;
            }
        }
    }

    QString messageText =
        arkRiskNavigationRejectionText(outcome, liveDecision, liveDecisionEvaluated, noProcessBinding);
    messageText += QStringLiteral("\n\n");
    messageText += ks::i18n::contextText(
        QStringLiteral("monitor.ark_risk.nav.context"),
        QStringLiteral("证据 id：%1\nPID：%2\n判据：%3 / %4"))
        .arg(evidenceId.isEmpty() ? QStringLiteral("-") : evidenceId)
        .arg(rejectedProcessId)
        .arg(QString::fromLatin1(evidence::NavigationOutcomeName(outcome)),
            liveDecisionEvaluated
                ? QString::fromLatin1(evidence::LiveNavigationDecisionName(liveDecision))
                : QStringLiteral("-"));
    if (!identityDiagnosticParts.isEmpty())
    {
        messageText += QStringLiteral("\n");
        messageText += ks::i18n::contextText(
            QStringLiteral("monitor.ark_risk.nav.diagnostic"),
            QStringLiteral("身份查询诊断：%1"))
            .arg(identityDiagnosticParts.join(QStringLiteral(" | ")));
    }

    QMessageBox::information(
        this,
        ks::i18n::contextText(
            QStringLiteral("monitor.ark_risk.nav.title"),
            QStringLiteral("无法转到进程详情")),
        messageText);
}

void MonitorDock::updateArkRiskExportActions()
{
    const bool busy = m_arkRiskExportOperation && m_arkRiskExportOperation->isBusy();
    for (auto* button : { m_arkRiskExportJsonButton, m_arkRiskExportCsvButton })
        if (button) {
            button->setEnabled(!busy);
            button->setToolTip(busy ? ks::i18n::LanguageManager::instance().text(
                QStringLiteral("monitor.export.in_progress"), QStringLiteral("导出进行中")) : QString());
        }
}

void MonitorDock::startArkRiskCenterExport(const bool asCsv)
{
    const QPointer<MonitorDock> guard(this);
    if (m_arkRiskExportOperation) m_arkRiskExportOperation->reconcile();
    if (!guard) return;
    if (m_arkRiskExportOperation && m_arkRiskExportOperation->isBusy()) return;
    if (m_arkRiskCenterEntries->empty()) {
        QMessageBox::information(this, QStringLiteral("ARK 风险中心"), QStringLiteral("当前没有可导出的风险记录。"));
        return;
    }
    const QString suffix = asCsv ? QStringLiteral("csv") : QStringLiteral("json");
    const QString defaultPath = QDir::home().filePath(QStringLiteral("ark-risk-center-%1.%2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss")), suffix));
    const QString filePath = QFileDialog::getSaveFileName(this,
        asCsv ? QStringLiteral("导出 ARK 风险 CSV") : QStringLiteral("导出 ARK 风险 JSON"), defaultPath,
        asCsv ? QStringLiteral("CSV (*.csv)") : QStringLiteral("JSON (*.json)"));
    if (!guard || filePath.isEmpty() || (m_arkRiskExportOperation && m_arkRiskExportOperation->isBusy())) return;
    if (!m_arkRiskExportOperation)
    {
        m_arkRiskExportOperation = new ks::ui::CompleteExportOperation(this);
        m_arkRiskExportOperation->setStateChangedCallback([guard]() {
            if (guard) guard->updateArkRiskExportActions();
        });
    }
    // O(1) publication root retains the complete canonical cache, including rows
    // outside the current search/high-risk projection. No GUI model traversal.
    const auto frozen = std::make_shared<const ArkRiskExportSnapshot>(ArkRiskExportSnapshot{m_arkRiskCenterEntries, asCsv});
    auto input = ks::ui::MakeCompleteExportInput(frozen, QStringLiteral("monitor/ark-risk"),
        m_arkRiskEntriesVersion, std::optional<std::uint64_t>(frozen->entries->size()),
        ks::i18n::LanguageManager::instance().sourceTextSnapshot(),
        [](const ArkRiskExportSnapshot& snapshot, ks::ui::CompleteExportSink& sink,
            const ks::i18n::SourceTextSnapshot&) { return formatArkRiskExport(snapshot, sink); });
    input.fileMode = asCsv ? ks::ui::CompleteExportFileMode::NativeText : ks::ui::CompleteExportFileMode::Binary;
    const int progress = kPro.add(this, "监控", "导出");
    const auto accepted = m_arkRiskExportOperation->startFile(filePath, std::move(input),
        [guard, progress](const ks::ui::CompleteExportReceipt& receipt) {
            const bool unavailable = receipt.phase == ks::ui::CompleteExportPhase::OutcomeUnavailable;
            kPro.finish(progress, unavailable ? kProgressState::LegacyCompleted
                    : receipt.succeeded() ? kProgressState::Success : kProgressState::Failure,
                unavailable ? "导出结果待确认" : receipt.succeeded() ? "导出完成" : "导出失败");
            if (guard && unavailable) {
                auto& language = ks::i18n::LanguageManager::instance();
                QMessageBox::warning(guard, language.text(QStringLiteral("monitor.export.result_unconfirmed_title"),
                    QStringLiteral("导出结果待确认")),
                    language.text(QStringLiteral("monitor.export.result_unconfirmed"),
                        QStringLiteral("导出结果尚未确认，请核对目标文件：%1")).arg(receipt.outputPath));
                return;
            }
            if (guard && !receipt.succeeded())
                QMessageBox::warning(guard, QStringLiteral("导出失败"),
                    receipt.nativeError.isEmpty() ? QStringLiteral("导出失败") : receipt.nativeError);
        });
    if (!accepted) kPro.finish(progress, kProgressState::Canceled, "导出已取消");
    updateArkRiskExportActions();
}

void MonitorDock::exportArkRiskCenterAsJson() const
{
    const_cast<MonitorDock*>(this)->startArkRiskCenterExport(false);
}

void MonitorDock::exportArkRiskCenterAsCsv() const
{
    const_cast<MonitorDock*>(this)->startArkRiskCenterExport(true);
}
