#include "KernelBaseNamedObjectsTab.h"

// ============================================================
// KernelBaseNamedObjectsTab.cpp
// 作用：
// 1) 展示 BaseNamedObjects 专项聚合结果；
// 2) 支持 Session、对象类型和关键字过滤；
// 3) 后台只读枚举对象目录，不编译/加载/调用驱动。
// ============================================================

#include "../theme.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <thread>
#include <utility>

namespace
{
    enum class BaseNamedObjectsColumn : int
    {
        Scope = 0,
        DirectoryPath,
        ObjectName,
        ObjectType,
        FullPath,
        SymbolicTarget,
        Status,
        Count
    };

    QString buttonStyle()
    {
        return QStringLiteral("QPushButton{background:%1;color:#FFFFFF;border-radius:3px;padding:4px 10px;}"
                              "QPushButton:disabled{background:#9AA8B7;color:#EDF2F7;}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    QString inputStyle()
    {
        return QStringLiteral("QLineEdit,QComboBox{background:%1;color:%2;border:1px solid %3;border-radius:3px;padding:4px 6px;}")
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::BorderHex());
    }

    QString tableStyle()
    {
        return QStringLiteral("QTableWidget{background:%1;color:%2;alternate-background-color:%3;gridline-color:%4;}"
                              "QTableWidget::item:selected{background:%5;color:#FFFFFF;}")
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::SurfaceAltHex())
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::PrimaryBlueHex);
    }

    QTableWidgetItem* readOnlyItem(const QString& textValue)
    {
        auto* item = new QTableWidgetItem(textValue);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }

    QString safeCellText(const QString& textValue)
    {
        return textValue.isEmpty() ? QStringLiteral("-") : textValue;
    }

    QString sessionFilterKey(const KernelBaseNamedObjectEntry& entry)
    {
        return entry.hasSessionId
            ? QStringLiteral("session:%1").arg(entry.sessionId)
            : QStringLiteral("global");
    }
}

KernelBaseNamedObjectsTab::KernelBaseNamedObjectsTab(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
    initializeConnections();
    refreshSnapshotAsync(false);
}

void KernelBaseNamedObjectsTab::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(6, 6, 6, 6);
    m_rootLayout->setSpacing(6);

    auto* toolbarLayout = new QHBoxLayout();
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(6);

    auto* titleLabel = new QLabel(QStringLiteral("BaseNamedObjects"), this);
    titleLabel->setStyleSheet(
        QStringLiteral("font-size:16px;font-weight:700;color:%1;")
        .arg(KswordTheme::TextPrimaryHex()));
    toolbarLayout->addWidget(titleLabel, 0);

    m_refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    m_refreshButton->setToolTip(QStringLiteral("重新枚举 Global 与 Session BaseNamedObjects"));
    m_refreshButton->setStyleSheet(buttonStyle());
    toolbarLayout->addWidget(m_refreshButton, 0);

    m_sessionFilterCombo = new QComboBox(this);
    m_sessionFilterCombo->setToolTip(QStringLiteral("按 Global / Session 过滤"));
    m_sessionFilterCombo->setStyleSheet(inputStyle());
    toolbarLayout->addWidget(m_sessionFilterCombo, 0);

    m_typeFilterCombo = new QComboBox(this);
    m_typeFilterCombo->setToolTip(QStringLiteral("按对象类型过滤"));
    m_typeFilterCombo->setStyleSheet(inputStyle());
    toolbarLayout->addWidget(m_typeFilterCombo, 0);

    m_keywordFilterEdit = new QLineEdit(this);
    m_keywordFilterEdit->setPlaceholderText(QStringLiteral("过滤 scope / 目录 / 名称 / 类型 / 目标 / 状态"));
    m_keywordFilterEdit->setClearButtonEnabled(true);
    m_keywordFilterEdit->setStyleSheet(inputStyle());
    toolbarLayout->addWidget(m_keywordFilterEdit, 1);

    m_statusLabel = new QLabel(QStringLiteral("等待刷新"), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;").arg(KswordTheme::TextSecondaryHex()));
    toolbarLayout->addWidget(m_statusLabel, 0);
    m_rootLayout->addLayout(toolbarLayout, 0);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(static_cast<int>(BaseNamedObjectsColumn::Count));
    m_table->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("scope"),
        QStringLiteral("directoryPath"),
        QStringLiteral("objectName"),
        QStringLiteral("objectType"),
        QStringLiteral("fullPath"),
        QStringLiteral("symbolicTarget"),
        QStringLiteral("statusText")
        });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setWordWrap(false);
    m_table->setStyleSheet(tableStyle());
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(static_cast<int>(BaseNamedObjectsColumn::FullPath), QHeaderView::Stretch);
    m_rootLayout->addWidget(m_table, 1);
}

