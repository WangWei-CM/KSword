
#include "KernelDock.h"

// ============================================================
// KernelDock.cpp
// 作用说明：
// 1) 实现内核 Dock 的三页 UI（对象命名空间 / 原子表 / 历史 NtQuery）；
// 2) 实现异步刷新、筛选、详情联动与右键菜单；
// 3) 具体底层枚举逻辑放在 Worker 文件，当前文件仅做界面和交互编排。
// ============================================================

#include "../UI/CodeEditorWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace
{
    // blueButtonStyle：
    // - 作用：统一图标按钮样式（带悬停与按下态）。
    QString blueButtonStyle()
    {
        return QStringLiteral(
            "QPushButton{color:%1;background:%5;border:1px solid %2;border-radius:3px;padding:3px 8px;}"
            "QPushButton:hover{background:%3;color:#FFFFFF;border:1px solid %3;}"
            "QPushButton:pressed{background:%4;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(QStringLiteral("#2E8BFF"))
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(KswordTheme::SurfaceHex());
    }

    // blueInputStyle：
    // - 作用：统一筛选输入框样式。
    QString blueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %2;border-radius:3px;background:%3;color:%4;padding:2px 6px;}"
            "QLineEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    // headerStyle：
    // - 作用：统一表头样式，强化列标题可读性。
    QString headerStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{color:%1;background:%2;border:1px solid %3;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    // itemSelectionStyle：
    // - 作用：统一表格/树控件选中高亮为主题蓝，避免系统默认配色差异。
    QString itemSelectionStyle()
    {
        return QStringLiteral(
            "QTableWidget::item:selected{background:#2E8BFF;color:#FFFFFF;}"
            "QTreeWidget::item:selected{background:#2E8BFF;color:#FFFFFF;}");
    }

    // statusLabelStyle：
    // - 作用：统一状态标签的颜色与字重。
    QString statusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    // ObjectNamespaceColumn：对象命名空间树列索引。
    enum class ObjectNamespaceColumn : int
    {
        Name = 0,
        Type,
        PathOrScope,
        Status,
        SymbolicTarget,
        Count
    };

    // AtomColumn：原子表列索引。
    enum class AtomColumn : int
    {
        Value = 0,
        Hex,
        Name,
        Source,
        Status,
        Count
    };

    // NtQueryColumn：历史 NtQuery 表列索引。
    enum class NtQueryColumn : int
    {
        Category = 0,
        Function,
        QueryItem,
        Status,
        Summary,
        Count
    };
}

KernelDock::KernelDock(QWidget* parent)
    : QWidget(parent)
{
    kLogEvent initEvent;
    info << initEvent << "[KernelDock] 构造开始，准备初始化三页内核视图。" << eol;

    initializeUi();
    initializeConnections();

    // 默认先刷新新功能页，再刷新历史页。
    refreshObjectNamespaceAsync();
    refreshAtomTableAsync();
    refreshNtQueryAsync();

    info << initEvent << "[KernelDock] 构造完成。" << eol;
}

void KernelDock::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(6);

    m_tabWidget = new QTabWidget(this);
    m_rootLayout->addWidget(m_tabWidget, 1);

    initializeObjectNamespaceTab();
    initializeAtomTableTab();
    initializeNtQueryTab();

    m_tabWidget->setCurrentWidget(m_objectNamespacePage);
}

