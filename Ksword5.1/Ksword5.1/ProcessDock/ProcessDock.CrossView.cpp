#include "ProcessDock.h"
#include "../UI/TableContextMenu.h"
#include "../Internationalization/LanguageManager.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/PageControlStyle.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/TablePresentation.h"
#include "../UI/FlatButtonTheme.h"
#include "../UI/ThemeBinding.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/StructuredFieldView.h"
#include "../UI/DetailLayoutRegistry.h"
#include "../UI/DetailLayoutHost.h"
#include "../UI/TableColumnAutoFit.h"
#include "../UI/TableInteractionSupport.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QEvent>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QModelIndex>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QSize>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <sstream>

namespace
{
    constexpr const char* CrossViewExposureProperty = "ksword_cross_view_projection_exposed";
    bool crossViewProjectionExposed(const QTableWidget* table)
    {
        return table != nullptr && table->isVisible() &&
            table->property(CrossViewExposureProperty).toBool() && !table->window()->isMinimized();
    }

    // Show/Hide record actual exposure; isVisible alone remains true on native minimize.
    class CrossViewShowObserver final : public QObject
    {
    public:
        CrossViewShowObserver(QWidget* table, std::function<void()> schedule)
            : QObject(table), m_schedule(std::move(schedule))
        {
            table->setProperty(CrossViewExposureProperty,
                table->isVisible() && !table->window()->isMinimized());
            table->installEventFilter(this);
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event->type() == QEvent::Show)
            {
                watched->setProperty(CrossViewExposureProperty, true);
                m_schedule();
            }
            else if (event->type() == QEvent::Hide)
            {
                watched->setProperty(CrossViewExposureProperty, false);
            }
            return false;
        }

