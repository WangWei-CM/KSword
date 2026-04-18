#include "RegistryDock.h"

// ============================================================
// RegistryDock.cpp
// 说明：
// 1) 提供类 regedit 的键树导航和键值编辑；
// 2) 支持导入/导出 .reg；
// 3) 支持后台搜索，避免阻塞 UI。
// ============================================================

#include "../theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace
{
    // 统一按钮风格：与主界面保持同一主题。
    QString blueButtonStyle()
    {
        return QStringLiteral(
            "QPushButton{color:%1;background:#FFFFFF;border:1px solid %2;border-radius:3px;padding:3px 8px;}"
            "QPushButton:hover{background:%3;}"
            "QPushButton:pressed{background:%4;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueHoverHex)
            .arg(KswordTheme::PrimaryBluePressedHex);
    }

    // 统一输入框风格：路径栏、搜索栏复用同一套样式。
    QString blueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid #C8DDF4;border-radius:3px;background:#FFFFFF;padding:2px 6px;}"
            "QLineEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // 表头风格：提升信息密集列表的可读性。
    QString blueHeaderStyle()
    {
        return QStringLiteral("QHeaderView::section{color:%1;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    // TreeItem 角色常量：保存路径和懒加载状态。
    constexpr int kRolePath = Qt::UserRole + 1;
    constexpr int kRoleLoaded = Qt::UserRole + 2;
    constexpr int kRolePlaceholder = Qt::UserRole + 3;

    // 根键映射结构：支持全名与缩写两种输入。
    struct RootEntry
    {
        const wchar_t* fullName = nullptr;
        const wchar_t* shortName = nullptr;
        HKEY root = nullptr;
    };

    const std::array<RootEntry, 5> kRootMap{
        RootEntry{ L"HKEY_CLASSES_ROOT", L"HKCR", HKEY_CLASSES_ROOT },
        RootEntry{ L"HKEY_CURRENT_USER", L"HKCU", HKEY_CURRENT_USER },
        RootEntry{ L"HKEY_LOCAL_MACHINE", L"HKLM", HKEY_LOCAL_MACHINE },
        RootEntry{ L"HKEY_USERS", L"HKU", HKEY_USERS },
        RootEntry{ L"HKEY_CURRENT_CONFIG", L"HKCC", HKEY_CURRENT_CONFIG }
    };

    // trimDefaultValueName：界面“默认值”映射为 WinAPI 空名字。
    QString trimDefaultValueName(const QString& valueName)
    {
        const QString trimmed = valueName.trimmed();
        if (trimmed.isEmpty() || trimmed == QStringLiteral("(默认)"))
        {
            return QString();
        }
        return trimmed;
    }

    // bytesToHex：把二进制输出为十六进制字符串。
    QString bytesToHex(const QByteArray& bytes, int maxCount)
    {
        QStringList parts;
        const int showCount = std::min<int>(maxCount, bytes.size());
        for (int i = 0; i < showCount; ++i)
        {
            parts << QStringLiteral("%1").arg(static_cast<unsigned char>(bytes.at(i)), 2, 16, QLatin1Char('0')).toUpper();
        }
        if (bytes.size() > showCount)
        {
            parts << QStringLiteral("...");
        }
        return parts.join(' ');
    }
}

RegistryDock::RegistryDock(QWidget* parent)
    : QWidget(parent)
{
    {
        kLogEvent event;
        info << event << "[RegistryDock] 构造开始，准备初始化注册表模块。" << eol;
    }

    initializeUi();
    initializeConnections();
    initializeRootItems();
    navigateToPath(QStringLiteral("HKEY_CURRENT_USER"), true);

    {
        kLogEvent event;
        info << event << "[RegistryDock] 构造完成，默认定位到 HKEY_CURRENT_USER。" << eol;
    }
}

RegistryDock::~RegistryDock()
{
    kLogEvent event;
    info << event << "[RegistryDock] 析构开始，准备停止搜索线程。" << eol;

    stopSearch(true);
    if (m_searchFlushTimer != nullptr)
    {
        m_searchFlushTimer->stop();
    }

    kLogEvent finishEvent;
    info << finishEvent << "[RegistryDock] 析构完成，后台资源已回收。" << eol;
}

void RegistryDock::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(4, 4, 4, 4);
    m_rootLayout->setSpacing(6);

    m_toolBarWidget = new QWidget(this);
    m_toolBarLayout = new QHBoxLayout(m_toolBarWidget);
    m_toolBarLayout->setContentsMargins(0, 0, 0, 0);
    m_toolBarLayout->setSpacing(4);

    // 导航图标与文件管理器保持一致，统一“后退/前进”视觉语义。
    m_backButton = new QPushButton(QIcon(":/Icon/file_nav_back.svg"), QString(), m_toolBarWidget);
    m_forwardButton = new QPushButton(QIcon(":/Icon/file_nav_forward.svg"), QString(), m_toolBarWidget);
    m_refreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_toolBarWidget);
    m_newKeyButton = new QPushButton(QIcon(":/Icon/process_open_folder.svg"), QString(), m_toolBarWidget);
    m_newValueButton = new QPushButton(QIcon(":/Icon/process_details.svg"), QString(), m_toolBarWidget);
    m_renameButton = new QPushButton(QIcon(":/Icon/process_priority.svg"), QString(), m_toolBarWidget);
    m_deleteButton = new QPushButton(QIcon(":/Icon/process_terminate.svg"), QString(), m_toolBarWidget);
    m_importButton = new QPushButton(QIcon(":/Icon/process_resume.svg"), QString(), m_toolBarWidget);
    m_exportButton = new QPushButton(QIcon(":/Icon/log_export.svg"), QString(), m_toolBarWidget);
    m_searchButton = new QPushButton(QIcon(":/Icon/process_start.svg"), QString(), m_toolBarWidget);
    m_stopSearchButton = new QPushButton(QIcon(":/Icon/process_pause.svg"), QString(), m_toolBarWidget);

    m_backButton->setToolTip(QStringLiteral("后退"));
    m_forwardButton->setToolTip(QStringLiteral("前进"));
    m_refreshButton->setToolTip(QStringLiteral("刷新"));
    m_newKeyButton->setToolTip(QStringLiteral("新建子键"));
    m_newValueButton->setToolTip(QStringLiteral("新建值"));
    m_renameButton->setToolTip(QStringLiteral("重命名"));
    m_deleteButton->setToolTip(QStringLiteral("删除"));
    m_importButton->setToolTip(QStringLiteral("导入 .reg"));
    m_exportButton->setToolTip(QStringLiteral("导出 .reg"));
    m_searchButton->setToolTip(QStringLiteral("开始搜索"));
    m_stopSearchButton->setToolTip(QStringLiteral("停止搜索"));

    for (QPushButton* button : { m_backButton, m_forwardButton, m_refreshButton, m_newKeyButton, m_newValueButton,
            m_renameButton, m_deleteButton, m_importButton, m_exportButton, m_searchButton, m_stopSearchButton })
    {
        button->setStyleSheet(blueButtonStyle());
        button->setFixedWidth(34);
    }

    m_pathEdit = new QLineEdit(m_toolBarWidget);
    m_pathEdit->setStyleSheet(blueInputStyle());
    m_pathEdit->setPlaceholderText(QStringLiteral("输入路径后回车，例如 HKEY_LOCAL_MACHINE\\SOFTWARE"));

    m_searchEdit = new QLineEdit(m_toolBarWidget);
    m_searchEdit->setStyleSheet(blueInputStyle());
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索键/值/数据"));
    m_searchEdit->setMaximumWidth(320);

    m_toolBarLayout->addWidget(m_backButton);
    m_toolBarLayout->addWidget(m_forwardButton);
    m_toolBarLayout->addWidget(m_refreshButton);
    m_toolBarLayout->addWidget(m_newKeyButton);
    m_toolBarLayout->addWidget(m_newValueButton);
    m_toolBarLayout->addWidget(m_renameButton);
    m_toolBarLayout->addWidget(m_deleteButton);
    m_toolBarLayout->addWidget(m_importButton);
    m_toolBarLayout->addWidget(m_exportButton);
    m_toolBarLayout->addWidget(m_pathEdit, 1);
    m_toolBarLayout->addWidget(m_searchEdit, 0);
    m_toolBarLayout->addWidget(m_searchButton);
    m_toolBarLayout->addWidget(m_stopSearchButton);

    m_rootLayout->addWidget(m_toolBarWidget, 0);

    m_mainSplitter = new QSplitter(Qt::Horizontal, this);
    m_rootLayout->addWidget(m_mainSplitter, 1);

    m_keyTree = new QTreeWidget(m_mainSplitter);
    m_keyTree->setColumnCount(1);
    m_keyTree->setHeaderLabel(QStringLiteral("注册表键"));
    m_keyTree->header()->setStyleSheet(blueHeaderStyle());
    m_keyTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_keyTree->setMinimumWidth(360);

    m_rightTabWidget = new QTabWidget(m_mainSplitter);

    m_valueTable = new QTableWidget(m_rightTabWidget);
    m_valueTable->setColumnCount(3);
    m_valueTable->setHorizontalHeaderLabels(QStringList{ QStringLiteral("名称"), QStringLiteral("类型"), QStringLiteral("数据") });
    m_valueTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_valueTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_valueTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_valueTable->setAlternatingRowColors(true);
    m_valueTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_valueTable->horizontalHeader()->setStyleSheet(blueHeaderStyle());
    m_valueTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_valueTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_valueTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);

    m_searchResultTable = new QTableWidget(m_rightTabWidget);
    m_searchResultTable->setColumnCount(5);
    m_searchResultTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("键路径"), QStringLiteral("值名"), QStringLiteral("类型"), QStringLiteral("数据预览"), QStringLiteral("命中来源")
        });
    m_searchResultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_searchResultTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_searchResultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_searchResultTable->setAlternatingRowColors(true);
    m_searchResultTable->horizontalHeader()->setStyleSheet(blueHeaderStyle());
    m_searchResultTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_searchResultTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_searchResultTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_searchResultTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_searchResultTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);

    m_rightTabWidget->addTab(m_valueTable, QStringLiteral("值列表"));
    m_rightTabWidget->addTab(m_searchResultTable, QStringLiteral("搜索结果"));

    m_mainSplitter->setStretchFactor(0, 1);
    m_mainSplitter->setStretchFactor(1, 2);

    m_statusBar = new QStatusBar(this);
    m_pathStatusLabel = new QLabel(QStringLiteral("路径: -"), m_statusBar);
    m_summaryStatusLabel = new QLabel(QStringLiteral("状态: 就绪"), m_statusBar);
    m_statusBar->addWidget(m_pathStatusLabel, 1);
    m_statusBar->addPermanentWidget(m_summaryStatusLabel, 0);
    m_rootLayout->addWidget(m_statusBar, 0);

    m_searchFlushTimer = new QTimer(this);
    m_searchFlushTimer->setInterval(100);
    m_stopSearchButton->setEnabled(false);
}