void KernelBaseNamedObjectsTab::initializeConnections()
{
    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        refreshSnapshotAsync(true);
    });
    connect(m_sessionFilterCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        applyFilters();
    });
    connect(m_typeFilterCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        applyFilters();
    });
    connect(m_keywordFilterEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        applyFilters();
    });
}

void KernelBaseNamedObjectsTab::refreshSnapshotAsync(const bool forceRefresh)
{
    bool expected = false;
    if (!m_refreshing.compare_exchange_strong(expected, true))
    {
        if (forceRefresh)
        {
            setStatusText(QStringLiteral("正在刷新，请稍候。"));
        }
        return;
    }

    if (m_refreshButton != nullptr)
    {
        m_refreshButton->setEnabled(false);
    }
    setStatusText(forceRefresh ? QStringLiteral("正在刷新...") : QStringLiteral("正在加载..."));

    QPointer<KernelBaseNamedObjectsTab> safeThis(this);
    std::thread([safeThis]() {
        std::vector<KernelBaseNamedObjectEntry> rows;
        QString errorText;
        const bool success = runBaseNamedObjectsSnapshotTask(rows, errorText);

        if (safeThis.isNull())
        {
            return;
        }

        QMetaObject::invokeMethod(
            safeThis.data(),
            [safeThis, success, rows = std::move(rows), errorText]() mutable {
                if (safeThis.isNull())
                {
                    return;
                }

                safeThis->m_refreshing.store(false);
                if (safeThis->m_refreshButton != nullptr)
                {
                    safeThis->m_refreshButton->setEnabled(true);
                }
                if (!success)
                {
                    safeThis->setStatusText(QStringLiteral("刷新失败：%1").arg(errorText));
                    return;
                }
                safeThis->populateTable(rows);
            },
            Qt::QueuedConnection);
    }).detach();
}

void KernelBaseNamedObjectsTab::populateTable(const std::vector<KernelBaseNamedObjectEntry>& rows)
{
    m_rows = rows;
    rebuildFilterOptions();

    if (m_table == nullptr)
    {
        return;
    }

    m_table->setRowCount(static_cast<int>(m_rows.size()));
    for (int rowIndex = 0; rowIndex < static_cast<int>(m_rows.size()); ++rowIndex)
    {
        const KernelBaseNamedObjectEntry& entry = m_rows[static_cast<std::size_t>(rowIndex)];
        auto* scopeItem = readOnlyItem(entry.scopeText);
        scopeItem->setData(Qt::UserRole, rowIndex);

        m_table->setItem(rowIndex, static_cast<int>(BaseNamedObjectsColumn::Scope), scopeItem);
        m_table->setItem(rowIndex, static_cast<int>(BaseNamedObjectsColumn::DirectoryPath), readOnlyItem(entry.directoryPathText));
        m_table->setItem(rowIndex, static_cast<int>(BaseNamedObjectsColumn::ObjectName), readOnlyItem(safeCellText(entry.objectNameText)));
        m_table->setItem(rowIndex, static_cast<int>(BaseNamedObjectsColumn::ObjectType), readOnlyItem(safeCellText(entry.objectTypeText)));
        m_table->setItem(rowIndex, static_cast<int>(BaseNamedObjectsColumn::FullPath), readOnlyItem(safeCellText(entry.fullPathText)));
        m_table->setItem(rowIndex, static_cast<int>(BaseNamedObjectsColumn::SymbolicTarget), readOnlyItem(safeCellText(entry.symbolicTargetText)));
        m_table->setItem(rowIndex, static_cast<int>(BaseNamedObjectsColumn::Status), readOnlyItem(safeCellText(entry.statusText)));
    }

    applyFilters();
}