void KernelDock::initializeObjectNamespaceTab()
{
    m_objectNamespacePage = new QWidget(m_tabWidget);
    m_objectNamespaceLayout = new QVBoxLayout(m_objectNamespacePage);
    m_objectNamespaceLayout->setContentsMargins(4, 4, 4, 4);
    m_objectNamespaceLayout->setSpacing(6);

    m_objectNamespaceToolLayout = new QHBoxLayout();
    m_objectNamespaceToolLayout->setContentsMargins(0, 0, 0, 0);
    m_objectNamespaceToolLayout->setSpacing(6);

    m_refreshObjectNamespaceButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_objectNamespacePage);
    m_refreshObjectNamespaceButton->setToolTip(QStringLiteral("刷新对象命名空间枚举结果"));
    m_refreshObjectNamespaceButton->setStyleSheet(blueButtonStyle());
    m_refreshObjectNamespaceButton->setFixedWidth(34);

    m_objectNamespaceFilterEdit = new QLineEdit(m_objectNamespacePage);
    m_objectNamespaceFilterEdit->setPlaceholderText(QStringLiteral("按根目录/目录路径/对象名/对象类型/状态筛选"));
    m_objectNamespaceFilterEdit->setToolTip(QStringLiteral("输入关键字后实时过滤对象命名空间树"));
    m_objectNamespaceFilterEdit->setClearButtonEnabled(true);
    m_objectNamespaceFilterEdit->setStyleSheet(blueInputStyle());

    m_objectNamespaceStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_objectNamespacePage);
    m_objectNamespaceStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_objectNamespaceToolLayout->addWidget(m_refreshObjectNamespaceButton, 0);
    m_objectNamespaceToolLayout->addWidget(m_objectNamespaceFilterEdit, 1);
    m_objectNamespaceToolLayout->addWidget(m_objectNamespaceStatusLabel, 0);
    m_objectNamespaceLayout->addLayout(m_objectNamespaceToolLayout);

    QSplitter* verticalSplitter = new QSplitter(Qt::Vertical, m_objectNamespacePage);
    m_objectNamespaceLayout->addWidget(verticalSplitter, 1);

    QSplitter* horizontalSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);

    m_objectNamespaceTree = new QTreeWidget(horizontalSplitter);
    m_objectNamespaceTree->setColumnCount(static_cast<int>(ObjectNamespaceColumn::Count));
    m_objectNamespaceTree->setHeaderLabels(QStringList{
        QStringLiteral("名称"),
        QStringLiteral("类型"),
        QStringLiteral("路径/说明"),
        QStringLiteral("状态"),
        QStringLiteral("符号链接目标")
        });
    m_objectNamespaceTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_objectNamespaceTree->setAlternatingRowColors(true);
    m_objectNamespaceTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_objectNamespaceTree->setStyleSheet(itemSelectionStyle());
    m_objectNamespaceTree->setUniformRowHeights(true);
    m_objectNamespaceTree->setRootIsDecorated(true);
    m_objectNamespaceTree->header()->setStyleSheet(headerStyle());
    // 列宽策略：始终按可用宽度自适应，避免出现横向滚动条。
    m_objectNamespaceTree->header()->setSectionResizeMode(QHeaderView::Stretch);
    m_objectNamespaceTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_objectNamespaceTree->setToolTip(QStringLiteral("文件管理器式对象命名空间树，支持逐级展开与右键操作"));

    m_objectNamespacePropertyTable = new QTableWidget(horizontalSplitter);
    m_objectNamespacePropertyTable->setColumnCount(2);
    m_objectNamespacePropertyTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("属性项"),
        QStringLiteral("值")
        });
    m_objectNamespacePropertyTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_objectNamespacePropertyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_objectNamespacePropertyTable->setAlternatingRowColors(true);
    m_objectNamespacePropertyTable->setStyleSheet(itemSelectionStyle());
    m_objectNamespacePropertyTable->setCornerButtonEnabled(false);
    m_objectNamespacePropertyTable->verticalHeader()->setVisible(false);
    m_objectNamespacePropertyTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_objectNamespacePropertyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_objectNamespacePropertyTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_objectNamespacePropertyTable->setColumnWidth(0, 220);
    m_objectNamespacePropertyTable->setToolTip(QStringLiteral("当前选中节点的字段详情（字段名 + 字段值）"));

    m_objectNamespaceDetailEditor = new CodeEditorWidget(verticalSplitter);
    m_objectNamespaceDetailEditor->setReadOnly(true);
    m_objectNamespaceDetailEditor->setText(QStringLiteral("请选择对象命名空间节点查看详情。"));

    horizontalSplitter->setStretchFactor(0, 3);
    horizontalSplitter->setStretchFactor(1, 2);
    verticalSplitter->setStretchFactor(0, 4);
    verticalSplitter->setStretchFactor(1, 2);

    const int objectNamespaceTabIndex = m_tabWidget->addTab(
        m_objectNamespacePage,
        QIcon(":/Icon/process_tree.svg"),
        QStringLiteral("对象命名空间"));
    m_tabWidget->setTabToolTip(objectNamespaceTabIndex, QStringLiteral("对象管理器命名空间遍历（默认页）"));
}

