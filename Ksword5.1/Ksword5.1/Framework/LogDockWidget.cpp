#include "LogDockWidget.h"
#include "../theme.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QSize>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QSvgRenderer>

// Windows 下优先使用原生 shell 保存对话框（满足需求中的 explorer 场景）。
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <commdlg.h>
#pragma comment(lib, "Comdlg32.lib")
#endif

namespace
{
    // 表格列常量：确保同一语义在代码中统一引用。
    constexpr int LevelColumn = 0;      // 等级列（仅彩色方块，标题留空）。
    constexpr int TimeColumn = 1;       // 时间列。
    constexpr int ContentColumn = 2;    // 内容列。
    constexpr int FileColumn = 3;       // 文件列。
    constexpr int FunctionColumn = 4;   // 函数列。
    constexpr int TotalColumns = 5;     // 列总数。

    // 统一的资源路径常量，避免硬编码散落。
    constexpr const char* IconExportPath = ":/Icon/log_export.svg";
    constexpr const char* IconClearPath = ":/Icon/log_clear.svg";
    constexpr const char* IconCopyPath = ":/Icon/log_copy.svg";
    constexpr const char* IconClipboardPath = ":/Icon/log_clipboard.svg";
    constexpr const char* IconTrackPath = ":/Icon/log_track.svg";
    constexpr const char* IconCancelTrackPath = ":/Icon/log_cancel_track.svg";

    // DefaultButtonIconSize：日志面板按钮与菜单图标默认尺寸。
    constexpr QSize DefaultButtonIconSize(16, 16);