    private:
        std::function<void()> m_schedule;
    };

    struct CrossViewSelectionIdentity
    {
        qulonglong id = 0;
        qulonglong object = 0;
        bool valid = false;
    };

    CrossViewSelectionIdentity crossViewSelectionIdentity(QTableWidget* table)
    {
        const int row = table != nullptr ? table->currentRow() : -1;
        const QTableWidgetItem* id = row >= 0 ? table->item(row, 0) : nullptr;
        const QTableWidgetItem* object = row >= 0 ? table->item(row, 1) : nullptr;
        bool idOk = false;
        bool objectOk = false;
        const qulonglong idValue = id != nullptr ? id->data(Qt::UserRole).toULongLong(&idOk) : 0ULL;
        const qulonglong objectValue = object != nullptr ? object->data(Qt::UserRole).toULongLong(&objectOk) : 0ULL;
        return { idValue, objectValue, idOk && objectOk };
    }

    struct CrossViewProjectionUiState
    {
        bool enabled = true;
        bool updatesEnabled = true;
        bool sortingEnabled = true;
        bool autoFitEnabled = true;
        QList<QHeaderView::ResizeMode> resizeModes;

        static CrossViewProjectionUiState capture(QTableWidget* table)
        {
            CrossViewProjectionUiState state;
            state.enabled = !table->testAttribute(Qt::WA_ForceDisabled);
            state.updatesEnabled = table->updatesEnabled();
            state.sortingEnabled = table->isSortingEnabled();
            state.autoFitEnabled = !table->property("_ksword_table_column_auto_fit_disabled").toBool();
            for (int column = 0; column < table->columnCount(); ++column)
            {
                state.resizeModes.push_back(table->horizontalHeader()->sectionResizeMode(column));
            }
            return state;
        }
    };

    // 一份可替换投影的 GUI 工作；规范证据不截断，也不在 worker 访问 QWidget。
    class CrossViewProjectionJob final : public QObject
    {
    public:
        CrossViewProjectionJob(
            QTableWidget* table,
            const std::uint64_t revision,
            const std::size_t sourceCount,
            const CrossViewSelectionIdentity& selection,
            CrossViewProjectionUiState uiState,
            std::function<bool()> isCurrent,
            std::function<bool(std::size_t)> acceptRow,
            std::function<void(std::size_t, int)> appendRow,
            std::function<void()> appendDiagnostic,
            std::function<void()> completed)
            : QObject(table), m_table(table), m_revision(revision), m_sourceCount(sourceCount),
              m_selection(selection), m_uiState(std::move(uiState)), m_isCurrent(std::move(isCurrent)),
              m_acceptRow(std::move(acceptRow)), m_appendRow(std::move(appendRow)),
              m_appendDiagnostic(std::move(appendDiagnostic)), m_completed(std::move(completed))
        {
            m_indexes.reserve(sourceCount);
            table->setEnabled(false); // TableActionBar/冻结与对比视图均是该表的子控件。
            table->setUpdatesEnabled(false);
            table->setSortingEnabled(false);
            ks::ui::SetTableColumnAutoFitEnabled(table, false);
            table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
            // 同时挡住禁用之前已排队、会先被全局表格过滤器接收的复制/菜单事件。
            if (qApp != nullptr) qApp->installEventFilter(this);
        }

        ~CrossViewProjectionJob() override
        {
            if (qApp != nullptr) qApp->removeEventFilter(this);
            // 表在析构时不得再访问其内部 model；取消/替换由下一任务继承 UI 状态。
        }

        std::uint64_t revision() const { return m_revision; }
        CrossViewSelectionIdentity selection() const { return m_selection; }
        CrossViewProjectionUiState uiState() const { return m_uiState; }

        void schedule()
        {
            if (m_scheduled || m_finished) return;
            m_scheduled = true;
            QTimer::singleShot(0, this, [this]()
            {
                m_scheduled = false;
                advance();
            });
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event->type() != QEvent::KeyPress && event->type() != QEvent::ContextMenu)
            {
                return false;
            }
            const QWidget* widget = qobject_cast<QWidget*>(watched);
            return !m_finished && m_table != nullptr && widget != nullptr
                && (widget == m_table || m_table->isAncestorOf(widget));
        }

    private:
        enum class Stage { Clear, Filter, Fill, Selection, Finish };

        void advance()
        {
            if (m_table == nullptr || !m_isCurrent() || !crossViewProjectionExposed(m_table))
            {
                return; // 隐藏时暂停；Show 继续，换代则由新任务替换。
            }
            const QPointer<CrossViewProjectionJob> guardThis(this);
            if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
                this, QStringLiteral("process-cross-view-projection-slice"), { m_table.data() },
                [guardThis]() { if (guardThis != nullptr) guardThis->schedule(); }))
            {
                return; // 分片之间继续遵守菜单/左 Ctrl 多选屏障。
            }
            QTableWidget* table = m_table.data();
            const QSignalBlocker blocker(table);
            QElapsedTimer slice;
            slice.start();
            do
            {
                if (!m_isCurrent() || !crossViewProjectionExposed(table)) return;
                switch (m_stage)
                {
                case Stage::Clear:
                    if (table->rowCount() > 0)
                    {
                        // 尾部每批最多 64 行，避免一次清整表，也避免逐行通知数万次。
                        table->setRowCount(std::max(0, table->rowCount() - 64));
                    }
                    else
                    {
                        m_stage = Stage::Filter;
                    }
                    break;
                case Stage::Filter:
                    if (m_sourceIndex < m_sourceCount)
                    {
                        if (m_acceptRow(m_sourceIndex)) m_indexes.push_back(m_sourceIndex);
                        ++m_sourceIndex;
                    }
                    else
                    {
                        // 单次 Qt 指针槽分配仍是不可中断操作；最大行数受原协议约束。
                        table->setRowCount(static_cast<int>(m_indexes.size()));
                        m_stage = Stage::Fill;
                    }
                    break;
                case Stage::Fill:
                    if (m_outputIndex < m_indexes.size())
                    {
                        m_appendRow(m_indexes[m_outputIndex], static_cast<int>(m_outputIndex));
                        ++m_outputIndex;
                    }
                    else
                    {
                        if (m_indexes.empty()) m_appendDiagnostic();
                        m_stage = Stage::Selection;
                    }
                    break;
                case Stage::Selection:
                    if (m_selection.valid && m_selectionScan < table->rowCount())
                    {
                        const QTableWidgetItem* id = table->item(m_selectionScan, 0);
                        const QTableWidgetItem* object = table->item(m_selectionScan, 1);
                        if (id != nullptr && object != nullptr
                            && id->data(Qt::UserRole).toULongLong() == m_selection.id
                            && object->data(Qt::UserRole).toULongLong() == m_selection.object)
                        {
                            m_selectedRow = m_selectionScan;
                            m_stage = Stage::Finish;
                        }
                        ++m_selectionScan;
                    }
                    else
                    {
                        m_stage = Stage::Finish;
                    }
                    break;
                case Stage::Finish:
                    finish();
                    return;
                }
            } while (slice.elapsed() < 4);
            schedule();
        }

        void finish()
        {
            QTableWidget* table = m_table.data();
            if (table == nullptr || !m_isCurrent()) return;
            if (table->rowCount() > 0) table->setCurrentCell(m_selectedRow, 0);
            // Qt 的单次排序/表头尺寸恢复不可中断；不把 4ms 分片宣称为硬耗时上限。
            table->setSortingEnabled(m_uiState.sortingEnabled);
            for (int column = 0; column < m_uiState.resizeModes.size(); ++column)
            {
                table->horizontalHeader()->setSectionResizeMode(column, m_uiState.resizeModes[column]);
            }
            ks::ui::SetTableColumnAutoFitEnabled(table, m_uiState.autoFitEnabled);
            m_finished = true;
            if (qApp != nullptr) qApp->removeEventFilter(this);
            table->setEnabled(m_uiState.enabled);
            table->setUpdatesEnabled(m_uiState.updatesEnabled);
            // 详情投影可能通过外部观察器关闭宿主；离开当前 blocker/slice 后才回调。
            QTimer::singleShot(0, this, [this]()
            {
                const QPointer<CrossViewProjectionJob> guardThis(this);
                const auto completed = m_completed;
                completed();
                if (guardThis != nullptr) guardThis->deleteLater();
            });
        }

        QPointer<QTableWidget> m_table;
        std::uint64_t m_revision = 0;
        std::size_t m_sourceCount = 0;
        CrossViewSelectionIdentity m_selection;
        CrossViewProjectionUiState m_uiState;
        std::function<bool()> m_isCurrent;
        std::function<bool(std::size_t)> m_acceptRow;
        std::function<void(std::size_t, int)> m_appendRow;
        std::function<void()> m_appendDiagnostic;
        std::function<void()> m_completed;
        std::vector<std::size_t> m_indexes;
        Stage m_stage = Stage::Clear;
        std::size_t m_sourceIndex = 0;
        std::size_t m_outputIndex = 0;
        int m_selectionScan = 0;
        int m_selectedRow = 0;
        bool m_scheduled = false;
        bool m_finished = false;
    };

    enum class CrossViewColumn : int
    {
        Id = 0,
        Object,
        Process,
        Public,
        ActiveOrThreadList,
        Anomaly,
        Confidence,
        Detail,
        Count
    };

    int columnIndex(const CrossViewColumn column)
    {
        // 输入：Cross-View 表格列枚举。
        // 处理：转换为 Qt 表格列索引。
        // 返回：列号。
        return static_cast<int>(column);
    }

    QString hex64(const std::uint64_t value)
    {
        // 输入：地址或 capability 值。
        // 处理：格式化为 0x 前缀大写十六进制。
        // 返回：展示文本。
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 16, 16, QChar('0'))
            .toUpper();
    }

    QString sourceYesNo(const std::uint32_t sourceMask, const std::uint32_t bit)
    {
        // 输入：来源矩阵 sourceMask 和目标 bit。
        // 处理：映射为勾选文本。
        // 返回：是/否。
        return (sourceMask & bit) ? QStringLiteral("是") : QStringLiteral("-");
    }

    QString anomalyText(const std::uint32_t flags)
    {
        // 输入：KSWORD_ARK_CROSSVIEW_ANOMALY_* 位集合。
        // 处理：转换为短标签，便于 ProcessDock/MonitorDock 使用一致语义。
        // 返回：异常文本；无异常返回“正常”。
        if (flags == 0U)
        {
            return QStringLiteral("正常");
        }
        QStringList parts;
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_CID_ONLY) parts << QStringLiteral("仅单源");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_ACTIVE_ONLY) parts << QStringLiteral("Active-only");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_ACTIVE_LIST) parts << QStringLiteral("缺活跃源");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_MISSING_FROM_CID_TABLE) parts << QStringLiteral("缺辅助源");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_ORPHAN) parts << QStringLiteral("孤儿线程");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_THREAD_NOT_IN_PROCESS_LIST) parts << QStringLiteral("线程进程缺失");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_START_ADDRESS_OUTSIDE_MODULE) parts << QStringLiteral("入口出模块");
        if (flags & KSWORD_ARK_CROSSVIEW_ANOMALY_DANGLING_OBJECT) parts << QStringLiteral("悬空对象");
        return parts.join(QStringLiteral(" | "));
    }

    // friendlyDriverDetail：
    // - 输入 rawDetail：R0 返回的原始 detail 字段；
    // - 处理：把常见协议/IO 字符串转换成人类可读说明；
    // - 返回：表格末列可直接展示的短说明。
    QString friendlyDriverDetail(const QString& rawDetail)
    {
        const QString trimmedText = rawDetail.trimmed();
        if (trimmedText.isEmpty())
        {
            return QStringLiteral("驱动未返回额外说明");
        }
        if (trimmedText.contains(QStringLiteral("DeviceIoControl"), Qt::CaseInsensitive))
        {
            return QStringLiteral("驱动调用失败或协议版本不匹配");
        }
        if (trimmedText.contains(QStringLiteral("unsupported"), Qt::CaseInsensitive) ||
            trimmedText.contains(QStringLiteral("not supported"), Qt::CaseInsensitive))
        {
            return QStringLiteral("当前驱动不支持该 cross-view 查询");
        }
        if (trimmedText.contains(QStringLiteral("capability"), Qt::CaseInsensitive) ||
            trimmedText.contains(QStringLiteral("DynData"), Qt::CaseInsensitive))
        {
            return QStringLiteral("动态偏移能力未完全满足，请查看详情区的 capability/DynData 信息");
        }
        return trimmedText.left(180);
    }

    // crossViewTableDetail：
    // - 输入 isThread/sourceMask/anomalyFlags/confidence/rawDetail：当前 cross-view 行关键信息；
    // - 处理：生成表格末列摘要，避免直接把原始 R0 字符串塞进表格；
    // - 返回：一行中文说明，原始 detail 仍在详情区完整展示。
    QString crossViewTableDetail(
        const bool isThread,
        const std::uint32_t sourceMask,
        const std::uint32_t anomalyFlags,
        const std::uint32_t confidence,
        const QString& rawDetail)
    {
        const std::uint32_t listBit = isThread
            ? KSWORD_ARK_CROSSVIEW_SOURCE_THREAD_LIST
            : KSWORD_ARK_CROSSVIEW_SOURCE_ACTIVE_LIST;
        const QString objectKindText = isThread ? QStringLiteral("线程") : QStringLiteral("进程");
        const QString sourceText = QStringLiteral("PublicWalk=%1，%2=%3")
            .arg(sourceYesNo(sourceMask, KSWORD_ARK_CROSSVIEW_SOURCE_PUBLIC_WALK))
            .arg(isThread ? QStringLiteral("ThreadList") : QStringLiteral("ActiveList"))
            .arg(sourceYesNo(sourceMask, listBit));
        const QString anomalySummaryText = anomalyFlags == 0U
            ? QStringLiteral("未发现 cross-view 异常")
            : QStringLiteral("异常：%1").arg(anomalyText(anomalyFlags));

        return QStringLiteral("%1；%2；%3；置信度 %4；%5")
            .arg(objectKindText)
            .arg(sourceText)
            .arg(anomalySummaryText)
            .arg(confidence)
            .arg(friendlyDriverDetail(rawDetail));
    }

    QString narrowToQString(const std::string& value)
    {
        // 输入：ArkDriverClient 的窄字符串。
        // 处理：按 UTF-8 转换，失败场景 Qt 会保留可显示替代字符。
        // 返回：QString。
        return QString::fromStdString(value);
    }

    class NumericItem final : public QTableWidgetItem
    {
    public:
        NumericItem(const QString& text, const qulonglong value)
            : QTableWidgetItem(text)
        {
            // 输入：显示文本和排序数值。
            // 处理：数值写入 UserRole，保留文本原样。
            // 返回：构造函数无返回值。
            setData(Qt::UserRole, QVariant::fromValue<qulonglong>(value));
            setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        }

        bool operator<(const QTableWidgetItem& other) const override
        {
            // 输入：另一单元格。
            // 处理：优先按 UserRole 数值排序。
            // 返回：true 表示当前项更小。
            bool leftOk = false;
            bool rightOk = false;
            const qulonglong leftValue = data(Qt::UserRole).toULongLong(&leftOk);
            const qulonglong rightValue = other.data(Qt::UserRole).toULongLong(&rightOk);
            if (leftOk && rightOk)
            {
                return leftValue < rightValue;
            }
            return QTableWidgetItem::operator<(other);
        }
    };

    QTableWidgetItem* textItem(const QString& value)
    {
        // 输入：展示文本。
        // 处理：创建只读单元格项。
        // 返回：交给 QTableWidget 接管生命周期的 item。
        QTableWidgetItem* item = new QTableWidgetItem(value);
        item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        return item;
    }

    QTableWidgetItem* numericItem(const QString& text, const qulonglong value)
    {
        // 输入：展示文本和排序数值。
        // 处理：创建可数值排序单元格。
        // 返回：交给 QTableWidget 接管生命周期的 item。
        return new NumericItem(text, value);
    }

    QString crossViewTableCellText(QTableWidget* table, const int rowIndex, const int columnIndex)
    {
        // crossViewTableCellText：
        // - 输入：Cross-View 表格、行号、列号；
        // - 处理：安全读取单元格文本；
        // - 返回：单元格不存在时返回空字符串。
        if (table == nullptr)
        {
            return QString();
        }
        const QTableWidgetItem* item = table->item(rowIndex, columnIndex);
        return item != nullptr ? item->text() : QString();
    }

    QString crossViewEmptyStateDetail(
        const QString& titleText,
        const bool ioOk,
        const bool unsupported,
        const std::size_t cacheCount,
        const std::uint32_t returnedCount,
        const std::uint32_t totalCount,
        const std::uint64_t missingCapabilityMask,
        const QString& rawMessageText)
    {
        // crossViewEmptyStateDetail：
        // - 输入：单个 cross-view wrapper 的 IO 状态、计数和原始消息；
        // - 处理：生成表格空状态诊断行的完整说明；
        // - 返回：可放入详情区或表格末列的人读文本。
        if (cacheCount > 0U)
        {
            return QStringLiteral("%1：当前过滤条件隐藏了全部 %2 条缓存记录；请清空过滤或关闭“仅异常”。")
                .arg(titleText)
                .arg(static_cast<qulonglong>(cacheCount));
        }

        QString stateText;
        if (ioOk)
        {
            stateText = QStringLiteral("驱动接口可用，但本次没有返回结构化行");
        }
        else if (unsupported)
        {
            stateText = QStringLiteral("当前驱动/协议暂不支持该 Cross-View 查询");
        }
        else
        {
            stateText = QStringLiteral("驱动查询暂不可用");
        }

        return QStringLiteral("%1：%2；驱动报告 %3/%4 行；missingCapability=%5；说明=%6")
            .arg(titleText)
            .arg(stateText)
            .arg(returnedCount)
            .arg(totalCount)
            .arg(hex64(missingCapabilityMask))
            .arg(friendlyDriverDetail(rawMessageText));
    }

    void setCrossViewDiagnosticRow(
        QTableWidget* table,
        const QString& idText,
        const QString& anomalyText,
        const QString& detailText)
    {
        // setCrossViewDiagnosticRow：
        // - 输入：目标表格、首列提示、异常列提示和详情文本；
        // - 处理：写入一行不可编辑诊断，UserRole+2 保存完整详情；
        // - 返回：无。用于避免 R0 空结果/过滤空结果导致表格完全空白。
        if (table == nullptr)
        {
            return;
        }

        table->setRowCount(1);
        QTableWidgetItem* idItem = textItem(idText);
        idItem->setData(Qt::UserRole + 2, detailText);
        table->setItem(0, columnIndex(CrossViewColumn::Id), idItem);
        table->setItem(0, columnIndex(CrossViewColumn::Object), textItem(QStringLiteral("N/A")));
        table->setItem(0, columnIndex(CrossViewColumn::Process), textItem(QStringLiteral("N/A")));
        table->setItem(0, columnIndex(CrossViewColumn::Public), textItem(QStringLiteral("-")));
        table->setItem(0, columnIndex(CrossViewColumn::ActiveOrThreadList), textItem(QStringLiteral("-")));
        table->setItem(0, columnIndex(CrossViewColumn::Anomaly), textItem(anomalyText));
        table->setItem(0, columnIndex(CrossViewColumn::Confidence), numericItem(QStringLiteral("0"), 0));
        table->setItem(0, columnIndex(CrossViewColumn::Detail), textItem(detailText));
        table->setCurrentCell(0, columnIndex(CrossViewColumn::Id));
    }

    QString crossViewCurrentRowText(QTableWidget* table)
    {
        // 固定当前显示行的 TSV；菜单进入嵌套事件循环后不再读取可能变化的投影。
        if (table == nullptr)
        {
            return QString();
        }

        const int rowIndex = table->currentRow();
        if (rowIndex < 0 || rowIndex >= table->rowCount())
        {
            return QString();
        }

        QStringList fields;
        fields.reserve(table->columnCount());
        for (int columnIndex = 0; columnIndex < table->columnCount(); ++columnIndex)
        {
            fields.push_back(crossViewTableCellText(table, rowIndex, columnIndex));
        }
        return fields.join(QLatin1Char('\t'));
    }

    void installCrossViewContextMenu(
        QTableWidget* table,
        const std::function<quint32(const QTableWidget*, int)>& processIdForRow)
    {
        // installCrossViewContextMenu：
        // - 输入：Cross-View 表；
        // - 处理：安装复制当前行和按表格明确 PID 来源跳转详情的右键菜单；
        // - 返回：无。
        if (table == nullptr)
        {
            return;
        }

        table->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(table, &QTableWidget::customContextMenuRequested, table, [table, processIdForRow](const QPoint& localPosition) {
            const QModelIndex clickedIndex = table->indexAt(localPosition);
            if (clickedIndex.isValid())
            {
                table->setCurrentCell(clickedIndex.row(), clickedIndex.column());
            }

            // 固定用户实际点击的显示行，不能先投影再用旧坐标重新命中另一实体。
            const QString rowText = crossViewCurrentRowText(table);
            const quint32 processId = processIdForRow != nullptr
                ? processIdForRow(table, table->currentRow())
                : 0U;
            QMenu contextMenu(table);
            contextMenu.setStyleSheet(KswordTheme::ContextMenuStyle());
            QAction* copyRowAction = contextMenu.addAction(
                QIcon(QStringLiteral(":/Icon/process_copy_row.svg")),
                QStringLiteral("复制当前行"));
            // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
            ks::ui::SetTableContextActionRole(copyRowAction, ks::ui::TableContextActionRole::CopyRows);
            copyRowAction->setEnabled(table->currentRow() >= 0);
            QAction* openProcessAction = contextMenu.addAction(
                QIcon(QStringLiteral(":/Icon/process_details.svg")),
                QStringLiteral("转到进程详细信息"));
            openProcessAction->setEnabled(processId != 0U);

            QAction* selectedAction = contextMenu.exec(table->viewport()->mapToGlobal(localPosition));
            if (selectedAction == copyRowAction)
            {
                if (QGuiApplication::clipboard() != nullptr)
                {
                    QGuiApplication::clipboard()->setText(rowText);
                }
            }
            else if (selectedAction == openProcessAction)
            {
                ks::ui::OpenProcessDetailByPid(processId);
            }
        });
    }

    bool textContainsFilter(const QStringList& fields, const QString& filter)
    {
        // 输入：待匹配字段集合和过滤文本。
        // 处理：大小写不敏感 contains。
        // 返回：true 表示命中过滤。
        if (filter.isEmpty())
        {
            return true;
        }
        for (const QString& field : fields)
        {
            if (field.contains(filter, Qt::CaseInsensitive))
            {
                return true;
            }
        }
        return false;
    }

    ks::ui::FieldDocument offsetsDocument(const ksword::ark::CrossViewFieldOffsets& offsets)
    {
        // 输入：R0 返回的 DynData offset 快照。
        // 处理：直接生成偏移结构字段。
        // 返回：可复制的偏移说明。
        ks::ui::FieldDocument document;
        document.section(QStringLiteral("DynData Offsets"));
        document.field(QStringLiteral("EPROCESS.UniqueProcessId"), QStringLiteral("0x%1").arg(offsets.epUniqueProcessId, 8, 16, QChar('0')));
        document.field(QStringLiteral("EPROCESS.ActiveProcessLinks"), QStringLiteral("0x%1").arg(offsets.epActiveProcessLinks, 8, 16, QChar('0')));
        document.field(QStringLiteral("EPROCESS.ThreadListHead"), QStringLiteral("0x%1").arg(offsets.epThreadListHead, 8, 16, QChar('0')));
        document.field(QStringLiteral("EPROCESS.ImageFileName"), QStringLiteral("0x%1").arg(offsets.epImageFileName, 8, 16, QChar('0')));
        document.field(QStringLiteral("ETHREAD.ThreadListEntry"), QStringLiteral("0x%1").arg(offsets.etThreadListEntry, 8, 16, QChar('0')));
        document.field(QStringLiteral("ETHREAD.StartAddress"), QStringLiteral("0x%1").arg(offsets.etStartAddress, 8, 16, QChar('0')));
        document.field(QStringLiteral("KTHREAD.Process"), QStringLiteral("0x%1").arg(offsets.ktProcess, 8, 16, QChar('0')));
        return document;
    }
}

