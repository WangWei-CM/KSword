#include "WinAPIDock.h"

// ============================================================
// WinAPIDock.Ui.cpp
// 作用：
// 1) 集中构建 WinAPI Dock 的控件层次与布局；
// 2) 保持头文件简洁，避免 UI 代码继续堆大；
// 3) 为按钮统一设置图标、tooltip 和蓝色主题样式。
// ============================================================

#include <QAbstractItemView>
#include <QCheckBox>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSize>
#include <QSplitter>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QStringList>
#include <QVBoxLayout>

namespace
{
    // configureIconButton：
    // - 作用：统一设置“仅图标按钮”的图标、提示与尺寸；
    // - 调用：刷新、浏览、开始、停止、导出等简单语义按钮都走这里。
    void configureIconButton(
        QPushButton* buttonPointer,
        const QIcon& iconValue,
        const QString& toolTipText)
    {
        if (buttonPointer == nullptr)
        {
            return;
        }

        buttonPointer->setIcon(iconValue);
        buttonPointer->setText(QString());
        buttonPointer->setToolTip(toolTipText);
        buttonPointer->setFixedWidth(34);
        buttonPointer->setIconSize(QSize(18, 18));
    }
}

void WinAPIDock::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(8, 8, 8, 8);
    m_rootLayout->setSpacing(8);

    m_topSplitter = new QSplitter(Qt::Horizontal, this);
    m_topSplitter->setChildrenCollapsible(false);
    m_rootLayout->addWidget(m_topSplitter, 0);

    m_processPanel = new QWidget(m_topSplitter);
    QVBoxLayout* processPanelLayout = new QVBoxLayout(m_processPanel);
    processPanelLayout->setContentsMargins(0, 0, 0, 0);
    processPanelLayout->setSpacing(6);

    QHBoxLayout* processControlLayout = new QHBoxLayout();
    processControlLayout->setSpacing(6);
    m_processFilterEdit = new QLineEdit(m_processPanel);
    m_processFilterEdit->setPlaceholderText(QStringLiteral("过滤 PID / 进程名 / 路径 / 用户"));
    m_processFilterEdit->setStyleSheet(blueInputStyle());

    m_processRefreshButton = new QPushButton(m_processPanel);
    configureIconButton(
        m_processRefreshButton,
        style()->standardIcon(QStyle::SP_BrowserReload),
        QStringLiteral("刷新进程列表"));
    m_processRefreshButton->setStyleSheet(blueButtonStyle());

    processControlLayout->addWidget(m_processFilterEdit, 1);
    processControlLayout->addWidget(m_processRefreshButton, 0);
    processPanelLayout->addLayout(processControlLayout);

    m_processStatusLabel = new QLabel(QStringLiteral("● 正在准备进程列表..."), m_processPanel);
    m_processStatusLabel->setStyleSheet(buildStatusStyle(monitorInfoColorHex()));
    processPanelLayout->addWidget(m_processStatusLabel, 0);

    m_processTable = new QTableWidget(m_processPanel);
    m_processTable->setColumnCount(ProcessColumnCount);
    m_processTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_processTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_processTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_processTable->setAlternatingRowColors(true);
    m_processTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_processTable->setHorizontalHeaderLabels(
        QStringList{ QStringLiteral("PID"), QStringLiteral("进程"), QStringLiteral("路径"), QStringLiteral("用户") });
    m_processTable->horizontalHeader()->setStyleSheet(blueHeaderStyle());
    m_processTable->horizontalHeader()->setStretchLastSection(false);
    m_processTable->horizontalHeader()->setSectionResizeMode(ProcessColumnPid, QHeaderView::ResizeToContents);
    m_processTable->horizontalHeader()->setSectionResizeMode(ProcessColumnName, QHeaderView::ResizeToContents);
    m_processTable->horizontalHeader()->setSectionResizeMode(ProcessColumnPath, QHeaderView::Stretch);
    m_processTable->horizontalHeader()->setSectionResizeMode(ProcessColumnUser, QHeaderView::ResizeToContents);
    m_processTable->setStyleSheet(blueInputStyle());
    processPanelLayout->addWidget(m_processTable, 1);

    m_sessionPanel = new QWidget(m_topSplitter);
    QVBoxLayout* sessionPanelLayout = new QVBoxLayout(m_sessionPanel);
    sessionPanelLayout->setContentsMargins(0, 0, 0, 0);
    sessionPanelLayout->setSpacing(8);

    QLabel* sessionTitleLabel = new QLabel(QStringLiteral("WinAPI Agent 会话"), m_sessionPanel);
    sessionTitleLabel->setStyleSheet(buildStatusStyle(monitorInfoColorHex()));
    sessionPanelLayout->addWidget(sessionTitleLabel, 0);

    QFormLayout* sessionFormLayout = new QFormLayout();
    sessionFormLayout->setContentsMargins(0, 0, 0, 0);
    sessionFormLayout->setHorizontalSpacing(8);
    sessionFormLayout->setVerticalSpacing(8);

    m_manualPidEdit = new QLineEdit(m_sessionPanel);
    m_manualPidEdit->setPlaceholderText(QStringLiteral("可留空；为空时取左侧选中进程"));
    m_manualPidEdit->setToolTip(QStringLiteral("优先使用这里输入的 PID；为空时使用左侧表格当前选中行。"));
    m_manualPidEdit->setStyleSheet(blueInputStyle());
    sessionFormLayout->addRow(QStringLiteral("目标 PID"), m_manualPidEdit);

    QWidget* dllPathRowWidget = new QWidget(m_sessionPanel);
    QHBoxLayout* dllPathLayout = new QHBoxLayout(dllPathRowWidget);
    dllPathLayout->setContentsMargins(0, 0, 0, 0);
    dllPathLayout->setSpacing(6);
    m_agentDllPathEdit = new QLineEdit(dllPathRowWidget);
    m_agentDllPathEdit->setText(defaultDllPathHint());
    m_agentDllPathEdit->setToolTip(QStringLiteral("需要注入到目标进程中的 APIMonitor_x64.dll 路径。"));
    m_agentDllPathEdit->setStyleSheet(blueInputStyle());

    m_browseAgentDllButton = new QPushButton(dllPathRowWidget);
    configureIconButton(
        m_browseAgentDllButton,
        style()->standardIcon(QStyle::SP_DirOpenIcon),
        QStringLiteral("浏览 Agent DLL 路径"));
    m_browseAgentDllButton->setStyleSheet(blueButtonStyle());

    dllPathLayout->addWidget(m_agentDllPathEdit, 1);
    dllPathLayout->addWidget(m_browseAgentDllButton, 0);
    sessionFormLayout->addRow(QStringLiteral("Agent DLL"), dllPathRowWidget);
    sessionPanelLayout->addLayout(sessionFormLayout);

    QFrame* categoryFrame = new QFrame(m_sessionPanel);
    categoryFrame->setFrameShape(QFrame::StyledPanel);
    QVBoxLayout* categoryLayout = new QVBoxLayout(categoryFrame);
    categoryLayout->setContentsMargins(8, 8, 8, 8);
    categoryLayout->setSpacing(6);

    QLabel* categoryTitleLabel = new QLabel(QStringLiteral("Hook 分类"), categoryFrame);
    categoryLayout->addWidget(categoryTitleLabel, 0);

    m_hookFileCheck = new QCheckBox(QStringLiteral("文件 API"), categoryFrame);
    m_hookRegistryCheck = new QCheckBox(QStringLiteral("注册表 API"), categoryFrame);
    m_hookNetworkCheck = new QCheckBox(QStringLiteral("网络 API"), categoryFrame);
    m_hookProcessCheck = new QCheckBox(QStringLiteral("进程 API"), categoryFrame);
    m_hookLoaderCheck = new QCheckBox(QStringLiteral("加载器 API"), categoryFrame);

    m_hookFileCheck->setChecked(true);
    m_hookRegistryCheck->setChecked(true);
    m_hookNetworkCheck->setChecked(true);
    m_hookProcessCheck->setChecked(true);
    m_hookLoaderCheck->setChecked(false);

    m_hookFileCheck->setToolTip(QStringLiteral("CreateFileW / ReadFile / WriteFile 等文件访问相关 API。"));
    m_hookRegistryCheck->setToolTip(QStringLiteral("RegOpenKeyExW / RegSetValueExW 等注册表相关 API。"));
    m_hookNetworkCheck->setToolTip(QStringLiteral("connect / send / recv 等网络相关 API。"));
    m_hookProcessCheck->setToolTip(QStringLiteral("CreateProcessW 等进程控制相关 API。"));
    m_hookLoaderCheck->setToolTip(QStringLiteral("LoadLibraryW / LoadLibraryExW 等模块加载相关 API。该类 Hook 对 GUI 进程稳定性风险更高，默认关闭。"));

    categoryLayout->addWidget(m_hookFileCheck, 0);
    categoryLayout->addWidget(m_hookRegistryCheck, 0);
    categoryLayout->addWidget(m_hookNetworkCheck, 0);
    categoryLayout->addWidget(m_hookProcessCheck, 0);
    categoryLayout->addWidget(m_hookLoaderCheck, 0);
    sessionPanelLayout->addWidget(categoryFrame, 0);

    QHBoxLayout* sessionButtonLayout = new QHBoxLayout();
    sessionButtonLayout->setSpacing(6);

    m_startButton = new QPushButton(m_sessionPanel);
    configureIconButton(
        m_startButton,
        style()->standardIcon(QStyle::SP_MediaPlay),
        QStringLiteral("启动 WinAPI 监控"));
    m_startButton->setStyleSheet(blueButtonStyle());

    m_stopButton = new QPushButton(m_sessionPanel);
    configureIconButton(
        m_stopButton,
        style()->standardIcon(QStyle::SP_MediaStop),
        QStringLiteral("停止 WinAPI 监控"));
    m_stopButton->setStyleSheet(blueButtonStyle());

    m_terminateHookButton = new QPushButton(m_sessionPanel);
    configureIconButton(
        m_terminateHookButton,
        style()->standardIcon(QStyle::SP_BrowserStop),
        QStringLiteral("手动终止目标进程中的 Hook"));
    m_terminateHookButton->setStyleSheet(blueButtonStyle());

    m_exportButton = new QPushButton(m_sessionPanel);
    configureIconButton(
        m_exportButton,
        style()->standardIcon(QStyle::SP_DialogSaveButton),
        QStringLiteral("导出当前可见事件为 TSV"));
    m_exportButton->setStyleSheet(blueButtonStyle());

    m_clearEventButton = new QPushButton(m_sessionPanel);
    configureIconButton(
        m_clearEventButton,
        style()->standardIcon(QStyle::SP_DialogResetButton),
        QStringLiteral("清空当前事件表"));
    m_clearEventButton->setStyleSheet(blueButtonStyle());

    sessionButtonLayout->addWidget(m_startButton, 0);
    sessionButtonLayout->addWidget(m_stopButton, 0);
    sessionButtonLayout->addWidget(m_terminateHookButton, 0);
    sessionButtonLayout->addWidget(m_exportButton, 0);
    sessionButtonLayout->addWidget(m_clearEventButton, 0);
    sessionButtonLayout->addStretch(1);
    sessionPanelLayout->addLayout(sessionButtonLayout);

    m_sessionStatusLabel = new QLabel(QStringLiteral("● 空闲"), m_sessionPanel);
    m_sessionStatusLabel->setStyleSheet(buildStatusStyle(monitorIdleColorHex()));
    sessionPanelLayout->addWidget(m_sessionStatusLabel, 0);
    sessionPanelLayout->addStretch(1);

    m_filterPanel = new QWidget(this);
    QHBoxLayout* filterLayout = new QHBoxLayout(m_filterPanel);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(6);

    m_eventFilterEdit = new QLineEdit(m_filterPanel);
    m_eventFilterEdit->setPlaceholderText(QStringLiteral("过滤 API / 分类 / 结果 / 详情"));
    m_eventFilterEdit->setStyleSheet(blueInputStyle());

    m_eventFilterClearButton = new QPushButton(m_filterPanel);
    configureIconButton(
        m_eventFilterClearButton,
        style()->standardIcon(QStyle::SP_DialogResetButton),
        QStringLiteral("清空事件过滤条件"));
    m_eventFilterClearButton->setStyleSheet(blueButtonStyle());

    m_eventKeepBottomCheck = new QCheckBox(QStringLiteral("保持贴底"), m_filterPanel);
    m_eventKeepBottomCheck->setChecked(true);
    m_eventKeepBottomCheck->setToolTip(QStringLiteral("新事件到来时自动滚动到最底部。"));

    m_eventFilterStatusLabel = new QLabel(QStringLiteral("筛选结果：0 / 0"), m_filterPanel);
    m_eventFilterStatusLabel->setStyleSheet(buildStatusStyle(monitorIdleColorHex()));

    filterLayout->addWidget(m_eventFilterEdit, 1);
    filterLayout->addWidget(m_eventFilterClearButton, 0);
    filterLayout->addWidget(m_eventKeepBottomCheck, 0);
    filterLayout->addWidget(m_eventFilterStatusLabel, 0);
    m_rootLayout->addWidget(m_filterPanel, 0);

    m_eventTable = new QTableWidget(this);
    m_eventTable->setColumnCount(EventColumnCount);
    m_eventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_eventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_eventTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_eventTable->setAlternatingRowColors(true);
    m_eventTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_eventTable->setHorizontalHeaderLabels(
        QStringList{
            QStringLiteral("时间(100ns)"),
            QStringLiteral("分类"),
            QStringLiteral("API"),
            QStringLiteral("结果"),
            QStringLiteral("PID/TID"),
            QStringLiteral("详情")
        });
    m_eventTable->horizontalHeader()->setStyleSheet(blueHeaderStyle());
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnTime100ns, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnCategory, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnApi, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnResult, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnPidTid, QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(EventColumnDetail, QHeaderView::Stretch);
    m_eventTable->setStyleSheet(blueInputStyle());
    m_rootLayout->addWidget(m_eventTable, 1);

    m_uiFlushTimer = new QTimer(this);
    m_uiFlushTimer->setInterval(120);

    m_topSplitter->setStretchFactor(0, 3);
    m_topSplitter->setStretchFactor(1, 2);
}