void KernelDock::initializeAtomTableTab()
{
    m_atomPage = new QWidget(m_tabWidget);
    m_atomLayout = new QVBoxLayout(m_atomPage);
    m_atomLayout->setContentsMargins(4, 4, 4, 4);
    m_atomLayout->setSpacing(6);

    m_atomToolLayout = new QHBoxLayout();
    m_atomToolLayout->setContentsMargins(0, 0, 0, 0);
    m_atomToolLayout->setSpacing(6);

    m_refreshAtomButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_atomPage);
    m_refreshAtomButton->setToolTip(QStringLiteral("刷新原子表遍历结果"));
    m_refreshAtomButton->setStyleSheet(blueButtonStyle());
    m_refreshAtomButton->setFixedWidth(34);

    m_atomFilterEdit = new QLineEdit(m_atomPage);
    m_atomFilterEdit->setPlaceholderText(QStringLiteral("按 Atom 值/十六进制/名称/来源筛选"));
    m_atomFilterEdit->setToolTip(QStringLiteral("输入关键字后实时过滤原子表"));
    m_atomFilterEdit->setClearButtonEnabled(true);
    m_atomFilterEdit->setStyleSheet(blueInputStyle());

    m_atomStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_atomPage);
    m_atomStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_atomToolLayout->addWidget(m_refreshAtomButton, 0);
    m_atomToolLayout->addWidget(m_atomFilterEdit, 1);
    m_atomToolLayout->addWidget(m_atomStatusLabel, 0);
    m_atomLayout->addLayout(m_atomToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_atomPage);
    m_atomLayout->addWidget(splitter, 1);

    m_atomTable = new QTableWidget(splitter);
    m_atomTable->setColumnCount(static_cast<int>(AtomColumn::Count));
    m_atomTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("Atom值"),
        QStringLiteral("十六进制"),
        QStringLiteral("名称"),
        QStringLiteral("来源"),
        QStringLiteral("状态")
        });
    m_atomTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_atomTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_atomTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_atomTable->setAlternatingRowColors(true);
    m_atomTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_atomTable->setStyleSheet(itemSelectionStyle());
    m_atomTable->setCornerButtonEnabled(false);
    m_atomTable->verticalHeader()->setVisible(false);
    m_atomTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_atomTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_atomTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(AtomColumn::Name), QHeaderView::Stretch);
    m_atomTable->setColumnWidth(static_cast<int>(AtomColumn::Value), 110);
    m_atomTable->setColumnWidth(static_cast<int>(AtomColumn::Hex), 110);
    m_atomTable->setColumnWidth(static_cast<int>(AtomColumn::Source), 220);
    m_atomTable->setColumnWidth(static_cast<int>(AtomColumn::Status), 160);

    m_atomDetailEditor = new CodeEditorWidget(splitter);
    m_atomDetailEditor->setReadOnly(true);
    m_atomDetailEditor->setText(QStringLiteral("请选择一条原子记录查看详情。"));

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    const int atomTabIndex = m_tabWidget->addTab(
        m_atomPage,
        QIcon(":/Icon/process_threads.svg"),
        QStringLiteral("原子表遍历"));
    m_tabWidget->setTabToolTip(atomTabIndex, QStringLiteral("遍历全局原子范围并提供校验操作"));
}

