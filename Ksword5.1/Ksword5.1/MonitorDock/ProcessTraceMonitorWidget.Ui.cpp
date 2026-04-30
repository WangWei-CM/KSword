#include "ProcessTraceMonitorWidget.h"

// ============================================================
// ProcessTraceMonitorWidget.Ui.cpp
// 作用：
// 1) 构建进程定向监控页的全部可视控件；
// 2) 统一按钮图标、提示文本与表格布局；
// 3) 保持单文件体积可控，避免继续放大 MonitorDock.cpp。
// ============================================================

#include "../theme.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSplitter>
#include <QSvgRenderer>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
    // createBlueIcon：
    // - 作用：把 SVG 图标统一染成主题蓝色；
    // - 调用：本页所有简单语义按钮统一使用图标代替文字。
    QIcon createBlueIcon(const char* resourcePath, const QSize& iconSize = QSize(16, 16))
    {
        const QString iconPath = QString::fromUtf8(resourcePath);
        QSvgRenderer renderer(iconPath);
        if (!renderer.isValid())
        {
            return QIcon(iconPath);
        }

        QPixmap pixmap(iconSize);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&painter, QRectF(0, 0, iconSize.width(), iconSize.height()));
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), KswordTheme::PrimaryBlueColor);
        painter.end();

        return QIcon(pixmap);
    }

    // createIconButton：
    // - 作用：统一创建纯图标按钮，并强制写入 tooltip；
    // - 调用：刷新/添加/开始/停止/导出等简单语义按钮全部复用。
    QPushButton* createIconButton(
        QWidget* parentWidget,
        const char* resourcePath,
        const QString& tooltipText)
    {
        QPushButton* buttonPointer = new QPushButton(parentWidget);
        buttonPointer->setIcon(createBlueIcon(resourcePath, QSize(16, 16)));
        buttonPointer->setText(QString());
        buttonPointer->setToolTip(tooltipText);
        buttonPointer->setFixedSize(QSize(28, 28));
        return buttonPointer;
    }
}

