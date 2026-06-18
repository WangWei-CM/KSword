
#include "KernelObjectDirectoryDeepTab.h"

// ============================================================
// KernelObjectDirectoryDeepTab.cpp
// 作用说明：
// 1) 提供目录递归能力的独立 QWidget；
// 2) 后台调用 KernelObjectDirectoryDeepWorker；
// 3) 将结果以树和详情框形式展示。
// ============================================================

#include "../UI/CodeEditorWidget.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <thread>

namespace
{
    enum class DirectoryDeepColumn : int
    {
        Name = 0,
        Type,
        FullPath,
        Depth,
        Status,
        Count
    };

    constexpr int SourceIndexRole = Qt::UserRole + 1;
    constexpr qulonglong InvalidSourceIndex = std::numeric_limits<qulonglong>::max();

    QString normalizeObjectPathForUi(const QString& rawPath)
    {
        // 输入：用户输入或 Worker 返回的对象路径。
        // 处理：压缩斜杠并保证以 "\" 开头，便于树节点 key 统一。
        // 返回：规范化路径；空输入返回 "\"。
        QString path = rawPath.trimmed();
        path.replace('/', '\\');
        while (path.contains(QStringLiteral("\\\\")))
        {
            path.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
        }
        if (path.isEmpty())
        {
            return QStringLiteral("\\");
        }
        if (!path.startsWith('\\'))
        {
            path.prepend('\\');
        }
        while (path.size() > 1 && path.endsWith('\\'))
        {
            path.chop(1);
        }
        return path;
    }

    QString leafNameFromObjectPathForUi(const QString& objectPath)
    {
        // 输入：完整对象路径。
        // 处理：取末段名称，根路径返回 "\"。
        // 返回：树节点名称。
        const QString normalizedPath = normalizeObjectPathForUi(objectPath);
        if (normalizedPath == QStringLiteral("\\"))
        {
            return QStringLiteral("\\");
        }
        const int slashIndex = normalizedPath.lastIndexOf('\\');
        return slashIndex >= 0 ? normalizedPath.mid(slashIndex + 1) : normalizedPath;
    }

    QString parentPathFromObjectPathForUi(const QString& objectPath)
    {
        // 输入：完整对象路径。
        // 处理：取父路径，一级对象父路径为 "\"。
        // 返回：父目录路径。
        const QString normalizedPath = normalizeObjectPathForUi(objectPath);
        if (normalizedPath == QStringLiteral("\\"))
        {
            return QStringLiteral("\\");
        }
        const int slashIndex = normalizedPath.lastIndexOf('\\');
        if (slashIndex <= 0)
        {
            return QStringLiteral("\\");
        }
        return normalizedPath.left(slashIndex);
    }

    QString statusLabelStyle(const QString& colorHex)
    {
        // 输入：颜色十六进制字符串。
        // 返回：状态标签样式字符串。
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    QTreeWidgetItem* ensurePathItem(
        QTreeWidget* treeWidget,
        QMap<QString, QTreeWidgetItem*>& itemByPath,
        const QString& objectPath)
    {
        // 输入：树控件、路径到节点的缓存、要确保存在的对象路径。
        // 处理：递归确保父节点存在，再创建当前路径节点。
        // 返回：对应路径的树节点；treeWidget 为空时返回 nullptr。
        if (treeWidget == nullptr)
        {
            return nullptr;
        }

        const QString normalizedPath = normalizeObjectPathForUi(objectPath);
        QTreeWidgetItem* existingItem = itemByPath.value(normalizedPath, nullptr);
        if (existingItem != nullptr)
        {
            return existingItem;
        }

        QTreeWidgetItem* newItem = nullptr;
        if (normalizedPath == QStringLiteral("\\"))
        {
            newItem = new QTreeWidgetItem(treeWidget);
            newItem->setText(static_cast<int>(DirectoryDeepColumn::Name), QStringLiteral("\\"));
            newItem->setText(static_cast<int>(DirectoryDeepColumn::Type), QStringLiteral("Directory"));
            newItem->setText(static_cast<int>(DirectoryDeepColumn::FullPath), QStringLiteral("\\"));
            newItem->setText(static_cast<int>(DirectoryDeepColumn::Depth), QStringLiteral("0"));
            newItem->setText(static_cast<int>(DirectoryDeepColumn::Status), QStringLiteral("根目录"));
        }
        else
        {
            QTreeWidgetItem* parentItem = ensurePathItem(
                treeWidget,
                itemByPath,
                parentPathFromObjectPathForUi(normalizedPath));
            newItem = new QTreeWidgetItem(parentItem != nullptr ? parentItem : treeWidget->invisibleRootItem());
            newItem->setText(static_cast<int>(DirectoryDeepColumn::Name), leafNameFromObjectPathForUi(normalizedPath));
            newItem->setText(static_cast<int>(DirectoryDeepColumn::Type), QStringLiteral("Directory"));
            newItem->setText(static_cast<int>(DirectoryDeepColumn::FullPath), normalizedPath);
            newItem->setText(static_cast<int>(DirectoryDeepColumn::Depth), QStringLiteral("-"));
            newItem->setText(static_cast<int>(DirectoryDeepColumn::Status), QStringLiteral("父目录占位"));
        }
        newItem->setData(0, SourceIndexRole, InvalidSourceIndex);
        itemByPath.insert(normalizedPath, newItem);
        return newItem;
    }
}

KernelObjectDirectoryDeepTab::KernelObjectDirectoryDeepTab(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();
}

void KernelObjectDirectoryDeepTab::initializeUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(6);

