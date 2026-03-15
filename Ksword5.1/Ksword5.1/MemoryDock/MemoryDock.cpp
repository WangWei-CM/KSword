#include "MemoryDock.h"

#include "../theme.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QPointer>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThreadPool>
#include <QTimer>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>

// Win32 API 头文件：进程枚举、模块枚举、内存遍历、读写内存全部来自这些头。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>

// 读取进程内存和映射文件路径需要链接 Psapi。
#pragma comment(lib, "Psapi.lib")

namespace
{
    // ========================================================
    // 主题样式函数：统一按钮/输入框/下拉框风格。
    // ========================================================

    QString buildBlueButtonStyle()
    {
        return QStringLiteral(
            "QPushButton {"
            "  color: %1;"
            "  background: #FFFFFF;"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  padding: 4px 10px;"
            "}"
            "QPushButton:hover {"
            "  background: %3;"
            "}"
            "QPushButton:pressed {"
            "  background: %4;"
            "  color: #FFFFFF;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueHoverHex)
            .arg(KswordTheme::PrimaryBluePressedHex);
    }

    QString buildBlueComboStyle()
    {
        return QStringLiteral(
            "QComboBox {"
            "  border: 1px solid %1;"
            "  border-radius: 3px;"
            "  padding: 2px 6px;"
            "  background: #FFFFFF;"
            "}"
            "QComboBox:hover {"
            "  border-color: %2;"
            "}"
            "QComboBox::drop-down {"
            "  border: none;"
            "}")
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueHex);
    }

    QString buildBlueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit, QTextEdit, QPlainTextEdit {"
            "  border: 1px solid #C8DDF4;"
            "  border-radius: 3px;"
            "  background: #FFFFFF;"
            "  padding: 3px 5px;"
            "}"
            "QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus {"
            "  border: 1px solid %1;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // 十六进制查看器常量：每行 16 字节，共 32 行，每页 512 字节。
    constexpr int kHexBytesPerRow = 16;
    constexpr int kHexRowCount = 32;
    constexpr std::uint64_t kHexPageBytes = static_cast<std::uint64_t>(kHexBytesPerRow * kHexRowCount);

    // PID 转 DWORD 的显式封装，避免隐式转换警告。
    DWORD toDwordPid(const std::uint32_t pid)
    {
        return static_cast<DWORD>(pid);
    }

    // 判断内存保护属性是否可读。
    bool isReadableProtect(const std::uint32_t protectValue)
    {
        if ((protectValue & PAGE_GUARD) != 0 || (protectValue & PAGE_NOACCESS) != 0)
        {
            return false;
        }
        const std::uint32_t baseProtect = protectValue & 0xFF;
        switch (baseProtect)
        {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    // 解析两位十六进制字节文本，例如 "7F"、"ff"。
    bool parseHexByte(const QString& text, std::uint8_t& valueOut)
    {
        bool parseOk = false;
        const int value = text.trimmed().toInt(&parseOk, 16);
        if (!parseOk || value < 0 || value > 0xFF)
        {
            return false;
        }
        valueOut = static_cast<std::uint8_t>(value);
        return true;
    }
}

MemoryDock::MemoryDock(QWidget* parent)
    : QWidget(parent)
{
    // 构造阶段按固定顺序执行，确保 UI 控件先创建再绑定信号。
    initializeUi();
    initializeConnections();
    initializeBookmarkRefreshTimer();
    refreshProcessList(false);
    updateStatusBarText();
}

MemoryDock::~MemoryDock()
{
    // 析构前先取消扫描，避免后台线程继续使用已销毁控件。
    cancelCurrentScan();
    detachProcess();
}

void MemoryDock::initializeUi()
{
    // 根布局只做三件事：顶部工具栏、中部 Tab、底部状态栏。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(4);

    initializeToolbar();
    initializeTabs();
    initializeStatusBar();
}

void MemoryDock::initializeToolbar()
{
    // 顶部工具栏放在独立容器内，便于统一 margin 和 spacing。
    QWidget* toolbarContainer = new QWidget(this);
    m_toolbarLayout = new QHBoxLayout(toolbarContainer);
    m_toolbarLayout->setContentsMargins(6, 6, 6, 2);
    m_toolbarLayout->setSpacing(6);

    const QString buttonStyle = buildBlueButtonStyle();
    const QString comboStyle = buildBlueComboStyle();

    m_processCombo = new QComboBox(toolbarContainer);
    m_processCombo->setMinimumWidth(280);
    m_processCombo->setStyleSheet(comboStyle);
    m_processCombo->setToolTip("选择目标进程（进程名 + PID）。");

    m_attachButton = new QPushButton("附加", toolbarContainer);
    m_detachButton = new QPushButton("分离", toolbarContainer);
    m_refreshButton = new QPushButton("刷新", toolbarContainer);
    m_settingsButton = new QPushButton("设置", toolbarContainer);
    m_attachButton->setStyleSheet(buttonStyle);
    m_detachButton->setStyleSheet(buttonStyle);
    m_refreshButton->setStyleSheet(buttonStyle);
    m_settingsButton->setStyleSheet(buttonStyle);

    m_toolbarLayout->addWidget(new QLabel("进程:", toolbarContainer));
    m_toolbarLayout->addWidget(m_processCombo, 1);
    m_toolbarLayout->addWidget(m_attachButton);
    m_toolbarLayout->addWidget(m_detachButton);
    m_toolbarLayout->addWidget(m_refreshButton);
    m_toolbarLayout->addWidget(m_settingsButton);
    m_toolbarLayout->addStretch(1);

    m_rootLayout->addWidget(toolbarContainer);
}

void MemoryDock::initializeTabs()
{
    // 五个子页面统一由 QTabWidget 承载。
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setDocumentMode(true);
    m_rootLayout->addWidget(m_tabWidget, 1);

    initializeProcessModuleTab();
    initializeMemoryRegionTab();
    initializeMemorySearchTab();
    initializeMemoryViewerTab();
    initializeBreakpointBookmarkTab();
}

void MemoryDock::initializeProcessModuleTab()
{
    // Tab1：进程与模块。
    m_tabProcessModule = new QWidget(m_tabWidget);
    QVBoxLayout* tabLayout = new QVBoxLayout(m_tabProcessModule);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);

    // 上下分割布局：上进程表，下模块表。
    QSplitter* splitter = new QSplitter(Qt::Vertical, m_tabProcessModule);

    QWidget* processPanel = new QWidget(splitter);
    QVBoxLayout* processLayout = new QVBoxLayout(processPanel);
    processLayout->setContentsMargins(0, 0, 0, 0);
    processLayout->setSpacing(4);
    processLayout->addWidget(new QLabel("进程列表（双击自动附加）", processPanel));

    m_processTable = new QTableWidget(processPanel);
    m_processTable->setColumnCount(5);
    m_processTable->setHorizontalHeaderLabels(QStringList{ "进程名", "PID", "会话ID", "CPU(可选)", "工作集" });
    m_processTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_processTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_processTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_processTable->setSortingEnabled(true);
    m_processTable->setAlternatingRowColors(true);
    m_processTable->horizontalHeader()->setStretchLastSection(true);
    processLayout->addWidget(m_processTable, 1);

    QWidget* modulePanel = new QWidget(splitter);
    QVBoxLayout* moduleLayout = new QVBoxLayout(modulePanel);
    moduleLayout->setContentsMargins(0, 0, 0, 0);
    moduleLayout->setSpacing(4);

    QHBoxLayout* filterLayout = new QHBoxLayout();
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(6);
    filterLayout->addWidget(new QLabel("模块过滤:", modulePanel));
    m_moduleFilterEdit = new QLineEdit(modulePanel);
    m_moduleFilterEdit->setPlaceholderText("输入模块名关键字");
    m_moduleFilterEdit->setStyleSheet(buildBlueInputStyle());
    filterLayout->addWidget(m_moduleFilterEdit, 1);
    moduleLayout->addLayout(filterLayout);

    m_moduleTable = new QTableWidget(modulePanel);
    m_moduleTable->setColumnCount(4);
    m_moduleTable->setHorizontalHeaderLabels(QStringList{ "模块名", "基址", "大小", "路径" });
    m_moduleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_moduleTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_moduleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_moduleTable->setAlternatingRowColors(true);
    m_moduleTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_moduleTable->horizontalHeader()->setStretchLastSection(true);
    moduleLayout->addWidget(m_moduleTable, 1);

    splitter->addWidget(processPanel);
    splitter->addWidget(modulePanel);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 5);

    tabLayout->addWidget(splitter, 1);
    m_tabWidget->addTab(m_tabProcessModule, "进程与模块");
}

void MemoryDock::initializeMemoryRegionTab()
{
    // Tab2：内存区域。
    m_tabRegions = new QWidget(m_tabWidget);
    QVBoxLayout* tabLayout = new QVBoxLayout(m_tabRegions);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);

    QHBoxLayout* filterLayout = new QHBoxLayout();
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(10);
    m_regionCommittedOnlyCheck = new QCheckBox("仅已提交(MEM_COMMIT)", m_tabRegions);
    m_regionImageOnlyCheck = new QCheckBox("仅映像(IMAGE)", m_tabRegions);
    m_regionReadableOnlyCheck = new QCheckBox("仅可读", m_tabRegions);
    m_regionCommittedOnlyCheck->setChecked(true);
    m_regionReadableOnlyCheck->setChecked(true);
    filterLayout->addWidget(m_regionCommittedOnlyCheck);
    filterLayout->addWidget(m_regionImageOnlyCheck);
    filterLayout->addWidget(m_regionReadableOnlyCheck);
    filterLayout->addStretch(1);
    tabLayout->addLayout(filterLayout);

    m_regionTable = new QTableWidget(m_tabRegions);
    m_regionTable->setColumnCount(6);
    m_regionTable->setHorizontalHeaderLabels(QStringList{
        "基址", "大小", "保护属性", "状态", "类型", "映射文件"
        });
    m_regionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_regionTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_regionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_regionTable->setAlternatingRowColors(true);
    m_regionTable->setSortingEnabled(true);
    m_regionTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_regionTable->horizontalHeader()->setStretchLastSection(true);
    tabLayout->addWidget(m_regionTable, 1);

    m_tabWidget->addTab(m_tabRegions, "内存区域");
}

void MemoryDock::initializeMemorySearchTab()
{
    // Tab3：内存搜索。
    m_tabSearch = new QWidget(m_tabWidget);
    QVBoxLayout* tabLayout = new QVBoxLayout(m_tabSearch);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);

    const QString inputStyle = buildBlueInputStyle();
    const QString comboStyle = buildBlueComboStyle();
    const QString buttonStyle = buildBlueButtonStyle();

    // 搜索条件面板。
    QGroupBox* conditionGroup = new QGroupBox("搜索条件", m_tabSearch);
    QGridLayout* conditionLayout = new QGridLayout(conditionGroup);
    conditionLayout->setHorizontalSpacing(8);
    conditionLayout->setVerticalSpacing(6);

    m_searchTypeCombo = new QComboBox(conditionGroup);
    m_searchTypeCombo->addItem("字节", static_cast<int>(SearchValueType::Byte));
    m_searchTypeCombo->addItem("2字节", static_cast<int>(SearchValueType::Int16));
    m_searchTypeCombo->addItem("4字节", static_cast<int>(SearchValueType::Int32));
    m_searchTypeCombo->addItem("8字节", static_cast<int>(SearchValueType::Int64));
    m_searchTypeCombo->addItem("浮点数", static_cast<int>(SearchValueType::Float32));
    m_searchTypeCombo->addItem("双精度", static_cast<int>(SearchValueType::Float64));
    m_searchTypeCombo->addItem("字节数组(支持??)", static_cast<int>(SearchValueType::ByteArray));
    m_searchTypeCombo->addItem("ASCII字符串", static_cast<int>(SearchValueType::StringAscii));
    m_searchTypeCombo->addItem("Unicode字符串", static_cast<int>(SearchValueType::StringUnicode));
    m_searchTypeCombo->setStyleSheet(comboStyle);

    m_searchValueEdit = new QLineEdit(conditionGroup);
    m_searchValueEdit->setPlaceholderText("输入搜索值");
    m_searchValueEdit->setStyleSheet(inputStyle);

    m_searchRangeCombo = new QComboBox(conditionGroup);
    m_searchRangeCombo->addItem("整个内存");
    m_searchRangeCombo->addItem("自定义范围");
    m_searchRangeCombo->setStyleSheet(comboStyle);

    m_searchRangeStartEdit = new QLineEdit(conditionGroup);
    m_searchRangeEndEdit = new QLineEdit(conditionGroup);
    m_searchRangeStartEdit->setPlaceholderText("起始地址");
    m_searchRangeEndEdit->setPlaceholderText("结束地址");
    m_searchRangeStartEdit->setStyleSheet(inputStyle);
    m_searchRangeEndEdit->setStyleSheet(inputStyle);
    m_searchRangeStartEdit->setEnabled(false);
    m_searchRangeEndEdit->setEnabled(false);

    m_searchImageOnlyCheck = new QCheckBox("仅映像", conditionGroup);
    m_searchHeapOnlyCheck = new QCheckBox("仅堆(近似)", conditionGroup);
    m_searchStackOnlyCheck = new QCheckBox("仅栈(近似)", conditionGroup);

    m_firstScanButton = new QPushButton("首次扫描", conditionGroup);
    m_nextScanButton = new QPushButton("再次扫描", conditionGroup);
    m_resetScanButton = new QPushButton("重置", conditionGroup);
    m_cancelScanButton = new QPushButton("取消扫描", conditionGroup);
    m_firstScanButton->setStyleSheet(buttonStyle);
    m_nextScanButton->setStyleSheet(buttonStyle);
    m_resetScanButton->setStyleSheet(buttonStyle);
    m_cancelScanButton->setStyleSheet(buttonStyle);
    m_nextScanButton->setEnabled(false);
    m_cancelScanButton->setEnabled(false);

    conditionLayout->addWidget(new QLabel("数据类型", conditionGroup), 0, 0);
    conditionLayout->addWidget(m_searchTypeCombo, 0, 1);
    conditionLayout->addWidget(new QLabel("值", conditionGroup), 0, 2);
    conditionLayout->addWidget(m_searchValueEdit, 0, 3, 1, 3);
    conditionLayout->addWidget(new QLabel("范围", conditionGroup), 1, 0);
    conditionLayout->addWidget(m_searchRangeCombo, 1, 1);
    conditionLayout->addWidget(m_searchRangeStartEdit, 1, 2);
    conditionLayout->addWidget(m_searchRangeEndEdit, 1, 3);
    conditionLayout->addWidget(m_searchImageOnlyCheck, 1, 4);
    conditionLayout->addWidget(m_searchHeapOnlyCheck, 1, 5);
    conditionLayout->addWidget(m_searchStackOnlyCheck, 1, 6);
    conditionLayout->addWidget(m_firstScanButton, 2, 1);
    conditionLayout->addWidget(m_nextScanButton, 2, 2);
    conditionLayout->addWidget(m_resetScanButton, 2, 3);
    conditionLayout->addWidget(m_cancelScanButton, 2, 4);

    tabLayout->addWidget(conditionGroup);

    QGroupBox* compareGroup = new QGroupBox("再次扫描过滤", m_tabSearch);
    QHBoxLayout* compareLayout = new QHBoxLayout(compareGroup);
    compareLayout->setContentsMargins(8, 6, 8, 6);
    compareLayout->setSpacing(8);

    m_nextScanCompareCombo = new QComboBox(compareGroup);
    m_nextScanCompareCombo->addItem("等于", static_cast<int>(SearchCompareMode::Equal));
    m_nextScanCompareCombo->addItem("大于", static_cast<int>(SearchCompareMode::Greater));
    m_nextScanCompareCombo->addItem("小于", static_cast<int>(SearchCompareMode::Less));
    m_nextScanCompareCombo->addItem("介于", static_cast<int>(SearchCompareMode::Between));
    m_nextScanCompareCombo->addItem("变化", static_cast<int>(SearchCompareMode::Changed));
    m_nextScanCompareCombo->addItem("未变化", static_cast<int>(SearchCompareMode::Unchanged));
    m_nextScanCompareCombo->addItem("增加", static_cast<int>(SearchCompareMode::Increased));
    m_nextScanCompareCombo->addItem("减少", static_cast<int>(SearchCompareMode::Decreased));
    m_nextScanCompareCombo->setStyleSheet(comboStyle);

    m_nextScanValueEdit = new QLineEdit(compareGroup);
    m_nextScanValueBEdit = new QLineEdit(compareGroup);
    m_nextScanValueEdit->setPlaceholderText("值A");
    m_nextScanValueBEdit->setPlaceholderText("值B");
    m_nextScanValueEdit->setStyleSheet(inputStyle);
    m_nextScanValueBEdit->setStyleSheet(inputStyle);
    m_nextScanValueBEdit->setVisible(false);

    compareLayout->addWidget(new QLabel("条件", compareGroup));
    compareLayout->addWidget(m_nextScanCompareCombo);
    compareLayout->addWidget(new QLabel("值", compareGroup));
    compareLayout->addWidget(m_nextScanValueEdit, 1);
    compareLayout->addWidget(m_nextScanValueBEdit, 1);
    tabLayout->addWidget(compareGroup);

    m_searchResultTable = new QTableWidget(m_tabSearch);
    m_searchResultTable->setColumnCount(4);
    m_searchResultTable->setHorizontalHeaderLabels(QStringList{ "地址", "当前值", "前次值", "备注" });
    m_searchResultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_searchResultTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_searchResultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_searchResultTable->setAlternatingRowColors(true);
    m_searchResultTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_searchResultTable->horizontalHeader()->setStretchLastSection(true);
    tabLayout->addWidget(m_searchResultTable, 1);

    QHBoxLayout* progressLayout = new QHBoxLayout();
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(8);
    m_scanProgressBar = new QProgressBar(m_tabSearch);
    m_scanProgressBar->setRange(0, 100);
    m_scanStatusLabel = new QLabel("就绪", m_tabSearch);
    progressLayout->addWidget(m_scanProgressBar, 1);
    progressLayout->addWidget(m_scanStatusLabel);
    tabLayout->addLayout(progressLayout);

    m_tabWidget->addTab(m_tabSearch, "内存搜索");
}