    // createBlueThemedIcon 作用：
    // - 把 qrc 中的单色 SVG 图标重新着色为主题蓝色；
    // - 避免修改原始 SVG 文件，实现运行时统一换色。
    // 参数 resourcePath：qrc 资源路径（如 :/Icon/log_copy.svg）。
    // 参数 iconSize：输出图标尺寸。
    // 返回值：重着色后的 QIcon；若渲染失败则回退原图标。
    QIcon createBlueThemedIcon(const char* resourcePath, const QSize& iconSize = DefaultButtonIconSize)
    {
        const QString iconPath = QString::fromUtf8(resourcePath);

        // 使用 Qt SVG 渲染器把矢量图绘制到透明像素缓冲。
        QSvgRenderer svgRenderer(iconPath);
        if (!svgRenderer.isValid())
        {
            // 资源异常时回退默认图标，避免功能不可见。
            return QIcon(iconPath);
        }

        QPixmap tintedPixmap(iconSize);
        tintedPixmap.fill(Qt::transparent);

        // 第一步先渲染原 SVG；第二步用 SourceIn 把非透明像素统一染成主题蓝。
        QPainter painter(&tintedPixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        svgRenderer.render(&painter, QRectF(0, 0, iconSize.width(), iconSize.height()));
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(tintedPixmap.rect(), KswordTheme::PrimaryBlueColor);
        painter.end();

        return QIcon(tintedPixmap);
    }

    // buildBlueCheckBoxStyleSheet 作用：
    // - 生成日志面板统一复选框蓝色样式（文字/边框/选中态）。
    // 返回值：QSS 字符串。
    QString buildBlueCheckBoxStyleSheet()
    {
        return QStringLiteral(
            "QCheckBox {"
            "  color: %4;"
            "  spacing: 6px;"
            "}"
            "QCheckBox::indicator {"
            "  width: 14px;"
            "  height: 14px;"
            "  border: 1px solid %2;"
            "  border-radius: 2px;"
            "  background: %5;"
            "}"
            "QCheckBox::indicator:checked {"
            "  border: 1px solid %2;"
            "  background: %3;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::SurfaceHex());
    }

    // buildBlueButtonStyleSheet 作用：
    // - 生成日志面板按钮蓝色边框与交互样式。
    // 返回值：QSS 字符串。
    QString buildBlueButtonStyleSheet()
    {
        return QStringLiteral(
            "QPushButton {"
            "  color: #FFFFFF;"
            "  background: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  padding: 4px 12px;"
            "}"
            "QPushButton:hover {"
            "  background: %3;"
            "  color: #FFFFFF;"
            "  border: 1px solid %3;"
            "}"
            "QPushButton:pressed {"
            "  background: %4;"
            "  color: #FFFFFF;"
            "  border: 1px solid %4;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(QStringLiteral("#2E8BFF"))
            .arg(KswordTheme::PrimaryBluePressedHex);
    }

    // buildBlueHeaderStyleSheet 作用：
    // - 统一日志表格头部文本为主题蓝色。
    // 返回值：QHeaderView section 样式字符串。
    QString buildBlueHeaderStyleSheet()
    {
        return QStringLiteral(
            "QHeaderView::section {"
            "  color: %1;"
            "  background: %2;"
            "  border: 1px solid %3;"
            "  padding: 4px;"
            "  font-weight: 600;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }
}

LogDockWidget::LogDockWidget(QWidget* parent)
    : QWidget(parent)
{
    // 构造阶段按照“UI -> 信号 -> 刷新器”的顺序初始化，便于维护。
    initializeUi();
    initializeConnections();
    initializeRefreshTimer();

    // 首次显示前先强制刷新一次，确保打开 Dock 即可看到已有日志。
    refreshTableFromManager(true);
}

void LogDockWidget::initializeUi()
{
    // 根布局：上下排列三层（筛选行、按钮行、表格）。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(6);

    // 第一行：五个等级复选框，默认全部勾选。
    m_filterLayout = new QHBoxLayout();
    m_filterLayout->setContentsMargins(6, 6, 6, 0);
    m_filterLayout->setSpacing(12);

    m_debugCheck = new QCheckBox("Debug", this);
    m_infoCheck = new QCheckBox("Info", this);
    m_warnCheck = new QCheckBox("Warn", this);
    m_errorCheck = new QCheckBox("Error", this);
    m_fatalCheck = new QCheckBox("Fatal", this);

    m_debugCheck->setChecked(true);
    m_infoCheck->setChecked(true);
    m_warnCheck->setChecked(true);
    m_errorCheck->setChecked(true);
    m_fatalCheck->setChecked(true);

    m_filterLayout->addWidget(m_debugCheck);
    m_filterLayout->addWidget(m_infoCheck);
    m_filterLayout->addWidget(m_warnCheck);
    m_filterLayout->addWidget(m_errorCheck);
    m_filterLayout->addWidget(m_fatalCheck);
    m_filterLayout->addStretch(1);

    // 第二行：操作按钮 + 自动滚动复选框。
    m_actionLayout = new QHBoxLayout();
    m_actionLayout->setContentsMargins(6, 0, 6, 0);
    m_actionLayout->setSpacing(8);

    m_exportButton = new QPushButton(createBlueThemedIcon(IconExportPath), "导出日志", this);
    m_clearButton = new QPushButton(createBlueThemedIcon(IconClearPath), "清空日志列表", this);
    m_copyVisibleButton = new QPushButton(createBlueThemedIcon(IconCopyPath), "复制可见", this);
    m_autoScrollCheck = new QCheckBox("保持滚动到最底端", this);

    // 按钮均设置图标与提示，满足“尽量用图标 + 悬停说明功能”。
    m_exportButton->setToolTip("导出全部日志到 .txt 文件");
    m_clearButton->setToolTip("清空日志管理器中的全部日志（双重确认）");
    m_copyVisibleButton->setToolTip("复制当前可见列表内容（支持追踪过滤状态）");
    m_autoScrollCheck->setToolTip("开启后表格刷新将自动滚动到最后一行");

    // 图标尺寸统一，保证按钮视觉一致。
    m_exportButton->setIconSize(DefaultButtonIconSize);
    m_clearButton->setIconSize(DefaultButtonIconSize);
    m_copyVisibleButton->setIconSize(DefaultButtonIconSize);
    m_autoScrollCheck->setChecked(true);

    // 把日志区按钮边框统一改为欢迎页同款蓝色。
    const QString blueButtonStyle = buildBlueButtonStyleSheet();
    m_exportButton->setStyleSheet(blueButtonStyle);
    m_clearButton->setStyleSheet(blueButtonStyle);
    m_copyVisibleButton->setStyleSheet(blueButtonStyle);

    // 把复选框文字与指示器统一改为欢迎页同款蓝色。
    const QString blueCheckBoxStyle = buildBlueCheckBoxStyleSheet();
    m_debugCheck->setStyleSheet(blueCheckBoxStyle);
    m_infoCheck->setStyleSheet(blueCheckBoxStyle);
    m_warnCheck->setStyleSheet(blueCheckBoxStyle);
    m_errorCheck->setStyleSheet(blueCheckBoxStyle);
    m_fatalCheck->setStyleSheet(blueCheckBoxStyle);
    m_autoScrollCheck->setStyleSheet(blueCheckBoxStyle);

    m_actionLayout->addWidget(m_exportButton);
    m_actionLayout->addWidget(m_clearButton);
    m_actionLayout->addWidget(m_copyVisibleButton);
    m_actionLayout->addStretch(1);
    m_actionLayout->addWidget(m_autoScrollCheck);

    // 第三层：日志表格，要求不可编辑且占满 Dock。
    m_logTable = new QTableWidget(this);
    m_logTable->setColumnCount(TotalColumns);
    m_logTable->setHorizontalHeaderLabels(QStringList() << "" << "时间" << "内容" << "文件" << "函数");
    m_logTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_logTable->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_logTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_logTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_logTable->setWordWrap(false);

    // 表格头部策略：等级列固定宽度，其余列可交互拖动。
    QHeaderView* horizontalHeader = m_logTable->horizontalHeader();
    horizontalHeader->setSectionResizeMode(LevelColumn, QHeaderView::Fixed);
    horizontalHeader->setSectionResizeMode(TimeColumn, QHeaderView::Interactive);
    horizontalHeader->setSectionResizeMode(ContentColumn, QHeaderView::Stretch);
    horizontalHeader->setSectionResizeMode(FileColumn, QHeaderView::Interactive);
    horizontalHeader->setSectionResizeMode(FunctionColumn, QHeaderView::Interactive);
    horizontalHeader->setStretchLastSection(false);
    horizontalHeader->setStyleSheet(buildBlueHeaderStyleSheet());

    // 等级列只容纳彩色方块，因此锁定窄列宽。
    m_logTable->setColumnWidth(LevelColumn, 24);
    m_logTable->setColumnWidth(TimeColumn, 150);
    m_logTable->setColumnWidth(ContentColumn, 320);
    m_logTable->setColumnWidth(FileColumn, 240);
    m_logTable->setColumnWidth(FunctionColumn, 320);
    m_logTable->verticalHeader()->setVisible(false);

    // 默认隐藏“文件/函数”两列，减少首屏信息密度。
    m_logTable->setColumnHidden(FileColumn, true);
    m_logTable->setColumnHidden(FunctionColumn, true);

    // 最后拼装三层布局。
    m_rootLayout->addLayout(m_filterLayout);
    m_rootLayout->addLayout(m_actionLayout);
    m_rootLayout->addWidget(m_logTable, 1);
}

void LogDockWidget::initializeConnections()
{
    // 五个等级复选框任意变化时都强制刷新表格。
    connect(m_debugCheck, &QCheckBox::toggled, this, [this]() { refreshTableFromManager(true); });
    connect(m_infoCheck, &QCheckBox::toggled, this, [this]() { refreshTableFromManager(true); });
    connect(m_warnCheck, &QCheckBox::toggled, this, [this]() { refreshTableFromManager(true); });
    connect(m_errorCheck, &QCheckBox::toggled, this, [this]() { refreshTableFromManager(true); });
    connect(m_fatalCheck, &QCheckBox::toggled, this, [this]() { refreshTableFromManager(true); });

    // 按钮动作连接：导出/清空/复制可见。
    connect(m_exportButton, &QPushButton::clicked, this, [this]() { exportAllLogs(); });
    connect(m_clearButton, &QPushButton::clicked, this, [this]() { clearAllLogsWithDoubleConfirm(); });
    connect(m_copyVisibleButton, &QPushButton::clicked, this, [this]() { copyVisibleRows(); });

    // 右键菜单连接。
    connect(m_logTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        showTableContextMenu(position);
    });
}

void LogDockWidget::initializeRefreshTimer()
{
    // 周期刷新间隔 250ms：实时性与性能折中。
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(250);

    // 非强制刷新：仅 revision 变化时重建 UI。
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        refreshTableFromManager(false);
    });
    m_refreshTimer->start();
}

void LogDockWidget::refreshTableFromManager(const bool forceRefresh)
{
    const std::size_t currentRevision = KswordARKEventEntry.Revision();
    if (!forceRefresh && currentRevision == m_lastRevision)
    {
        return;
    }
    m_lastRevision = currentRevision;

    // 从全局日志管理器读取完整快照，然后在 UI 层应用筛选。
    const std::vector<kEvent> allEvents = KswordARKEventEntry.Snapshot();
    std::vector<kEvent> filteredEvents;
    filteredEvents.reserve(allEvents.size());

    for (const kEvent& singleEvent : allEvents)
    {
        // 追踪模式开启时，仅显示同 GUID 事件。
        if (m_isTracking && !IsSameGuid(singleEvent.guid, m_trackingGuid))
        {
            continue;
        }

        // 等级筛选由复选框控制。
        if (!isLevelEnabledByCheckbox(singleEvent.level))
        {
            continue;
        }

        filteredEvents.push_back(singleEvent);
    }

    // 记录当前可见事件列表，供复制与追踪逻辑复用。
    m_visibleEvents = filteredEvents;
    rebuildTable(filteredEvents);
}

void LogDockWidget::rebuildTable(const std::vector<kEvent>& filteredEvents)
{
    // 若未开启自动滚动，先记住滚动条位置，刷新后恢复。
    const int previousScrollValue = m_logTable->verticalScrollBar()->value();

    m_logTable->setRowCount(static_cast<int>(filteredEvents.size()));

    for (int row = 0; row < static_cast<int>(filteredEvents.size()); ++row)
    {
        const kEvent& logItem = filteredEvents[static_cast<std::size_t>(row)];

        // 等级列：仅放置彩色方块，不放文字，列标题也为空。
        auto* levelItem = new QTableWidgetItem();
        levelItem->setIcon(makeLevelSquareIcon(getLevelColor(logItem.level)));
        levelItem->setTextAlignment(Qt::AlignCenter);
        levelItem->setToolTip(getLevelText(logItem.level));
        m_logTable->setItem(row, LevelColumn, levelItem);

        // 时间列：显示“YYYY-MM-DD HH:MM:SS”。
        auto* timeItem = new QTableWidgetItem(QString::fromStdString(FormatTimeToString(logItem.timestamp)));
        timeItem->setTextAlignment(Qt::AlignCenter);
        m_logTable->setItem(row, TimeColumn, timeItem);

        // 内容列：显示日志正文。
        auto* contentItem = new QTableWidgetItem(QString::fromStdString(logItem.content));
        contentItem->setToolTip(QString::fromStdString(logItem.content));
        m_logTable->setItem(row, ContentColumn, contentItem);

        // 文件列：显示文件路径 + 行号。
        auto* fileItem = new QTableWidgetItem(QString::fromStdString(logItem.fileLocation));
        fileItem->setToolTip(QString::fromStdString(logItem.fileLocation));
        m_logTable->setItem(row, FileColumn, fileItem);

        // 函数列：显示函数签名。
        auto* functionItem = new QTableWidgetItem(QString::fromStdString(logItem.functionName));
        functionItem->setToolTip(QString::fromStdString(logItem.functionName));
        m_logTable->setItem(row, FunctionColumn, functionItem);

        // 按等级着色整行（Error/Fatal 有特殊底色要求）。
        applyRowStyle(row, logItem);
        m_logTable->setRowHeight(row, 22);
    }

    // 根据“保持滚动到底端”开关决定刷新后的滚动行为。
    if (m_autoScrollCheck->isChecked())
    {
        m_logTable->scrollToBottom();
    }
    else
    {
        m_logTable->verticalScrollBar()->setValue(previousScrollValue);
    }
}

void LogDockWidget::applyRowStyle(const int row, const kEvent& logItem)
{
    // 默认情况下不改变行配色，仅 Error/Fatal 特殊着色。
    QColor rowBackground;
    QColor rowForeground;
    bool shouldApply = false;

    if (logItem.level == kLogLevel::Fatal)
    {
        rowBackground = QColor(0, 0, 0);         // Fatal 整行黑底
        rowForeground = QColor(255, 255, 255);   // Fatal 整行白字
        shouldApply = true;
    }
    else if (logItem.level == kLogLevel::Error)
    {
        rowBackground = KswordTheme::IsDarkModeEnabled()
            ? QColor(138, 48, 48)
            : QColor(220, 72, 72);     // Error 深浅色分支红底
        rowForeground = QColor(255, 255, 255);   // Error 统一白字保证可读
        shouldApply = true;
    }

    if (!shouldApply)
    {
        return;
    }

    // 对本行每个单元格统一应用前景与背景色。
    for (int column = 0; column < TotalColumns; ++column)
    {
        QTableWidgetItem* item = m_logTable->item(row, column);
        if (item != nullptr)
        {
            item->setBackground(rowBackground);
            item->setForeground(rowForeground);
        }
    }
}

QIcon LogDockWidget::makeLevelSquareIcon(const QColor& color) const
{
    // 使用 QPainter 在透明底图上绘制纯色小方块。
    QPixmap squarePixmap(12, 12);
    squarePixmap.fill(Qt::transparent);

    QPainter painter(&squarePixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRect(1, 1, 10, 10);

    return QIcon(squarePixmap);
}

QColor LogDockWidget::getLevelColor(const kLogLevel level) const
{
    switch (level)
    {
    case kLogLevel::Debug:
        return QColor(52, 127, 235);
    case kLogLevel::Info:
        return KswordTheme::IsDarkModeEnabled() ? QColor(104, 204, 116) : QColor(64, 173, 74);
    case kLogLevel::Warn:
        return KswordTheme::IsDarkModeEnabled() ? QColor(245, 196, 94) : QColor(232, 176, 42);
    case kLogLevel::Error:
        return KswordTheme::IsDarkModeEnabled() ? QColor(228, 120, 120) : QColor(214, 66, 66);
    case kLogLevel::Fatal:
        return KswordTheme::IsDarkModeEnabled() ? QColor(200, 80, 80) : QColor(170, 20, 20);
    default:
        return QColor(128, 128, 128);
    }
}

QString LogDockWidget::getLevelText(const kLogLevel level) const
{
    return QString::fromStdString(LogLevelToString(level));
}

bool LogDockWidget::isLevelEnabledByCheckbox(const kLogLevel level) const
{
    switch (level)
    {
    case kLogLevel::Debug:
        return m_debugCheck->isChecked();
    case kLogLevel::Info:
        return m_infoCheck->isChecked();
    case kLogLevel::Warn:
        return m_warnCheck->isChecked();
    case kLogLevel::Error:
        return m_errorCheck->isChecked();
    case kLogLevel::Fatal:
        return m_fatalCheck->isChecked();
    default:
        return true;
    }
}

void LogDockWidget::showTableContextMenu(const QPoint& position)
{
    // 通过右键位置定位行列，若点在空白区域 row/column 为 -1。
    int row = -1;
    int column = -1;
    if (QTableWidgetItem* clickedItem = m_logTable->itemAt(position))
    {
        row = clickedItem->row();
        column = clickedItem->column();
    }

    QMenu contextMenu(this);
    QAction* copyCellAction = contextMenu.addAction(createBlueThemedIcon(IconCopyPath), "复制单元格");
    QAction* copyRowAction = contextMenu.addAction(createBlueThemedIcon(IconClipboardPath), "复制行");

    // 若未点中有效单元格，则复制操作不可用。
    const bool hasValidCell = row >= 0 && column >= 0 && row < static_cast<int>(m_visibleEvents.size());
    copyCellAction->setEnabled(hasValidCell);
    copyRowAction->setEnabled(hasValidCell);

    // 第三个动作按状态切换为“跟踪事件”或“取消追踪”。
    QAction* trackAction = nullptr;
    if (m_isTracking)
    {
        trackAction = contextMenu.addAction(createBlueThemedIcon(IconCancelTrackPath), "取消追踪");
    }
    else
    {
        trackAction = contextMenu.addAction(createBlueThemedIcon(IconTrackPath), "跟踪事件");
        trackAction->setEnabled(hasValidCell);
    }

    QAction* selectedAction = contextMenu.exec(m_logTable->viewport()->mapToGlobal(position));
    if (selectedAction == nullptr)
    {
        return;
    }

    if (selectedAction == copyCellAction)
    {
        copySingleCell(row, column);
        return;
    }
    if (selectedAction == copyRowAction)
    {
        copySingleRow(row);
        return;
    }
    if (selectedAction == trackAction)
    {
        if (m_isTracking)
        {
            cancelTracking();
        }
        else
        {
            startTrackingByRow(row);
        }
    }
}

void LogDockWidget::copySingleCell(const int row, const int column)
{
    if (row < 0 || column < 0 || row >= static_cast<int>(m_visibleEvents.size()))
    {
        return;
    }

    QString textToCopy;
    if (column == LevelColumn)
    {
        // 等级列视觉上只有彩色方块，复制时补充等级文本。
        textToCopy = getLevelText(m_visibleEvents[static_cast<std::size_t>(row)].level);
    }
    else if (QTableWidgetItem* item = m_logTable->item(row, column))
    {
        textToCopy = item->text();
    }

    QApplication::clipboard()->setText(textToCopy);
}

void LogDockWidget::copySingleRow(const int row)
{
    if (row < 0 || row >= static_cast<int>(m_visibleEvents.size()))
    {
        return;
    }

    // 行复制使用可读文本：等级/时间/内容/文件/函数。
    const kEvent& logItem = m_visibleEvents[static_cast<std::size_t>(row)];
    const QString rowText =
        getLevelText(logItem.level) + "\t" +
        QString::fromStdString(FormatTimeToString(logItem.timestamp)) + "\t" +
        QString::fromStdString(logItem.content) + "\t" +
        QString::fromStdString(logItem.fileLocation) + "\t" +
        QString::fromStdString(logItem.functionName);

    QApplication::clipboard()->setText(rowText);
}

void LogDockWidget::copyVisibleRows()
{
    QStringList lines;
    lines.reserve(static_cast<int>(m_visibleEvents.size()));

    // 遍历当前可见事件列表，保证与屏幕展示一致（含追踪/筛选状态）。
    for (const kEvent& logItem : m_visibleEvents)
    {
        const QString lineText =
            getLevelText(logItem.level) + "\t" +
            QString::fromStdString(FormatTimeToString(logItem.timestamp)) + "\t" +
            QString::fromStdString(logItem.content) + "\t" +
            QString::fromStdString(logItem.fileLocation) + "\t" +
            QString::fromStdString(logItem.functionName);
        lines.push_back(lineText);
    }

    QApplication::clipboard()->setText(lines.join("\n"));
}

void LogDockWidget::startTrackingByRow(const int row)
{
    if (row < 0 || row >= static_cast<int>(m_visibleEvents.size()))
    {
        return;
    }

    // 读取目标行 GUID 并进入追踪状态。
    m_trackingGuid = m_visibleEvents[static_cast<std::size_t>(row)].guid;
    m_isTracking = true;
    refreshTableFromManager(true);
}

void LogDockWidget::cancelTracking()
{
    // 退出追踪后回到“仅等级筛选”的常规视图。
    m_isTracking = false;
    m_trackingGuid = GUID{};
    refreshTableFromManager(true);
}

void LogDockWidget::exportAllLogs()
{
    QString outputPath = chooseExportPath();
    if (outputPath.isEmpty())
    {
        return;
    }

    // 统一保证后缀为 .txt。
    if (!outputPath.endsWith(".txt", Qt::CaseInsensitive))
    {
        outputPath += ".txt";
    }

    // 使用 UTF-8 字符串传给 Save，兼容中文路径。
    const bool saveOk = KswordARKEventEntry.Save(outputPath.toUtf8().toStdString());
    if (!saveOk)
    {
        QMessageBox::critical(this, "导出失败", "日志文件写入失败，请检查路径和权限。");
        return;
    }

    QMessageBox::information(this, "导出成功", "日志导出完成。");
}

QString LogDockWidget::chooseExportPath()
{
#ifdef _WIN32
    // Windows：优先调用原生 shell 保存对话框。
    wchar_t fileBuffer[MAX_PATH] = L"KswordARK_Log.txt";
    OPENFILENAMEW saveDialog{};
    saveDialog.lStructSize = sizeof(OPENFILENAMEW);
    saveDialog.hwndOwner = reinterpret_cast<HWND>(this->winId());
    saveDialog.lpstrFilter = L"Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    saveDialog.lpstrFile = fileBuffer;
    saveDialog.nMaxFile = MAX_PATH;
    saveDialog.lpstrDefExt = L"txt";
    saveDialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

    if (::GetSaveFileNameW(&saveDialog) != FALSE)
    {
        return QString::fromWCharArray(fileBuffer).trimmed();
    }

    // 只有在 CommDlgExtendedError != 0 时认为对话框打开失败（而非用户取消）。
    const DWORD shellErrorCode = ::CommDlgExtendedError();
    if (shellErrorCode != 0)
    {
        bool inputOk = false;
        const QString manualPath = QInputDialog::getText(
            this,
            "Shell 打开失败",
            "检测到原生保存对话框调用失败，请手动输入导出路径：",
            QLineEdit::Normal,
            "KswordARK_Log.txt",
            &inputOk).trimmed();

        if (inputOk)
        {
            return manualPath;
        }
    }
    return QString();
#else
    // 非 Windows 平台：使用 Qt 原生接口。
    bool inputOk = false;
    const QString manualPath = QInputDialog::getText(
        this,
        "导出日志",
        "请输入导出路径：",
        QLineEdit::Normal,
        "KswordARK_Log.txt",
        &inputOk).trimmed();
    if (inputOk)
    {
        return manualPath;
    }
    return QString();
#endif
}

void LogDockWidget::clearAllLogsWithDoubleConfirm()
{
    // 第一次确认：常规确认。
    const QMessageBox::StandardButton firstConfirm = QMessageBox::question(
        this,
        "确认清空",
        "确定要清空日志列表吗？",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (firstConfirm != QMessageBox::Yes)
    {
        return;
    }

    // 第二次确认：不可恢复提示。
    const QMessageBox::StandardButton secondConfirm = QMessageBox::warning(
        this,
        "二次确认",
        "该操作不可恢复，是否继续清空全部日志？",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (secondConfirm != QMessageBox::Yes)
    {
        return;
    }

    // 真正执行清空，并退出追踪状态避免空追踪锁死视图。
    KswordARKEventEntry.clear();
    m_isTracking = false;
    m_trackingGuid = GUID{};
    refreshTableFromManager(true);
}