void ProcessDock::initializeCrossViewPage()
{
    // 输入：无，由 initializeUi 调用。
    // 处理：创建 Cross-View 页，展示进程和线程来源矩阵。
    // 返回：无。
    m_crossViewPage = new QWidget(this);
    m_crossViewPageLayout = new QVBoxLayout(m_crossViewPage);
    // 外围留白由进程主Tab统一处理，工具行和结果页签在局部分组内直接相连。
    m_crossViewPageLayout->setContentsMargins(6, 0, 6, 6);
    m_crossViewPageLayout->setSpacing(6);

    // 来源矩阵保持原详情分割器，只取消查询工具行与结果Tab之间的重复间隔。
    QWidget* const crossViewResults = new QWidget(m_crossViewPage);
    auto* const crossViewResultsLayout = new QVBoxLayout(crossViewResults);
    crossViewResultsLayout->setContentsMargins(0, 0, 0, 0);
    crossViewResultsLayout->setSpacing(0);
    m_crossViewPageLayout->addWidget(crossViewResults, 1);

    // 来源矩阵工具区保持一行，用轻底面分开查询条件与结果。
    QWidget* const crossViewToolbar = new QWidget(m_crossViewPage);
    ks::ui::StylePrimaryToolbar(crossViewToolbar);
    m_crossViewTopLayout = new QHBoxLayout(crossViewToolbar);
    m_crossViewTopLayout->setContentsMargins(8, 4, 8, 4);
    m_crossViewTopLayout->setSpacing(8);

    m_crossViewRefreshButton = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QString(), m_crossViewPage);
    KswordTheme::ApplyStandardIconButtonMetrics(m_crossViewRefreshButton);
    // 刷新是独立动作，保留可辨识的中性按钮底面。
    ks::ui::ApplyFlatButtonTheme(m_crossViewRefreshButton,
        ks::ui::FlatButtonTone::Neutral, ks::ui::FlatButtonAppearance::Solid);
    m_crossViewRefreshButton->setToolTip(QStringLiteral("查询 R0 Process/Thread Cross-View 证据"));

    m_crossViewSearchEdit = new QLineEdit(m_crossViewPage);
    m_crossViewSearchEdit->setClearButtonEnabled(true);
    m_crossViewSearchEdit->setPlaceholderText(QStringLiteral("过滤 PID/TID/进程名/异常/详情"));
    // 搜索位于两张内层 Tab 表格之外，需显式登记，保留既有过滤文本和连接。
    ks::ui::StyleSearchField(m_crossViewSearchEdit);

    m_crossViewAnomalyOnlyCheck = new QCheckBox(QStringLiteral("仅异常"), m_crossViewPage);
    m_crossViewAnomalyOnlyCheck->setChecked(true);

    m_crossViewStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_crossViewPage);
    m_crossViewStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_crossViewStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    m_crossViewTopLayout->addWidget(m_crossViewRefreshButton);
    m_crossViewTopLayout->addWidget(m_crossViewAnomalyOnlyCheck);
    m_crossViewTopLayout->addWidget(m_crossViewSearchEdit, 1);
    m_crossViewTopLayout->addWidget(m_crossViewStatusLabel);
    ks::ui::NormalizeToolbarRow(m_crossViewTopLayout);
    crossViewResultsLayout->addWidget(crossViewToolbar);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_crossViewPage);
    crossViewResultsLayout->addWidget(splitter, 1);

    QTabWidget* innerTabs = new QTabWidget(splitter);
    // 结果表与整条页签等宽，工具行底部接上页签；下方详情仍由原分割器管理。
    ks::ui::StyleJoinedResultTabs(innerTabs, crossViewToolbar);
    m_processCrossViewTable = new ks::ui::VisibleTableWidget(innerTabs);
    m_threadCrossViewTable = new ks::ui::VisibleTableWidget(innerTabs);
    // 两张来源矩阵均为独立 R0 证据，保留完整快照对比。
    ks::ui::SetTableActionBarMode(m_processCrossViewTable, ks::ui::TableActionBarMode::Full);
    ks::ui::SetTableActionBarMode(m_threadCrossViewTable, ks::ui::TableActionBarMode::Full);
    for (QTableWidget* table : { m_processCrossViewTable, m_threadCrossViewTable })
    {
        table->setColumnCount(columnIndex(CrossViewColumn::Count));
        table->setHorizontalHeaderLabels(QStringList{
            QStringLiteral("ID"),
            QStringLiteral("对象"),
            QStringLiteral("进程"),
            QStringLiteral("PublicWalk"),
            QStringLiteral("Active/ThreadList"),
            QStringLiteral("异常"),
            QStringLiteral("置信度"),
            QStringLiteral("说明")
            });
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true);
        table->setSortingEnabled(true);
        table->verticalHeader()->setVisible(false);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(columnIndex(CrossViewColumn::Detail), QHeaderView::Stretch);
        // 两张来源表保留独立圆角外壳，内部冻结窗格仍由共享层保持连体方角。
        ks::ui::ApplyTablePresentation(table);
    }
    for (QTableWidget* table : { m_processCrossViewTable, m_threadCrossViewTable })
    {
        new CrossViewShowObserver(table, [guardThis = QPointer<ProcessDock>(this)]()
        {
            if (guardThis != nullptr) guardThis->scheduleCrossViewTableRebuild();
        });
    }
    installCrossViewContextMenu(
        m_processCrossViewTable,
        [](const QTableWidget* table, const int rowIndex)
        {
            const QTableWidgetItem* processIdItem = table != nullptr
                ? table->item(rowIndex, columnIndex(CrossViewColumn::Id))
                : nullptr;
            bool ok = false;
            const qulonglong processId = processIdItem != nullptr
                ? processIdItem->data(Qt::UserRole).toULongLong(&ok)
                : 0ULL;
            return ok && processId <= std::numeric_limits<quint32>::max()
                ? static_cast<quint32>(processId)
                : 0U;
        });
    installCrossViewContextMenu(
        m_threadCrossViewTable,
        [](const QTableWidget* table, const int rowIndex)
        {
            const QTableWidgetItem* threadIdItem = table != nullptr
                ? table->item(rowIndex, columnIndex(CrossViewColumn::Id))
                : nullptr;
            bool ok = false;
            const qulonglong processId = threadIdItem != nullptr
                ? threadIdItem->data(Qt::UserRole + 3).toULongLong(&ok)
                : 0ULL;
            return ok && processId <= std::numeric_limits<quint32>::max()
                ? static_cast<quint32>(processId)
                : 0U;
        });
    registerNavigationTab(NavigationPage::CrossViewProcess, innerTabs, m_processCrossViewTable);
    registerNavigationTab(NavigationPage::CrossViewThread, innerTabs, m_threadCrossViewTable);
    splitter->addWidget(innerTabs);

    // Cross-View 详情区直接接受结构字段，保留查找、复制与全部详情布局。
    m_crossViewDetailEdit = new ks::ui::StructuredFieldView(splitter);
    m_crossViewDetailEdit->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("选择一条记录查看进程来源、异常标记和内核详情。")));
    splitter->addWidget(m_crossViewDetailEdit);
    auto* detailHost = ks::ui::DetailLayoutRegistry::registerStructuredHost(
        m_processCrossViewTable, m_crossViewDetailEdit, m_crossViewPage, splitter, innerTabs, m_crossViewDetailEdit);
    const QPointer<ks::ui::DetailLayoutHost> hostGuard(detailHost);
    connect(innerTabs, &QTabWidget::currentChanged, m_crossViewPage, [this, hostGuard](int index) {
        m_crossViewDetailEdit->setProperty("ks_cross_view_thread_selected", index == 1);
        if (!hostGuard.isNull()) hostGuard->setTableView(index == 1 ? m_threadCrossViewTable : m_processCrossViewTable);
        if (m_crossViewFilterTimer != nullptr && m_crossViewFilterTimer->isActive())
        {
            invalidateCrossViewTables();
        }
        else
        {
            rebuildCrossViewTables();
        }
        showCrossViewDetailForCurrentRow(index == 1);
    });
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    registerNavigationTab(NavigationPage::CrossView, m_sideTabWidget, m_crossViewPage, blueTintedIcon(":/Icon/process_tree.svg"));
}