void MemoryDock::initializeMemoryViewerTab()
{
    // Tab4：内存查看器。
    m_tabViewer = new QWidget(m_tabWidget);
    QVBoxLayout* tabLayout = new QVBoxLayout(m_tabViewer);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);

    QHBoxLayout* navLayout = new QHBoxLayout();
    navLayout->setContentsMargins(0, 0, 0, 0);
    navLayout->setSpacing(8);
    navLayout->addWidget(new QLabel("地址:", m_tabViewer));
    m_viewAddressEdit = new QLineEdit(m_tabViewer);
    m_viewAddressEdit->setPlaceholderText("输入地址后跳转");
    m_viewAddressEdit->setStyleSheet(buildBlueInputStyle());
    m_viewJumpButton = new QPushButton("跳转", m_tabViewer);
    m_viewJumpButton->setStyleSheet(buildBlueButtonStyle());
    m_viewProtectLabel = new QLabel("保护属性: -", m_tabViewer);
    navLayout->addWidget(m_viewAddressEdit, 1);
    navLayout->addWidget(m_viewJumpButton);
    navLayout->addWidget(m_viewProtectLabel);
    tabLayout->addLayout(navLayout);

    // 16进制表格：地址 + 16字节 + ASCII。
    m_hexTable = new QTableWidget(m_tabViewer);
    m_hexTable->setRowCount(kHexRowCount);
    m_hexTable->setColumnCount(kHexBytesPerRow + 2);
    QStringList headers;
    headers << "地址";
    for (int column = 0; column < kHexBytesPerRow; ++column)
    {
        headers << QString("%1").arg(column, 2, 16, QChar('0')).toUpper();
    }
    headers << "ASCII";
    m_hexTable->setHorizontalHeaderLabels(headers);
    m_hexTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_hexTable->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_hexTable->setAlternatingRowColors(true);
    m_hexTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_hexTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_hexTable->horizontalHeader()->setStretchLastSection(true);
    tabLayout->addWidget(m_hexTable, 1);

    m_viewerStatusLabel = new QLabel("未附加进程。", m_tabViewer);
    tabLayout->addWidget(m_viewerStatusLabel);

    m_tabWidget->addTab(m_tabViewer, "内存查看器");
}

void MemoryDock::initializeBreakpointBookmarkTab()
{
    // Tab5：断点与书签。
    m_tabBpBookmark = new QWidget(m_tabWidget);
    QVBoxLayout* tabLayout = new QVBoxLayout(m_tabBpBookmark);
    tabLayout->setContentsMargins(6, 6, 6, 6);
    tabLayout->setSpacing(6);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_tabBpBookmark);
    const QString buttonStyle = buildBlueButtonStyle();

    QWidget* breakpointPanel = new QWidget(splitter);
    QVBoxLayout* breakpointLayout = new QVBoxLayout(breakpointPanel);
    breakpointLayout->setContentsMargins(0, 0, 0, 0);
    breakpointLayout->setSpacing(4);

    QHBoxLayout* bpButtonLayout = new QHBoxLayout();
    bpButtonLayout->setContentsMargins(0, 0, 0, 0);
    bpButtonLayout->setSpacing(6);
    m_addBreakpointButton = new QPushButton("添加断点", breakpointPanel);
    m_removeBreakpointButton = new QPushButton("删除断点", breakpointPanel);
    m_toggleBreakpointButton = new QPushButton("启用/禁用", breakpointPanel);
    m_addBreakpointButton->setStyleSheet(buttonStyle);
    m_removeBreakpointButton->setStyleSheet(buttonStyle);
    m_toggleBreakpointButton->setStyleSheet(buttonStyle);
    bpButtonLayout->addWidget(m_addBreakpointButton);
    bpButtonLayout->addWidget(m_removeBreakpointButton);
    bpButtonLayout->addWidget(m_toggleBreakpointButton);
    bpButtonLayout->addStretch(1);
    breakpointLayout->addLayout(bpButtonLayout);

    m_breakpointTable = new QTableWidget(breakpointPanel);
    m_breakpointTable->setColumnCount(5);
    m_breakpointTable->setHorizontalHeaderLabels(QStringList{ "地址", "原字节", "状态", "命中次数", "描述" });
    m_breakpointTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_breakpointTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_breakpointTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_breakpointTable->setAlternatingRowColors(true);
    m_breakpointTable->horizontalHeader()->setStretchLastSection(true);
    breakpointLayout->addWidget(m_breakpointTable, 1);

    QWidget* bookmarkPanel = new QWidget(splitter);
    QVBoxLayout* bookmarkLayout = new QVBoxLayout(bookmarkPanel);
    bookmarkLayout->setContentsMargins(0, 0, 0, 0);
    bookmarkLayout->setSpacing(4);

    QHBoxLayout* bmButtonLayout = new QHBoxLayout();
    bmButtonLayout->setContentsMargins(0, 0, 0, 0);
    bmButtonLayout->setSpacing(6);
    m_addBookmarkButton = new QPushButton("添加书签", bookmarkPanel);
    m_removeBookmarkButton = new QPushButton("删除书签", bookmarkPanel);
    m_refreshBookmarkButton = new QPushButton("刷新值", bookmarkPanel);
    m_jumpBookmarkButton = new QPushButton("跳转", bookmarkPanel);
    m_addBookmarkButton->setStyleSheet(buttonStyle);
    m_removeBookmarkButton->setStyleSheet(buttonStyle);
    m_refreshBookmarkButton->setStyleSheet(buttonStyle);
    m_jumpBookmarkButton->setStyleSheet(buttonStyle);
    bmButtonLayout->addWidget(m_addBookmarkButton);
    bmButtonLayout->addWidget(m_removeBookmarkButton);
    bmButtonLayout->addWidget(m_refreshBookmarkButton);
    bmButtonLayout->addWidget(m_jumpBookmarkButton);
    bmButtonLayout->addStretch(1);
    bookmarkLayout->addLayout(bmButtonLayout);

    m_bookmarkTable = new QTableWidget(bookmarkPanel);
    m_bookmarkTable->setColumnCount(4);
    m_bookmarkTable->setHorizontalHeaderLabels(QStringList{ "地址", "当前值", "备注", "添加时间" });
    m_bookmarkTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_bookmarkTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_bookmarkTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bookmarkTable->setAlternatingRowColors(true);
    m_bookmarkTable->horizontalHeader()->setStretchLastSection(true);
    bookmarkLayout->addWidget(m_bookmarkTable, 1);

    splitter->addWidget(breakpointPanel);
    splitter->addWidget(bookmarkPanel);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 5);
    tabLayout->addWidget(splitter, 1);

    m_tabWidget->addTab(m_tabBpBookmark, "断点与书签");
}