void KernelDock::initializeNtQueryTab()
{
    m_ntQueryPage = new QWidget(m_tabWidget);
    m_ntQueryLayout = new QVBoxLayout(m_ntQueryPage);
    m_ntQueryLayout->setContentsMargins(4, 4, 4, 4);
    m_ntQueryLayout->setSpacing(6);

    m_ntQueryToolLayout = new QHBoxLayout();
    m_ntQueryToolLayout->setContentsMargins(0, 0, 0, 0);
    m_ntQueryToolLayout->setSpacing(6);

    m_refreshNtQueryButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_ntQueryPage);
    m_refreshNtQueryButton->setToolTip(QStringLiteral("刷新历史 NtQuery 信息"));
    m_refreshNtQueryButton->setStyleSheet(blueButtonStyle());
    m_refreshNtQueryButton->setFixedWidth(34);

    m_ntQueryStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_ntQueryPage);
    m_ntQueryStatusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_ntQueryToolLayout->addWidget(m_refreshNtQueryButton, 0);
    m_ntQueryToolLayout->addWidget(m_ntQueryStatusLabel, 1);
    m_ntQueryLayout->addLayout(m_ntQueryToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_ntQueryPage);
    m_ntQueryLayout->addWidget(splitter, 1);

    m_ntQueryTable = new QTableWidget(splitter);
    m_ntQueryTable->setColumnCount(static_cast<int>(NtQueryColumn::Count));
    m_ntQueryTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("类别"),
        QStringLiteral("函数"),
        QStringLiteral("查询项"),
        QStringLiteral("状态"),
        QStringLiteral("摘要")
        });
    m_ntQueryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ntQueryTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_ntQueryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_ntQueryTable->setAlternatingRowColors(true);
    m_ntQueryTable->setStyleSheet(itemSelectionStyle());
    m_ntQueryTable->setCornerButtonEnabled(false);
    m_ntQueryTable->verticalHeader()->setVisible(false);
    m_ntQueryTable->horizontalHeader()->setStyleSheet(headerStyle());
    m_ntQueryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_ntQueryTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(NtQueryColumn::Summary), QHeaderView::Stretch);

    m_ntQueryDetailEditor = new CodeEditorWidget(splitter);
    m_ntQueryDetailEditor->setReadOnly(true);
    m_ntQueryDetailEditor->setText(QStringLiteral("请选择一条 NtQuery 结果查看详情。"));

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    const int ntQueryTabIndex = m_tabWidget->addTab(
        m_ntQueryPage,
        QIcon(":/Icon/process_details.svg"),
        QStringLiteral("历史NtQuery"));
    m_tabWidget->setTabToolTip(ntQueryTabIndex, QStringLiteral("旧版内核 NtQuery 信息页"));
}

void KernelDock::initializeConnections()
{
    // 对象命名空间页连接：刷新、筛选、详情联动、右键菜单。
    connect(m_refreshObjectNamespaceButton, &QPushButton::clicked, this, [this]() {
        refreshObjectNamespaceAsync();
    });
    connect(m_objectNamespaceFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterText) {
        rebuildObjectNamespaceTable(filterText.trimmed());
    });
    connect(m_objectNamespaceTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) {
        showObjectNamespaceDetailByCurrentRow();
    });
    connect(m_objectNamespaceTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showObjectNamespaceContextMenu(localPosition);
    });

    // 原子表页连接：刷新、筛选、详情联动、右键菜单。
    connect(m_refreshAtomButton, &QPushButton::clicked, this, [this]() {
        refreshAtomTableAsync();
    });
    connect(m_atomFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterText) {
        rebuildAtomTable(filterText.trimmed());
    });
    connect(m_atomTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showAtomDetailByCurrentRow();
    });
    connect(m_atomTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showAtomContextMenu(localPosition);
    });

    // 历史 NtQuery 页连接：刷新与详情联动。
    connect(m_refreshNtQueryButton, &QPushButton::clicked, this, [this]() {
        refreshNtQueryAsync();
    });
    connect(m_ntQueryTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showNtQueryDetailByCurrentRow();
    });
}