void KernelBaseNamedObjectsTab::rebuildFilterOptions()
{
    if (m_sessionFilterCombo == nullptr || m_typeFilterCombo == nullptr)
    {
        return;
    }

    const QString oldSessionKey = m_sessionFilterCombo->currentData().toString();
    const QString oldTypeText = m_typeFilterCombo->currentData().toString();

    m_sessionFilterCombo->blockSignals(true);
    m_typeFilterCombo->blockSignals(true);
    m_sessionFilterCombo->clear();
    m_typeFilterCombo->clear();
    m_sessionFilterCombo->addItem(QStringLiteral("全部 Session"), QStringLiteral("*"));
    m_typeFilterCombo->addItem(QStringLiteral("全部类型"), QStringLiteral("*"));

    std::set<QString> sessionKeys;
    std::set<QString> typeKeys;
    for (const KernelBaseNamedObjectEntry& entry : m_rows)
    {
        sessionKeys.insert(sessionFilterKey(entry));
        typeKeys.insert(entry.typeCategoryText);
    }

    for (const QString& keyText : sessionKeys)
    {
        if (keyText == QStringLiteral("global"))
        {
            m_sessionFilterCombo->addItem(QStringLiteral("Global"), keyText);
        }
        else
        {
            m_sessionFilterCombo->addItem(keyText.mid(QStringLiteral("session:").size()), keyText);
        }
    }
    for (const QString& typeText : typeKeys)
    {
        m_typeFilterCombo->addItem(typeText, typeText);
    }

    const int sessionIndex = m_sessionFilterCombo->findData(oldSessionKey);
    if (sessionIndex >= 0) m_sessionFilterCombo->setCurrentIndex(sessionIndex);
    const int typeIndex = m_typeFilterCombo->findData(oldTypeText);
    if (typeIndex >= 0) m_typeFilterCombo->setCurrentIndex(typeIndex);

    m_sessionFilterCombo->blockSignals(false);
    m_typeFilterCombo->blockSignals(false);
}

void KernelBaseNamedObjectsTab::applyFilters()
{
    if (m_table == nullptr)
    {
        return;
    }

    int visibleCount = 0;
    for (int rowIndex = 0; rowIndex < m_table->rowCount(); ++rowIndex)
    {
        const QTableWidgetItem* scopeItem = m_table->item(rowIndex, static_cast<int>(BaseNamedObjectsColumn::Scope));
        const int sourceIndex = scopeItem != nullptr ? scopeItem->data(Qt::UserRole).toInt() : -1;
        const bool matches =
            sourceIndex >= 0
            && sourceIndex < static_cast<int>(m_rows.size())
            && rowMatchesFilters(m_rows[static_cast<std::size_t>(sourceIndex)]);

        m_table->setRowHidden(rowIndex, !matches);
        if (matches)
        {
            ++visibleCount;
        }
    }

    setStatusText(QStringLiteral("共 %1 项，当前显示 %2 项").arg(m_rows.size()).arg(visibleCount));
}

bool KernelBaseNamedObjectsTab::rowMatchesFilters(const KernelBaseNamedObjectEntry& entry) const
{
    const QString sessionKey = m_sessionFilterCombo != nullptr
        ? m_sessionFilterCombo->currentData().toString()
        : QStringLiteral("*");
    if (sessionKey != QStringLiteral("*") && sessionKey != sessionFilterKey(entry))
    {
        return false;
    }

    const QString typeKey = m_typeFilterCombo != nullptr
        ? m_typeFilterCombo->currentData().toString()
        : QStringLiteral("*");
    if (typeKey != QStringLiteral("*") && typeKey.compare(entry.typeCategoryText, Qt::CaseInsensitive) != 0)
    {
        return false;
    }

    const QString keyword = m_keywordFilterEdit != nullptr
        ? m_keywordFilterEdit->text().trimmed()
        : QString();
    if (keyword.isEmpty())
    {
        return true;
    }

    return entry.scopeText.contains(keyword, Qt::CaseInsensitive)
        || entry.directoryPathText.contains(keyword, Qt::CaseInsensitive)
        || entry.objectNameText.contains(keyword, Qt::CaseInsensitive)
        || entry.objectTypeText.contains(keyword, Qt::CaseInsensitive)
        || entry.typeCategoryText.contains(keyword, Qt::CaseInsensitive)
        || entry.fullPathText.contains(keyword, Qt::CaseInsensitive)
        || entry.symbolicTargetText.contains(keyword, Qt::CaseInsensitive)
        || entry.statusText.contains(keyword, Qt::CaseInsensitive);
}

void KernelBaseNamedObjectsTab::setStatusText(const QString& statusText)
{
    if (m_statusLabel != nullptr)
    {
        m_statusLabel->setText(statusText);
    }
}