void MemoryDock::initializeConnections()
{
    // ========================================================
    // 工具栏逻辑
    // ========================================================

    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        // 刷新动作按当前 Tab 路由，减少无关页面刷新开销。
        const int tabIndex = m_tabWidget->currentIndex();
        if (tabIndex == 0)
        {
            refreshProcessList(true);
            if (m_attachedPid != 0)
            {
                refreshModuleListForPid(m_attachedPid);
            }
            return;
        }
        if (tabIndex == 1)
        {
            refreshMemoryRegionList(true);
            return;
        }
        if (tabIndex == 2)
        {
            refreshMemoryRegionList(true);
            return;
        }
        if (tabIndex == 3)
        {
            reloadMemoryViewerPage();
            return;
        }
        if (tabIndex == 4)
        {
            refreshBookmarkValues();
        }
        });

    connect(m_processCombo, &QComboBox::currentIndexChanged, this, [this](int indexValue) {
        // 顶部进程切换时只刷新模块预览，不自动附加。
        if (indexValue < 0)
        {
            return;
        }
        const std::uint32_t pid = static_cast<std::uint32_t>(
            m_processCombo->itemData(indexValue, Qt::UserRole).toUInt());
        refreshModuleListForPid(pid);
        });

    connect(m_attachButton, &QPushButton::clicked, this, [this]() {
        const int comboIndex = m_processCombo->currentIndex();
        if (comboIndex < 0)
        {
            QMessageBox::warning(this, "附加进程", "请先选择进程。");
            return;
        }
        const std::uint32_t pid = static_cast<std::uint32_t>(
            m_processCombo->itemData(comboIndex, Qt::UserRole).toUInt());
        const QString processName = m_processCombo->itemData(comboIndex, Qt::UserRole + 1).toString();
        attachToProcess(pid, processName, true);
        });

    connect(m_detachButton, &QPushButton::clicked, this, [this]() {
        detachProcess();
        QMessageBox::information(this, "分离进程", "已分离当前进程。");
        });

    connect(m_settingsButton, &QPushButton::clicked, this, [this]() {
        // 设置页面：允许调整扫描线程数和块大小（缓存大小近似）。
        QDialog dialog(this);
        dialog.setWindowTitle("内存扫描设置");
        QFormLayout* formLayout = new QFormLayout(&dialog);

        QSpinBox* threadSpin = new QSpinBox(&dialog);
        threadSpin->setRange(1, 32);
        threadSpin->setValue(static_cast<int>(m_scanThreadCount));

        QSpinBox* chunkSpin = new QSpinBox(&dialog);
        chunkSpin->setRange(64, 16384);
        chunkSpin->setValue(static_cast<int>(m_scanChunkSizeKB));
        chunkSpin->setSuffix(" KB");

        QPushButton* okButton = new QPushButton("确定", &dialog);
        QPushButton* cancelButton = new QPushButton("取消", &dialog);
        okButton->setStyleSheet(buildBlueButtonStyle());
        cancelButton->setStyleSheet(buildBlueButtonStyle());
        QHBoxLayout* buttonLayout = new QHBoxLayout();
        buttonLayout->addWidget(okButton);
        buttonLayout->addWidget(cancelButton);

        formLayout->addRow("扫描线程数", threadSpin);
        formLayout->addRow("读取块大小", chunkSpin);
        formLayout->addRow(buttonLayout);

        connect(okButton, &QPushButton::clicked, &dialog, &QDialog::accept);
        connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);

        if (dialog.exec() == QDialog::Accepted)
        {
            m_scanThreadCount = static_cast<std::uint32_t>(threadSpin->value());
            m_scanChunkSizeKB = static_cast<std::uint32_t>(chunkSpin->value());
            m_scanStatusLabel->setText(
                QString("设置已更新：线程=%1, 块=%2KB")
                .arg(m_scanThreadCount)
                .arg(m_scanChunkSizeKB));
        }
        });

    // ========================================================
    // Tab1：进程与模块
    // ========================================================

    connect(m_processTable, &QTableWidget::cellClicked, this, [this](int row, int column) {
        Q_UNUSED(column);
        if (row < 0)
        {
            return;
        }

        // 注意：表格允许排序，row 可能不再对应 m_processCache 原始下标。
        // 因此这里改为“从单元格文本/数据反解 PID + 进程名”，保证绑定准确。
        const QTableWidgetItem* pidItem = m_processTable->item(row, 1);
        const QTableWidgetItem* nameItem = m_processTable->item(row, 0);
        if (pidItem == nullptr || nameItem == nullptr)
        {
            return;
        }

        bool pidOk = false;
        const std::uint32_t pid = pidItem->text().toUInt(&pidOk);
        if (!pidOk || pid == 0)
        {
            return;
        }

        for (int index = 0; index < m_processCombo->count(); ++index)
        {
            if (m_processCombo->itemData(index, Qt::UserRole).toUInt() == pid)
            {
                m_processCombo->setCurrentIndex(index);
                break;
            }
        }
        refreshModuleListForPid(pid);
        });

    connect(m_processTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        Q_UNUSED(column);
        if (row < 0)
        {
            return;
        }
        // 双击附加同样不能直接拿 row 索引缓存，需从当前表格行动态取值。
        const QTableWidgetItem* pidItem = m_processTable->item(row, 1);
        const QTableWidgetItem* nameItem = m_processTable->item(row, 0);
        if (pidItem == nullptr || nameItem == nullptr)
        {
            return;
        }
        bool pidOk = false;
        const std::uint32_t pid = pidItem->text().toUInt(&pidOk);
        if (!pidOk || pid == 0)
        {
            return;
        }
        attachToProcess(pid, nameItem->text(), true);
        });

    connect(m_moduleFilterEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        // 模块过滤变更时按当前下拉 PID 重建模块列表。
        const int comboIndex = m_processCombo->currentIndex();
        if (comboIndex < 0)
        {
            return;
        }
        const std::uint32_t pid = static_cast<std::uint32_t>(
            m_processCombo->itemData(comboIndex, Qt::UserRole).toUInt());
        refreshModuleListForPid(pid);
        });

    connect(m_moduleTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        // 双击基址列时跳转到 Tab4 地址。
        if (row < 0 || column != 1)
        {
            return;
        }
        const QString baseText = m_moduleTable->item(row, 1) != nullptr
            ? m_moduleTable->item(row, 1)->text()
            : QString();
        std::uint64_t addressValue = 0;
        if (parseAddressText(baseText, addressValue))
        {
            jumpToAddress(addressValue);
        }
        });

    connect(m_moduleTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        QTableWidgetItem* clickedItem = m_moduleTable->itemAt(localPosition);
        if (clickedItem == nullptr)
        {
            return;
        }

        const int row = clickedItem->row();
        const QString baseText = m_moduleTable->item(row, 1) != nullptr
            ? m_moduleTable->item(row, 1)->text()
            : QString();

        QMenu menu(this);
        QAction* copyBaseAction = menu.addAction("复制基址");
        QAction* jumpViewerAction = menu.addAction("跳转到内存查看器");
        QAction* selectedAction = menu.exec(m_moduleTable->viewport()->mapToGlobal(localPosition));
        if (selectedAction == nullptr)
        {
            return;
        }

        if (selectedAction == copyBaseAction)
        {
            QApplication::clipboard()->setText(baseText);
            return;
        }
        if (selectedAction == jumpViewerAction)
        {
            std::uint64_t addressValue = 0;
            if (parseAddressText(baseText, addressValue))
            {
                jumpToAddress(addressValue);
            }
        }
        });

    // ========================================================
    // Tab2：内存区域
    // ========================================================

    const auto applyRegionFilter = [this]() {
        applyRegionFilterAndRebuildTable();
        };
    connect(m_regionCommittedOnlyCheck, &QCheckBox::toggled, this, applyRegionFilter);
    connect(m_regionImageOnlyCheck, &QCheckBox::toggled, this, applyRegionFilter);
    connect(m_regionReadableOnlyCheck, &QCheckBox::toggled, this, applyRegionFilter);

    connect(m_regionTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        Q_UNUSED(column);
        if (row < 0)
        {
            return;
        }
        const QString baseText = m_regionTable->item(row, 0) != nullptr
            ? m_regionTable->item(row, 0)->text()
            : QString();
        std::uint64_t baseAddress = 0;
        if (parseAddressText(baseText, baseAddress))
        {
            jumpToAddress(baseAddress);
        }
        });

    connect(m_regionTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        QTableWidgetItem* clickedItem = m_regionTable->itemAt(localPosition);
        if (clickedItem == nullptr)
        {
            return;
        }

        const int row = clickedItem->row();
        const QString baseText = m_regionTable->item(row, 0) != nullptr
            ? m_regionTable->item(row, 0)->text()
            : QString();

        QMenu menu(this);
        QAction* viewAction = menu.addAction("查看此区域");
        QAction* copyAction = menu.addAction("复制基址");
        QAction* searchAction = menu.addAction("搜索此区域");
        QAction* selectedAction = menu.exec(m_regionTable->viewport()->mapToGlobal(localPosition));
        if (selectedAction == nullptr)
        {
            return;
        }

        if (selectedAction == copyAction)
        {
            QApplication::clipboard()->setText(baseText);
            return;
        }

        std::uint64_t baseAddress = 0;
        if (!parseAddressText(baseText, baseAddress))
        {
            return;
        }

        if (selectedAction == viewAction)
        {
            jumpToAddress(baseAddress);
            return;
        }
        if (selectedAction == searchAction)
        {
            // 搜索此区域：切换到 Tab3 并填充起止地址。
            // 注意：区域表启用了排序，row 与 m_regionCache 下标不再等价。
            // 因此基于表格 item 的 UserRole 中缓存的 base/size 进行计算。
            const QTableWidgetItem* baseItem = m_regionTable->item(row, 0);
            const QTableWidgetItem* sizeItem = m_regionTable->item(row, 1);
            if (baseItem != nullptr && sizeItem != nullptr)
            {
                const std::uint64_t regionBase = baseItem->data(Qt::UserRole).toULongLong();
                const std::uint64_t regionSize = sizeItem->data(Qt::UserRole).toULongLong();
                if (regionSize > 0)
                {
                    const std::uint64_t regionEnd = regionBase + regionSize - 1;
                    m_searchRangeCombo->setCurrentIndex(1);
                    m_searchRangeStartEdit->setText(formatAddress(regionBase));
                    m_searchRangeEndEdit->setText(formatAddress(regionEnd));
                    m_tabWidget->setCurrentWidget(m_tabSearch);
                }
            }
        }
        });

    // ========================================================
    // Tab3：搜索
    // ========================================================

    connect(m_searchRangeCombo, &QComboBox::currentIndexChanged, this, [this](int indexValue) {
        const bool customRange = (indexValue == 1);
        m_searchRangeStartEdit->setEnabled(customRange);
        m_searchRangeEndEdit->setEnabled(customRange);
        });

    connect(m_nextScanCompareCombo, &QComboBox::currentIndexChanged, this, [this](int indexValue) {
        const auto compareMode = static_cast<SearchCompareMode>(
            m_nextScanCompareCombo->itemData(indexValue).toInt());
        const bool needValueInput = !(
            compareMode == SearchCompareMode::Changed ||
            compareMode == SearchCompareMode::Unchanged ||
            compareMode == SearchCompareMode::Increased ||
            compareMode == SearchCompareMode::Decreased);
        const bool needSecondValue = (compareMode == SearchCompareMode::Between);
        m_nextScanValueEdit->setEnabled(needValueInput);
        m_nextScanValueBEdit->setVisible(needSecondValue);
        });

    connect(m_firstScanButton, &QPushButton::clicked, this, [this]() {
        startFirstScan();
        });
    connect(m_nextScanButton, &QPushButton::clicked, this, [this]() {
        startNextScan();
        });
    connect(m_resetScanButton, &QPushButton::clicked, this, [this]() {
        resetScanState();
        });
    connect(m_cancelScanButton, &QPushButton::clicked, this, [this]() {
        cancelCurrentScan();
        });

    connect(m_searchResultTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        Q_UNUSED(column);
        if (row < 0 || row >= static_cast<int>(m_searchResultCache.size()))
        {
            return;
        }
        jumpToAddress(m_searchResultCache[static_cast<std::size_t>(row)].address);
        });

    connect(m_searchResultTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        QTableWidgetItem* clickedItem = m_searchResultTable->itemAt(localPosition);
        if (clickedItem == nullptr)
        {
            return;
        }

        const int row = clickedItem->row();
        if (row < 0 || row >= static_cast<int>(m_searchResultCache.size()))
        {
            return;
        }

        QMenu menu(this);
        QAction* viewAction = menu.addAction("查看此地址");
        QAction* addBookmarkAction = menu.addAction("添加到书签");
        QAction* copyAddressAction = menu.addAction("复制地址");
        QAction* copyValueAction = menu.addAction("复制值");
        QAction* selectedAction = menu.exec(m_searchResultTable->viewport()->mapToGlobal(localPosition));
        if (selectedAction == nullptr)
        {
            return;
        }

        const SearchResultEntry& entry = m_searchResultCache[static_cast<std::size_t>(row)];
        if (selectedAction == viewAction)
        {
            jumpToAddress(entry.address);
            return;
        }
        if (selectedAction == addBookmarkAction)
        {
            addBookmarkByAddress(entry.address, "来自内存搜索结果");
            rebuildBookmarkTable();
            return;
        }
        if (selectedAction == copyAddressAction)
        {
            QApplication::clipboard()->setText(formatAddress(entry.address));
            return;
        }
        if (selectedAction == copyValueAction)
        {
            QApplication::clipboard()->setText(
                bytesToDisplayString(entry.currentValueBytes, m_lastSearchValueType));
        }
        });

    // ========================================================
    // Tab4：查看器
    // ========================================================

    connect(m_viewJumpButton, &QPushButton::clicked, this, [this]() {
        jumpToAddressFromUi();
        });
    connect(m_viewAddressEdit, &QLineEdit::returnPressed, this, [this]() {
        jumpToAddressFromUi();
        });

    connect(m_hexTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* changedItem) {
        if (m_hexTableProgrammaticUpdate || changedItem == nullptr)
        {
            return;
        }

        const int row = changedItem->row();
        const int column = changedItem->column();
        if (column < 1 || column > kHexBytesPerRow)
        {
            return;
        }

        std::uint8_t newValue = 0;
        if (!parseHexByte(changedItem->text(), newValue))
        {
            QMessageBox::warning(this, "内存编辑", "请输入两位十六进制数。");
            reloadMemoryViewerPage();
            return;
        }

        const std::uint64_t offset =
            static_cast<std::uint64_t>(row * kHexBytesPerRow + (column - 1));
        const std::uint64_t address = m_currentViewerAddress + offset;

        QString errorText;
        if (!writeSingleByteAtViewer(address, newValue, errorText))
        {
            QMessageBox::warning(this, "内存编辑", errorText);
            reloadMemoryViewerPage();
            return;
        }

        reloadMemoryViewerPage();
        });

    connect(m_hexTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        QTableWidgetItem* clickedItem = m_hexTable->itemAt(localPosition);
        if (clickedItem == nullptr)
        {
            return;
        }

        QMenu menu(this);
        QAction* copyAction = menu.addAction("复制");
        QAction* addBookmarkAction = menu.addAction("添加书签");
        QAction* addBreakpointAction = menu.addAction("添加断点");
        QAction* selectedAction = menu.exec(m_hexTable->viewport()->mapToGlobal(localPosition));
        if (selectedAction == nullptr)
        {
            return;
        }

        if (selectedAction == copyAction)
        {
            QApplication::clipboard()->setText(clickedItem->text());
            return;
        }

        if (clickedItem->column() < 1 || clickedItem->column() > kHexBytesPerRow)
        {
            return;
        }
        const std::uint64_t offset =
            static_cast<std::uint64_t>(clickedItem->row() * kHexBytesPerRow + (clickedItem->column() - 1));
        const std::uint64_t address = m_currentViewerAddress + offset;

        if (selectedAction == addBookmarkAction)
        {
            addBookmarkByAddress(address, "来自内存查看器");
            rebuildBookmarkTable();
            return;
        }
        if (selectedAction == addBreakpointAction)
        {
            QString errorText;
            if (!addBreakpointByAddress(address, "来自内存查看器", errorText))
            {
                QMessageBox::warning(this, "添加断点", errorText);
            }
            rebuildBreakpointTable();
        }
        });

    // ========================================================
    // Tab5：断点与书签
    // ========================================================

    connect(m_addBreakpointButton, &QPushButton::clicked, this, [this]() {
        bool inputOk = false;
        const QString addressText = QInputDialog::getText(
            this,
            "添加断点",
            "输入断点地址",
            QLineEdit::Normal,
            "0x0",
            &inputOk);
        if (!inputOk)
        {
            return;
        }

        std::uint64_t address = 0;
        if (!parseAddressText(addressText, address))
        {
            QMessageBox::warning(this, "添加断点", "地址格式无效。");
            return;
        }

        QString errorText;
        if (!addBreakpointByAddress(address, "手动添加", errorText))
        {
            QMessageBox::warning(this, "添加断点", errorText);
            return;
        }
        rebuildBreakpointTable();
        });

    connect(m_removeBreakpointButton, &QPushButton::clicked, this, [this]() {
        const int row = m_breakpointTable->currentRow();
        if (row < 0)
        {
            return;
        }
        removeBreakpointByRow(row);
        rebuildBreakpointTable();
        });

    connect(m_toggleBreakpointButton, &QPushButton::clicked, this, [this]() {
        const int row = m_breakpointTable->currentRow();
        if (row < 0 || row >= static_cast<int>(m_breakpointCache.size()))
        {
            return;
        }
        const bool nextState = !m_breakpointCache[static_cast<std::size_t>(row)].enabled;
        if (!setBreakpointEnabledByRow(row, nextState))
        {
            QMessageBox::warning(this, "断点切换", "切换失败，请检查权限或进程状态。");
        }
        rebuildBreakpointTable();
        });

    connect(m_addBookmarkButton, &QPushButton::clicked, this, [this]() {
        bool inputOk = false;
        const QString addressText = QInputDialog::getText(
            this,
            "添加书签",
            "输入书签地址",
            QLineEdit::Normal,
            "0x0",
            &inputOk);
        if (!inputOk)
        {
            return;
        }
        std::uint64_t address = 0;
        if (!parseAddressText(addressText, address))
        {
            QMessageBox::warning(this, "添加书签", "地址格式无效。");
            return;
        }
        addBookmarkByAddress(address, "手动添加");
        rebuildBookmarkTable();
        });

    connect(m_removeBookmarkButton, &QPushButton::clicked, this, [this]() {
        const int row = m_bookmarkTable->currentRow();
        if (row < 0 || row >= static_cast<int>(m_bookmarkCache.size()))
        {
            return;
        }
        m_bookmarkCache.erase(m_bookmarkCache.begin() + row);
        rebuildBookmarkTable();
        });

    connect(m_refreshBookmarkButton, &QPushButton::clicked, this, [this]() {
        refreshBookmarkValues();
        });

    connect(m_jumpBookmarkButton, &QPushButton::clicked, this, [this]() {
        const int row = m_bookmarkTable->currentRow();
        if (row < 0 || row >= static_cast<int>(m_bookmarkCache.size()))
        {
            return;
        }
        jumpToAddress(m_bookmarkCache[static_cast<std::size_t>(row)].address);
        });
}