void RegistryDock::initializeConnections()
{
    connect(m_backButton, &QPushButton::clicked, this, [this]() {
        if (m_navigationIndex <= 0 || m_navigationHistory.empty()) return;
        m_navigationIndex -= 1;
        navigateToPath(m_navigationHistory[static_cast<std::size_t>(m_navigationIndex)], false);
    });

    connect(m_forwardButton, &QPushButton::clicked, this, [this]() {
        if (m_navigationHistory.empty()) return;
        const int nextIndex = m_navigationIndex + 1;
        if (nextIndex < 0 || nextIndex >= static_cast<int>(m_navigationHistory.size())) return;
        m_navigationIndex = nextIndex;
        navigateToPath(m_navigationHistory[static_cast<std::size_t>(m_navigationIndex)], false);
    });

    connect(m_refreshButton, &QPushButton::clicked, this, [this]() { refreshCurrentKey(true); });
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this]() { navigateToPath(m_pathEdit->text().trimmed(), true); });

    connect(m_keyTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) { ensureTreeItemLoaded(item); });
    connect(m_keyTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
        if (item == nullptr || item->data(0, kRolePlaceholder).toBool()) return;
        const QString path = item->data(0, kRolePath).toString();
        if (!path.isEmpty() && path.compare(m_currentPath, Qt::CaseInsensitive) != 0) navigateToPath(path, true);
    });

    connect(m_keyTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showTreeContextMenu(pos); });
    connect(m_valueTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showValueContextMenu(pos); });
    connect(m_valueTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem*) { editSelectedValue(); });

    connect(m_newKeyButton, &QPushButton::clicked, this, [this]() { createSubKey(); });
    connect(m_newValueButton, &QPushButton::clicked, this, [this]() { createValue(); });
    connect(m_renameButton, &QPushButton::clicked, this, [this]() { renameSelectedObject(); });
    connect(m_deleteButton, &QPushButton::clicked, this, [this]() { deleteSelectedObject(); });
    connect(m_importButton, &QPushButton::clicked, this, [this]() { importRegFileAsync(); });
    connect(m_exportButton, &QPushButton::clicked, this, [this]() { exportCurrentKeyAsync(); });
    connect(m_searchButton, &QPushButton::clicked, this, [this]() { startSearchAsync(); });
    connect(m_stopSearchButton, &QPushButton::clicked, this, [this]() { stopSearch(false); });
    connect(m_searchEdit, &QLineEdit::returnPressed, this, [this]() { startSearchAsync(); });
    connect(m_searchFlushTimer, &QTimer::timeout, this, [this]() { flushPendingSearchRows(); });

    connect(m_searchResultTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* item) {
        if (item == nullptr) return;
        QTableWidgetItem* pathItem = m_searchResultTable->item(item->row(), 0);
        if (pathItem == nullptr) return;
        navigateToPath(pathItem->text().trimmed(), true);
        m_rightTabWidget->setCurrentWidget(m_valueTable);
    });
}

void RegistryDock::initializeRootItems()
{
    m_keyTree->clear();
    for (const RootEntry& entry : kRootMap)
    {
        QTreeWidgetItem* item = new QTreeWidgetItem(m_keyTree);
        item->setText(0, QString::fromWCharArray(entry.fullName));
        item->setData(0, kRolePath, QString::fromWCharArray(entry.fullName));
        item->setData(0, kRoleLoaded, false);
        item->setData(0, kRolePlaceholder, false);

        QTreeWidgetItem* placeholder = new QTreeWidgetItem(item);
        placeholder->setText(0, QStringLiteral("..."));
        placeholder->setData(0, kRolePlaceholder, true);
    }
}
bool RegistryDock::parseRegistryPath(const QString& pathText, HKEY* rootKeyOut, QString* subPathOut)
{
    if (rootKeyOut == nullptr || subPathOut == nullptr) return false;

    QString text = pathText.trimmed();
    text.replace('/', '\\');
    while (text.contains(QStringLiteral("\\\\"))) text.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
    if (text.endsWith('\\')) text.chop(1);
    if (text.isEmpty()) return false;

    const int split = text.indexOf('\\');
    const QString rootText = split < 0 ? text : text.left(split);
    const QString subPath = split < 0 ? QString() : text.mid(split + 1);

    for (const RootEntry& entry : kRootMap)
    {
        const QString full = QString::fromWCharArray(entry.fullName);
        const QString shortName = QString::fromWCharArray(entry.shortName);
        if (rootText.compare(full, Qt::CaseInsensitive) == 0 || rootText.compare(shortName, Qt::CaseInsensitive) == 0)
        {
            *rootKeyOut = entry.root;
            *subPathOut = subPath;
            return true;
        }
    }
    return false;
}

QString RegistryDock::normalizeRegistryPath(const QString& pathText)
{
    HKEY root = nullptr;
    QString subPath;
    if (!parseRegistryPath(pathText, &root, &subPath)) return QString();
    QString output = rootKeyToText(root);
    if (!subPath.isEmpty()) output += QStringLiteral("\\") + subPath;
    return output;
}

QString RegistryDock::rootKeyToText(HKEY rootKey)
{
    for (const RootEntry& entry : kRootMap)
    {
        if (entry.root == rootKey) return QString::fromWCharArray(entry.fullName);
    }
    return QStringLiteral("<Unknown>");
}

QString RegistryDock::valueTypeToText(DWORD type)
{
    switch (type)
    {
    case REG_NONE: return QStringLiteral("REG_NONE");
    case REG_SZ: return QStringLiteral("REG_SZ");
    case REG_EXPAND_SZ: return QStringLiteral("REG_EXPAND_SZ");
    case REG_BINARY: return QStringLiteral("REG_BINARY");
    case REG_DWORD: return QStringLiteral("REG_DWORD");
    case REG_MULTI_SZ: return QStringLiteral("REG_MULTI_SZ");
    case REG_QWORD: return QStringLiteral("REG_QWORD");
    default: return QStringLiteral("REG_%1").arg(type);
    }
}