    auto* toolLayout = new QHBoxLayout();
    toolLayout->setContentsMargins(0, 0, 0, 0);
    toolLayout->setSpacing(6);

    auto* rootPathLabel = new QLabel(QStringLiteral("根路径:"), this);
    m_rootPathEdit = new QLineEdit(this);
    m_rootPathEdit->setText(QStringLiteral("\\"));
    m_rootPathEdit->setPlaceholderText(QStringLiteral("\\、\\Device、\\BaseNamedObjects、\\Sessions"));
    m_rootPathEdit->setToolTip(QStringLiteral("Object Manager Directory 根路径，必须是 Directory 对象。"));

    auto* depthLabel = new QLabel(QStringLiteral("最大深度:"), this);
    m_maxDepthSpinBox = new QSpinBox(this);
    m_maxDepthSpinBox->setRange(0, 32);
    m_maxDepthSpinBox->setValue(4);
    m_maxDepthSpinBox->setToolTip(QStringLiteral("只对 TypeName == Directory 的对象继续下钻，达到深度后停止。"));

    m_refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    m_refreshButton->setToolTip(QStringLiteral("后台递归枚举指定 Object Manager Directory。"));

    m_statusLabel = new QLabel(QStringLiteral("状态：等待刷新"), this);
    m_statusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#6B7280")));

    toolLayout->addWidget(rootPathLabel, 0);
    toolLayout->addWidget(m_rootPathEdit, 1);
    toolLayout->addWidget(depthLabel, 0);
    toolLayout->addWidget(m_maxDepthSpinBox, 0);
    toolLayout->addWidget(m_refreshButton, 0);
    toolLayout->addWidget(m_statusLabel, 0);
    rootLayout->addLayout(toolLayout);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    rootLayout->addWidget(splitter, 1);

    m_resultTree = new QTreeWidget(splitter);
    m_resultTree->setColumnCount(static_cast<int>(DirectoryDeepColumn::Count));
    m_resultTree->setHeaderLabels(QStringList{
        QStringLiteral("名称"),
        QStringLiteral("类型"),
        QStringLiteral("完整路径"),
        QStringLiteral("深度"),
        QStringLiteral("状态")
        });
    m_resultTree->setAlternatingRowColors(true);
    m_resultTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultTree->setRootIsDecorated(true);
    if (m_resultTree->header() != nullptr)
    {
        m_resultTree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
        m_resultTree->header()->setSectionResizeMode(static_cast<int>(DirectoryDeepColumn::FullPath), QHeaderView::Stretch);
    }

    m_detailEditor = new CodeEditorWidget(splitter);
    m_detailEditor->setReadOnly(true);
    m_detailEditor->setText(QStringLiteral("输入根路径后点击刷新。"));

    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 2);

    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        startRefresh();
    });
    connect(m_rootPathEdit, &QLineEdit::returnPressed, this, [this]() {
        startRefresh();
    });
    connect(m_resultTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) {
        showCurrentItemDetail();
    });
}