void MemoryDock::initializeStatusBar()
{
    // 底部状态栏统一显示进程名、PID、读写能力。
    m_statusBar = new QStatusBar(this);
    m_statusBar->setSizeGripEnabled(false);

    m_statusProcessLabel = new QLabel("进程: 未附加", m_statusBar);
    m_statusPidLabel = new QLabel("PID: -", m_statusBar);
    m_statusMemoryIoLabel = new QLabel("读写状态: 未就绪", m_statusBar);
    m_statusBar->addPermanentWidget(m_statusProcessLabel, 1);
    m_statusBar->addPermanentWidget(m_statusPidLabel);
    m_statusBar->addPermanentWidget(m_statusMemoryIoLabel, 1);

    m_rootLayout->addWidget(m_statusBar);
}

void MemoryDock::initializeBookmarkRefreshTimer()
{
    // 书签值默认每秒刷新一次，便于观察变量变化。
    m_bookmarkRefreshTimer = new QTimer(this);
    m_bookmarkRefreshTimer->setInterval(1000);
    connect(m_bookmarkRefreshTimer, &QTimer::timeout, this, [this]() {
        refreshBookmarkValues();
        });
    m_bookmarkRefreshTimer->start();
}

void MemoryDock::refreshProcessList(const bool keepSelection)
{
    // 刷新前记录当前选择 PID，刷新后尽量恢复体验。
    std::uint32_t previousPid = 0;
    if (keepSelection)
    {
        const int index = m_processCombo->currentIndex();
        if (index >= 0)
        {
            previousPid = static_cast<std::uint32_t>(m_processCombo->itemData(index, Qt::UserRole).toUInt());
        }
    }

    m_processCache.clear();

    // 进程枚举按需求使用 Toolhelp 快照接口。
    HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshotHandle == INVALID_HANDLE_VALUE)
    {
        QMessageBox::warning(this, "进程刷新", "CreateToolhelp32Snapshot 失败。");
        return;
    }

    PROCESSENTRY32W processEntry{};
    processEntry.dwSize = sizeof(processEntry);
    if (::Process32FirstW(snapshotHandle, &processEntry) == FALSE)
    {
        ::CloseHandle(snapshotHandle);
        return;
    }

    do
    {
        ProcessEntry entry{};
        entry.pid = static_cast<std::uint32_t>(processEntry.th32ProcessID);
        entry.processName = QString::fromWCharArray(processEntry.szExeFile);
        entry.cpuPercent = 0.0; // CPU 为可选字段，当前版本保留 0。

        DWORD sessionId = 0;
        if (::ProcessIdToSessionId(toDwordPid(entry.pid), &sessionId) != FALSE)
        {
            entry.sessionId = static_cast<std::uint32_t>(sessionId);
        }

        // 尝试读取工作集大小：失败则保持 0，不影响主流程。
        HANDLE processHandle = ::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
            FALSE,
            toDwordPid(entry.pid));
        if (processHandle != nullptr)
        {
            PROCESS_MEMORY_COUNTERS_EX memoryCounter{};
            if (::GetProcessMemoryInfo(
                processHandle,
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memoryCounter),
                sizeof(memoryCounter)) != FALSE)
            {
                entry.workingSetMB = static_cast<double>(memoryCounter.WorkingSetSize) / (1024.0 * 1024.0);
            }
            ::CloseHandle(processHandle);
        }
        m_processCache.push_back(std::move(entry));
    } while (::Process32NextW(snapshotHandle, &processEntry) != FALSE);
    ::CloseHandle(snapshotHandle);

    // 为了稳定展示顺序，这里按 PID 升序排序。
    std::sort(m_processCache.begin(), m_processCache.end(), [](const ProcessEntry& left, const ProcessEntry& right) {
        return left.pid < right.pid;
        });

    // 先重建进程表。
    m_processTable->setSortingEnabled(false);
    m_processTable->setRowCount(static_cast<int>(m_processCache.size()));
    for (int row = 0; row < static_cast<int>(m_processCache.size()); ++row)
    {
        const ProcessEntry& entry = m_processCache[static_cast<std::size_t>(row)];
        m_processTable->setItem(row, 0, new QTableWidgetItem(entry.processName));
        m_processTable->setItem(row, 1, new QTableWidgetItem(QString::number(entry.pid)));
        m_processTable->setItem(row, 2, new QTableWidgetItem(QString::number(entry.sessionId)));
        m_processTable->setItem(row, 3, new QTableWidgetItem(QString::number(entry.cpuPercent, 'f', 2) + "%"));
        m_processTable->setItem(row, 4, new QTableWidgetItem(QString("%1 MB").arg(entry.workingSetMB, 0, 'f', 1)));
    }
    m_processTable->setSortingEnabled(true);

    // 再同步重建顶部下拉框。
    updateProcessComboFromCache();

    // 尝试恢复之前选中 PID。
    if (previousPid != 0)
    {
        for (int comboIndex = 0; comboIndex < m_processCombo->count(); ++comboIndex)
        {
            if (m_processCombo->itemData(comboIndex, Qt::UserRole).toUInt() == previousPid)
            {
                m_processCombo->setCurrentIndex(comboIndex);
                break;
            }
        }
    }
}

void MemoryDock::updateProcessComboFromCache()
{
    // 下拉框重建期间阻断信号，防止反复触发模块刷新。
    QSignalBlocker blocker(m_processCombo);
    m_processCombo->clear();

    for (const ProcessEntry& entry : m_processCache)
    {
        const QString text = QString("%1 [PID:%2]").arg(entry.processName).arg(entry.pid);
        m_processCombo->addItem(text, QVariant::fromValue(static_cast<uint>(entry.pid)));
        const int row = m_processCombo->count() - 1;
        m_processCombo->setItemData(row, entry.processName, Qt::UserRole + 1);
    }

    // 当前附加 PID 还存在时，默认选中它。
    if (m_attachedPid != 0)
    {
        for (int comboIndex = 0; comboIndex < m_processCombo->count(); ++comboIndex)
        {
            if (m_processCombo->itemData(comboIndex, Qt::UserRole).toUInt() == m_attachedPid)
            {
                m_processCombo->setCurrentIndex(comboIndex);
                return;
            }
        }
    }
    if (m_processCombo->count() > 0)
    {
        m_processCombo->setCurrentIndex(0);
    }
}

bool MemoryDock::refreshModuleListForPid(const std::uint32_t pid)
{
    // 每次刷新模块都先清空缓存，确保结果和 PID 一致。
    m_moduleCache.clear();

    HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        toDwordPid(pid));
    if (snapshotHandle == INVALID_HANDLE_VALUE)
    {
        m_moduleTable->setRowCount(0);
        return false;
    }

    MODULEENTRY32W moduleEntry{};
    moduleEntry.dwSize = sizeof(moduleEntry);
    if (::Module32FirstW(snapshotHandle, &moduleEntry) == FALSE)
    {
        ::CloseHandle(snapshotHandle);
        m_moduleTable->setRowCount(0);
        return false;
    }

    do
    {
        ModuleEntry entry{};
        entry.moduleName = QString::fromWCharArray(moduleEntry.szModule);
        entry.baseAddress = reinterpret_cast<std::uintptr_t>(moduleEntry.modBaseAddr);
        entry.sizeBytes = static_cast<std::uint64_t>(moduleEntry.modBaseSize);
        entry.fullPath = QString::fromWCharArray(moduleEntry.szExePath);
        m_moduleCache.push_back(std::move(entry));
    } while (::Module32NextW(snapshotHandle, &moduleEntry) != FALSE);
    ::CloseHandle(snapshotHandle);

    std::sort(m_moduleCache.begin(), m_moduleCache.end(), [](const ModuleEntry& left, const ModuleEntry& right) {
        return left.baseAddress < right.baseAddress;
        });

    // 模块过滤：按名称包含匹配。
    const QString filterText = m_moduleFilterEdit->text().trimmed();
    std::vector<const ModuleEntry*> filteredModules;
    filteredModules.reserve(m_moduleCache.size());
    for (const ModuleEntry& entry : m_moduleCache)
    {
        if (!filterText.isEmpty() && !entry.moduleName.contains(filterText, Qt::CaseInsensitive))
        {
            continue;
        }
        filteredModules.push_back(&entry);
    }

    m_moduleTable->setRowCount(static_cast<int>(filteredModules.size()));
    for (int row = 0; row < static_cast<int>(filteredModules.size()); ++row)
    {
        const ModuleEntry& entry = *filteredModules[static_cast<std::size_t>(row)];
        m_moduleTable->setItem(row, 0, new QTableWidgetItem(entry.moduleName));
        m_moduleTable->setItem(row, 1, new QTableWidgetItem(formatAddress(entry.baseAddress)));
        m_moduleTable->setItem(row, 2, new QTableWidgetItem(formatSize(entry.sizeBytes)));
        m_moduleTable->setItem(row, 3, new QTableWidgetItem(entry.fullPath));
    }
    return true;
}

bool MemoryDock::attachToProcess(
    const std::uint32_t pid,
    const QString& processName,
    const bool showMessage)
{
    // 附加前先清理旧进程上下文，避免残留句柄。
    detachProcess();

    // 首选读写权限，满足写内存/断点能力。
    HANDLE processHandle = ::OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
        FALSE,
        toDwordPid(pid));
    if (processHandle == nullptr)
    {
        // 失败时回退只读权限，至少保证浏览能力。
        processHandle = ::OpenProcess(
            PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
            FALSE,
            toDwordPid(pid));
        if (processHandle == nullptr)
        {
            if (showMessage)
            {
                QMessageBox::warning(
                    this,
                    "附加失败",
                    "OpenProcess 失败，可能权限不足。\n请尝试以管理员身份运行。");
            }
            updateStatusBarText();
            return false;
        }
        m_canReadWriteMemory = false;
    }
    else
    {
        m_canReadWriteMemory = true;
    }

    m_attachedProcessHandle = processHandle;
    m_attachedPid = pid;
    m_attachedProcessName = processName;
    updateStatusBarText();

    // 附加后立即刷新模块与区域，减少下一步等待。
    refreshModuleListForPid(pid);
    refreshMemoryRegionList(true);

    if (showMessage)
    {
        QMessageBox::information(
            this,
            "附加成功",
            QString("已附加到 %1 (PID=%2)\n读写状态: %3")
            .arg(processName)
            .arg(pid)
            .arg(m_canReadWriteMemory ? "可读可写" : "只读"));
    }
    return true;
}

void MemoryDock::detachProcess()
{
    // 先请求扫描取消，防止后台继续使用旧句柄。
    cancelCurrentScan();

    if (m_attachedProcessHandle != nullptr)
    {
        ::CloseHandle(m_attachedProcessHandle);
        m_attachedProcessHandle = nullptr;
    }

    m_attachedPid = 0;
    m_attachedProcessName.clear();
    m_canReadWriteMemory = false;

    // 分离时清理依赖上下文的数据缓存。
    m_moduleCache.clear();
    m_regionCache.clear();
    m_searchResultCache.clear();

    // 清理表格展示，避免用户误操作旧数据。
    m_moduleTable->setRowCount(0);
    m_regionTable->setRowCount(0);
    m_searchResultTable->setRowCount(0);
    m_hexTable->clearContents();
    m_hexTable->setRowCount(kHexRowCount);
    m_viewerStatusLabel->setText("未附加进程。");

    updateStatusBarText();
}

HANDLE MemoryDock::openProcessHandleForRead(const std::uint32_t pid, QString* const errorTextOut) const
{
    HANDLE processHandle = ::OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE,
        toDwordPid(pid));
    if (processHandle == nullptr && errorTextOut != nullptr)
    {
        *errorTextOut = QString("OpenProcess 失败，错误码=%1").arg(::GetLastError());
    }
    return processHandle;
}

void MemoryDock::refreshMemoryRegionList(const bool forceRequery)
{
    if (m_attachedProcessHandle == nullptr)
    {
        m_regionCache.clear();
        m_regionTable->setRowCount(0);
        return;
    }

    if (forceRequery || m_regionCache.empty())
    {
        QString errorText;
        std::vector<RegionEntry> regionList;
        if (!enumerateMemoryRegionsByVirtualQuery(m_attachedProcessHandle, regionList, &errorText))
        {
            QMessageBox::warning(this, "区域刷新", errorText);
            return;
        }
        m_regionCache = std::move(regionList);
    }

    applyRegionFilterAndRebuildTable();
}

bool MemoryDock::enumerateMemoryRegionsByVirtualQuery(
    HANDLE processHandle,
    std::vector<RegionEntry>& regionsOut,
    QString* const errorTextOut) const
{
    regionsOut.clear();

    SYSTEM_INFO systemInfo{};
    ::GetSystemInfo(&systemInfo);
    const std::uint64_t minAddress = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const std::uint64_t maxAddress = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);

    std::uint64_t currentAddress = minAddress;
    while (currentAddress < maxAddress)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T querySize = ::VirtualQueryEx(
            processHandle,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(currentAddress)),
            &mbi,
            sizeof(mbi));
        if (querySize != sizeof(mbi))
        {
            break;
        }

        RegionEntry entry{};
        entry.baseAddress = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        entry.regionSize = static_cast<std::uint64_t>(mbi.RegionSize);
        entry.protect = static_cast<std::uint32_t>(mbi.Protect);
        entry.state = static_cast<std::uint32_t>(mbi.State);
        entry.type = static_cast<std::uint32_t>(mbi.Type);

        // IMAGE/MAPPED 区域尽量解析映射文件路径，便于用户定位模块。
        if (entry.state == MEM_COMMIT && (entry.type == MEM_IMAGE || entry.type == MEM_MAPPED))
        {
            wchar_t mappedPath[MAX_PATH] = {};
            const DWORD length = ::GetMappedFileNameW(
                processHandle,
                mbi.BaseAddress,
                mappedPath,
                static_cast<DWORD>(std::size(mappedPath)));
            if (length > 0)
            {
                entry.mappedFilePath = QString::fromWCharArray(mappedPath, static_cast<int>(length));
            }
        }

        regionsOut.push_back(std::move(entry));
        if (mbi.RegionSize == 0)
        {
            break;
        }

        const std::uint64_t nextAddress =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) +
            static_cast<std::uint64_t>(mbi.RegionSize);
        if (nextAddress <= currentAddress)
        {
            break;
        }
        currentAddress = nextAddress;
    }

    if (regionsOut.empty())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = "VirtualQueryEx 未返回有效区域。";
        }
        return false;
    }
    return true;
}