void ProcessDock::initializeCrossViewConnections()
{
    // 输入：无，由 initializeConnections 调用。
    // 处理：连接刷新、过滤和选择变化。
    // 返回：无。
    connect(m_crossViewRefreshButton, &QPushButton::clicked, this, [this]() {
        refreshCrossViewAsync();
    });
    m_crossViewFilterTimer = new QTimer(this);
    m_crossViewFilterTimer->setSingleShot(true);
    m_crossViewFilterTimer->setInterval(100);
    connect(m_crossViewFilterTimer, &QTimer::timeout, this, &ProcessDock::invalidateCrossViewTables);
    connect(m_crossViewSearchEdit, &QLineEdit::textChanged, m_crossViewFilterTimer,
        [this]() { m_crossViewFilterTimer->start(); });
    connect(m_crossViewAnomalyOnlyCheck, &QCheckBox::toggled, this,
        [this]() { invalidateCrossViewTables(); });
    connect(m_processCrossViewTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showCrossViewDetailForCurrentRow(false);
    });
    connect(m_threadCrossViewTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showCrossViewDetailForCurrentRow(true);
    });
}

void ProcessDock::refreshCrossViewAsync()
{
    // 输入：用户刷新动作。
    // 处理：后台同时请求进程和线程 cross-view，主线程回填缓存。
    // 返回：无。
    if (m_crossViewRefreshInProgress)
    {
        return;
    }
    m_crossViewRefreshInProgress = true;
    const std::uint64_t ticket = ++m_crossViewRefreshTicket;
    if (m_crossViewRefreshButton != nullptr)
    {
        m_crossViewRefreshButton->setEnabled(false);
    }
    if (m_crossViewStatusLabel != nullptr)
    {
        m_crossViewStatusLabel->setText(QStringLiteral("状态：查询中..."));
        m_crossViewStatusLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:700;").arg(KswordTheme::PrimaryBlueHex));
    }

    QPointer<ProcessDock> guardThis(this);
    QRunnable* task = QRunnable::create([guardThis, ticket]() {
        const ksword::ark::DriverClient client;
        ksword::ark::ProcessCrossViewResult processResult = client.queryProcessCrossView();
        ksword::ark::ThreadCrossViewResult threadResult = client.queryThreadCrossView();

        using Snapshot = std::pair<ksword::ark::ProcessCrossViewResult, ksword::ark::ThreadCrossViewResult>;
        // 回调/菜单屏障可以复制闭包，但不再深复制两份完整证据；只在 GUI 消费一次。
        const auto pendingSnapshot = std::make_shared<std::optional<Snapshot>>(
            std::in_place, std::move(processResult), std::move(threadResult));
        QMetaObject::invokeMethod(qApp, [guardThis, ticket, pendingSnapshot]() mutable {
            if (guardThis == nullptr || guardThis->m_crossViewRefreshTicket != ticket)
            {
                return;
            }
            auto applySnapshot = [guardThis, ticket, pendingSnapshot]() mutable
            {
                if (guardThis == nullptr || guardThis->m_crossViewRefreshTicket != ticket
                    || !pendingSnapshot->has_value())
                {
                    return;
                }
                Snapshot snapshot = std::move(pendingSnapshot->value());
                pendingSnapshot->reset();
                auto& processSnapshot = snapshot.first;
                auto& threadSnapshot = snapshot.second;

                guardThis->m_crossViewRefreshInProgress = false;
                if (guardThis->m_crossViewRefreshButton != nullptr)
                {
                    guardThis->m_crossViewRefreshButton->setEnabled(true);
                }

                // 每类只持有一份完整规范行；Result 保留全部 IO/计数/capability 元信息。
                guardThis->m_processCrossViewCache = std::move(processSnapshot.entries);
                guardThis->m_threadCrossViewCache = std::move(threadSnapshot.entries);
                guardThis->m_lastProcessCrossViewResult = std::move(processSnapshot);
                guardThis->m_lastThreadCrossViewResult = std::move(threadSnapshot);
                const auto& processMetadata = guardThis->m_lastProcessCrossViewResult;
                const auto& threadMetadata = guardThis->m_lastThreadCrossViewResult;
                guardThis->invalidateCrossViewTables();

                QString statusText;
                if (!processMetadata.io.ok || !threadMetadata.io.ok)
                {
                    // 将底层 IO 诊断转换为用户可读说明：
                    // - 输入：ArkDriverClient 的 unsupported 标记和 io.message；
                    // - 处理：保留“未集成/驱动过旧”的明确语义，其余交给 friendlyDriverDetail 归一化；
                    // - 返回：状态栏短文本，不直接暴露 DeviceIoControl 等底层字符串。
                    const QString processMessageText = processMetadata.unsupported
                        ? QStringLiteral("进程未集成/驱动过旧")
                        : friendlyDriverDetail(narrowToQString(processMetadata.io.message));
                    const QString threadMessageText = threadMetadata.unsupported
                        ? QStringLiteral("线程未集成/驱动过旧")
                        : friendlyDriverDetail(narrowToQString(threadMetadata.io.message));
                    statusText = QStringLiteral("状态：%1 / %2")
                        .arg(processMessageText)
                        .arg(threadMessageText);
                    guardThis->m_crossViewStatusLabel->setStyleSheet(
                        QStringLiteral("color:%1; font-weight:700;")
                            .arg(KswordTheme::ErrorColor().name(QColor::HexRgb)));
                }
                else
                {
                    statusText = QStringLiteral("状态：进程 %1/%2，线程 %3/%4，missingCaps=0x%5/0x%6")
                        .arg(guardThis->m_processCrossViewCache.size())
                        .arg(processMetadata.totalCount)
                        .arg(guardThis->m_threadCrossViewCache.size())
                        .arg(threadMetadata.totalCount)
                        .arg(static_cast<qulonglong>(processMetadata.missingCapabilityMask), 0, 16)
                        .arg(static_cast<qulonglong>(threadMetadata.missingCapabilityMask), 0, 16);
                    guardThis->m_crossViewStatusLabel->setStyleSheet(
                        QStringLiteral("color:%1; font-weight:700;")
                            .arg(KswordTheme::SuccessColor().name(QColor::HexRgb)));
                }
                guardThis->m_crossViewStatusLabel->setText(statusText);
                guardThis->showCrossViewDetailForCurrentRow(guardThis->m_crossViewDetailEdit != nullptr
                    && guardThis->m_crossViewDetailEdit->property("ks_cross_view_thread_selected").toBool());
            };

            if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
                    guardThis.data(),
                    QStringLiteral("process-cross-view-snapshot-apply"),
                    {guardThis->m_processCrossViewTable, guardThis->m_threadCrossViewTable},
                    applySnapshot))
            {
                return;
            }
            applySnapshot();
        }, Qt::QueuedConnection);
    });
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void ProcessDock::invalidateCrossViewTables()
{
    if (m_crossViewFilterTimer != nullptr) m_crossViewFilterTimer->stop();
    ++m_crossViewProjectionRevision;
    m_processCrossViewProjectionDirty = true;
    m_threadCrossViewProjectionDirty = true;
    rebuildCrossViewTables();
}