QString RegistryDock::formatValueData(DWORD type, const QByteArray& data)
{
    if (data.isEmpty()) return QStringLiteral("<empty>");

    if (type == REG_SZ || type == REG_EXPAND_SZ)
    {
        QString text = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(data.constData()), data.size() / sizeof(wchar_t));
        text.remove(QChar::Null);
        return text;
    }
    if (type == REG_MULTI_SZ)
    {
        QString text = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(data.constData()), data.size() / sizeof(wchar_t));
        return text.split(QChar::Null, Qt::SkipEmptyParts).join(QStringLiteral(" | "));
    }
    if (type == REG_DWORD && data.size() >= static_cast<int>(sizeof(DWORD)))
    {
        const DWORD value = *reinterpret_cast<const DWORD*>(data.constData());
        return QStringLiteral("0x%1 (%2)").arg(value, 8, 16, QLatin1Char('0')).arg(value);
    }
    if (type == REG_QWORD && data.size() >= static_cast<int>(sizeof(quint64)))
    {
        const quint64 value = *reinterpret_cast<const quint64*>(data.constData());
        return QStringLiteral("0x%1 (%2)").arg(static_cast<qulonglong>(value), 16, 16, QLatin1Char('0')).arg(static_cast<qulonglong>(value));
    }
    return bytesToHex(data, 64);
}

QString RegistryDock::winErrorText(LONG code)
{
    wchar_t* buffer = nullptr;
    const DWORD size = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(code),
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    QString text = QStringLiteral("错误码 %1").arg(code);
    if (size > 0 && buffer != nullptr)
    {
        text += QStringLiteral(": ") + QString::fromWCharArray(buffer, static_cast<int>(size)).trimmed();
    }
    if (buffer != nullptr) ::LocalFree(buffer);
    return text;
}

bool RegistryDock::readRegistryValueRaw(HKEY root, const QString& subPath, const QString& valueName, DWORD* typeOut, QByteArray* dataOut, QString* errorOut)
{
    if (typeOut == nullptr || dataOut == nullptr) return false;
    if (errorOut != nullptr) errorOut->clear();

    HKEY key = nullptr;
    LONG openResult = ::RegOpenKeyExW(root, subPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(subPath.utf16()), 0, KEY_QUERY_VALUE, &key);
    if (openResult != ERROR_SUCCESS)
    {
        if (errorOut != nullptr) *errorOut = winErrorText(openResult);
        return false;
    }

    const QString realName = trimDefaultValueName(valueName);
    const wchar_t* valuePtr = realName.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(realName.utf16());

    DWORD type = REG_NONE;
    DWORD size = 0;
    LONG queryResult = ::RegQueryValueExW(key, valuePtr, nullptr, &type, nullptr, &size);
    if (queryResult != ERROR_SUCCESS)
    {
        ::RegCloseKey(key);
        if (errorOut != nullptr) *errorOut = winErrorText(queryResult);
        return false;
    }

    QByteArray data;
    data.resize(static_cast<int>(size));
    if (size > 0)
    {
        queryResult = ::RegQueryValueExW(key, valuePtr, nullptr, &type, reinterpret_cast<LPBYTE>(data.data()), &size);
        if (queryResult != ERROR_SUCCESS)
        {
            ::RegCloseKey(key);
            if (errorOut != nullptr) *errorOut = winErrorText(queryResult);
            return false;
        }
    }

    ::RegCloseKey(key);
    *typeOut = type;
    *dataOut = data;
    return true;
}

bool RegistryDock::writeRegistryValue(HKEY root, const QString& subPath, const QString& valueName, DWORD type, const QByteArray& rawData, QString* errorOut)
{
    if (errorOut != nullptr) errorOut->clear();

    HKEY key = nullptr;
    LONG openResult = ::RegOpenKeyExW(root, subPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(subPath.utf16()), 0, KEY_SET_VALUE, &key);
    if (openResult != ERROR_SUCCESS)
    {
        if (errorOut != nullptr) *errorOut = winErrorText(openResult);
        return false;
    }

    const QString realName = trimDefaultValueName(valueName);
    const wchar_t* valuePtr = realName.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(realName.utf16());
    LONG setResult = ::RegSetValueExW(
        key,
        valuePtr,
        0,
        type,
        reinterpret_cast<const BYTE*>(rawData.constData()),
        static_cast<DWORD>(rawData.size()));
    ::RegCloseKey(key);

    if (setResult != ERROR_SUCCESS)
    {
        if (errorOut != nullptr) *errorOut = winErrorText(setResult);
        return false;
    }
    return true;
}

void RegistryDock::updateStatusBar(const QString& message)
{
    m_pathStatusLabel->setText(QStringLiteral("路径: %1").arg(m_currentPath));
    m_summaryStatusLabel->setText(message);
}

void RegistryDock::navigateToPath(const QString& path, bool recordHistory)
{
    {
        kLogEvent event;
        info << event
            << "[RegistryDock] 导航请求, input="
            << path.toStdString()
            << ", recordHistory="
            << (recordHistory ? "true" : "false")
            << eol;
    }

    const QString normalized = normalizeRegistryPath(path);
    if (normalized.isEmpty())
    {
        kLogEvent event;
        warn << event << "[RegistryDock] 导航失败：无效路径, input=" << path.toStdString() << eol;
        QMessageBox::warning(this, QStringLiteral("注册表"), QStringLiteral("无效路径：%1").arg(path));
        return;
    }

    m_currentPath = normalized;
    m_pathEdit->setText(normalized);

    if (recordHistory)
    {
        if (m_navigationIndex + 1 < static_cast<int>(m_navigationHistory.size()))
        {
            m_navigationHistory.erase(m_navigationHistory.begin() + m_navigationIndex + 1, m_navigationHistory.end());
        }
        if (m_navigationHistory.empty() || m_navigationHistory.back().compare(normalized, Qt::CaseInsensitive) != 0)
        {
            m_navigationHistory.push_back(normalized);
        }
        m_navigationIndex = static_cast<int>(m_navigationHistory.size()) - 1;
    }

    m_backButton->setEnabled(m_navigationIndex > 0);
    m_forwardButton->setEnabled(m_navigationIndex >= 0 && (m_navigationIndex + 1) < static_cast<int>(m_navigationHistory.size()));

    selectTreeItemByPath(normalized);
    refreshCurrentKey(true);

    {
        kLogEvent event;
        info << event
            << "[RegistryDock] 导航成功, normalized="
            << normalized.toStdString()
            << ", historySize="
            << m_navigationHistory.size()
            << ", historyIndex="
            << m_navigationIndex
            << eol;
    }
}

void RegistryDock::selectTreeItemByPath(const QString& path)
{
    const QString normalized = normalizeRegistryPath(path);
    if (normalized.isEmpty()) return;

    const QStringList segments = normalized.split('\\', Qt::SkipEmptyParts);
    if (segments.isEmpty()) return;

    QTreeWidgetItem* current = nullptr;
    for (int i = 0; i < m_keyTree->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* item = m_keyTree->topLevelItem(i);
        if (item->text(0).compare(segments.first(), Qt::CaseInsensitive) == 0)
        {
            current = item;
            break;
        }
    }
    if (current == nullptr) return;

    ensureTreeItemLoaded(current);
    for (int i = 1; i < segments.size(); ++i)
    {
        ensureTreeItemLoaded(current);
        QTreeWidgetItem* next = nullptr;
        for (int childIndex = 0; childIndex < current->childCount(); ++childIndex)
        {
            QTreeWidgetItem* child = current->child(childIndex);
            if (child == nullptr || child->data(0, kRolePlaceholder).toBool()) continue;
            if (child->text(0).compare(segments.at(i), Qt::CaseInsensitive) == 0)
            {
                next = child;
                break;
            }
        }
        if (next == nullptr) break;
        current = next;
    }

    QSignalBlocker blocker(m_keyTree);
    m_keyTree->setCurrentItem(current);
    m_keyTree->scrollToItem(current);
}