void MemoryDock::applyRegionFilterAndRebuildTable()
{
    // 过滤条件组合：已提交 / IMAGE / 可读。
    std::vector<const RegionEntry*> filteredRegions;
    filteredRegions.reserve(m_regionCache.size());
    for (const RegionEntry& entry : m_regionCache)
    {
        if (m_regionCommittedOnlyCheck->isChecked() && entry.state != MEM_COMMIT)
        {
            continue;
        }
        if (m_regionImageOnlyCheck->isChecked() && entry.type != MEM_IMAGE)
        {
            continue;
        }
        if (m_regionReadableOnlyCheck->isChecked() && !isReadableProtect(entry.protect))
        {
            continue;
        }
        filteredRegions.push_back(&entry);
    }

    m_regionTable->setSortingEnabled(false);
    m_regionTable->setRowCount(static_cast<int>(filteredRegions.size()));
    for (int row = 0; row < static_cast<int>(filteredRegions.size()); ++row)
    {
        const RegionEntry& entry = *filteredRegions[static_cast<std::size_t>(row)];
        // 列 0 和列 1 附带 UserRole 原始值，便于排序/右键动作时可靠反查。
        QTableWidgetItem* baseItem = new QTableWidgetItem(formatAddress(entry.baseAddress));
        baseItem->setData(Qt::EditRole, QVariant::fromValue<qulonglong>(entry.baseAddress));
        baseItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(static_cast<qulonglong>(entry.baseAddress)));
        m_regionTable->setItem(row, 0, baseItem);

        QTableWidgetItem* sizeItem = new QTableWidgetItem(formatSize(entry.regionSize));
        sizeItem->setData(Qt::EditRole, QVariant::fromValue<qulonglong>(entry.regionSize));
        sizeItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(static_cast<qulonglong>(entry.regionSize)));
        m_regionTable->setItem(row, 1, sizeItem);

        m_regionTable->setItem(row, 2, new QTableWidgetItem(protectToText(entry.protect)));
        m_regionTable->setItem(row, 3, new QTableWidgetItem(stateToText(entry.state)));
        m_regionTable->setItem(row, 4, new QTableWidgetItem(typeToText(entry.type)));
        m_regionTable->setItem(row, 5, new QTableWidgetItem(entry.mappedFilePath));
    }
    m_regionTable->setSortingEnabled(true);
}

bool MemoryDock::parseSearchPatternFromUi(
    ParsedSearchPattern& patternOut,
    QString& errorTextOut) const
{
    // 每次解析前都先清空输出结构体，避免上一轮数据污染本轮匹配规则。
    patternOut = ParsedSearchPattern{};

    // 数据类型来自下拉框 itemData，定义与 SearchValueType 枚举一一对应。
    const int typeIndex = m_searchTypeCombo->currentIndex();
    if (typeIndex < 0)
    {
        errorTextOut = "请选择有效的数据类型。";
        return false;
    }
    patternOut.valueType = static_cast<SearchValueType>(m_searchTypeCombo->itemData(typeIndex).toInt());

    // 搜索值文本是首次扫描的核心输入；为空时直接拒绝并提示用户。
    const QString valueText = m_searchValueEdit->text().trimmed();
    if (valueText.isEmpty())
    {
        errorTextOut = "搜索值不能为空。";
        return false;
    }

    // 按数据类型分别解析，最终都转成“exactBytes + wildcardMask”统一结构。
    switch (patternOut.valueType)
    {
    case SearchValueType::Byte:
    {
        std::uint64_t value = 0;
        if (!parseUnsignedNumber(valueText, value) || value > 0xFF)
        {
            errorTextOut = "字节类型请输入 0~255（支持十进制或 0x 十六进制）。";
            return false;
        }
        const std::uint8_t byteValue = static_cast<std::uint8_t>(value);
        patternOut.exactBytes = QByteArray(reinterpret_cast<const char*>(&byteValue), sizeof(byteValue));
        patternOut.lowerBound = static_cast<double>(byteValue);
        patternOut.upperBound = static_cast<double>(byteValue);
        break;
    }
    case SearchValueType::Int16:
    {
        bool parseOk = false;
        const qint16 value = static_cast<qint16>(valueText.toLongLong(&parseOk, 0));
        if (!parseOk)
        {
            errorTextOut = "2字节整数解析失败。";
            return false;
        }
        patternOut.exactBytes = QByteArray(reinterpret_cast<const char*>(&value), sizeof(value));
        patternOut.lowerBound = static_cast<double>(value);
        patternOut.upperBound = static_cast<double>(value);
        break;
    }
    case SearchValueType::Int32:
    {
        bool parseOk = false;
        const qint32 value = static_cast<qint32>(valueText.toLongLong(&parseOk, 0));
        if (!parseOk)
        {
            errorTextOut = "4字节整数解析失败。";
            return false;
        }
        patternOut.exactBytes = QByteArray(reinterpret_cast<const char*>(&value), sizeof(value));
        patternOut.lowerBound = static_cast<double>(value);
        patternOut.upperBound = static_cast<double>(value);
        break;
    }
    case SearchValueType::Int64:
    {
        bool parseOk = false;
        const qint64 value = static_cast<qint64>(valueText.toLongLong(&parseOk, 0));
        if (!parseOk)
        {
            errorTextOut = "8字节整数解析失败。";
            return false;
        }
        patternOut.exactBytes = QByteArray(reinterpret_cast<const char*>(&value), sizeof(value));
        patternOut.lowerBound = static_cast<double>(value);
        patternOut.upperBound = static_cast<double>(value);
        break;
    }
    case SearchValueType::Float32:
    {
        bool parseOk = false;
        const float value = valueText.toFloat(&parseOk);
        if (!parseOk)
        {
            errorTextOut = "浮点数解析失败。";
            return false;
        }
        patternOut.exactBytes = QByteArray(reinterpret_cast<const char*>(&value), sizeof(value));
        patternOut.lowerBound = static_cast<double>(value);
        patternOut.upperBound = static_cast<double>(value);
        patternOut.epsilon = 0.00001;
        break;
    }
    case SearchValueType::Float64:
    {
        bool parseOk = false;
        const double value = valueText.toDouble(&parseOk);
        if (!parseOk)
        {
            errorTextOut = "双精度浮点数解析失败。";
            return false;
        }
        patternOut.exactBytes = QByteArray(reinterpret_cast<const char*>(&value), sizeof(value));
        patternOut.lowerBound = value;
        patternOut.upperBound = value;
        patternOut.epsilon = 0.0000001;
        break;
    }
    case SearchValueType::ByteArray:
    {
        // 字节数组支持“AA BB CC”与“AA??CC”混合写法，这里统一按空格切词。
        QString normalizedText = valueText;
        normalizedText.replace(',', ' ');
        normalizedText.replace(';', ' ');
        const QStringList tokens = normalizedText.split(' ', Qt::SkipEmptyParts);
        if (tokens.isEmpty())
        {
            errorTextOut = "字节数组不能为空。示例：48 8B ?? ?? 89";
            return false;
        }

        for (const QString& tokenText : tokens)
        {
            const QString token = tokenText.trimmed().toUpper();
            if (token == "??")
            {
                patternOut.exactBytes.push_back('\0');
                patternOut.wildcardMask.push_back('\0');
                continue;
            }

            std::uint8_t parsedByte = 0;
            if (!parseHexByte(token, parsedByte))
            {
                errorTextOut = QString("无效字节项：%1").arg(tokenText);
                return false;
            }

            patternOut.exactBytes.push_back(static_cast<char>(parsedByte));
            patternOut.wildcardMask.push_back('\1');
        }
        break;
    }
    case SearchValueType::StringAscii:
    {
        patternOut.exactBytes = valueText.toLatin1();
        break;
    }
    case SearchValueType::StringUnicode:
    {
        // QString 内部是 UTF-16，直接拷贝到底层字节即可得到 LE 序列。
        const QString unicodeText = valueText;
        const auto* utf16Data = reinterpret_cast<const char*>(unicodeText.utf16());
        patternOut.exactBytes = QByteArray(utf16Data, unicodeText.size() * static_cast<int>(sizeof(char16_t)));
        break;
    }
    default:
        errorTextOut = "未知数据类型。";
        return false;
    }

    // 任何类型都必须形成至少 1 字节的匹配模式，否则无法执行扫描。
    if (patternOut.exactBytes.isEmpty())
    {
        errorTextOut = "解析后匹配模式为空，请检查输入值。";
        return false;
    }
    return true;
}

bool MemoryDock::collectSearchRegionsFromUi(
    std::vector<RegionEntry>& regionsOut,
    QString& errorTextOut)
{
    // 扫描前必须已附加目标进程，否则 ReadProcessMemory 无法进行。
    if (m_attachedProcessHandle == nullptr || m_attachedPid == 0)
    {
        errorTextOut = "请先附加目标进程。";
        return false;
    }

    // 区域缓存为空时主动刷新一次，避免首次进入扫描页时没有范围数据。
    if (m_regionCache.empty())
    {
        refreshMemoryRegionList(true);
    }

    if (m_regionCache.empty())
    {
        errorTextOut = "当前没有可用的内存区域，请先刷新区域列表。";
        return false;
    }

    // 自定义范围模式下，需要把用户输入地址解析成闭区间 [start, end]。
    bool useCustomRange = (m_searchRangeCombo->currentIndex() == 1);
    std::uint64_t rangeStart = 0;
    std::uint64_t rangeEnd = std::numeric_limits<std::uint64_t>::max();
    if (useCustomRange)
    {
        if (!parseAddressText(m_searchRangeStartEdit->text().trimmed(), rangeStart))
        {
            errorTextOut = "起始地址格式无效。";
            return false;
        }
        if (!parseAddressText(m_searchRangeEndEdit->text().trimmed(), rangeEnd))
        {
            errorTextOut = "结束地址格式无效。";
            return false;
        }
        if (rangeEnd < rangeStart)
        {
            errorTextOut = "结束地址不能小于起始地址。";
            return false;
        }
    }

    const bool imageOnly = m_searchImageOnlyCheck->isChecked();
    const bool heapOnly = m_searchHeapOnlyCheck->isChecked();
    const bool stackOnly = m_searchStackOnlyCheck->isChecked();

    // “仅堆”和“仅栈”同时开启没有明确业务意义，这里直接提示用户二选一。
    if (heapOnly && stackOnly)
    {
        errorTextOut = "“仅堆”与“仅栈”不能同时启用。";
        return false;
    }

    regionsOut.clear();
    regionsOut.reserve(m_regionCache.size());

    for (const RegionEntry& region : m_regionCache)
    {
        // 只扫描已提交 + 可读区域，避免大量无效访问或 NOACCESS 报错。
        if (region.state != MEM_COMMIT || !isReadableProtect(region.protect))
        {
            continue;
        }

        // 按类型过滤：仅映像 -> MEM_IMAGE；仅堆 -> MEM_PRIVATE（近似）。
        if (imageOnly && region.type != MEM_IMAGE)
        {
            continue;
        }
        if (heapOnly && region.type != MEM_PRIVATE)
        {
            continue;
        }

        // 栈区域无法用单一字段准确识别，这里采用“PRIVATE + GUARD 或 RW”近似。
        if (stackOnly)
        {
            const bool maybeStack =
                (region.type == MEM_PRIVATE) &&
                (((region.protect & PAGE_GUARD) != 0) || ((region.protect & PAGE_READWRITE) != 0));
            if (!maybeStack)
            {
                continue;
            }
        }

        // 非自定义范围时直接入选；自定义范围时按交集裁剪区域边界。
        if (!useCustomRange)
        {
            regionsOut.push_back(region);
            continue;
        }

        if (region.regionSize == 0)
        {
            continue;
        }

        const std::uint64_t regionStart = region.baseAddress;
        const std::uint64_t regionEnd = region.baseAddress + region.regionSize - 1;
        if (regionEnd < rangeStart || regionStart > rangeEnd)
        {
            continue;
        }

        RegionEntry clippedRegion = region;
        clippedRegion.baseAddress = std::max(regionStart, rangeStart);
        const std::uint64_t clippedEnd = std::min(regionEnd, rangeEnd);
        clippedRegion.regionSize = clippedEnd - clippedRegion.baseAddress + 1;
        regionsOut.push_back(std::move(clippedRegion));
    }

    if (regionsOut.empty())
    {
        errorTextOut = "过滤后没有可扫描区域，请调整范围/过滤条件。";
        return false;
    }

    return true;
}

void MemoryDock::startFirstScan()
{
    // 正在扫描时不允许重复发起，避免多个后台线程并发读同一状态。
    if (m_scanInProgress.load())
    {
        QMessageBox::information(this, "内存扫描", "当前已有扫描任务正在执行。");
        return;
    }

    // 首次扫描前必须附加进程。
    if (m_attachedProcessHandle == nullptr || m_attachedPid == 0)
    {
        QMessageBox::warning(this, "内存扫描", "请先附加目标进程。");
        return;
    }

    // 先解析输入值，再收集扫描区域，两步都可能失败并反馈可读错误。
    ParsedSearchPattern pattern{};
    QString parseError;
    if (!parseSearchPatternFromUi(pattern, parseError))
    {
        QMessageBox::warning(this, "内存扫描", parseError);
        return;
    }

    std::vector<RegionEntry> scanRegions;
    QString regionError;
    if (!collectSearchRegionsFromUi(scanRegions, regionError))
    {
        QMessageBox::warning(this, "内存扫描", regionError);
        return;
    }

    // 首次扫描会重置上一轮结果，并把“最近扫描类型”更新为当前下拉框类型。
    m_lastSearchValueType = pattern.valueType;
    m_searchResultCache.clear();
    rebuildSearchResultTable();

    // UI 状态切换：禁用发起按钮，启用取消按钮，展示准备中的提示文本。
    m_scanInProgress.store(true);
    m_scanCancelRequested.store(false);
    m_firstScanButton->setEnabled(false);
    m_nextScanButton->setEnabled(false);
    m_resetScanButton->setEnabled(false);
    m_cancelScanButton->setEnabled(true);
    m_scanProgressBar->setValue(0);
    m_scanStatusLabel->setText(
        QString("扫描中：区域=%1，线程=%2")
        .arg(scanRegions.size())
        .arg(m_scanThreadCount));

    // 进入后台扫描线程，避免 ReadProcessMemory 循环阻塞主界面。
    scanMemoryRegionsInBackground(scanRegions, pattern);
}