void ProcessDock::scheduleCrossViewTableRebuild()
{
    if (m_crossViewProjectionQueued) return;
    m_crossViewProjectionQueued = true;
    const QPointer<ProcessDock> guardThis(this);
    QTimer::singleShot(0, this, [guardThis]()
    {
        if (guardThis == nullptr) return;
        guardThis->m_crossViewProjectionQueued = false;
        guardThis->rebuildCrossViewTables();
    });
}

void ProcessDock::rebuildCrossViewTables()
{
    const bool rebuildProcess = m_processCrossViewProjectionDirty
        && crossViewProjectionExposed(m_processCrossViewTable);
    const bool rebuildThread = m_threadCrossViewProjectionDirty
        && crossViewProjectionExposed(m_threadCrossViewTable);
    if (!rebuildProcess && !rebuildThread) return;
    const QPointer<ProcessDock> safeThis(this);
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(
        this,
        QStringLiteral("process-cross-view-tables-rebuild"),
        {m_processCrossViewTable, m_threadCrossViewTable},
        [safeThis]()
        {
            if (!safeThis.isNull())
            {
                safeThis->rebuildCrossViewTables();
            }
        }))
    {
        return;
    }

    ks::ui::DetailLayoutRegistry::prepareDataRebuild(m_crossViewDetailEdit);
    // 两份规范证据仍完整保留；隐藏表只留下 dirty，不为不可见页生成 item。
    const QString filter = m_crossViewSearchEdit != nullptr ? m_crossViewSearchEdit->text().trimmed() : QString();
    const bool anomalyOnly = m_crossViewAnomalyOnlyCheck != nullptr && m_crossViewAnomalyOnlyCheck->isChecked();

    if (rebuildProcess)
    {
        auto* previousJob = static_cast<CrossViewProjectionJob*>(m_processCrossViewProjectionJob.data());
        if (previousJob != nullptr && previousJob->revision() == m_crossViewProjectionRevision)
        {
            previousJob->schedule();
        }
        else
        {
            const CrossViewSelectionIdentity selectedIdentity = previousJob != nullptr
                ? previousJob->selection() : crossViewSelectionIdentity(m_processCrossViewTable);
            CrossViewProjectionUiState uiState = previousJob != nullptr
                ? previousJob->uiState() : CrossViewProjectionUiState::capture(m_processCrossViewTable);
            delete previousJob;
            const QPointer<ProcessDock> guardThis(this);
            const std::uint64_t revision = m_crossViewProjectionRevision;
            auto* job = new CrossViewProjectionJob(
                m_processCrossViewTable, revision, m_processCrossViewCache.size(), selectedIdentity, std::move(uiState),
                [guardThis, revision]()
                {
                    return guardThis != nullptr && guardThis->m_crossViewProjectionRevision == revision
                        && guardThis->m_processCrossViewProjectionDirty;
                },
                [guardThis, filter, anomalyOnly](const std::size_t cacheIndex)
                {
                    const auto& row = guardThis->m_processCrossViewCache[cacheIndex];
                    if (anomalyOnly && row.anomalyFlags == 0U)
                    {
                        return false;
                    }
                    if (!textContainsFilter({
                        QString::number(row.processId),
                        narrowToQString(row.imageName),
                        hex64(row.objectAddress),
                        anomalyText(row.anomalyFlags),
                        narrowToQString(row.detail)
                        }, filter))
                    {
                        return false;
                    }
                    return true;
                },
                [guardThis](const std::size_t cacheIndex, const int tableRow)
                {
                    const auto& row = guardThis->m_processCrossViewCache[cacheIndex];
                    QTableWidgetItem* idItem = numericItem(QString::number(row.processId), row.processId);
                    idItem->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(static_cast<qulonglong>(cacheIndex)));
                    guardThis->m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Id), idItem);
                    guardThis->m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Object), numericItem(hex64(row.objectAddress), row.objectAddress));
                    guardThis->m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Process), textItem(narrowToQString(row.imageName)));
                    guardThis->m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Public), textItem(sourceYesNo(row.sourceMask, KSWORD_ARK_CROSSVIEW_SOURCE_PUBLIC_WALK)));
                    guardThis->m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::ActiveOrThreadList), textItem(sourceYesNo(row.sourceMask, KSWORD_ARK_CROSSVIEW_SOURCE_ACTIVE_LIST)));
                    guardThis->m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Anomaly), textItem(anomalyText(row.anomalyFlags)));
                    guardThis->m_processCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Confidence), numericItem(QString::number(row.confidence), row.confidence));
                    guardThis->m_processCrossViewTable->setItem(
                        tableRow,
                        columnIndex(CrossViewColumn::Detail),
                        textItem(crossViewTableDetail(false, row.sourceMask, row.anomalyFlags, row.confidence, narrowToQString(row.detail))));
                },
                [guardThis]()
                {
                    const QString detailText = crossViewEmptyStateDetail(
                        QStringLiteral("进程 Cross-View"),
                        guardThis->m_lastProcessCrossViewResult.io.ok,
                        guardThis->m_lastProcessCrossViewResult.unsupported,
                        guardThis->m_processCrossViewCache.size(),
                        guardThis->m_lastProcessCrossViewResult.returnedCount,
                        guardThis->m_lastProcessCrossViewResult.totalCount,
                        guardThis->m_lastProcessCrossViewResult.missingCapabilityMask,
                        narrowToQString(guardThis->m_lastProcessCrossViewResult.io.message));
                    setCrossViewDiagnosticRow(
                        guardThis->m_processCrossViewTable,
                        QStringLiteral("<无进程证据>"),
                        QStringLiteral("诊断"),
                        detailText);
                },
                [guardThis, revision]()
                {
                    if (guardThis == nullptr || guardThis->m_crossViewProjectionRevision != revision) return;
                    guardThis->m_processCrossViewProjectionDirty = false;
                    guardThis->m_processCrossViewProjectionJob = nullptr;
                    guardThis->showCrossViewDetailForCurrentRow(false);
                });
            m_processCrossViewProjectionJob = job;
            job->schedule();
        }
    }

    if (rebuildThread)
    {
        auto* previousJob = static_cast<CrossViewProjectionJob*>(m_threadCrossViewProjectionJob.data());
        if (previousJob != nullptr && previousJob->revision() == m_crossViewProjectionRevision)
        {
            previousJob->schedule();
        }
        else
        {
            const CrossViewSelectionIdentity selectedIdentity = previousJob != nullptr
                ? previousJob->selection() : crossViewSelectionIdentity(m_threadCrossViewTable);
            CrossViewProjectionUiState uiState = previousJob != nullptr
                ? previousJob->uiState() : CrossViewProjectionUiState::capture(m_threadCrossViewTable);
            delete previousJob;
            const QPointer<ProcessDock> guardThis(this);
            const std::uint64_t revision = m_crossViewProjectionRevision;
            auto* job = new CrossViewProjectionJob(
                m_threadCrossViewTable, revision, m_threadCrossViewCache.size(), selectedIdentity, std::move(uiState),
                [guardThis, revision]()
                {
                    return guardThis != nullptr && guardThis->m_crossViewProjectionRevision == revision
                        && guardThis->m_threadCrossViewProjectionDirty;
                },
                [guardThis, filter, anomalyOnly](const std::size_t cacheIndex)
                {
                    const auto& row = guardThis->m_threadCrossViewCache[cacheIndex];
                    if (anomalyOnly && row.anomalyFlags == 0U)
                    {
                        return false;
                    }
                    if (!textContainsFilter({
                        QString::number(row.threadId),
                        QString::number(row.processId),
                        narrowToQString(row.imageName),
                        hex64(row.objectAddress),
                        anomalyText(row.anomalyFlags),
                        narrowToQString(row.detail)
                        }, filter))
                    {
                        return false;
                    }
                    return true;
                },
                [guardThis](const std::size_t cacheIndex, const int tableRow)
                {
                    const auto& row = guardThis->m_threadCrossViewCache[cacheIndex];
                    QTableWidgetItem* idItem = numericItem(QString::number(row.threadId), row.threadId);
                    idItem->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(static_cast<qulonglong>(cacheIndex)));
                    // 菜单读取显示代的 owner PID，不能用旧 cacheIndex 访问新证据。
                    idItem->setData(Qt::UserRole + 3, QVariant::fromValue<qulonglong>(row.processId));
                    guardThis->m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Id), idItem);
                    guardThis->m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Object), numericItem(hex64(row.objectAddress), row.objectAddress));
                    guardThis->m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Process), textItem(QStringLiteral("%1 %2").arg(row.processId).arg(narrowToQString(row.imageName))));
                    guardThis->m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Public), textItem(sourceYesNo(row.sourceMask, KSWORD_ARK_CROSSVIEW_SOURCE_PUBLIC_WALK)));
                    guardThis->m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::ActiveOrThreadList), textItem(sourceYesNo(row.sourceMask, KSWORD_ARK_CROSSVIEW_SOURCE_THREAD_LIST)));
                    guardThis->m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Anomaly), textItem(anomalyText(row.anomalyFlags)));
                    guardThis->m_threadCrossViewTable->setItem(tableRow, columnIndex(CrossViewColumn::Confidence), numericItem(QString::number(row.confidence), row.confidence));
                    guardThis->m_threadCrossViewTable->setItem(
                        tableRow,
                        columnIndex(CrossViewColumn::Detail),
                        textItem(crossViewTableDetail(true, row.sourceMask, row.anomalyFlags, row.confidence, narrowToQString(row.detail))));
                },
                [guardThis]()
                {
                    const QString detailText = crossViewEmptyStateDetail(
                        QStringLiteral("线程 Cross-View"),
                        guardThis->m_lastThreadCrossViewResult.io.ok,
                        guardThis->m_lastThreadCrossViewResult.unsupported,
                        guardThis->m_threadCrossViewCache.size(),
                        guardThis->m_lastThreadCrossViewResult.returnedCount,
                        guardThis->m_lastThreadCrossViewResult.totalCount,
                        guardThis->m_lastThreadCrossViewResult.missingCapabilityMask,
                        narrowToQString(guardThis->m_lastThreadCrossViewResult.io.message));
                    setCrossViewDiagnosticRow(
                        guardThis->m_threadCrossViewTable,
                        QStringLiteral("<无线程证据>"),
                        QStringLiteral("诊断"),
                        detailText);
                },
                [guardThis, revision]()
                {
                    if (guardThis == nullptr || guardThis->m_crossViewProjectionRevision != revision) return;
                    guardThis->m_threadCrossViewProjectionDirty = false;
                    guardThis->m_threadCrossViewProjectionJob = nullptr;
                    guardThis->showCrossViewDetailForCurrentRow(true);
                });
            m_threadCrossViewProjectionJob = job;
            job->schedule();
        }
    }
}