void RegistryDock::ensureTreeItemLoaded(QTreeWidgetItem* item)
{
    if (item == nullptr || item->data(0, kRolePlaceholder).toBool()) return;
    if (item->data(0, kRoleLoaded).toBool()) return;

    const QString itemPath = item->data(0, kRolePath).toString();
    {
        kLogEvent event;
        dbg << event << "[RegistryDock] 展开节点并加载子键, path=" << itemPath.toStdString() << eol;
    }

    HKEY root = nullptr;
    QString subPath;
    if (!parseRegistryPath(itemPath, &root, &subPath))
    {
        item->setData(0, kRoleLoaded, true);
        return;
    }

    HKEY key = nullptr;
    LONG openResult = ::RegOpenKeyExW(root, subPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(subPath.utf16()), 0, KEY_ENUMERATE_SUB_KEYS, &key);

    item->takeChildren();
    if (openResult != ERROR_SUCCESS)
    {
        kLogEvent event;
        warn << event
            << "[RegistryDock] 加载子键失败, path="
            << itemPath.toStdString()
            << ", error="
            << winErrorText(openResult).toStdString()
            << eol;
        item->setData(0, kRoleLoaded, true);
        return;
    }

    wchar_t nameBuffer[512] = {};
    DWORD index = 0;
    DWORD nameLength = static_cast<DWORD>(std::size(nameBuffer));
    int childCount = 0;
    while (::RegEnumKeyExW(key, index, nameBuffer, &nameLength, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
    {
        const QString name = QString::fromWCharArray(nameBuffer, static_cast<int>(nameLength));
        QTreeWidgetItem* child = new QTreeWidgetItem(item);
        child->setText(0, name);
        child->setData(0, kRolePath, item->data(0, kRolePath).toString() + QStringLiteral("\\") + name);
        child->setData(0, kRoleLoaded, false);
        child->setData(0, kRolePlaceholder, false);

        QTreeWidgetItem* placeholder = new QTreeWidgetItem(child);
        placeholder->setText(0, QStringLiteral("..."));
        placeholder->setData(0, kRolePlaceholder, true);

        ++index;
        ++childCount;
        nameLength = static_cast<DWORD>(std::size(nameBuffer));
    }

    ::RegCloseKey(key);
    item->setData(0, kRoleLoaded, true);

    {
        kLogEvent event;
        info << event
            << "[RegistryDock] 子键加载完成, path="
            << itemPath.toStdString()
            << ", childCount="
            << childCount
            << eol;
    }
}

void RegistryDock::refreshCurrentKey(bool)
{
    kLogEvent event;
    info << event << "[RegistryDock] 刷新当前键, path=" << m_currentPath.toStdString() << eol;
    refreshValueTable();
}

void RegistryDock::refreshValueTable()
{
    {
        kLogEvent event;
        dbg << event << "[RegistryDock] 开始刷新值列表, path=" << m_currentPath.toStdString() << eol;
    }

    m_valueTable->setRowCount(0);

    HKEY root = nullptr;
    QString subPath;
    if (!parseRegistryPath(m_currentPath, &root, &subPath))
    {
        kLogEvent event;
        warn << event << "[RegistryDock] 刷新失败：路径无效, path=" << m_currentPath.toStdString() << eol;
        updateStatusBar(QStringLiteral("状态: 路径无效"));
        return;
    }

    HKEY key = nullptr;
    LONG openResult = ::RegOpenKeyExW(root, subPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(subPath.utf16()), 0, KEY_QUERY_VALUE, &key);
    if (openResult != ERROR_SUCCESS)
    {
        kLogEvent event;
        warn << event
            << "[RegistryDock] 打开键失败, path="
            << m_currentPath.toStdString()
            << ", error="
            << winErrorText(openResult).toStdString()
            << eol;
        updateStatusBar(QStringLiteral("状态: 打开失败 - %1").arg(winErrorText(openResult)));
        return;
    }

    DWORD valueCount = 0;
    DWORD maxNameLength = 0;
    DWORD maxDataLength = 0;
    ::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &valueCount, &maxNameLength, &maxDataLength, nullptr, nullptr);

    DWORD defaultType = REG_NONE;
    DWORD defaultSize = 0;
    LONG defaultQuery = ::RegQueryValueExW(key, nullptr, nullptr, &defaultType, nullptr, &defaultSize);
    if (defaultQuery == ERROR_SUCCESS)
    {
        QByteArray defaultData;
        defaultData.resize(static_cast<int>(defaultSize));
        if (defaultSize > 0)
        {
            ::RegQueryValueExW(key, nullptr, nullptr, &defaultType, reinterpret_cast<LPBYTE>(defaultData.data()), &defaultSize);
        }

        m_valueTable->insertRow(0);
        QTableWidgetItem* nameItem = new QTableWidgetItem(QStringLiteral("(默认)"));
        nameItem->setData(Qt::UserRole, QString());
        m_valueTable->setItem(0, 0, nameItem);
        m_valueTable->setItem(0, 1, new QTableWidgetItem(valueTypeToText(defaultType)));
        m_valueTable->setItem(0, 2, new QTableWidgetItem(formatValueData(defaultType, defaultData)));
    }

    std::vector<wchar_t> nameBuffer(static_cast<std::size_t>(maxNameLength + 4), L'\0');
    std::vector<unsigned char> dataBuffer(static_cast<std::size_t>(maxDataLength + 8), 0);

    for (DWORD index = 0; index < valueCount; ++index)
    {
        DWORD nameLength = static_cast<DWORD>(nameBuffer.size() - 1);
        DWORD dataLength = static_cast<DWORD>(dataBuffer.size());
        DWORD type = REG_NONE;
        LONG enumResult = ::RegEnumValueW(key, index, nameBuffer.data(), &nameLength, nullptr, &type, dataBuffer.data(), &dataLength);
        if (enumResult != ERROR_SUCCESS) continue;

        const QString valueName = QString::fromWCharArray(nameBuffer.data(), static_cast<int>(nameLength));
        if (valueName.isEmpty()) continue;

        const QByteArray bytes(reinterpret_cast<const char*>(dataBuffer.data()), static_cast<int>(dataLength));
        const int row = m_valueTable->rowCount();
        m_valueTable->insertRow(row);
        QTableWidgetItem* nameItem = new QTableWidgetItem(valueName);
        nameItem->setData(Qt::UserRole, valueName);
        m_valueTable->setItem(row, 0, nameItem);
        m_valueTable->setItem(row, 1, new QTableWidgetItem(valueTypeToText(type)));
        m_valueTable->setItem(row, 2, new QTableWidgetItem(formatValueData(type, bytes)));
    }

    ::RegCloseKey(key);
    updateStatusBar(QStringLiteral("状态: 已加载 %1 个值").arg(m_valueTable->rowCount()));

    kLogEvent finishEvent;
    info << finishEvent
        << "[RegistryDock] 值列表刷新完成, path="
        << m_currentPath.toStdString()
        << ", valueCount="
        << m_valueTable->rowCount()
        << eol;
}
void RegistryDock::showTreeContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = m_keyTree->itemAt(pos);
    if (item != nullptr && !item->data(0, kRolePlaceholder).toBool()) m_keyTree->setCurrentItem(item);

    QMenu menu(this);
    // 显式填充菜单背景，避免浅色模式下继承透明样式出现黑底。
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* newKeyAction = menu.addAction(QIcon(":/Icon/process_open_folder.svg"), QStringLiteral("新建子键"));
    QAction* renameAction = menu.addAction(QIcon(":/Icon/process_priority.svg"), QStringLiteral("重命名"));
    QAction* deleteAction = menu.addAction(QIcon(":/Icon/process_terminate.svg"), QStringLiteral("删除"));
    menu.addSeparator();
    QAction* copyPathAction = menu.addAction(QIcon(":/Icon/process_copy_cell.svg"), QStringLiteral("复制路径"));
    QAction* refreshAction = menu.addAction(QIcon(":/Icon/process_refresh.svg"), QStringLiteral("刷新"));
    menu.addSeparator();
    QAction* exportAction = menu.addAction(QIcon(":/Icon/log_export.svg"), QStringLiteral("导出 .reg"));
    QAction* importAction = menu.addAction(QIcon(":/Icon/process_resume.svg"), QStringLiteral("导入 .reg"));

    QAction* action = menu.exec(m_keyTree->viewport()->mapToGlobal(pos));
    if (action == nullptr) return;

    {
        kLogEvent event;
        info << event
            << "[RegistryDock] 树右键动作, action="
            << action->text().toStdString()
            << ", currentPath="
            << m_currentPath.toStdString()
            << eol;
    }
    if (action == newKeyAction) createSubKey();
    else if (action == renameAction) renameSelectedObject();
    else if (action == deleteAction) deleteSelectedObject();
    else if (action == copyPathAction) copyCurrentPathToClipboard();
    else if (action == refreshAction) refreshCurrentKey(true);
    else if (action == exportAction) exportCurrentKeyAsync();
    else if (action == importAction) importRegFileAsync();
}

void RegistryDock::showValueContextMenu(const QPoint& pos)
{
    const QModelIndex hit = m_valueTable->indexAt(pos);
    if (hit.isValid()) m_valueTable->setCurrentCell(hit.row(), hit.column());

    QMenu menu(this);
    // 显式填充菜单背景，避免浅色模式下继承透明样式出现黑底。
    menu.setStyleSheet(KswordTheme::ContextMenuStyle());
    QAction* editAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("修改"));
    QAction* newAction = menu.addAction(QIcon(":/Icon/process_start.svg"), QStringLiteral("新建值"));
    QAction* renameAction = menu.addAction(QIcon(":/Icon/process_priority.svg"), QStringLiteral("重命名"));
    QAction* deleteAction = menu.addAction(QIcon(":/Icon/process_terminate.svg"), QStringLiteral("删除"));
    QAction* copyPathAction = menu.addAction(QIcon(":/Icon/process_copy_cell.svg"), QStringLiteral("复制路径"));

    QAction* action = menu.exec(m_valueTable->viewport()->mapToGlobal(pos));
    if (action == nullptr) return;

    {
        kLogEvent event;
        info << event
            << "[RegistryDock] 值右键动作, action="
            << action->text().toStdString()
            << ", currentPath="
            << m_currentPath.toStdString()
            << eol;
    }
    if (action == editAction) editSelectedValue();
    else if (action == newAction) createValue();
    else if (action == renameAction) renameSelectedObject();
    else if (action == deleteAction) deleteSelectedObject();
    else if (action == copyPathAction) copyCurrentPathToClipboard();
}