void MemoryDock::startNextScan()
{
    // 再次扫描依赖首次扫描结果；无结果时直接提示，不执行无意义循环。
    if (m_scanInProgress.load())
    {
        QMessageBox::information(this, "再次扫描", "当前正在执行扫描，请稍候。");
        return;
    }
    if (m_attachedProcessHandle == nullptr || m_searchResultCache.empty())
    {
        QMessageBox::warning(this, "再次扫描", "请先完成首次扫描并保留结果。");
        return;
    }

    // 将常用“字节转数值”逻辑内联为局部 lambda，便于多条件比较复用。
    const auto bytesToDouble = [](const QByteArray& sourceBytes, const SearchValueType valueType, double& numberOut) -> bool
        {
            if (sourceBytes.isEmpty())
            {
                return false;
            }

            switch (valueType)
            {
            case SearchValueType::Byte:
            {
                if (sourceBytes.size() < static_cast<int>(sizeof(std::uint8_t))) return false;
                std::uint8_t value = 0;
                std::memcpy(&value, sourceBytes.constData(), sizeof(value));
                numberOut = static_cast<double>(value);
                return true;
            }
            case SearchValueType::Int16:
            {
                if (sourceBytes.size() < static_cast<int>(sizeof(std::int16_t))) return false;
                std::int16_t value = 0;
                std::memcpy(&value, sourceBytes.constData(), sizeof(value));
                numberOut = static_cast<double>(value);
                return true;
            }
            case SearchValueType::Int32:
            {
                if (sourceBytes.size() < static_cast<int>(sizeof(std::int32_t))) return false;
                std::int32_t value = 0;
                std::memcpy(&value, sourceBytes.constData(), sizeof(value));
                numberOut = static_cast<double>(value);
                return true;
            }
            case SearchValueType::Int64:
            {
                if (sourceBytes.size() < static_cast<int>(sizeof(std::int64_t))) return false;
                std::int64_t value = 0;
                std::memcpy(&value, sourceBytes.constData(), sizeof(value));
                numberOut = static_cast<double>(value);
                return true;
            }
            case SearchValueType::Float32:
            {
                if (sourceBytes.size() < static_cast<int>(sizeof(float))) return false;
                float value = 0.0f;
                std::memcpy(&value, sourceBytes.constData(), sizeof(value));
                numberOut = static_cast<double>(value);
                return true;
            }
            case SearchValueType::Float64:
            {
                if (sourceBytes.size() < static_cast<int>(sizeof(double))) return false;
                double value = 0.0;
                std::memcpy(&value, sourceBytes.constData(), sizeof(value));
                numberOut = value;
                return true;
            }
            default:
                return false;
            }
        };

    // 当前比较模式决定是否需要输入数值，变化类条件不需要额外输入。
    const auto compareMode = static_cast<SearchCompareMode>(
        m_nextScanCompareCombo->itemData(m_nextScanCompareCombo->currentIndex()).toInt());
    const bool needNumericInput = (
        compareMode == SearchCompareMode::Equal ||
        compareMode == SearchCompareMode::Greater ||
        compareMode == SearchCompareMode::Less ||
        compareMode == SearchCompareMode::Between);

    const bool isNumericType = (
        m_lastSearchValueType == SearchValueType::Byte ||
        m_lastSearchValueType == SearchValueType::Int16 ||
        m_lastSearchValueType == SearchValueType::Int32 ||
        m_lastSearchValueType == SearchValueType::Int64 ||
        m_lastSearchValueType == SearchValueType::Float32 ||
        m_lastSearchValueType == SearchValueType::Float64);

    // 非数值类型不支持 > < between 增减 比较，避免语义混乱。
    if (!isNumericType && compareMode != SearchCompareMode::Equal &&
        compareMode != SearchCompareMode::Changed &&
        compareMode != SearchCompareMode::Unchanged)
    {
        QMessageBox::warning(this, "再次扫描", "当前数据类型仅支持 等于/变化/未变化 条件。");
        return;
    }

    double compareA = 0.0;
    double compareB = 0.0;
    if (needNumericInput && isNumericType)
    {
        bool parseOkA = false;
        compareA = m_nextScanValueEdit->text().trimmed().toDouble(&parseOkA);
        if (!parseOkA)
        {
            QMessageBox::warning(this, "再次扫描", "条件值A解析失败。");
            return;
        }

        if (compareMode == SearchCompareMode::Between)
        {
            bool parseOkB = false;
            compareB = m_nextScanValueBEdit->text().trimmed().toDouble(&parseOkB);
            if (!parseOkB)
            {
                QMessageBox::warning(this, "再次扫描", "条件值B解析失败。");
                return;
            }
            if (compareA > compareB)
            {
                std::swap(compareA, compareB);
            }
        }
    }

    // 非数值类型在“等于”条件下按用户输入文本重新构造比较字节。
    QByteArray nonNumericCompareBytes;
    QByteArray nonNumericWildcardMask;
    if (!isNumericType && compareMode == SearchCompareMode::Equal)
    {
        const QString compareText = m_nextScanValueEdit->text().trimmed();
        if (compareText.isEmpty())
        {
            QMessageBox::warning(this, "再次扫描", "请输入用于“等于”比较的目标值。");
            return;
        }

        if (m_lastSearchValueType == SearchValueType::StringAscii)
        {
            nonNumericCompareBytes = compareText.toLatin1();
        }
        else if (m_lastSearchValueType == SearchValueType::StringUnicode)
        {
            const auto* utf16Data = reinterpret_cast<const char*>(compareText.utf16());
            nonNumericCompareBytes = QByteArray(
                utf16Data,
                compareText.size() * static_cast<int>(sizeof(char16_t)));
        }
        else
        {
            // ByteArray 模式支持 “AA ?? BB” 语法，mask=0 表示该字节位置忽略。
            QString normalizedText = compareText;
            normalizedText.replace(',', ' ');
            normalizedText.replace(';', ' ');
            const QStringList tokens = normalizedText.split(' ', Qt::SkipEmptyParts);
            if (tokens.isEmpty())
            {
                QMessageBox::warning(this, "再次扫描", "字节数组比较值无效。");
                return;
            }

            for (const QString& tokenText : tokens)
            {
                const QString token = tokenText.trimmed().toUpper();
                if (token == "??")
                {
                    nonNumericCompareBytes.push_back('\0');
                    nonNumericWildcardMask.push_back('\0');
                    continue;
                }

                std::uint8_t parsedByte = 0;
                if (!parseHexByte(token, parsedByte))
                {
                    QMessageBox::warning(this, "再次扫描", QString("字节数组项无效：%1").arg(tokenText));
                    return;
                }
                nonNumericCompareBytes.push_back(static_cast<char>(parsedByte));
                nonNumericWildcardMask.push_back('\1');
            }
        }
    }

    // 再次扫描只对上一轮命中地址逐个复查，因此通常速度比首次扫描更快。
    std::vector<SearchResultEntry> nextResultCache;
    nextResultCache.reserve(m_searchResultCache.size());

    const auto scanStartTime = std::chrono::steady_clock::now();
    const int totalCount = static_cast<int>(m_searchResultCache.size());
    int processedCount = 0;

    m_scanProgressBar->setValue(0);
    m_scanStatusLabel->setText(QString("再次扫描中：总计 %1 条").arg(totalCount));

    for (const SearchResultEntry& oldEntry : m_searchResultCache)
    {
        // 每条结果的读取长度遵循“上一轮值字节长度”，确保比较口径一致。
        const std::size_t readLength = std::max<std::size_t>(1, static_cast<std::size_t>(oldEntry.currentValueBytes.size()));
        QByteArray currentBytes(static_cast<int>(readLength), '\0');
        SIZE_T bytesRead = 0;
        const BOOL readOk = ::ReadProcessMemory(
            m_attachedProcessHandle,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(oldEntry.address)),
            currentBytes.data(),
            static_cast<SIZE_T>(readLength),
            &bytesRead);
        if (readOk == FALSE || bytesRead != readLength)
        {
            ++processedCount;
            continue;
        }

        bool keepThisEntry = false;
        switch (compareMode)
        {
        case SearchCompareMode::Equal:
        {
            if (isNumericType)
            {
                double currentNumber = 0.0;
                if (bytesToDouble(currentBytes, m_lastSearchValueType, currentNumber))
                {
                    keepThisEntry = (std::fabs(currentNumber - compareA) <= 0.000001);
                }
            }
            else
            {
                // 字节数组/字符串等于比较按用户输入字节执行，支持 ByteArray 通配掩码。
                if (currentBytes.size() != nonNumericCompareBytes.size())
                {
                    keepThisEntry = false;
                }
                else if (nonNumericWildcardMask.size() == nonNumericCompareBytes.size())
                {
                    keepThisEntry = true;
                    for (int byteIndex = 0; byteIndex < nonNumericCompareBytes.size(); ++byteIndex)
                    {
                        if (nonNumericWildcardMask.at(byteIndex) == '\0')
                        {
                            continue;
                        }
                        if (currentBytes.at(byteIndex) != nonNumericCompareBytes.at(byteIndex))
                        {
                            keepThisEntry = false;
                            break;
                        }
                    }
                }
                else
                {
                    keepThisEntry = (currentBytes == nonNumericCompareBytes);
                }
            }
            break;
        }
        case SearchCompareMode::Greater:
        {
            double currentNumber = 0.0;
            keepThisEntry = bytesToDouble(currentBytes, m_lastSearchValueType, currentNumber) &&
                (currentNumber > compareA);
            break;
        }
        case SearchCompareMode::Less:
        {
            double currentNumber = 0.0;
            keepThisEntry = bytesToDouble(currentBytes, m_lastSearchValueType, currentNumber) &&
                (currentNumber < compareA);
            break;
        }
        case SearchCompareMode::Between:
        {
            double currentNumber = 0.0;
            keepThisEntry = bytesToDouble(currentBytes, m_lastSearchValueType, currentNumber) &&
                (currentNumber >= compareA && currentNumber <= compareB);
            break;
        }
        case SearchCompareMode::Changed:
            keepThisEntry = (currentBytes != oldEntry.currentValueBytes);
            break;
        case SearchCompareMode::Unchanged:
            keepThisEntry = (currentBytes == oldEntry.currentValueBytes);
            break;
        case SearchCompareMode::Increased:
        {
            double currentNumber = 0.0;
            double previousNumber = 0.0;
            keepThisEntry =
                bytesToDouble(currentBytes, m_lastSearchValueType, currentNumber) &&
                bytesToDouble(oldEntry.currentValueBytes, m_lastSearchValueType, previousNumber) &&
                (currentNumber > previousNumber);
            break;
        }
        case SearchCompareMode::Decreased:
        {
            double currentNumber = 0.0;
            double previousNumber = 0.0;
            keepThisEntry =
                bytesToDouble(currentBytes, m_lastSearchValueType, currentNumber) &&
                bytesToDouble(oldEntry.currentValueBytes, m_lastSearchValueType, previousNumber) &&
                (currentNumber < previousNumber);
            break;
        }
        default:
            keepThisEntry = false;
            break;
        }

        if (keepThisEntry)
        {
            SearchResultEntry nextEntry = oldEntry;
            nextEntry.previousValueBytes = oldEntry.currentValueBytes;
            nextEntry.currentValueBytes = currentBytes;
            nextResultCache.push_back(std::move(nextEntry));
        }

        // 每处理一条都更新进度条，数量很大时 Qt 会自动压缩 repaint 频率。
        ++processedCount;
        const int progressPercent = (totalCount <= 0) ? 100 : (processedCount * 100 / totalCount);
        m_scanProgressBar->setValue(progressPercent);
    }

    // 将过滤结果提交到缓存并重绘表格。
    m_searchResultCache = std::move(nextResultCache);
    rebuildSearchResultTable();

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - scanStartTime).count();
    m_scanStatusLabel->setText(
        QString("再次扫描完成：保留 %1 项，耗时 %2 ms")
        .arg(m_searchResultCache.size())
        .arg(elapsedMs));
}

void MemoryDock::resetScanState()
{
    // 重置动作会先发起取消请求，避免后台线程晚到结果覆盖清空状态。
    cancelCurrentScan();

    m_searchResultCache.clear();
    m_lastSearchValueType = SearchValueType::Byte;
    rebuildSearchResultTable();

    // 恢复按钮状态，并把进度条与状态文本还原到“就绪”。
    m_scanProgressBar->setValue(0);
    m_scanStatusLabel->setText("已重置，等待首次扫描。");
    m_firstScanButton->setEnabled(true);
    m_nextScanButton->setEnabled(false);
    m_resetScanButton->setEnabled(true);
    m_cancelScanButton->setEnabled(false);
}

void MemoryDock::cancelCurrentScan()
{
    // 仅当扫描进行中才设置取消标志；空闲状态下无需修改。
    if (m_scanInProgress.load())
    {
        m_scanCancelRequested.store(true);
        m_scanStatusLabel->setText("正在请求取消扫描...");
    }
}

void MemoryDock::rebuildSearchResultTable()
{
    // 表格重建前先关闭排序，避免行插入期间地址和内容错位。
    m_searchResultTable->setSortingEnabled(false);
    m_searchResultTable->setRowCount(static_cast<int>(m_searchResultCache.size()));

    for (int row = 0; row < static_cast<int>(m_searchResultCache.size()); ++row)
    {
        const SearchResultEntry& entry = m_searchResultCache[static_cast<std::size_t>(row)];

        QTableWidgetItem* addressItem = new QTableWidgetItem(formatAddress(entry.address));
        addressItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(static_cast<qulonglong>(entry.address)));
        m_searchResultTable->setItem(row, 0, addressItem);

        m_searchResultTable->setItem(
            row,
            1,
            new QTableWidgetItem(bytesToDisplayString(entry.currentValueBytes, m_lastSearchValueType)));

        m_searchResultTable->setItem(
            row,
            2,
            new QTableWidgetItem(bytesToDisplayString(entry.previousValueBytes, m_lastSearchValueType)));

        m_searchResultTable->setItem(row, 3, new QTableWidgetItem(entry.noteText));
    }

    // 有结果时允许再次扫描；无结果时禁用该按钮避免误操作。
    m_nextScanButton->setEnabled(!m_searchResultCache.empty());
}