void ProcessDock::showCrossViewDetailForCurrentRow(const bool preferThreadTable)
{
    // 输入：preferThreadTable 指示优先读取线程表还是进程表。
    // 处理：展开当前行 source/anomaly/DynData 细节到只读文本框。
    // 返回：无。
    QTableWidget* const sourceTable = preferThreadTable ? m_threadCrossViewTable : m_processCrossViewTable;
    if (m_crossViewDetailEdit == nullptr || !crossViewProjectionExposed(sourceTable)
        || (preferThreadTable ? m_threadCrossViewProjectionDirty : m_processCrossViewProjectionDirty))
    {
        // 隐藏旧投影的 cacheIndex 属于旧快照，必须先重建后才读取新规范证据。
        return;
    }

    if (preferThreadTable && m_threadCrossViewTable != nullptr && m_threadCrossViewTable->currentRow() >= 0)
    {
        const QTableWidgetItem* item = m_threadCrossViewTable->item(m_threadCrossViewTable->currentRow(), columnIndex(CrossViewColumn::Id));
        const QString diagnosticText = item != nullptr
            ? item->data(Qt::UserRole + 2).toString()
            : QString();
        if (!diagnosticText.isEmpty())
        {
            m_crossViewDetailEdit->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("线程 Cross-View 诊断\n%1").arg(diagnosticText)));
            return;
        }

        bool ok = false;
        const qulonglong cacheIndex = item != nullptr ? item->data(Qt::UserRole + 1).toULongLong(&ok) : 0ULL;
        if (ok && cacheIndex < static_cast<qulonglong>(m_threadCrossViewCache.size()))
        {
            const auto& row = m_threadCrossViewCache[static_cast<std::size_t>(cacheIndex)];
            const QString rawDetailText = narrowToQString(row.detail);
            const QString readableDetailText = friendlyDriverDetail(rawDetailText);
            ks::ui::FieldDocument document;
            document.section(QStringLiteral("线程 Cross-View 详情"));
            document.field(QStringLiteral("TID"), QString::number(row.threadId));
            document.field(QStringLiteral("PID"), QString::number(row.processId));
            document.field(QStringLiteral("Image"), narrowToQString(row.imageName));
            document.field(QStringLiteral("ThreadObject"), hex64(row.objectAddress));
            document.field(QStringLiteral("ProcessObject"), hex64(row.processObjectAddress));
            document.field(QStringLiteral("StartAddress"), hex64(row.startAddress));
            document.field(QStringLiteral("SourceMask"), QStringLiteral("0x%1").arg(row.sourceMask, 8, 16, QChar('0')));
            document.field(QStringLiteral("AnomalyFlags"), QStringLiteral("%1 (0x%2)").arg(anomalyText(row.anomalyFlags)).arg(row.anomalyFlags, 8, 16, QChar('0')), true);
            document.field(QStringLiteral("DynDataCapabilityMask"), hex64(row.dynDataCapabilityMask));
            document.nodes += offsetsDocument(row.fieldOffsets).nodes;
            document.section(QStringLiteral("诊断"));
            document.field(QStringLiteral("LastStatus"), QStringLiteral("0x%1").arg(static_cast<qulonglong>(static_cast<unsigned long>(row.lastStatus)), 8, 16, QChar('0')));
            document.field(QStringLiteral("Confidence"), QString::number(row.confidence));
            document.field(QStringLiteral("驱动说明"), readableDetailText, true);
            document.field(QStringLiteral("驱动原始说明"), rawDetailText);
            m_crossViewDetailEdit->setDocument(document);
            return;
        }
    }

    if (preferThreadTable)
    { m_crossViewDetailEdit->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("请选择一条 Cross-View 记录查看详情。"))); return; }
    if (m_processCrossViewTable != nullptr && m_processCrossViewTable->currentRow() >= 0)
    {
        const QTableWidgetItem* item = m_processCrossViewTable->item(m_processCrossViewTable->currentRow(), columnIndex(CrossViewColumn::Id));
        const QString diagnosticText = item != nullptr
            ? item->data(Qt::UserRole + 2).toString()
            : QString();
        if (!diagnosticText.isEmpty())
        {
            m_crossViewDetailEdit->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("进程 Cross-View 诊断\n%1").arg(diagnosticText)));
            return;
        }

        bool ok = false;
        const qulonglong cacheIndex = item != nullptr ? item->data(Qt::UserRole + 1).toULongLong(&ok) : 0ULL;
        if (ok && cacheIndex < static_cast<qulonglong>(m_processCrossViewCache.size()))
        {
            const auto& row = m_processCrossViewCache[static_cast<std::size_t>(cacheIndex)];
            const QString rawDetailText = narrowToQString(row.detail);
            const QString readableDetailText = friendlyDriverDetail(rawDetailText);
            ks::ui::FieldDocument document;
            document.section(QStringLiteral("进程 Cross-View 详情"));
            document.field(QStringLiteral("PID"), QString::number(row.processId));
            document.field(QStringLiteral("PPID"), QString::number(row.parentProcessId));
            document.field(QStringLiteral("Image"), narrowToQString(row.imageName));
            document.field(QStringLiteral("ProcessObject"), hex64(row.objectAddress));
            document.field(QStringLiteral("StartAddress"), hex64(row.startAddress));
            document.field(QStringLiteral("SourceMask"), QStringLiteral("0x%1").arg(row.sourceMask, 8, 16, QChar('0')));
            document.field(QStringLiteral("AnomalyFlags"), QStringLiteral("%1 (0x%2)").arg(anomalyText(row.anomalyFlags)).arg(row.anomalyFlags, 8, 16, QChar('0')), true);
            document.field(QStringLiteral("DynDataCapabilityMask"), hex64(row.dynDataCapabilityMask));
            document.nodes += offsetsDocument(row.fieldOffsets).nodes;
            document.section(QStringLiteral("诊断"));
            document.field(QStringLiteral("LastStatus"), QStringLiteral("0x%1").arg(static_cast<qulonglong>(static_cast<unsigned long>(row.lastStatus)), 8, 16, QChar('0')));
            document.field(QStringLiteral("Confidence"), QString::number(row.confidence));
            document.field(QStringLiteral("驱动说明"), readableDetailText, true);
            document.field(QStringLiteral("驱动原始说明"), rawDetailText);
            m_crossViewDetailEdit->setDocument(document);
            return;
        }
    }

    m_crossViewDetailEdit->setDocument(ks::ui::FieldDocument{}.note(QStringLiteral("请选择一条 Cross-View 记录查看详情。")));
}