void RegistryDock::createSubKey()
{
    bool ok = false;
    const QString keyName = QInputDialog::getText(this, QStringLiteral("新建子键"), QStringLiteral("请输入子键名称："), QLineEdit::Normal, QStringLiteral("New Key"), &ok).trimmed();
    if (!ok || keyName.isEmpty()) return;

    {
        kLogEvent event;
        info << event
            << "[RegistryDock] 新建子键请求, parentPath="
            << m_currentPath.toStdString()
            << ", keyName="
            << keyName.toStdString()
            << eol;
    }

    HKEY root = nullptr;
    QString subPath;
    if (!parseRegistryPath(m_currentPath, &root, &subPath)) return;

    HKEY key = nullptr;
    LONG openResult = ::RegOpenKeyExW(root, subPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(subPath.utf16()), 0, KEY_CREATE_SUB_KEY, &key);
    if (openResult != ERROR_SUCCESS)
    {
        kLogEvent event;
        warn << event << "[RegistryDock] 新建子键失败：打开父键失败, error=" << winErrorText(openResult).toStdString() << eol;
        QMessageBox::warning(this, QStringLiteral("新建子键"), winErrorText(openResult));
        return;
    }

    HKEY created = nullptr;
    LONG createResult = ::RegCreateKeyExW(key, reinterpret_cast<const wchar_t*>(keyName.utf16()), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, nullptr, &created, nullptr);
    if (created != nullptr) ::RegCloseKey(created);
    ::RegCloseKey(key);

    if (createResult != ERROR_SUCCESS)
    {
        kLogEvent event;
        warn << event << "[RegistryDock] 新建子键失败：创建失败, error=" << winErrorText(createResult).toStdString() << eol;
        QMessageBox::warning(this, QStringLiteral("新建子键"), winErrorText(createResult));
        return;
    }

    kLogEvent event;
    info << event << "[RegistryDock] 新建子键成功, fullPath=" << (m_currentPath + QStringLiteral("\\") + keyName).toStdString() << eol;
    navigateToPath(m_currentPath + QStringLiteral("\\") + keyName, true);
}