void MemoryDock::scanMemoryRegionsInBackground(
    const std::vector<RegionEntry>& scanRegions,
    const ParsedSearchPattern& pattern)
{
    // 使用 QPointer 保护 this 指针，防止窗口销毁后回调访问悬空对象。
    const QPointer<MemoryDock> selfGuard(this);

    // 扫描参数固定为值捕获，避免 UI 后续修改导致后台线程读到变化中的对象。
    const HANDLE processHandle = m_attachedProcessHandle;
    const std::vector<RegionEntry> regions = scanRegions;
    const ParsedSearchPattern scanPattern = pattern;
    const std::uint32_t threadCount = std::max<std::uint32_t>(1, m_scanThreadCount);
    const std::size_t chunkSize = static_cast<std::size_t>(std::max<std::uint32_t>(64, m_scanChunkSizeKB) * 1024u);
    const auto startTime = std::chrono::steady_clock::now();

    // 后台主线程只负责拉起 worker 并汇总结果，不直接操作 UI 控件。
    std::thread([selfGuard, processHandle, regions, scanPattern, threadCount, chunkSize, startTime]() {
        if (selfGuard == nullptr || processHandle == nullptr)
        {
            return;
        }

        std::vector<SearchResultEntry> mergedResults;
        mergedResults.reserve(1024);
        std::mutex resultMutex;
        std::atomic<std::size_t> finishedRegionCount{ 0 };

        const std::size_t patternLength = static_cast<std::size_t>(scanPattern.exactBytes.size());
        if (patternLength == 0)
        {
            QMetaObject::invokeMethod(selfGuard.data(), [selfGuard]() {
                if (selfGuard == nullptr) return;
                selfGuard->m_scanInProgress.store(false);
                selfGuard->m_scanCancelRequested.store(false);
                selfGuard->m_firstScanButton->setEnabled(true);
                selfGuard->m_resetScanButton->setEnabled(true);
                selfGuard->m_cancelScanButton->setEnabled(false);
                selfGuard->m_scanStatusLabel->setText("扫描失败：匹配模式为空。");
                }, Qt::QueuedConnection);
            return;
        }

        // 单点匹配函数：支持精确匹配与 ByteArray 的通配掩码匹配。
        const auto matchesPatternAt = [&scanPattern, patternLength](const char* dataPtr) -> bool
            {
                for (std::size_t byteIndex = 0; byteIndex < patternLength; ++byteIndex)
                {
                    const bool hasMask = (scanPattern.wildcardMask.size() == static_cast<int>(patternLength));
                    if (hasMask && scanPattern.wildcardMask.at(static_cast<int>(byteIndex)) == '\0')
                    {
                        continue;
                    }
                    if (dataPtr[byteIndex] != scanPattern.exactBytes.at(static_cast<int>(byteIndex)))
                    {
                        return false;
                    }
                }
                return true;
            };

        // 每个 worker 处理“下标同余”的区域切片，实现低成本负载均衡。
        const auto workerBody = [&](const std::uint32_t workerId) {
            std::vector<SearchResultEntry> localResults;
            localResults.reserve(256);

            for (std::size_t regionIndex = workerId; regionIndex < regions.size(); regionIndex += threadCount)
            {
                if (selfGuard->m_scanCancelRequested.load())
                {
                    break;
                }

                const RegionEntry& region = regions[regionIndex];
                if (region.regionSize == 0)
                {
                    ++finishedRegionCount;
                    continue;
                }

                std::uint64_t cursor = region.baseAddress;
                std::uint64_t remainBytes = region.regionSize;
                QByteArray carryBytes;

                while (remainBytes > 0 && !selfGuard->m_scanCancelRequested.load())
                {
                    const std::size_t requestSize = static_cast<std::size_t>(
                        std::min<std::uint64_t>(remainBytes, static_cast<std::uint64_t>(chunkSize)));
                    QByteArray readBuffer(static_cast<int>(requestSize), '\0');
                    SIZE_T bytesRead = 0;
                    const BOOL readOk = ::ReadProcessMemory(
                        processHandle,
                        reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(cursor)),
                        readBuffer.data(),
                        static_cast<SIZE_T>(requestSize),
                        &bytesRead);
                    if (readOk == FALSE || bytesRead == 0)
                    {
                        // 某块读取失败时跳过该块继续扫描，保证任务具备容错能力。
                        cursor += requestSize;
                        remainBytes -= requestSize;
                        carryBytes.clear();
                        continue;
                    }

                    // 合并“上一块尾巴 + 当前块”处理边界命中，避免漏掉跨块模式。
                    const int validReadLength = static_cast<int>(bytesRead);
                    QByteArray mergedBuffer = carryBytes + readBuffer.left(validReadLength);
                    const std::size_t carryLength = static_cast<std::size_t>(carryBytes.size());
                    const std::uint64_t mergedBaseAddress = cursor - carryLength;

                    // 起始偏移计算：只复查“上块尾部不足一个模式长度”的窗口。
                    const std::size_t overlap = (patternLength > 0) ? (patternLength - 1) : 0;
                    const std::size_t startOffset = (carryLength > overlap) ? (carryLength - overlap) : 0;

                    if (mergedBuffer.size() >= static_cast<int>(patternLength))
                    {
                        const std::size_t maxOffset =
                            static_cast<std::size_t>(mergedBuffer.size()) - patternLength;
                        for (std::size_t offset = startOffset; offset <= maxOffset; ++offset)
                        {
                            const char* candidatePtr = mergedBuffer.constData() + static_cast<int>(offset);
                            if (!matchesPatternAt(candidatePtr))
                            {
                                continue;
                            }

                            SearchResultEntry resultEntry{};
                            resultEntry.address = mergedBaseAddress + offset;
                            resultEntry.currentValueBytes = QByteArray(candidatePtr, static_cast<int>(patternLength));
                            localResults.push_back(std::move(resultEntry));
                        }
                    }

                    // 保留末尾 patternLength-1 字节供下一块拼接，不足时全量保留。
                    if (patternLength > 1)
                    {
                        const int tailCount = std::min<int>(
                            static_cast<int>(patternLength - 1),
                            mergedBuffer.size());
                        carryBytes = mergedBuffer.right(tailCount);
                    }
                    else
                    {
                        carryBytes.clear();
                    }

                    cursor += requestSize;
                    remainBytes -= requestSize;
                }

                // 分区扫描完成后统一并入总结果，减少锁竞争次数。
                {
                    std::lock_guard<std::mutex> lock(resultMutex);
                    mergedResults.insert(
                        mergedResults.end(),
                        std::make_move_iterator(localResults.begin()),
                        std::make_move_iterator(localResults.end()));
                }
                localResults.clear();

                const std::size_t finishedCount = ++finishedRegionCount;
                if ((finishedCount % 16 == 0) || finishedCount == regions.size())
                {
                    QMetaObject::invokeMethod(selfGuard.data(), [selfGuard, finishedCount, totalCount = regions.size()]() {
                        if (selfGuard == nullptr || totalCount == 0)
                        {
                            return;
                        }
                        const int progress = static_cast<int>((finishedCount * 100) / totalCount);
                        selfGuard->m_scanProgressBar->setValue(progress);
                        selfGuard->m_scanStatusLabel->setText(
                            QString("扫描中：%1 / %2 区域")
                            .arg(finishedCount)
                            .arg(totalCount));
                        }, Qt::QueuedConnection);
                }
            }
            };

        // 拉起 worker 线程组；线程数不会超过区域数，避免空转。
        std::vector<std::thread> workers;
        workers.reserve(threadCount);
        const std::uint32_t realThreadCount = static_cast<std::uint32_t>(
            std::min<std::size_t>(regions.size(), static_cast<std::size_t>(threadCount)));
        for (std::uint32_t workerId = 0; workerId < realThreadCount; ++workerId)
        {
            workers.emplace_back(workerBody, workerId);
        }

        for (std::thread& worker : workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        // 统一按地址升序排列，便于用户浏览和“再次扫描”时的稳定结果顺序。
        std::sort(mergedResults.begin(), mergedResults.end(), [](const SearchResultEntry& left, const SearchResultEntry& right) {
            return left.address < right.address;
            });

        const bool cancelled = selfGuard->m_scanCancelRequested.load();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();

        // 所有 UI 修改统一切回主线程执行。
        QMetaObject::invokeMethod(selfGuard.data(), [selfGuard, cancelled, elapsedMs, finalResults = std::move(mergedResults)]() mutable {
            if (selfGuard == nullptr)
            {
                return;
            }

            selfGuard->m_scanInProgress.store(false);
            selfGuard->m_scanCancelRequested.store(false);
            selfGuard->m_firstScanButton->setEnabled(true);
            selfGuard->m_resetScanButton->setEnabled(true);
            selfGuard->m_cancelScanButton->setEnabled(false);

            if (cancelled)
            {
                selfGuard->m_scanStatusLabel->setText("扫描已取消。");
                return;
            }

            selfGuard->m_scanProgressBar->setValue(100);
            selfGuard->m_searchResultCache = std::move(finalResults);
            selfGuard->rebuildSearchResultTable();
            selfGuard->m_scanStatusLabel->setText(
                QString("首次扫描完成：命中 %1 项，耗时 %2 ms")
                .arg(selfGuard->m_searchResultCache.size())
                .arg(elapsedMs));
            }, Qt::QueuedConnection);
        }).detach();
}

void MemoryDock::jumpToAddressFromUi()
{
    // 地址栏支持十进制或 0x 十六进制。
    std::uint64_t targetAddress = 0;
    if (!parseAddressText(m_viewAddressEdit->text().trimmed(), targetAddress))
    {
        QMessageBox::warning(this, "地址跳转", "地址格式无效。");
        return;
    }
    jumpToAddress(targetAddress);
}

void MemoryDock::jumpToAddress(const std::uint64_t address)
{
    // 记录当前查看基址，供滚动、编辑、状态栏等多个逻辑复用。
    m_currentViewerAddress = address;
    m_viewAddressEdit->setText(formatAddress(address));

    // 跳转时自动切换到 Tab4，符合“模块/区域/搜索结果双击即查看”的预期。
    m_tabWidget->setCurrentWidget(m_tabViewer);
    reloadMemoryViewerPage();
}

void MemoryDock::reloadMemoryViewerPage()
{
    // 没有附加目标进程时，查看器仅显示提示，不尝试读内存。
    if (m_attachedProcessHandle == nullptr)
    {
        m_hexTableProgrammaticUpdate = true;
        m_hexTable->clearContents();
        m_hexTable->setRowCount(kHexRowCount);
        m_hexTableProgrammaticUpdate = false;
        m_viewProtectLabel->setText("保护属性: -");
        m_viewerStatusLabel->setText("未附加进程。");
        return;
    }

    // 每页固定读取 512 字节，兼顾可读性与刷新性能。
    QByteArray pageBytes(static_cast<int>(kHexPageBytes), '\0');
    SIZE_T bytesRead = 0;
    const BOOL readOk = ::ReadProcessMemory(
        m_attachedProcessHandle,
        reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(m_currentViewerAddress)),
        pageBytes.data(),
        static_cast<SIZE_T>(kHexPageBytes),
        &bytesRead);
    if (readOk == FALSE || bytesRead == 0)
    {
        m_currentViewerPageBytes.clear();
        m_hexTableProgrammaticUpdate = true;
        m_hexTable->clearContents();
        m_hexTable->setRowCount(kHexRowCount);
        m_hexTableProgrammaticUpdate = false;
        m_viewerStatusLabel->setText(
            QString("读取失败：地址=%1，错误码=%2")
            .arg(formatAddress(m_currentViewerAddress))
            .arg(::GetLastError()));
        return;
    }

    m_currentViewerPageBytes = pageBytes.left(static_cast<int>(bytesRead));

    // 用 QSignalBlocker + 标志位双保险，避免 setItem 时触发 itemChanged 回写。
    m_hexTableProgrammaticUpdate = true;
    QSignalBlocker signalBlocker(m_hexTable);
    m_hexTable->clearContents();
    m_hexTable->setRowCount(kHexRowCount);

    for (int row = 0; row < kHexRowCount; ++row)
    {
        const std::uint64_t rowBaseAddress =
            m_currentViewerAddress + static_cast<std::uint64_t>(row * kHexBytesPerRow);
        QTableWidgetItem* rowAddressItem = new QTableWidgetItem(formatAddress(rowBaseAddress));
        rowAddressItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_hexTable->setItem(row, 0, rowAddressItem);

        QString asciiText;
        asciiText.reserve(kHexBytesPerRow);

        for (int column = 0; column < kHexBytesPerRow; ++column)
        {
            const int byteIndex = row * kHexBytesPerRow + column;
            QTableWidgetItem* byteItem = nullptr;

            if (byteIndex < m_currentViewerPageBytes.size())
            {
                const unsigned char byteValue = static_cast<unsigned char>(m_currentViewerPageBytes.at(byteIndex));
                byteItem = new QTableWidgetItem(QString("%1").arg(byteValue, 2, 16, QChar('0')).toUpper());

                // 仅在可写句柄场景下允许直接编辑十六进制单元格。
                Qt::ItemFlags itemFlags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
                if (m_canReadWriteMemory)
                {
                    itemFlags |= Qt::ItemIsEditable;
                }
                byteItem->setFlags(itemFlags);

                // ASCII 区域仅显示可打印字符，不可打印统一替换为 '.'。
                if (byteValue >= 32 && byteValue <= 126)
                {
                    asciiText.push_back(QChar(byteValue));
                }
                else
                {
                    asciiText.push_back('.');
                }
            }
            else
            {
                byteItem = new QTableWidgetItem("--");
                byteItem->setFlags(Qt::ItemIsEnabled);
                asciiText.push_back(' ');
            }

            m_hexTable->setItem(row, column + 1, byteItem);
        }

        QTableWidgetItem* asciiItem = new QTableWidgetItem(asciiText);
        asciiItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_hexTable->setItem(row, kHexBytesPerRow + 1, asciiItem);
    }

    m_hexTableProgrammaticUpdate = false;

    // 更新当前地址保护属性显示，帮助用户判断是否可写/可执行。
    MEMORY_BASIC_INFORMATION mbi{};
    const SIZE_T querySize = ::VirtualQueryEx(
        m_attachedProcessHandle,
        reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(m_currentViewerAddress)),
        &mbi,
        sizeof(mbi));
    if (querySize == sizeof(mbi))
    {
        m_viewProtectLabel->setText(
            QString("保护属性: %1 | 状态: %2 | 类型: %3")
            .arg(protectToText(static_cast<std::uint32_t>(mbi.Protect)))
            .arg(stateToText(static_cast<std::uint32_t>(mbi.State)))
            .arg(typeToText(static_cast<std::uint32_t>(mbi.Type))));
    }
    else
    {
        m_viewProtectLabel->setText("保护属性: (查询失败)");
    }

    m_viewerStatusLabel->setText(
        QString("地址 %1 读取 %2 字节。")
        .arg(formatAddress(m_currentViewerAddress))
        .arg(bytesRead));
}

bool MemoryDock::writeSingleByteAtViewer(
    const std::uint64_t absoluteAddress,
    const std::uint8_t value,
    QString& errorTextOut)
{
    // 未附加进程或句柄只读时，禁止写入并明确返回失败原因。
    if (m_attachedProcessHandle == nullptr)
    {
        errorTextOut = "当前未附加进程，无法写入。";
        return false;
    }
    if (!m_canReadWriteMemory)
    {
        errorTextOut = "当前句柄为只读权限，无法写入内存。";
        return false;
    }

    SIZE_T bytesWritten = 0;
    const BOOL writeOk = ::WriteProcessMemory(
        m_attachedProcessHandle,
        reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(absoluteAddress)),
        &value,
        sizeof(value),
        &bytesWritten);
    if (writeOk == FALSE || bytesWritten != sizeof(value))
    {
        errorTextOut = QString("WriteProcessMemory 失败，错误码=%1").arg(::GetLastError());
        return false;
    }

    return true;
}