void KernelObjectDirectoryDeepTab::setRefreshRunning(const bool running)
{
    if (m_refreshButton != nullptr)
    {
        m_refreshButton->setEnabled(!running);
    }
    if (m_rootPathEdit != nullptr)
    {
        m_rootPathEdit->setEnabled(!running);
    }
    if (m_maxDepthSpinBox != nullptr)
    {
        m_maxDepthSpinBox->setEnabled(!running);
    }
    if (m_statusLabel != nullptr)
    {
        m_statusLabel->setText(running ? QStringLiteral("状态：递归枚举中...") : QStringLiteral("状态：空闲"));
        m_statusLabel->setStyleSheet(statusLabelStyle(running ? QStringLiteral("#2563EB") : QStringLiteral("#6B7280")));
    }
}

void KernelObjectDirectoryDeepTab::startRefresh()
{
    if (m_refreshRunning.exchange(true))
    {
        return;
    }

    KernelObjectDirectoryDeepOptions options;
    options.rootPath = m_rootPathEdit != nullptr ? m_rootPathEdit->text().trimmed() : QStringLiteral("\\");
    options.maxDepth = m_maxDepthSpinBox != nullptr ? m_maxDepthSpinBox->value() : 4;
    options.maxEntriesPerDirectory = 4096;
    options.maxTotalEntries = 50000;

    setRefreshRunning(true);
    if (m_resultTree != nullptr)
    {
        m_resultTree->clear();
    }
    if (m_detailEditor != nullptr)
    {
        m_detailEditor->setText(QStringLiteral("正在后台递归枚举：%1").arg(options.rootPath));
    }

    QPointer<KernelObjectDirectoryDeepTab> guardThis(this);
    std::thread([guardThis, options]() {
        KernelObjectDirectoryDeepResult result =
            runKernelObjectDirectoryDeepSnapshotTask(options);

        QMetaObject::invokeMethod(guardThis, [guardThis, result = std::move(result)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_refreshRunning.store(false);
            guardThis->setRefreshRunning(false);

            if (!result.success)
            {
                guardThis->m_rows.clear();
                if (guardThis->m_statusLabel != nullptr)
                {
                    guardThis->m_statusLabel->setText(QStringLiteral("状态：刷新失败"));
                    guardThis->m_statusLabel->setStyleSheet(statusLabelStyle(QStringLiteral("#B23A3A")));
                }
                if (guardThis->m_detailEditor != nullptr)
                {
                    guardThis->m_detailEditor->setText(result.errorText);
                }
                return;
            }

            guardThis->m_rows = std::move(result.rows);
            guardThis->rebuildTree();

            QString statusText = QStringLiteral("状态：已枚举 %1 项，目录 %2 个，失败目录 %3 个")
                .arg(guardThis->m_rows.size())
                .arg(result.visitedDirectoryCount)
                .arg(result.failedDirectoryCount);
            if (result.depthLimitReached)
            {
                statusText += QStringLiteral("，触达深度上限");
            }
            if (result.perDirectoryLimitReached)
            {
                statusText += QStringLiteral("，触达单目录上限");
            }
            if (result.totalLimitReached)
            {
                statusText += QStringLiteral("，触达总上限");
            }

            if (guardThis->m_statusLabel != nullptr)
            {
                guardThis->m_statusLabel->setText(statusText);
                guardThis->m_statusLabel->setStyleSheet(
                    statusLabelStyle(result.failedDirectoryCount == 0 ? QStringLiteral("#3A8F3A") : QStringLiteral("#D77A00")));
            }
            if (guardThis->m_resultTree != nullptr && guardThis->m_resultTree->topLevelItemCount() > 0)
            {
                guardThis->m_resultTree->expandToDepth(1);
                guardThis->m_resultTree->setCurrentItem(guardThis->m_resultTree->topLevelItem(0), 0);
            }
            else if (guardThis->m_detailEditor != nullptr)
            {
                guardThis->m_detailEditor->setText(QStringLiteral("当前根路径没有返回可显示记录。"));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelObjectDirectoryDeepTab::rebuildTree()
{
    if (m_resultTree == nullptr)
    {
        return;
    }

    m_resultTree->setUpdatesEnabled(false);
    m_resultTree->clear();

    QMap<QString, QTreeWidgetItem*> itemByPath;
    for (std::size_t rowIndex = 0; rowIndex < m_rows.size(); ++rowIndex)
    {
        const KernelObjectDirectoryDeepEntry& entry = m_rows[rowIndex];
        QTreeWidgetItem* parentItem = ensurePathItem(m_resultTree, itemByPath, entry.directoryPath);
        QTreeWidgetItem* item = itemByPath.value(normalizeObjectPathForUi(entry.fullPath), nullptr);
        if (item == nullptr || item->data(0, SourceIndexRole).toULongLong() != InvalidSourceIndex)
        {
            item = new QTreeWidgetItem(parentItem != nullptr ? parentItem : m_resultTree->invisibleRootItem());
            itemByPath.insert(normalizeObjectPathForUi(entry.fullPath), item);
        }

        item->setText(static_cast<int>(DirectoryDeepColumn::Name), entry.objectName);
        item->setText(static_cast<int>(DirectoryDeepColumn::Type), entry.objectType);
        item->setText(static_cast<int>(DirectoryDeepColumn::FullPath), entry.fullPath);
        item->setText(static_cast<int>(DirectoryDeepColumn::Depth), QString::number(entry.depth));
        item->setText(static_cast<int>(DirectoryDeepColumn::Status), entry.statusText);
        item->setData(0, SourceIndexRole, static_cast<qulonglong>(rowIndex));
    }

    m_resultTree->setUpdatesEnabled(true);
}

void KernelObjectDirectoryDeepTab::showCurrentItemDetail()
{
    if (m_resultTree == nullptr || m_detailEditor == nullptr)
    {
        return;
    }

    QTreeWidgetItem* currentItem = m_resultTree->currentItem();
    if (currentItem == nullptr)
    {
        m_detailEditor->setText(QStringLiteral("请选择目录递归结果节点。"));
        return;
    }

    bool convertOk = false;
    const qulonglong sourceIndex = currentItem->data(0, SourceIndexRole).toULongLong(&convertOk);
    if (!convertOk || sourceIndex == InvalidSourceIndex || sourceIndex >= m_rows.size())
    {
        QString detailText;
        detailText += QStringLiteral("节点名称: %1\n").arg(currentItem->text(static_cast<int>(DirectoryDeepColumn::Name)));
        detailText += QStringLiteral("节点类型: %1\n").arg(currentItem->text(static_cast<int>(DirectoryDeepColumn::Type)));
        detailText += QStringLiteral("完整路径: %1\n").arg(currentItem->text(static_cast<int>(DirectoryDeepColumn::FullPath)));
        detailText += QStringLiteral("说明: 该节点为 UI 父目录占位，未直接绑定 Worker 返回记录。\n");
        m_detailEditor->setText(detailText);
        return;
    }

    m_detailEditor->setText(formatEntryDetail(m_rows[static_cast<std::size_t>(sourceIndex)]));
}

QString KernelObjectDirectoryDeepTab::formatEntryDetail(const KernelObjectDirectoryDeepEntry& entry)
{
    QString detailText;
    detailText += QStringLiteral("[Object Manager Directory Recursive Entry]\n");
    detailText += QStringLiteral("RootPath: %1\n").arg(entry.rootPath);
    detailText += QStringLiteral("DirectoryPath: %1\n").arg(entry.directoryPath);
    detailText += QStringLiteral("ObjectName: %1\n").arg(entry.objectName);
    detailText += QStringLiteral("ObjectType: %1\n").arg(entry.objectType);
    detailText += QStringLiteral("FullPath: %1\n").arg(entry.fullPath);
    detailText += QStringLiteral("Depth: %1\n").arg(entry.depth);
    detailText += QStringLiteral("IsDirectory: %1\n").arg(entry.isDirectory ? QStringLiteral("true") : QStringLiteral("false"));
    detailText += QStringLiteral("QuerySucceeded: %1\n").arg(entry.querySucceeded ? QStringLiteral("true") : QStringLiteral("false"));
    detailText += QStringLiteral("Status: %1\n").arg(entry.statusText);
    return detailText;
}