void RegistryDock::createValue()
{
    bool ok = false;
    const QString valueName = QInputDialog::getText(this, QStringLiteral("新建值"), QStringLiteral("值名称（默认值留空）："), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok) return;

    const QStringList typeItems{ QStringLiteral("REG_SZ"), QStringLiteral("REG_DWORD"), QStringLiteral("REG_QWORD"), QStringLiteral("REG_BINARY") };
    const QString typeText = QInputDialog::getItem(this, QStringLiteral("新建值"), QStringLiteral("值类型："), typeItems, 0, false, &ok);
    if (!ok || typeText.isEmpty()) return;

    QString dataText = QInputDialog::getText(this, QStringLiteral("新建值"), QStringLiteral("值数据："), QLineEdit::Normal, QString(), &ok);
    if (!ok) return;

    {
        kLogEvent event;
        info << event
            << "[RegistryDock] 新建值请求, path="
            << m_currentPath.toStdString()
            << ", valueName="
            << valueName.toStdString()
            << ", type="
            << typeText.toStdString()
            << eol;
    }

    DWORD type = REG_SZ;
    QByteArray data;

    if (typeText == QStringLiteral("REG_DWORD"))
    {
        type = REG_DWORD;
        bool parseOk = false;
        const quint32 value = dataText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
            ? dataText.mid(2).toUInt(&parseOk, 16)
            : dataText.toUInt(&parseOk, 10);
        if (!parseOk)
        {
            QMessageBox::warning(this, QStringLiteral("新建值"), QStringLiteral("DWORD 格式无效。"));
            return;
        }
        data = QByteArray(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    else if (typeText == QStringLiteral("REG_QWORD"))
    {
        type = REG_QWORD;
        bool parseOk = false;
        const quint64 value = dataText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
            ? dataText.mid(2).toULongLong(&parseOk, 16)
            : dataText.toULongLong(&parseOk, 10);
        if (!parseOk)
        {
            QMessageBox::warning(this, QStringLiteral("新建值"), QStringLiteral("QWORD 格式无效。"));
            return;
        }
        data = QByteArray(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    else if (typeText == QStringLiteral("REG_BINARY"))
    {
        type = REG_BINARY;
        const QStringList parts = dataText.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (const QString& part : parts)
        {
            bool parseOk = false;
            const int value = part.toInt(&parseOk, 16);
            if (!parseOk || value < 0 || value > 255)
            {
                QMessageBox::warning(this, QStringLiteral("新建值"), QStringLiteral("二进制字节无效：%1").arg(part));
                return;
            }
            data.push_back(static_cast<char>(value));
        }
    }
    else
    {
        type = REG_SZ;
        dataText.append(QChar::Null);
        data = QByteArray(reinterpret_cast<const char*>(dataText.utf16()), dataText.size() * sizeof(char16_t));
    }

    HKEY root = nullptr;
    QString subPath;
    if (!parseRegistryPath(m_currentPath, &root, &subPath)) return;

    QString errorText;
    if (!writeRegistryValue(root, subPath, valueName, type, data, &errorText))
    {
        kLogEvent event;
        warn << event << "[RegistryDock] 新建值失败, path=" << m_currentPath.toStdString() << ", error=" << errorText.toStdString() << eol;
        QMessageBox::warning(this, QStringLiteral("新建值"), errorText);
        return;
    }

    kLogEvent event;
    info << event << "[RegistryDock] 新建值成功, path=" << m_currentPath.toStdString() << ", valueName=" << valueName.toStdString() << eol;
    refreshValueTable();
}

void RegistryDock::renameSelectedObject()
{
    {
        kLogEvent event;
        info << event
            << "[RegistryDock] 重命名请求, path="
            << m_currentPath.toStdString()
            << ", valueTableFocus="
            << (m_valueTable->hasFocus() ? "true" : "false")
            << eol;
    }

    if (m_valueTable->hasFocus() && m_valueTable->currentRow() >= 0)
    {
        const int row = m_valueTable->currentRow();
        QTableWidgetItem* nameItem = m_valueTable->item(row, 0);
        if (nameItem == nullptr) return;

        const QString oldName = nameItem->data(Qt::UserRole).toString();
        if (oldName.isEmpty())
        {
            QMessageBox::information(this, QStringLiteral("重命名"), QStringLiteral("默认值不支持重命名。"));
            return;
        }

        bool ok = false;
        const QString newName = QInputDialog::getText(this, QStringLiteral("重命名值"), QStringLiteral("新名称："), QLineEdit::Normal, oldName, &ok).trimmed();
        if (!ok || newName.isEmpty() || newName.compare(oldName, Qt::CaseInsensitive) == 0) return;

        HKEY root = nullptr;
        QString subPath;
        if (!parseRegistryPath(m_currentPath, &root, &subPath)) return;

        DWORD type = REG_NONE;
        QByteArray data;
        QString errorText;
        if (!readRegistryValueRaw(root, subPath, oldName, &type, &data, &errorText))
        {
            QMessageBox::warning(this, QStringLiteral("重命名值"), errorText);
            return;
        }

        if (!writeRegistryValue(root, subPath, newName, type, data, &errorText))
        {
            kLogEvent event;
            warn << event << "[RegistryDock] 重命名值失败：写入新值失败, error=" << errorText.toStdString() << eol;
            QMessageBox::warning(this, QStringLiteral("重命名值"), errorText);
            return;
        }

        HKEY key = nullptr;
        LONG openResult = ::RegOpenKeyExW(root, subPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(subPath.utf16()), 0, KEY_SET_VALUE, &key);
        if (openResult == ERROR_SUCCESS)
        {
            ::RegDeleteValueW(key, reinterpret_cast<const wchar_t*>(oldName.utf16()));
            ::RegCloseKey(key);
        }

        kLogEvent event;
        info << event
            << "[RegistryDock] 重命名值成功, oldName="
            << oldName.toStdString()
            << ", newName="
            << newName.toStdString()
            << eol;

        refreshValueTable();
        return;
    }

    HKEY root = nullptr;
    QString subPath;
    if (!parseRegistryPath(m_currentPath, &root, &subPath)) return;
    if (subPath.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("重命名键"), QStringLiteral("根键不可重命名。"));
        return;
    }

    const int slashPos = subPath.lastIndexOf('\\');
    const QString parentPath = slashPos < 0 ? QString() : subPath.left(slashPos);
    const QString oldKeyName = slashPos < 0 ? subPath : subPath.mid(slashPos + 1);

    bool ok = false;
    const QString newKeyName = QInputDialog::getText(this, QStringLiteral("重命名键"), QStringLiteral("新键名："), QLineEdit::Normal, oldKeyName, &ok).trimmed();
    if (!ok || newKeyName.isEmpty() || newKeyName.compare(oldKeyName, Qt::CaseInsensitive) == 0) return;

    HKEY parentKey = nullptr;
    LONG openResult = ::RegOpenKeyExW(root, parentPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(parentPath.utf16()), 0, KEY_WRITE, &parentKey);
    if (openResult != ERROR_SUCCESS)
    {
        QMessageBox::warning(this, QStringLiteral("重命名键"), winErrorText(openResult));
        return;
    }

    using RegRenameKeyFunc = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR);
    RegRenameKeyFunc renameKey = reinterpret_cast<RegRenameKeyFunc>(::GetProcAddress(::GetModuleHandleW(L"Advapi32.dll"), "RegRenameKey"));
    if (renameKey == nullptr)
    {
        ::RegCloseKey(parentKey);
        QMessageBox::warning(this, QStringLiteral("重命名键"), QStringLiteral("系统不支持 RegRenameKey。"));
        return;
    }

    LONG renameResult = renameKey(parentKey, reinterpret_cast<const wchar_t*>(oldKeyName.utf16()), reinterpret_cast<const wchar_t*>(newKeyName.utf16()));
    ::RegCloseKey(parentKey);
    if (renameResult != ERROR_SUCCESS)
    {
        kLogEvent event;
        warn << event << "[RegistryDock] 重命名键失败, error=" << winErrorText(renameResult).toStdString() << eol;
        QMessageBox::warning(this, QStringLiteral("重命名键"), winErrorText(renameResult));
        return;
    }

    QString newPath = rootKeyToText(root);
    if (!parentPath.isEmpty()) newPath += QStringLiteral("\\") + parentPath;
    newPath += QStringLiteral("\\") + newKeyName;

    kLogEvent event;
    info << event
        << "[RegistryDock] 重命名键成功, oldKey="
        << oldKeyName.toStdString()
        << ", newKey="
        << newKeyName.toStdString()
        << ", newPath="
        << newPath.toStdString()
        << eol;
    navigateToPath(newPath, true);
}

void RegistryDock::deleteSelectedObject()
{
    {
        kLogEvent event;
        info << event
            << "[RegistryDock] 删除请求, path="
            << m_currentPath.toStdString()
            << ", valueTableFocus="
            << (m_valueTable->hasFocus() ? "true" : "false")
            << eol;
    }

    if (m_valueTable->hasFocus() && m_valueTable->currentRow() >= 0)
    {
        QTableWidgetItem* nameItem = m_valueTable->item(m_valueTable->currentRow(), 0);
        if (nameItem == nullptr) return;
        const QString valueName = nameItem->data(Qt::UserRole).toString();

        QMessageBox::StandardButton choice = QMessageBox::question(
            this,
            QStringLiteral("删除值"),
            QStringLiteral("确定删除值“%1”吗？").arg(valueName.isEmpty() ? QStringLiteral("(默认)") : valueName),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (choice != QMessageBox::Yes) return;

        HKEY root = nullptr;
        QString subPath;
        if (!parseRegistryPath(m_currentPath, &root, &subPath)) return;

        HKEY key = nullptr;
        LONG openResult = ::RegOpenKeyExW(root, subPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(subPath.utf16()), 0, KEY_SET_VALUE, &key);
        if (openResult != ERROR_SUCCESS)
        {
            QMessageBox::warning(this, QStringLiteral("删除值"), winErrorText(openResult));
            return;
        }

        LONG deleteResult = ::RegDeleteValueW(key, valueName.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(valueName.utf16()));
        ::RegCloseKey(key);
        if (deleteResult != ERROR_SUCCESS)
        {
            kLogEvent event;
            warn << event << "[RegistryDock] 删除值失败, error=" << winErrorText(deleteResult).toStdString() << eol;
            QMessageBox::warning(this, QStringLiteral("删除值"), winErrorText(deleteResult));
            return;
        }

        kLogEvent event;
        info << event << "[RegistryDock] 删除值成功, valueName=" << valueName.toStdString() << eol;
        refreshValueTable();
        return;
    }

    HKEY root = nullptr;
    QString subPath;
    if (!parseRegistryPath(m_currentPath, &root, &subPath)) return;
    if (subPath.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("删除键"), QStringLiteral("根键不可删除。"));
        return;
    }

    QMessageBox::StandardButton choice = QMessageBox::question(
        this,
        QStringLiteral("删除键"),
        QStringLiteral("确定删除键“%1”及其子项吗？").arg(m_currentPath),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (choice != QMessageBox::Yes) return;

    const int slashPos = subPath.lastIndexOf('\\');
    const QString parentPath = slashPos < 0 ? QString() : subPath.left(slashPos);
    const QString keyName = slashPos < 0 ? subPath : subPath.mid(slashPos + 1);

    HKEY parentKey = nullptr;
    LONG openResult = ::RegOpenKeyExW(root, parentPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(parentPath.utf16()), 0, KEY_WRITE, &parentKey);
    if (openResult != ERROR_SUCCESS)
    {
        QMessageBox::warning(this, QStringLiteral("删除键"), winErrorText(openResult));
        return;
    }

    LONG deleteResult = ::RegDeleteTreeW(parentKey, reinterpret_cast<const wchar_t*>(keyName.utf16()));
    ::RegCloseKey(parentKey);
    if (deleteResult != ERROR_SUCCESS)
    {
        kLogEvent event;
        warn << event << "[RegistryDock] 删除键失败, error=" << winErrorText(deleteResult).toStdString() << eol;
        QMessageBox::warning(this, QStringLiteral("删除键"), winErrorText(deleteResult));
        return;
    }

    kLogEvent event;
    info << event << "[RegistryDock] 删除键成功, keyName=" << keyName.toStdString() << eol;

    QString parentFullPath = rootKeyToText(root);
    if (!parentPath.isEmpty()) parentFullPath += QStringLiteral("\\") + parentPath;
    navigateToPath(parentFullPath, true);
}

void RegistryDock::editSelectedValue()
{
    const int row = m_valueTable->currentRow();
    {
        kLogEvent event;
        info << event << "[RegistryDock] 编辑值请求, path=" << m_currentPath.toStdString() << ", row=" << row << eol;
    }
    if (row < 0) return;
    QTableWidgetItem* nameItem = m_valueTable->item(row, 0);
    if (nameItem == nullptr) return;

    const QString valueName = nameItem->data(Qt::UserRole).toString();

    HKEY root = nullptr;
    QString subPath;
    if (!parseRegistryPath(m_currentPath, &root, &subPath)) return;

    DWORD type = REG_NONE;
    QByteArray data;
    QString errorText;
    if (!readRegistryValueRaw(root, subPath, valueName, &type, &data, &errorText))
    {
        kLogEvent event;
        warn << event << "[RegistryDock] 编辑值失败：读取原值失败, error=" << errorText.toStdString() << eol;
        QMessageBox::warning(this, QStringLiteral("编辑值"), errorText);
        return;
    }

    bool ok = false;
    QByteArray outputData = data;

    if (type == REG_DWORD || type == REG_QWORD)
    {
        qulonglong oldValue = 0;
        if (type == REG_DWORD && data.size() >= static_cast<int>(sizeof(DWORD))) oldValue = *reinterpret_cast<const DWORD*>(data.constData());
        if (type == REG_QWORD && data.size() >= static_cast<int>(sizeof(quint64))) oldValue = *reinterpret_cast<const quint64*>(data.constData());

        const QString text = QInputDialog::getText(this, QStringLiteral("编辑值"), QStringLiteral("输入新数值："), QLineEdit::Normal, QString::number(oldValue), &ok).trimmed();
        if (!ok) return;

        bool parseOk = false;
        const qulonglong parsed = text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
            ? text.mid(2).toULongLong(&parseOk, 16)
            : text.toULongLong(&parseOk, 10);
        if (!parseOk)
        {
            QMessageBox::warning(this, QStringLiteral("编辑值"), QStringLiteral("数值格式无效。"));
            return;
        }

        if (type == REG_DWORD)
        {
            const DWORD v = static_cast<DWORD>(parsed);
            outputData = QByteArray(reinterpret_cast<const char*>(&v), sizeof(v));
        }
        else
        {
            const quint64 v = static_cast<quint64>(parsed);
            outputData = QByteArray(reinterpret_cast<const char*>(&v), sizeof(v));
        }
    }
    else if (type == REG_BINARY)
    {
        const QString text = QInputDialog::getText(this, QStringLiteral("编辑值"), QStringLiteral("输入十六进制字节："), QLineEdit::Normal, bytesToHex(data, 512), &ok).trimmed();
        if (!ok) return;

        outputData.clear();
        const QStringList parts = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (const QString& part : parts)
        {
            bool parseOk = false;
            const int byteValue = part.toInt(&parseOk, 16);
            if (!parseOk || byteValue < 0 || byteValue > 255)
            {
                QMessageBox::warning(this, QStringLiteral("编辑值"), QStringLiteral("字节无效：%1").arg(part));
                return;
            }
            outputData.push_back(static_cast<char>(byteValue));
        }
    }
    else
    {
        QString text = QInputDialog::getText(this, QStringLiteral("编辑值"), QStringLiteral("输入字符串："), QLineEdit::Normal, formatValueData(type, data), &ok);
        if (!ok) return;
        text.append(QChar::Null);
        outputData = QByteArray(reinterpret_cast<const char*>(text.utf16()), text.size() * sizeof(char16_t));
    }

    if (!writeRegistryValue(root, subPath, valueName, type, outputData, &errorText))
    {
        kLogEvent event;
        warn << event << "[RegistryDock] 编辑值失败：写入失败, error=" << errorText.toStdString() << eol;
        QMessageBox::warning(this, QStringLiteral("编辑值"), errorText);
        return;
    }

    kLogEvent event;
    info << event
        << "[RegistryDock] 编辑值成功, valueName="
        << valueName.toStdString()
        << ", type="
        << valueTypeToText(type).toStdString()
        << eol;
    refreshValueTable();
}

void RegistryDock::copyCurrentPathToClipboard()
{
    QApplication::clipboard()->setText(m_currentPath);

    kLogEvent event;
    info << event << "[RegistryDock] 复制路径到剪贴板, path=" << m_currentPath.toStdString() << eol;
}

void RegistryDock::exportCurrentKeyAsync()
{
    if (m_currentPath.isEmpty()) return;

    kLogEvent event;
    info << event << "[RegistryDock] 导出请求, keyPath=" << m_currentPath.toStdString() << eol;

    const QString outputPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出 .reg"),
        QStringLiteral("registry_%1.reg").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))),
        QStringLiteral("REG 文件 (*.reg)"));
    if (outputPath.trimmed().isEmpty()) return;

    if (m_progressPid == 0) m_progressPid = kPro.add("注册表", "导出");
    kPro.set(m_progressPid, "导出中", 0, 20.0f);

    QPointer<RegistryDock> guardThis(this);
    const QString keyPath = m_currentPath;
    std::thread([guardThis, keyPath, outputPath]() {
        QProcess process;
        process.start(QStringLiteral("reg.exe"), QStringList{ QStringLiteral("export"), keyPath, outputPath, QStringLiteral("/y") });
        process.waitForFinished(-1);

        const bool ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        const QString errText = QString::fromLocal8Bit(process.readAllStandardError());

        QMetaObject::invokeMethod(qApp, [guardThis, ok, errText, outputPath]() {
            if (guardThis == nullptr) return;
            kPro.set(guardThis->m_progressPid, "导出完成", 0, 100.0f);
            if (ok)
            {
                kLogEvent event;
                info << event << "[RegistryDock] 导出成功, outputPath=" << outputPath.toStdString() << eol;
                QMessageBox::information(guardThis, QStringLiteral("导出 .reg"), QStringLiteral("导出成功：%1").arg(outputPath));
            }
            else
            {
                kLogEvent event;
                warn << event << "[RegistryDock] 导出失败, error=" << errText.toStdString() << eol;
                QMessageBox::warning(guardThis, QStringLiteral("导出 .reg"), QStringLiteral("导出失败：\n%1").arg(errText));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void RegistryDock::importRegFileAsync()
{
    const QString inputPath = QFileDialog::getOpenFileName(this, QStringLiteral("导入 .reg"), QString(), QStringLiteral("REG 文件 (*.reg)"));
    if (inputPath.trimmed().isEmpty()) return;

    kLogEvent event;
    info << event << "[RegistryDock] 导入请求, inputPath=" << inputPath.toStdString() << eol;

    if (m_progressPid == 0) m_progressPid = kPro.add("注册表", "导入");
    kPro.set(m_progressPid, "导入中", 0, 20.0f);

    QPointer<RegistryDock> guardThis(this);
    std::thread([guardThis, inputPath]() {
        QProcess process;
        process.start(QStringLiteral("reg.exe"), QStringList{ QStringLiteral("import"), inputPath });
        process.waitForFinished(-1);

        const bool ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        const QString errText = QString::fromLocal8Bit(process.readAllStandardError());

        QMetaObject::invokeMethod(qApp, [guardThis, ok, errText]() {
            if (guardThis == nullptr) return;
            kPro.set(guardThis->m_progressPid, "导入完成", 0, 100.0f);
            if (ok)
            {
                kLogEvent event;
                info << event << "[RegistryDock] 导入成功。" << eol;
                QMessageBox::information(guardThis, QStringLiteral("导入 .reg"), QStringLiteral("导入成功。"));
                guardThis->refreshCurrentKey(true);
            }
            else
            {
                kLogEvent event;
                warn << event << "[RegistryDock] 导入失败, error=" << errText.toStdString() << eol;
                QMessageBox::warning(guardThis, QStringLiteral("导入 .reg"), QStringLiteral("导入失败：\n%1").arg(errText));
            }
        }, Qt::QueuedConnection);
    }).detach();
}
void RegistryDock::startSearchAsync()
{
    if (m_searchRunning.load())
    {
        kLogEvent event;
        dbg << event << "[RegistryDock] 搜索请求被忽略：已有搜索在运行。" << eol;
        return;
    }

    const QString keyword = m_searchEdit->text().trimmed();
    if (keyword.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("搜索"), QStringLiteral("请输入关键字。"));
        return;
    }

    {
        kLogEvent event;
        info << event
            << "[RegistryDock] 启动搜索, path="
            << m_currentPath.toStdString()
            << ", keyword="
            << keyword.toStdString()
            << eol;
    }

    HKEY root = nullptr;
    QString subPath;
    if (!parseRegistryPath(m_currentPath, &root, &subPath)) return;

    m_searchRunning.store(true);
    m_searchStopFlag.store(false);
    m_searchScannedKeys = 0;
    m_searchHitCount = 0;
    m_searchResultTable->setRowCount(0);
    m_rightTabWidget->setCurrentWidget(m_searchResultTable);
    m_searchButton->setEnabled(false);
    m_stopSearchButton->setEnabled(true);

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingRows.clear();
    }

    if (m_progressPid == 0) m_progressPid = kPro.add("注册表", "搜索");
    kPro.set(m_progressPid, "搜索开始", 0, 5.0f);
    m_searchFlushTimer->start();

    QPointer<RegistryDock> guardThis(this);
    SearchOptions options;
    m_searchThread = std::make_unique<std::thread>([guardThis, root, subPath, keyword, options]() {
        if (guardThis == nullptr) return;

        std::size_t scanned = 0;
        std::size_t hits = 0;
        guardThis->searchRegistryRecursive(root, subPath, keyword, options, &scanned, &hits);

        QMetaObject::invokeMethod(qApp, [guardThis, scanned, hits]() {
            if (guardThis == nullptr) return;
            guardThis->flushPendingSearchRows();
            guardThis->m_searchRunning.store(false);
            guardThis->m_searchStopFlag.store(false);
            guardThis->m_searchButton->setEnabled(true);
            guardThis->m_stopSearchButton->setEnabled(false);
            guardThis->m_searchFlushTimer->stop();
            guardThis->updateStatusBar(QStringLiteral("状态: 搜索完成，扫描 %1 键，命中 %2 项").arg(scanned).arg(hits));
            kPro.set(guardThis->m_progressPid, "搜索完成", 0, 100.0f);

            kLogEvent event;
            info << event
                << "[RegistryDock] 搜索完成, scanned="
                << scanned
                << ", hits="
                << hits
                << eol;
        }, Qt::QueuedConnection);
    });
}

void RegistryDock::stopSearch(bool waitForThread)
{
    kLogEvent event;
    info << event
        << "[RegistryDock] 停止搜索请求, waitForThread="
        << (waitForThread ? "true" : "false")
        << eol;

    m_searchStopFlag.store(true);

    if (m_searchThread == nullptr || !m_searchThread->joinable())
    {
        m_searchThread.reset();
        m_searchRunning.store(false);
        m_searchButton->setEnabled(true);
        m_stopSearchButton->setEnabled(false);
        if (m_searchFlushTimer != nullptr) m_searchFlushTimer->stop();
        return;
    }

    if (waitForThread)
    {
        m_searchThread->join();
        m_searchThread.reset();
        m_searchRunning.store(false);
        m_searchButton->setEnabled(true);
        m_stopSearchButton->setEnabled(false);
        if (m_searchFlushTimer != nullptr) m_searchFlushTimer->stop();
        return;
    }

    std::unique_ptr<std::thread> joinThread = std::move(m_searchThread);
    QPointer<RegistryDock> guardThis(this);
    std::thread([joinThread = std::move(joinThread), guardThis]() mutable {
        if (joinThread != nullptr && joinThread->joinable()) joinThread->join();
        QMetaObject::invokeMethod(qApp, [guardThis]() {
            if (guardThis == nullptr) return;
            guardThis->flushPendingSearchRows();
            guardThis->m_searchRunning.store(false);
            guardThis->m_searchStopFlag.store(false);
            guardThis->m_searchButton->setEnabled(true);
            guardThis->m_stopSearchButton->setEnabled(false);
            if (guardThis->m_searchFlushTimer != nullptr) guardThis->m_searchFlushTimer->stop();
            guardThis->updateStatusBar(QStringLiteral("状态: 搜索已停止"));
            kPro.set(guardThis->m_progressPid, "搜索停止", 0, 100.0f);

            kLogEvent event;
            info << event << "[RegistryDock] 搜索已停止（异步回收完成）。" << eol;
        }, Qt::QueuedConnection);
    }).detach();
}

void RegistryDock::flushPendingSearchRows()
{
    std::vector<PendingSearchRow> rows;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (m_pendingRows.empty()) return;

        constexpr std::size_t kBatch = 200;
        const std::size_t count = std::min<std::size_t>(kBatch, m_pendingRows.size());
        rows.reserve(count);
        for (std::size_t i = 0; i < count; ++i) rows.push_back(std::move(m_pendingRows[i]));
        using DiffType = std::vector<PendingSearchRow>::difference_type;
        m_pendingRows.erase(m_pendingRows.begin(), m_pendingRows.begin() + static_cast<DiffType>(count));
    }

    for (const PendingSearchRow& row : rows)
    {
        const int index = m_searchResultTable->rowCount();
        m_searchResultTable->insertRow(index);
        m_searchResultTable->setItem(index, 0, new QTableWidgetItem(row.keyPathText));
        m_searchResultTable->setItem(index, 1, new QTableWidgetItem(row.valueNameText));
        m_searchResultTable->setItem(index, 2, new QTableWidgetItem(row.valueTypeText));
        m_searchResultTable->setItem(index, 3, new QTableWidgetItem(row.valueDataPreviewText));
        m_searchResultTable->setItem(index, 4, new QTableWidgetItem(row.hitSourceText));
    }
}

void RegistryDock::searchRegistryRecursive(HKEY root, const QString& subPath, const QString& keyword, const SearchOptions& options, std::size_t* scanned, std::size_t* hit)
{
    if (m_searchStopFlag.load()) return;

    HKEY key = nullptr;
    LONG openResult = ::RegOpenKeyExW(root, subPath.isEmpty() ? nullptr : reinterpret_cast<const wchar_t*>(subPath.utf16()), 0, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &key);
    if (openResult != ERROR_SUCCESS) return;

    if (scanned != nullptr) *scanned += 1;

    const QString fullPath = rootKeyToText(root) + (subPath.isEmpty() ? QString() : QStringLiteral("\\") + subPath);
    const QString keyName = subPath.isEmpty() ? rootKeyToText(root) : subPath.mid(subPath.lastIndexOf('\\') + 1);

    auto containsText = [&keyword, &options](const QString& text) {
        return options.caseSensitive ? text.contains(keyword) : text.contains(keyword, Qt::CaseInsensitive);
    };

    if (options.searchKeyName && containsText(keyName))
    {
        PendingSearchRow row;
        row.keyPathText = fullPath;
        row.valueNameText = QStringLiteral("<Key>");
        row.valueTypeText = QStringLiteral("<Key>");
        row.valueDataPreviewText = QStringLiteral("-");
        row.hitSourceText = QStringLiteral("KeyName");
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingRows.push_back(std::move(row));
        if (hit != nullptr) *hit += 1;
    }

    DWORD subKeyCount = 0;
    DWORD maxSubKeyLen = 0;
    DWORD valueCount = 0;
    DWORD maxValueNameLen = 0;
    DWORD maxValueDataLen = 0;
    ::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subKeyCount, &maxSubKeyLen, nullptr, &valueCount, &maxValueNameLen, &maxValueDataLen, nullptr, nullptr);

    std::vector<wchar_t> valueNameBuffer(static_cast<std::size_t>(maxValueNameLen + 4), L'\0');
    std::vector<unsigned char> valueDataBuffer(static_cast<std::size_t>(maxValueDataLen + 8), 0);

    for (DWORD index = 0; index < valueCount; ++index)
    {
        if (m_searchStopFlag.load()) break;

        DWORD valueNameLen = static_cast<DWORD>(valueNameBuffer.size() - 1);
        DWORD valueDataLen = static_cast<DWORD>(valueDataBuffer.size());
        DWORD valueType = REG_NONE;
        LONG enumResult = ::RegEnumValueW(key, index, valueNameBuffer.data(), &valueNameLen, nullptr, &valueType, valueDataBuffer.data(), &valueDataLen);
        if (enumResult != ERROR_SUCCESS) continue;

        const QString valueName = QString::fromWCharArray(valueNameBuffer.data(), static_cast<int>(valueNameLen));
        const QByteArray valueData(reinterpret_cast<const char*>(valueDataBuffer.data()), static_cast<int>(valueDataLen));
        const QString valueText = formatValueData(valueType, valueData);

        bool matched = false;
        QString sourceText;
        if (options.searchValueName && containsText(valueName))
        {
            matched = true;
            sourceText = QStringLiteral("ValueName");
        }
        if (!matched && options.searchValueData && containsText(valueText))
        {
            matched = true;
            sourceText = QStringLiteral("ValueData");
        }
        if (!matched) continue;

        PendingSearchRow row;
        row.keyPathText = fullPath;
        row.valueNameText = valueName.isEmpty() ? QStringLiteral("(默认)") : valueName;
        row.valueTypeText = valueTypeToText(valueType);
        row.valueDataPreviewText = valueText;
        row.hitSourceText = sourceText;

        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingRows.push_back(std::move(row));
        if (hit != nullptr) *hit += 1;
    }

    if (scanned != nullptr && (*scanned % 64 == 0))
    {
        const std::size_t scannedSnapshot = *scanned;
        const std::size_t hitSnapshot = (hit == nullptr) ? 0 : *hit;
        QPointer<RegistryDock> guardThis(this);
        QMetaObject::invokeMethod(qApp, [guardThis, scannedSnapshot, hitSnapshot]() {
            if (guardThis == nullptr) return;
            guardThis->updateStatusBar(QStringLiteral("状态: 搜索中，扫描 %1 键，命中 %2 项").arg(scannedSnapshot).arg(hitSnapshot));
            const float progress = 5.0f + static_cast<float>(std::min<std::size_t>(scannedSnapshot, 4000)) / 50.0f;
            kPro.set(guardThis->m_progressPid, "搜索中", 0, std::min(progress, 95.0f));
        }, Qt::QueuedConnection);
    }

    std::vector<wchar_t> subNameBuffer(static_cast<std::size_t>(maxSubKeyLen + 4), L'\0');
    for (DWORD subIndex = 0; subIndex < subKeyCount; ++subIndex)
    {
        if (m_searchStopFlag.load()) break;

        DWORD subNameLen = static_cast<DWORD>(subNameBuffer.size() - 1);
        LONG subResult = ::RegEnumKeyExW(key, subIndex, subNameBuffer.data(), &subNameLen, nullptr, nullptr, nullptr, nullptr);
        if (subResult != ERROR_SUCCESS) continue;

        const QString childName = QString::fromWCharArray(subNameBuffer.data(), static_cast<int>(subNameLen));
        const QString childPath = subPath.isEmpty() ? childName : subPath + QStringLiteral("\\") + childName;
        searchRegistryRecursive(root, childPath, keyword, options, scanned, hit);
    }

    ::RegCloseKey(key);
}