bool MemoryDock::addBreakpointByAddress(
    const std::uint64_t address,
    const QString& description,
    QString& errorTextOut)
{
    // 软件断点依赖 0xCC 写入，因此必须具备写内存权限。
    if (m_attachedProcessHandle == nullptr)
    {
        errorTextOut = "请先附加进程。";
        return false;
    }
    if (!m_canReadWriteMemory)
    {
        errorTextOut = "当前为只读句柄，无法设置断点。";
        return false;
    }

    // 避免重复写入相同地址断点，提升断点缓存一致性。
    for (const BreakpointEntry& cachedBp : m_breakpointCache)
    {
        if (cachedBp.address == address)
        {
            errorTextOut = "该地址已存在断点。";
            return false;
        }
    }

    std::uint8_t originalByte = 0;
    SIZE_T bytesRead = 0;
    const BOOL readOk = ::ReadProcessMemory(
        m_attachedProcessHandle,
        reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)),
        &originalByte,
        sizeof(originalByte),
        &bytesRead);
    if (readOk == FALSE || bytesRead != sizeof(originalByte))
    {
        errorTextOut = QString("读取原始字节失败，错误码=%1").arg(::GetLastError());
        return false;
    }

    const std::uint8_t int3Byte = 0xCC;
    SIZE_T bytesWritten = 0;
    const BOOL writeOk = ::WriteProcessMemory(
        m_attachedProcessHandle,
        reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(address)),
        &int3Byte,
        sizeof(int3Byte),
        &bytesWritten);
    if (writeOk == FALSE || bytesWritten != sizeof(int3Byte))
    {
        errorTextOut = QString("写入 0xCC 失败，错误码=%1").arg(::GetLastError());
        return false;
    }

    BreakpointEntry bpEntry{};
    bpEntry.address = address;
    bpEntry.originalByte = originalByte;
    bpEntry.enabled = true;
    bpEntry.hitCount = 0;
    bpEntry.description = description;
    m_breakpointCache.push_back(std::move(bpEntry));
    return true;
}

bool MemoryDock::removeBreakpointByRow(const int row)
{
    if (row < 0 || row >= static_cast<int>(m_breakpointCache.size()))
    {
        return false;
    }

    // 删除前若断点处于启用状态，先恢复原字节，避免目标进程留脏。
    if (m_breakpointCache[static_cast<std::size_t>(row)].enabled)
    {
        if (!setBreakpointEnabledByRow(row, false))
        {
            return false;
        }
    }

    m_breakpointCache.erase(m_breakpointCache.begin() + row);
    return true;
}

bool MemoryDock::setBreakpointEnabledByRow(const int row, const bool enabled)
{
    if (row < 0 || row >= static_cast<int>(m_breakpointCache.size()))
    {
        return false;
    }
    if (m_attachedProcessHandle == nullptr || !m_canReadWriteMemory)
    {
        return false;
    }

    BreakpointEntry& bpEntry = m_breakpointCache[static_cast<std::size_t>(row)];
    if (bpEntry.enabled == enabled)
    {
        return true;
    }

    const std::uint8_t targetByte = enabled ? 0xCC : bpEntry.originalByte;
    SIZE_T bytesWritten = 0;
    const BOOL writeOk = ::WriteProcessMemory(
        m_attachedProcessHandle,
        reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(bpEntry.address)),
        &targetByte,
        sizeof(targetByte),
        &bytesWritten);
    if (writeOk == FALSE || bytesWritten != sizeof(targetByte))
    {
        return false;
    }

    bpEntry.enabled = enabled;
    return true;
}

void MemoryDock::rebuildBreakpointTable()
{
    // 断点表直接由 m_breakpointCache 投影，避免 UI 与业务数据分叉。
    m_breakpointTable->setRowCount(static_cast<int>(m_breakpointCache.size()));
    for (int row = 0; row < static_cast<int>(m_breakpointCache.size()); ++row)
    {
        const BreakpointEntry& entry = m_breakpointCache[static_cast<std::size_t>(row)];
        m_breakpointTable->setItem(row, 0, new QTableWidgetItem(formatAddress(entry.address)));
        m_breakpointTable->setItem(row, 1, new QTableWidgetItem(
            QString("0x%1").arg(entry.originalByte, 2, 16, QChar('0')).toUpper()));
        m_breakpointTable->setItem(row, 2, new QTableWidgetItem(entry.enabled ? "启用" : "禁用"));
        m_breakpointTable->setItem(row, 3, new QTableWidgetItem(QString::number(entry.hitCount)));
        m_breakpointTable->setItem(row, 4, new QTableWidgetItem(entry.description));
    }
}

void MemoryDock::addBookmarkByAddress(const std::uint64_t address, const QString& noteText)
{
    // 已存在同地址书签时仅更新备注，不重复插入。
    for (BookmarkEntry& bookmark : m_bookmarkCache)
    {
        if (bookmark.address == address)
        {
            bookmark.noteText = noteText;
            return;
        }
    }

    BookmarkEntry bookmark{};
    bookmark.address = address;
    bookmark.noteText = noteText;
    bookmark.addTimeText = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");

    // 添加时尽力读取一份初始值，失败不阻断添加流程。
    if (m_attachedProcessHandle != nullptr)
    {
        QByteArray initialBytes(8, '\0');
        SIZE_T bytesRead = 0;
        if (::ReadProcessMemory(
            m_attachedProcessHandle,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)),
            initialBytes.data(),
            static_cast<SIZE_T>(initialBytes.size()),
            &bytesRead) != FALSE && bytesRead > 0)
        {
            bookmark.lastValueBytes = initialBytes.left(static_cast<int>(bytesRead));
        }
    }

    m_bookmarkCache.push_back(std::move(bookmark));
}

void MemoryDock::rebuildBookmarkTable()
{
    // 书签表每次全量重建，逻辑清晰且数量通常不大，维护成本最低。
    m_bookmarkTable->setRowCount(static_cast<int>(m_bookmarkCache.size()));
    for (int row = 0; row < static_cast<int>(m_bookmarkCache.size()); ++row)
    {
        const BookmarkEntry& bookmark = m_bookmarkCache[static_cast<std::size_t>(row)];
        m_bookmarkTable->setItem(row, 0, new QTableWidgetItem(formatAddress(bookmark.address)));

        // 书签“当前值”默认按十六进制字节串展示，通用于未知变量类型。
        const QString valueText = bytesToDisplayString(bookmark.lastValueBytes, SearchValueType::ByteArray);
        m_bookmarkTable->setItem(row, 1, new QTableWidgetItem(valueText));
        m_bookmarkTable->setItem(row, 2, new QTableWidgetItem(bookmark.noteText));
        m_bookmarkTable->setItem(row, 3, new QTableWidgetItem(bookmark.addTimeText));
    }
}

void MemoryDock::refreshBookmarkValues()
{
    // 未附加进程或没有书签时无需刷新，直接返回避免无意义系统调用。
    if (m_attachedProcessHandle == nullptr || m_bookmarkCache.empty())
    {
        return;
    }

    for (BookmarkEntry& bookmark : m_bookmarkCache)
    {
        const int requestLength = std::max<int>(8, bookmark.lastValueBytes.size());
        QByteArray valueBytes(requestLength, '\0');
        SIZE_T bytesRead = 0;
        const BOOL readOk = ::ReadProcessMemory(
            m_attachedProcessHandle,
            reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(bookmark.address)),
            valueBytes.data(),
            static_cast<SIZE_T>(valueBytes.size()),
            &bytesRead);
        if (readOk != FALSE && bytesRead > 0)
        {
            bookmark.lastValueBytes = valueBytes.left(static_cast<int>(bytesRead));
        }
    }

    // 刷新完成后重建表格，保证用户看到的是最新读回值。
    rebuildBookmarkTable();
}

void MemoryDock::updateStatusBarText()
{
    // 状态栏由三段组成：进程名、PID、读写状态，任何状态变化都统一经此函数刷新。
    if (m_attachedPid == 0 || m_attachedProcessHandle == nullptr)
    {
        m_statusProcessLabel->setText("进程: 未附加");
        m_statusPidLabel->setText("PID: -");
        m_statusMemoryIoLabel->setText("内存读写: 未就绪");
        return;
    }

    m_statusProcessLabel->setText(QString("进程: %1").arg(m_attachedProcessName));
    m_statusPidLabel->setText(QString("PID: %1").arg(m_attachedPid));
    m_statusMemoryIoLabel->setText(
        QString("内存读写: %1").arg(m_canReadWriteMemory ? "可读可写" : "只读"));
}

bool MemoryDock::parseAddressText(const QString& text, std::uint64_t& valueOut)
{
    // 地址解析与通用无符号整数解析共享一套规则。
    return parseUnsignedNumber(text, valueOut);
}

bool MemoryDock::parseUnsignedNumber(const QString& text, std::uint64_t& valueOut)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
    {
        return false;
    }

    bool parseOk = false;
    qulonglong parsedValue = 0;

    // 优先处理 0x 前缀（十六进制）形式。
    if (trimmed.startsWith("0x", Qt::CaseInsensitive))
    {
        parsedValue = trimmed.mid(2).toULongLong(&parseOk, 16);
        if (!parseOk)
        {
            return false;
        }
        valueOut = static_cast<std::uint64_t>(parsedValue);
        return true;
    }

    // 再尝试十进制；失败后再尝试“无前缀十六进制”以兼容输入习惯。
    parsedValue = trimmed.toULongLong(&parseOk, 10);
    if (!parseOk)
    {
        parsedValue = trimmed.toULongLong(&parseOk, 16);
        if (!parseOk)
        {
            return false;
        }
    }

    valueOut = static_cast<std::uint64_t>(parsedValue);
    return true;
}

QString MemoryDock::formatAddress(const std::uint64_t address)
{
    // 统一输出 16 位十六进制，便于 32/64 位地址在表格中对齐阅读。
    const QString hexText = QString("%1").arg(
        static_cast<qulonglong>(address),
        16,
        16,
        QChar('0')).toUpper();
    return QString("0x%1").arg(hexText);
}

QString MemoryDock::formatSize(const std::uint64_t sizeBytes)
{
    // 字节数可读化显示：B / KB / MB / GB 自动切换，保留两位小数。
    constexpr double kKB = 1024.0;
    constexpr double kMB = 1024.0 * 1024.0;
    constexpr double kGB = 1024.0 * 1024.0 * 1024.0;

    const double sizeValue = static_cast<double>(sizeBytes);
    if (sizeValue >= kGB)
    {
        return QString("%1 GB").arg(sizeValue / kGB, 0, 'f', 2);
    }
    if (sizeValue >= kMB)
    {
        return QString("%1 MB").arg(sizeValue / kMB, 0, 'f', 2);
    }
    if (sizeValue >= kKB)
    {
        return QString("%1 KB").arg(sizeValue / kKB, 0, 'f', 2);
    }
    return QString("%1 B").arg(sizeBytes);
}

QString MemoryDock::protectToText(const std::uint32_t protect)
{
    // PAGE_* 主值仅保留低 8 位；高位是 PAGE_GUARD/NOCACHE 等修饰位。
    const std::uint32_t baseProtect = protect & 0xFF;
    QString baseText;
    switch (baseProtect)
    {
    case PAGE_NOACCESS:          baseText = "---"; break;
    case PAGE_READONLY:          baseText = "R--"; break;
    case PAGE_READWRITE:         baseText = "RW-"; break;
    case PAGE_WRITECOPY:         baseText = "RC-"; break;
    case PAGE_EXECUTE:           baseText = "--X"; break;
    case PAGE_EXECUTE_READ:      baseText = "R-X"; break;
    case PAGE_EXECUTE_READWRITE: baseText = "RWX"; break;
    case PAGE_EXECUTE_WRITECOPY: baseText = "RCX"; break;
    default:                     baseText = "UNK"; break;
    }

    // 叠加修饰标记，帮助用户识别 Guard/NoCache/WriteCombine 特性。
    if ((protect & PAGE_GUARD) != 0)
    {
        baseText += "|G";
    }
    if ((protect & PAGE_NOCACHE) != 0)
    {
        baseText += "|NC";
    }
    if ((protect & PAGE_WRITECOMBINE) != 0)
    {
        baseText += "|WC";
    }

    return baseText;
}

QString MemoryDock::stateToText(const std::uint32_t state)
{
    switch (state)
    {
    case MEM_COMMIT:  return "MEM_COMMIT";
    case MEM_RESERVE: return "MEM_RESERVE";
    case MEM_FREE:    return "MEM_FREE";
    default:          return QString("UNKNOWN(0x%1)").arg(state, 0, 16);
    }
}

QString MemoryDock::typeToText(const std::uint32_t type)
{
    switch (type)
    {
    case MEM_IMAGE:   return "IMAGE";
    case MEM_MAPPED:  return "MAPPED";
    case MEM_PRIVATE: return "PRIVATE";
    case 0:           return "-";
    default:          return QString("UNKNOWN(0x%1)").arg(type, 0, 16);
    }
}

QString MemoryDock::bytesToDisplayString(const QByteArray& bytes, const SearchValueType valueType)
{
    // 空字节统一显示短横线，避免表格出现空白造成歧义。
    if (bytes.isEmpty())
    {
        return "-";
    }

    switch (valueType)
    {
    case SearchValueType::Byte:
    {
        if (bytes.size() < static_cast<int>(sizeof(std::uint8_t))) return "-";
        std::uint8_t value = 0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return QString("%1 (0x%2)")
            .arg(value)
            .arg(value, 2, 16, QChar('0')).toUpper();
    }
    case SearchValueType::Int16:
    {
        if (bytes.size() < static_cast<int>(sizeof(std::int16_t))) return "-";
        std::int16_t value = 0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return QString::number(value);
    }
    case SearchValueType::Int32:
    {
        if (bytes.size() < static_cast<int>(sizeof(std::int32_t))) return "-";
        std::int32_t value = 0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return QString::number(value);
    }
    case SearchValueType::Int64:
    {
        if (bytes.size() < static_cast<int>(sizeof(std::int64_t))) return "-";
        std::int64_t value = 0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return QString::number(value);
    }
    case SearchValueType::Float32:
    {
        if (bytes.size() < static_cast<int>(sizeof(float))) return "-";
        float value = 0.0f;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return QString::number(value, 'f', 6);
    }
    case SearchValueType::Float64:
    {
        if (bytes.size() < static_cast<int>(sizeof(double))) return "-";
        double value = 0.0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return QString::number(value, 'f', 8);
    }
    case SearchValueType::StringAscii:
    {
        return QString::fromLatin1(bytes);
    }
    case SearchValueType::StringUnicode:
    {
        // UTF-16 字节长度需为偶数，不足时截断最后 1 字节防止越界。
        const int alignedLength = bytes.size() - (bytes.size() % 2);
        if (alignedLength <= 0)
        {
            return "-";
        }
        return QString::fromUtf16(
            reinterpret_cast<const char16_t*>(bytes.constData()),
            alignedLength / 2);
    }
    case SearchValueType::ByteArray:
    default:
    {
        // 默认按十六进制字节串展示，兼容未知类型与书签显示。
        QStringList parts;
        parts.reserve(bytes.size());
        for (int index = 0; index < bytes.size(); ++index)
        {
            const auto byteValue = static_cast<unsigned char>(bytes.at(index));
            parts.push_back(QString("%1").arg(byteValue, 2, 16, QChar('0')).toUpper());
        }
        return parts.join(' ');
    }
    }
}