void ProcessTraceMonitorWidget::initializeUi()
{
    // 根布局：
    // - 顶部：可选进程与监控目标双栏；
    // - 中部：固定 Provider 说明与开始/停止控制；
    // - 下方：筛选器 + 事件表。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(6);

    m_topSplitter = new QSplitter(Qt::Horizontal, this);
    m_rootLayout->addWidget(m_topSplitter, 0);

    // 可选进程面板：
    // - 提供当前进程快照、关键词过滤与“添加到监控目标”入口；
    // - 手动 PID 添加与过滤栏放在同一行，减少顶部纵向占用。
    m_availablePanel = new QWidget(m_topSplitter);
    QVBoxLayout* availableLayout = new QVBoxLayout(m_availablePanel);
    availableLayout->setContentsMargins(6, 6, 6, 6);
    availableLayout->setSpacing(6);

    QHBoxLayout* availableHeaderLayout = new QHBoxLayout();
    availableHeaderLayout->setSpacing(6);
    availableHeaderLayout->addWidget(new QLabel(QStringLiteral("可选进程"), m_availablePanel), 0);

    m_availableFilterEdit = new QLineEdit(m_availablePanel);
    m_availableFilterEdit->setPlaceholderText(QStringLiteral("按 PID / 进程名 / 路径 / 用户过滤"));
    m_availableFilterEdit->setStyleSheet(blueInputStyle());
    m_availableFilterEdit->setMaximumWidth(320);
    availableHeaderLayout->addWidget(m_availableFilterEdit, 1);

    m_availableRefreshButton = createIconButton(
        m_availablePanel,
        ":/Icon/process_refresh.svg",
        QStringLiteral("刷新当前系统进程快照"));
    m_availableRefreshButton->setStyleSheet(blueButtonStyle());
    availableHeaderLayout->addWidget(m_availableRefreshButton, 0);

    m_addSelectedButton = createIconButton(
        m_availablePanel,
        ":/Icon/process_start.svg",
        QStringLiteral("把选中的进程加入监控目标"));
    m_addSelectedButton->setStyleSheet(blueButtonStyle());
    availableHeaderLayout->addWidget(m_addSelectedButton, 0);
    availableHeaderLayout->addWidget(new QLabel(QStringLiteral("手动 PID"), m_availablePanel), 0);

    m_manualPidEdit = new QLineEdit(m_availablePanel);
    m_manualPidEdit->setPlaceholderText(QStringLiteral("输入十进制或 0x 十六进制 PID"));
    m_manualPidEdit->setStyleSheet(blueInputStyle());
    m_manualPidEdit->setMinimumWidth(120);
    m_manualPidEdit->setMaximumWidth(160);
    availableHeaderLayout->addWidget(m_manualPidEdit, 0);

    m_addManualPidButton = createIconButton(
        m_availablePanel,
        ":/Icon/process_details.svg",
        QStringLiteral("按输入 PID 加入监控目标"));
    m_addManualPidButton->setStyleSheet(blueButtonStyle());
    availableHeaderLayout->addWidget(m_addManualPidButton, 0);

    availableLayout->addLayout(availableHeaderLayout);

    m_availableStatusLabel = new QLabel(QStringLiteral("● 等待首次刷新进程快照"), m_availablePanel);
    m_availableStatusLabel->setStyleSheet(buildStatusStyle(monitorIdleColorHex()));
    availableLayout->addWidget(m_availableStatusLabel, 0);

    m_availableTable = new QTableWidget(m_availablePanel);
    m_availableTable->setColumnCount(AvailableProcessColumnCount);
    m_availableTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("PID"),
        QStringLiteral("进程名"),
        QStringLiteral("路径"),
        QStringLiteral("用户")
        });
    m_availableTable->setAlternatingRowColors(true);
    m_availableTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_availableTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_availableTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_availableTable->setContextMenuPolicy(Qt::NoContextMenu);
    m_availableTable->setSortingEnabled(false);
    m_availableTable->verticalHeader()->setVisible(false);
    m_availableTable->horizontalHeader()->setStyleSheet(blueHeaderStyle());
    m_availableTable->horizontalHeader()->setSectionResizeMode(AvailableProcessColumnPid, QHeaderView::ResizeToContents);
    m_availableTable->horizontalHeader()->setSectionResizeMode(AvailableProcessColumnName, QHeaderView::ResizeToContents);
    m_availableTable->horizontalHeader()->setSectionResizeMode(AvailableProcessColumnPath, QHeaderView::Stretch);
    m_availableTable->horizontalHeader()->setSectionResizeMode(AvailableProcessColumnUser, QHeaderView::ResizeToContents);
    availableLayout->addWidget(m_availableTable, 1);

    // 监控目标面板：
    // - 默认展示用户手动选择的目标进程；
    // - 运行期若 ETW 识别到“目标进程拉起子进程”，也会自动加入本列表；
    // - 当列表中的进程收到 ETW 退出事件时，会自动从本列表移除。
    m_targetPanel = new QWidget(m_topSplitter);
    QVBoxLayout* targetLayout = new QVBoxLayout(m_targetPanel);
    targetLayout->setContentsMargins(6, 6, 6, 6);
    targetLayout->setSpacing(6);

    QHBoxLayout* targetHeaderLayout = new QHBoxLayout();
    targetHeaderLayout->setSpacing(6);
    targetHeaderLayout->addWidget(new QLabel(QStringLiteral("监控目标"), m_targetPanel), 0);
    targetHeaderLayout->addStretch(1);

    m_removeTargetButton = createIconButton(
        m_targetPanel,
        ":/Icon/process_pause.svg",
        QStringLiteral("移除选中的监控目标"));
    m_removeTargetButton->setStyleSheet(blueButtonStyle());
    targetHeaderLayout->addWidget(m_removeTargetButton, 0);

    m_clearTargetButton = createIconButton(
        m_targetPanel,
        ":/Icon/process_terminate.svg",
        QStringLiteral("清空全部监控目标"));
    m_clearTargetButton->setStyleSheet(blueButtonStyle());
    targetHeaderLayout->addWidget(m_clearTargetButton, 0);

    targetLayout->addLayout(targetHeaderLayout);

    m_targetStatusLabel = new QLabel(QStringLiteral("● 当前没有监控目标"), m_targetPanel);
    m_targetStatusLabel->setStyleSheet(buildStatusStyle(monitorIdleColorHex()));
    targetLayout->addWidget(m_targetStatusLabel, 0);

    m_targetTable = new QTableWidget(m_targetPanel);
    m_targetTable->setColumnCount(TargetProcessColumnCount);
    m_targetTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("状态"),
        QStringLiteral("PID"),
        QStringLiteral("进程名"),
        QStringLiteral("路径"),
        QStringLiteral("用户"),
        QStringLiteral("备注")
        });
    m_targetTable->setAlternatingRowColors(true);
    m_targetTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_targetTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_targetTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_targetTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_targetTable->verticalHeader()->setVisible(false);
    m_targetTable->horizontalHeader()->setStyleSheet(blueHeaderStyle());
    m_targetTable->horizontalHeader()->setSectionResizeMode(TargetProcessColumnState, QHeaderView::ResizeToContents);
    m_targetTable->horizontalHeader()->setSectionResizeMode(TargetProcessColumnPid, QHeaderView::ResizeToContents);
    m_targetTable->horizontalHeader()->setSectionResizeMode(TargetProcessColumnName, QHeaderView::ResizeToContents);
    m_targetTable->horizontalHeader()->setSectionResizeMode(TargetProcessColumnPath, QHeaderView::Stretch);
    m_targetTable->horizontalHeader()->setSectionResizeMode(TargetProcessColumnUser, QHeaderView::ResizeToContents);
    m_targetTable->horizontalHeader()->setSectionResizeMode(TargetProcessColumnRemark, QHeaderView::ResizeToContents);
    targetLayout->addWidget(m_targetTable, 1);

    m_topSplitter->addWidget(m_availablePanel);
    m_topSplitter->addWidget(m_targetPanel);
    m_topSplitter->setStretchFactor(0, 2);
    m_topSplitter->setStretchFactor(1, 3);

    // 控制栏：
    // - 中间说明固定采用“全量预置 Provider + 进程树辅助快照”；
    // - 用户不再手工勾选事件类型，统一由程序尽可能宽覆盖。
    m_controlPanel = new QWidget(this);
    QHBoxLayout* controlLayout = new QHBoxLayout(m_controlPanel);
    controlLayout->setContentsMargins(6, 6, 6, 6);
    controlLayout->setSpacing(6);

    QLabel* strategyLabel = new QLabel(
        QStringLiteral("策略：固定启用宽覆盖 ETW Provider，结合进程快照维护目标进程树，只保留与目标有关的事件。"),
        m_controlPanel);
    strategyLabel->setWordWrap(true);
    strategyLabel->setStyleSheet(QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
    controlLayout->addWidget(strategyLabel, 1);

    m_startButton = createIconButton(
        m_controlPanel,
        ":/Icon/process_start.svg",
        QStringLiteral("开始监控已选择的目标进程"));
    m_startButton->setStyleSheet(blueButtonStyle());
    controlLayout->addWidget(m_startButton, 0);

    m_stopButton = createIconButton(
        m_controlPanel,
        ":/Icon/process_terminate.svg",
        QStringLiteral("停止当前进程定向监控"));
    m_stopButton->setStyleSheet(blueButtonStyle());
    controlLayout->addWidget(m_stopButton, 0);

    m_pauseButton = createIconButton(
        m_controlPanel,
        ":/Icon/process_pause.svg",
        QStringLiteral("暂停处理与目标进程相关的事件"));
    m_pauseButton->setStyleSheet(blueButtonStyle());
    controlLayout->addWidget(m_pauseButton, 0);

    m_exportButton = createIconButton(
        m_controlPanel,
        ":/Icon/log_export.svg",
        QStringLiteral("导出当前事件表中可见的结果"));
    m_exportButton->setStyleSheet(blueButtonStyle());
    controlLayout->addWidget(m_exportButton, 0);

    m_statusLabel = new QLabel(QStringLiteral("● 空闲"), m_controlPanel);
    m_statusLabel->setStyleSheet(buildStatusStyle(monitorIdleColorHex()));
    controlLayout->addWidget(m_statusLabel, 0);

    m_rootLayout->addWidget(m_controlPanel, 0);

    // 事件筛选区：
    // - 类型单独做下拉框，其他字段使用文本过滤；
    // - 结果表刷新后统一走 applyEventFilter，便于维持交互一致性。
    m_filterPanel = new QWidget(this);
    QGridLayout* filterLayout = new QGridLayout(m_filterPanel);
    filterLayout->setContentsMargins(6, 6, 6, 6);
    filterLayout->setHorizontalSpacing(6);
    filterLayout->setVerticalSpacing(6);

    filterLayout->addWidget(new QLabel(QStringLiteral("类型"), m_filterPanel), 0, 0);
    m_eventTypeCombo = new QComboBox(m_filterPanel);
    m_eventTypeCombo->setStyleSheet(blueInputStyle());
    m_eventTypeCombo->addItems(QStringList{
        QStringLiteral("全部类型"),
        QStringLiteral("进程"),
        QStringLiteral("线程"),
        QStringLiteral("镜像"),
        QStringLiteral("文件"),
        QStringLiteral("注册表"),
        QStringLiteral("网络"),
        QStringLiteral("DNS"),
        QStringLiteral("PowerShell"),
        QStringLiteral("WMI"),
        QStringLiteral("计划任务"),
        QStringLiteral("安全审计"),
        QStringLiteral("Defender"),
        QStringLiteral("其他")
        });
    filterLayout->addWidget(m_eventTypeCombo, 0, 1);

    filterLayout->addWidget(new QLabel(QStringLiteral("Provider"), m_filterPanel), 0, 2);
    m_eventProviderFilterEdit = new QLineEdit(m_filterPanel);
    m_eventProviderFilterEdit->setPlaceholderText(QStringLiteral("Provider 名称"));
    m_eventProviderFilterEdit->setStyleSheet(blueInputStyle());
    filterLayout->addWidget(m_eventProviderFilterEdit, 0, 3);

    filterLayout->addWidget(new QLabel(QStringLiteral("进程"), m_filterPanel), 0, 4);
    m_eventProcessFilterEdit = new QLineEdit(m_filterPanel);
    m_eventProcessFilterEdit->setPlaceholderText(QStringLiteral("PID / 根 PID / 进程名 / 关系"));
    m_eventProcessFilterEdit->setStyleSheet(blueInputStyle());
    filterLayout->addWidget(m_eventProcessFilterEdit, 0, 5);

    filterLayout->addWidget(new QLabel(QStringLiteral("事件"), m_filterPanel), 0, 6);
    m_eventNameFilterEdit = new QLineEdit(m_filterPanel);
    m_eventNameFilterEdit->setPlaceholderText(QStringLiteral("事件名或事件 ID"));
    m_eventNameFilterEdit->setStyleSheet(blueInputStyle());
    filterLayout->addWidget(m_eventNameFilterEdit, 0, 7);

    filterLayout->addWidget(new QLabel(QStringLiteral("详情"), m_filterPanel), 1, 0);
    m_eventDetailFilterEdit = new QLineEdit(m_filterPanel);
    m_eventDetailFilterEdit->setPlaceholderText(QStringLiteral("属性详情关键字"));
    m_eventDetailFilterEdit->setStyleSheet(blueInputStyle());
    filterLayout->addWidget(m_eventDetailFilterEdit, 1, 1, 1, 3);

    filterLayout->addWidget(new QLabel(QStringLiteral("全字段"), m_filterPanel), 1, 4);
    m_eventGlobalFilterEdit = new QLineEdit(m_filterPanel);
    m_eventGlobalFilterEdit->setPlaceholderText(QStringLiteral("对整行文本做统一过滤"));
    m_eventGlobalFilterEdit->setStyleSheet(blueInputStyle());
    filterLayout->addWidget(m_eventGlobalFilterEdit, 1, 5, 1, 3);

    m_eventRegexCheck = new QCheckBox(QStringLiteral("正则"), m_filterPanel);
    m_eventCaseCheck = new QCheckBox(QStringLiteral("区分大小写"), m_filterPanel);
    m_eventInvertCheck = new QCheckBox(QStringLiteral("反向"), m_filterPanel);
    m_eventKeepBottomCheck = new QCheckBox(QStringLiteral("保持贴底"), m_filterPanel);
    m_eventKeepBottomCheck->setChecked(true);
    filterLayout->addWidget(m_eventRegexCheck, 2, 0);
    filterLayout->addWidget(m_eventCaseCheck, 2, 1);
    filterLayout->addWidget(m_eventInvertCheck, 2, 2);
    filterLayout->addWidget(m_eventKeepBottomCheck, 2, 3);

    m_eventClearFilterButton = createIconButton(
        m_filterPanel,
        ":/Icon/log_clear.svg",
        QStringLiteral("清空所有事件筛选条件"));
    m_eventClearFilterButton->setStyleSheet(blueButtonStyle());
    filterLayout->addWidget(m_eventClearFilterButton, 2, 4);

    m_eventFilterStatusLabel = new QLabel(QStringLiteral("筛选结果：0 / 0"), m_filterPanel);
    m_eventFilterStatusLabel->setStyleSheet(buildStatusStyle(monitorIdleColorHex()));
    filterLayout->addWidget(m_eventFilterStatusLabel, 2, 5, 1, 3);

    m_rootLayout->addWidget(m_filterPanel, 0);

    // ETW 时间轴：
    // - 高度固定 40px，宽度由根布局自动填满；
    // - 该控件只维护内部时间选区，不从图形点位反向推导事件集合；
    // - 框选结果与下方事件表已有后置筛选器叠加生效。
    m_eventTimelineWidget = new ProcessTraceTimelineWidget(this);
    m_eventTimelineWidget->setToolTip(QStringLiteral(
        "ETW 事件瀑布流时间轴：拖动矩形移动时间窗口；拖动左右边调整边界；滚轮向上放大窗口、向下缩小窗口。"));
    m_rootLayout->addWidget(m_eventTimelineWidget, 0);

    // 事件表：
    // - 单独保留类型、Provider、根 PID、关系等列，便于后续筛选；
    // - Detail 列尽量保存属性摘要，便于用户再做文本二次过滤。
    m_eventTable = new QTableWidget(this);
    m_eventTable->setColumnCount(EventColumnCount);
    m_eventTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("时间(100ns)"),
        QStringLiteral("类型"),
        QStringLiteral("Provider"),
        QStringLiteral("事件ID"),
        QStringLiteral("事件名"),
        QStringLiteral("PID / TID"),
        QStringLiteral("进程"),
        QStringLiteral("根PID"),
        QStringLiteral("关系"),
        QStringLiteral("详情"),
        QStringLiteral("ActivityId")
        });
    m_eventTable->setAlternatingRowColors(true);
    m_eventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_eventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_eventTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_eventTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_eventTable->verticalHeader()->setVisible(false);
    m_eventTable->horizontalHeader()->setStyleSheet(blueHeaderStyle());
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnTime100ns, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnType, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnProvider, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnEventId, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnEventName, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnPidTid, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnProcess, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnRootPid, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnRelation, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnDetail, QHeaderView::Stretch);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnActivityId, QHeaderView::ResizeToContents);
    m_rootLayout->addWidget(m_eventTable, 1);

    // 定时器：
    // - m_uiUpdateTimer：后台线程批量入队后，主线程按固定节奏刷入表格；
    // - m_runtimeRefreshTimer：运行中周期性补做进程快照，辅助维护进程树。
    m_uiUpdateTimer = new QTimer(this);
    m_uiUpdateTimer->setInterval(120);

    m_eventFilterDebounceTimer = new QTimer(this);
    m_eventFilterDebounceTimer->setInterval(180);
    m_eventFilterDebounceTimer->setSingleShot(true);

    m_runtimeRefreshTimer = new QTimer(this);
    m_runtimeRefreshTimer->setInterval(2000);

    updateActionState();
    updateStatusLabel();
}
