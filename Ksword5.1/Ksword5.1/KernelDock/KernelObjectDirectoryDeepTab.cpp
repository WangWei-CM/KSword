
#include "KernelObjectDirectoryDeepTab.h"
#include "../UI/TableContextMenu.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/FlatButtonTheme.h"
#include "KernelDock.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/TablePresentation.h"

#include <memory>

// ============================================================
// KernelObjectDirectoryDeepTab.cpp
// 作用说明：
// 1) 提供目录递归能力的独立 QWidget；
// 2) 后台调用 KernelObjectDirectoryDeepWorker；
// 3) 将结果以树和详情框形式展示。
// ============================================================

#include "../UI/StructuredFieldView.h"
#include "../UI/DetailLayoutRegistry.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStringList>
#include <QTreeView>
#include <QAbstractItemModel>
#include <QHash>
#include <QPersistentModelIndex>
#include <QItemSelectionModel>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <thread>

using ksword::kernel_dock_internal::kernelText;

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

    QString treeItemAsTsv(const QTreeView* treeWidget, const QModelIndex& treeItem)
    {
        // 输入：目录递归结果树和当前节点。
        // 处理：按当前列顺序读取文本，使用 Tab 分隔，便于复制到表格工具。
        // 返回：TSV 文本；输入为空时返回空字符串。
        if (treeWidget == nullptr || !treeItem.isValid())
        {
            return QString();
        }

        QStringList fieldList;
        fieldList.reserve(treeWidget->model()->columnCount());
        for (int columnIndex = 0; columnIndex < treeWidget->model()->columnCount(); ++columnIndex)
        {
            const QString cellText = treeItem.siblingAtColumn(columnIndex).data().toString().trimmed();
            fieldList.push_back(cellText.isEmpty() ? kernelText("kernel.object_directory.placeholder.empty", QStringLiteral("<空>")) : cellText);
        }
        return fieldList.join('\t');
    }

    void showTreeCopyRowMenu(QTreeView* treeWidget, const QPoint& localPosition)
    {
        if (treeWidget == nullptr) return;
        const QModelIndex hit = treeWidget->indexAt(localPosition);
        if (hit.isValid()) treeWidget->setCurrentIndex(hit);
        const QPersistentModelIndex currentItem(treeWidget->currentIndex());
        const QPointer<QTreeView> guard(treeWidget);
        QMenu contextMenu(treeWidget);
        contextMenu.setStyleSheet(KswordTheme::ContextMenuStyle());
        QAction* copyRowAction = contextMenu.addAction(
            QIcon(QStringLiteral(":/Icon/process_copy_row.svg")),
            kernelText("kernel.object_directory.menu.copy_row", QStringLiteral("复制当前行")));
        ks::ui::SetTableContextActionRole(copyRowAction, ks::ui::TableContextActionRole::CopyRows);
        copyRowAction->setEnabled(currentItem.isValid());
        const QAction* selectedAction = contextMenu.exec(treeWidget->viewport()->mapToGlobal(localPosition));
        if (guard.isNull() || selectedAction != copyRowAction || !currentItem.isValid()
            || currentItem.model() != guard->model()) return;
        QClipboard* clipboard = QApplication::clipboard();
        const QString rowText = treeItemAsTsv(guard, currentItem);
        if (clipboard != nullptr && !rowText.isEmpty()) clipboard->setText(rowText);
    }

}

struct KernelObjectDirectoryDeepTab::TreeSnapshot
{
    struct Node { QString path; int parent = -1; int row = 0; qulonglong source = InvalidSourceIndex; std::vector<int> children; };
    std::vector<KernelObjectDirectoryDeepEntry> rows;
    std::vector<Node> nodes;
    std::vector<int> roots;
};

std::shared_ptr<const KernelObjectDirectoryDeepTab::TreeSnapshot>
KernelObjectDirectoryDeepTab::prepareTreeSnapshot(std::vector<KernelObjectDirectoryDeepEntry> rows)
{
    auto snapshot = std::make_shared<TreeSnapshot>();
    snapshot->rows = std::move(rows);
    QMap<QString, int> byPath;
    const auto append = [&](const QString& path, int parent) {
        const int id = static_cast<int>(snapshot->nodes.size());
        auto& siblings = parent < 0 ? snapshot->roots : snapshot->nodes[parent].children;
        const int row = static_cast<int>(siblings.size());
        siblings.push_back(id);
        snapshot->nodes.push_back({path, parent, row, InvalidSourceIndex, {}});
        byPath.insert(path, id);
        return id;
    };
    std::function<int(const QString&)> ensurePath = [&](const QString& rawPath) {
        const QString path = normalizeObjectPathForUi(rawPath);
        const auto found = byPath.constFind(path);
        if (found != byPath.cend()) return found.value();
        const int parent = path == QStringLiteral("\\") ? -1 : ensurePath(parentPathFromObjectPathForUi(path));
        return append(path, parent);
    };
    for (std::size_t index = 0; index < snapshot->rows.size(); ++index)
    {
        const auto& entry = snapshot->rows[index];
        const int parent = ensurePath(entry.directoryPath);
        const QString path = normalizeObjectPathForUi(entry.fullPath);
        int id = byPath.value(path, -1);
        if (id < 0 || snapshot->nodes[id].source != InvalidSourceIndex) id = append(path, parent);
        snapshot->nodes[id].source = static_cast<qulonglong>(index);
    }
    return snapshot;
}

class KernelObjectDirectoryDeepTab::DirectoryModel final : public QAbstractItemModel
{
public:
    explicit DirectoryModel(QStringList headers, QObject* parent) : QAbstractItemModel(parent), m_headers(std::move(headers)) {}
    void setSnapshot(std::shared_ptr<const TreeSnapshot> snapshot)
    { beginResetModel(); m_snapshot = std::move(snapshot); endResetModel(); }
    int rowCount(const QModelIndex& parent = {}) const override
    {
        if (!m_snapshot || (parent.isValid() && parent.column() != 0)) return 0;
        const auto* value = node(parent);
        return static_cast<int>(value != nullptr ? value->children.size() : m_snapshot->roots.size());
    }
    int columnCount(const QModelIndex& = {}) const override { return static_cast<int>(DirectoryDeepColumn::Count); }
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override
    {
        if (!m_snapshot || row < 0 || column < 0 || column >= columnCount() || (parent.isValid() && parent.column() != 0)) return {};
        const auto* value = node(parent);
        const auto& siblings = value != nullptr ? value->children : m_snapshot->roots;
        if (row >= static_cast<int>(siblings.size())) return {};
        return createIndex(row, column, static_cast<quintptr>(siblings[row] + 1));
    }
    QModelIndex parent(const QModelIndex& index) const override
    {
        const auto* value = node(index);
        if (value == nullptr || value->parent < 0) return {};
        const auto& parent = m_snapshot->nodes[value->parent];
        return createIndex(parent.row, 0, static_cast<quintptr>(value->parent + 1));
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
    {
        if (orientation != Qt::Horizontal || section < 0 || section >= m_headers.size()) return {};
        return role == Qt::DisplayRole ? QVariant(m_headers[section]) : m_headerRoles.value(section).value(role);
    }
    bool setHeaderData(int section, Qt::Orientation orientation, const QVariant& value, int role) override
    {
        if (orientation != Qt::Horizontal || section < 0 || section >= m_headers.size()) return false;
        if (role == Qt::DisplayRole || role == Qt::EditRole) m_headers[section] = value.toString();
        else m_headerRoles[section][role] = value;
        emit headerDataChanged(orientation, section, section);
        return true;
    }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
    {
        const auto* value = node(index);
        if (value == nullptr) return {};
        if (role == SourceIndexRole && index.column() == 0) return QVariant::fromValue(value->source);
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};
        if (value->source != InvalidSourceIndex && value->source < m_snapshot->rows.size())
        {
            const auto& row = m_snapshot->rows[static_cast<std::size_t>(value->source)];
            switch (static_cast<DirectoryDeepColumn>(index.column()))
            {
            case DirectoryDeepColumn::Name: return row.objectName;
            case DirectoryDeepColumn::Type: return row.objectType;
            case DirectoryDeepColumn::FullPath: return row.fullPath;
            case DirectoryDeepColumn::Depth: return QString::number(row.depth);
            case DirectoryDeepColumn::Status: return row.statusText;
            default: return {};
            }
        }
        const bool root = value->path == QStringLiteral("\\");
        switch (static_cast<DirectoryDeepColumn>(index.column()))
        {
        case DirectoryDeepColumn::Name: return root ? QStringLiteral("\\") : leafNameFromObjectPathForUi(value->path);
        case DirectoryDeepColumn::Type: return QStringLiteral("Directory");
        case DirectoryDeepColumn::FullPath: return value->path;
        case DirectoryDeepColumn::Depth: return root ? QStringLiteral("0") : QStringLiteral("-");
        case DirectoryDeepColumn::Status: return root
            ? kernelText("kernel.object_directory.tree.root_status", QStringLiteral("根目录"))
            : kernelText("kernel.object_directory.tree.parent_placeholder", QStringLiteral("父目录占位"));
        default: return {};
        }
    }
private:
    const TreeSnapshot::Node* node(const QModelIndex& index) const
    {
        if (!m_snapshot || !index.isValid() || index.model() != this || index.internalId() == 0) return nullptr;
        const auto id = index.internalId() - 1;
        return id < m_snapshot->nodes.size() ? &m_snapshot->nodes[id] : nullptr;
    }
    std::shared_ptr<const TreeSnapshot> m_snapshot;
    QStringList m_headers;
    QHash<int, QHash<int, QVariant>> m_headerRoles;
};

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

    auto* rootPathLabel = new QLabel(kernelText("kernel.object_directory.toolbar.root_path.label", QStringLiteral("根路径:")), this);
    m_rootPathEdit = new QLineEdit(this);
    ks::ui::StyleStandaloneInput(m_rootPathEdit); // 对象根路径与同排查询动作使用统一控件轮廓。
    m_rootPathEdit->setText(QStringLiteral("\\"));
    m_rootPathEdit->setPlaceholderText(kernelText("kernel.object_directory.toolbar.root_path.placeholder", QStringLiteral("\\、\\Device、\\BaseNamedObjects、\\Sessions")));
    m_rootPathEdit->setToolTip(kernelText("kernel.object_directory.toolbar.root_path.tooltip", QStringLiteral("Object Manager Directory 根路径，必须是 Directory 对象。")));

    auto* depthLabel = new QLabel(kernelText("kernel.object_directory.toolbar.max_depth.label", QStringLiteral("最大深度:")), this);
    m_maxDepthSpinBox = new QSpinBox(this);
    ks::ui::StyleStandaloneInput(m_maxDepthSpinBox);
    m_maxDepthSpinBox->setRange(0, 32);
    m_maxDepthSpinBox->setValue(4);
    m_maxDepthSpinBox->setToolTip(kernelText("kernel.object_directory.toolbar.max_depth.tooltip", QStringLiteral("只对 TypeName == Directory 的对象继续下钻，达到深度后停止。")));

    m_refreshButton = new QPushButton(kernelText("kernel.object_directory.toolbar.refresh", QStringLiteral("刷新")), this);
    ks::ui::ApplyFlatButtonTheme(m_refreshButton, ks::ui::FlatButtonTone::Neutral,
        ks::ui::FlatButtonAppearance::Solid);
    m_refreshButton->setToolTip(kernelText("kernel.object_directory.toolbar.refresh.tooltip", QStringLiteral("后台递归枚举指定 Object Manager Directory。")));

    m_statusLabel = new QLabel(kernelText("kernel.object_directory.status.waiting", QStringLiteral("状态：等待刷新")), this);
    m_statusLabel->setStyleSheet(statusLabelStyle(KswordTheme::TextSecondaryHex()));

    toolLayout->addWidget(rootPathLabel, 0);
    toolLayout->addWidget(m_rootPathEdit, 1);
    toolLayout->addWidget(depthLabel, 0);
    toolLayout->addWidget(m_maxDepthSpinBox, 0);
    toolLayout->addWidget(m_refreshButton, 0);
    toolLayout->addWidget(m_statusLabel, 0);
    ks::ui::NormalizeToolbarRow(toolLayout);
    rootLayout->addLayout(toolLayout);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    rootLayout->addWidget(splitter, 1);

    m_resultTree = new QTreeView(splitter);
    m_model = new DirectoryModel(QStringList{
        kernelText("kernel.object_directory.header.name", QStringLiteral("名称")),
        kernelText("kernel.object_directory.header.type", QStringLiteral("类型")),
        kernelText("kernel.object_directory.header.full_path", QStringLiteral("完整路径")),
        kernelText("kernel.object_directory.header.depth", QStringLiteral("深度")),
        kernelText("kernel.object_directory.header.status", QStringLiteral("状态"))}, m_resultTree);
    m_resultTree->setModel(m_model);
    ks::ui::ApplyTablePresentation(m_resultTree);
    m_resultTree->setAlternatingRowColors(true);
    m_resultTree->setUniformRowHeights(true);
    m_resultTree->header()->setResizeContentsPrecision(100);
    m_resultTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultTree->setRootIsDecorated(true);
    m_resultTree->setContextMenuPolicy(Qt::CustomContextMenu);
    if (m_resultTree->header() != nullptr)
    {
        m_resultTree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
        m_resultTree->header()->setSectionResizeMode(static_cast<int>(DirectoryDeepColumn::FullPath), QHeaderView::Stretch);
    }

    m_detailEditor = new ks::ui::StructuredFieldView(splitter);

    m_detailEditor->setDocument(ks::ui::FieldDocument{}.note(kernelText("kernel.object_directory.detail.initial", QStringLiteral("输入根路径后点击刷新。"))));

    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 2);

    ks::ui::DetailLayoutRegistry::registerStructuredHost(m_resultTree, m_detailEditor, this);

    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        startRefresh();
    });
    connect(m_rootPathEdit, &QLineEdit::returnPressed, this, [this]() {
        startRefresh();
    });
    connect(m_resultTree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex&, const QModelIndex&) {
        showCurrentItemDetail();
    });
    connect(m_resultTree, &QTreeView::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showTreeCopyRowMenu(m_resultTree, localPosition);
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
        m_statusLabel->setText(running
            ? kernelText("kernel.object_directory.status.enumerating", QStringLiteral("状态：递归枚举中..."))
            : kernelText("kernel.object_directory.status.idle", QStringLiteral("状态：空闲")));
        m_statusLabel->setStyleSheet(statusLabelStyle(running ? KswordTheme::InfoHex() : KswordTheme::TextSecondaryHex()));
    }
}

void KernelObjectDirectoryDeepTab::startRefresh()
{
    const QPointer<KernelObjectDirectoryDeepTab> deferredGuard(this);
    if (ks::ui::DeferItemViewUiCommitIfContextMenuOpen(
        this,
        QStringLiteral("kernel-object-directory-refresh-start"),
        { m_resultTree },
        [deferredGuard]()
        {
            if (!deferredGuard.isNull())
            {
                deferredGuard->startRefresh();
            }
        }))
    {
        return;
    }

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
        ks::ui::DetailLayoutRegistry::prepareDataRebuild(m_detailEditor);
        m_model->setSnapshot({});
    }
    if (m_detailEditor != nullptr)
    {
        m_detailEditor->setDocument(ks::ui::FieldDocument{}.note(kernelText("kernel.object_directory.detail.background", QStringLiteral("正在后台递归枚举：%1")).arg(options.rootPath)));
    }

    QPointer<KernelObjectDirectoryDeepTab> guardThis(this);
    std::thread([guardThis, options]() {
        KernelObjectDirectoryDeepResult result =
            runKernelObjectDirectoryDeepSnapshotTask(options);

        auto treeSnapshot = result.success ? prepareTreeSnapshot(std::move(result.rows)) : nullptr;
        QMetaObject::invokeMethod(guardThis, [guardThis, result = std::move(result), treeSnapshot = std::move(treeSnapshot)]() mutable {
            const auto deferredResult =
                std::make_shared<KernelObjectDirectoryDeepResult>(std::move(result));
            auto commitResult = [guardThis, deferredResult, treeSnapshot]() mutable
            {
            KernelObjectDirectoryDeepResult& result = *deferredResult;
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_refreshRunning.store(false);
            guardThis->setRefreshRunning(false);

            if (!result.success)
            {
                guardThis->m_treeSnapshot.reset();
                if (guardThis->m_statusLabel != nullptr)
                {
                    guardThis->m_statusLabel->setText(kernelText("kernel.object_directory.status.failed", QStringLiteral("状态：刷新失败")));
                    guardThis->m_statusLabel->setStyleSheet(statusLabelStyle(KswordTheme::ErrorHex()));
                }
                if (guardThis->m_detailEditor != nullptr)
                {
                    guardThis->m_detailEditor->setDocument(ks::ui::FieldDocument{}.note(result.errorText));
                }
                return;
            }

            guardThis->m_treeSnapshot = treeSnapshot;
            guardThis->rebuildTree();

            QString statusText = kernelText("kernel.object_directory.status.summary", QStringLiteral("状态：已枚举 %1 项，目录 %2 个，失败目录 %3 个"))
                .arg(guardThis->m_treeSnapshot->rows.size())
                .arg(result.visitedDirectoryCount)
                .arg(result.failedDirectoryCount);
            if (result.depthLimitReached)
            {
                statusText += kernelText("kernel.object_directory.status.depth_limit", QStringLiteral("，触达深度上限"));
            }
            if (result.perDirectoryLimitReached)
            {
                statusText += kernelText("kernel.object_directory.status.per_directory_limit", QStringLiteral("，触达单目录上限"));
            }
            if (result.totalLimitReached)
            {
                statusText += kernelText("kernel.object_directory.status.total_limit", QStringLiteral("，触达总上限"));
            }

            if (guardThis->m_statusLabel != nullptr)
            {
                guardThis->m_statusLabel->setText(statusText);
                guardThis->m_statusLabel->setStyleSheet(
                    statusLabelStyle(result.failedDirectoryCount == 0 ? KswordTheme::SuccessHex() : KswordTheme::WarningHex()));
            }
            if (guardThis->m_resultTree != nullptr && guardThis->m_model->rowCount() > 0)
            {
                guardThis->m_resultTree->expandToDepth(1);
                guardThis->m_resultTree->setCurrentIndex(guardThis->m_model->index(0, 0));
            }
            else if (guardThis->m_detailEditor != nullptr)
            {
                guardThis->m_detailEditor->setDocument(ks::ui::FieldDocument{}.note(kernelText("kernel.object_directory.detail.no_records", QStringLiteral("当前根路径没有返回可显示记录。"))));
            }
            };

            if (guardThis == nullptr)
            {
                return;
            }
            if (ks::ui::DeferItemViewUiCommitIfContextMenuOpen(
                guardThis.data(),
                QStringLiteral("kernel-object-directory-snapshot-apply"),
                { guardThis->m_resultTree },
                commitResult))
            {
                return;
            }
            commitResult();
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelObjectDirectoryDeepTab::rebuildTree()
{
    if (m_resultTree == nullptr) return;
    ks::ui::DetailLayoutRegistry::prepareDataRebuild(m_detailEditor);
    m_model->setSnapshot(m_treeSnapshot);
}

void KernelObjectDirectoryDeepTab::showCurrentItemDetail()
{
    if (m_resultTree == nullptr || m_detailEditor == nullptr)
    {
        return;
    }

    const QModelIndex currentItem = m_resultTree->currentIndex();
    if (!currentItem.isValid())
    {
        m_detailEditor->setDocument(ks::ui::FieldDocument{}.note(kernelText("kernel.object_directory.detail.select_hint", QStringLiteral("请选择目录递归结果节点。"))));
        return;
    }

    bool convertOk = false;
    const qulonglong sourceIndex = currentItem.siblingAtColumn(0).data(SourceIndexRole).toULongLong(&convertOk);
    if (!convertOk || sourceIndex == InvalidSourceIndex || !m_treeSnapshot || sourceIndex >= m_treeSnapshot->rows.size())
    {
        ks::ui::FieldDocument detailText;
        detailText.field(QStringLiteral("节点名称"), QStringLiteral("%1").arg(currentItem.siblingAtColumn(static_cast<int>(DirectoryDeepColumn::Name)).data().toString()));
        detailText.field(QStringLiteral("节点类型"), QStringLiteral("%1").arg(currentItem.siblingAtColumn(static_cast<int>(DirectoryDeepColumn::Type)).data().toString()));
        detailText.field(QStringLiteral("完整路径"), QStringLiteral("%1").arg(currentItem.siblingAtColumn(static_cast<int>(DirectoryDeepColumn::FullPath)).data().toString()));
        detailText.field(QStringLiteral("说明"), QStringLiteral("该节点为 UI 父目录占位，未直接绑定 Worker 返回记录。"), true);
        m_detailEditor->setDocument(detailText);
        return;
    }

    m_detailEditor->setDocument(formatEntryDetail(m_treeSnapshot->rows[static_cast<std::size_t>(sourceIndex)]));
}

ks::ui::FieldDocument KernelObjectDirectoryDeepTab::formatEntryDetail(const KernelObjectDirectoryDeepEntry& entry)
{
    ks::ui::FieldDocument detailText;
    detailText.section(QStringLiteral("Object Manager Directory Recursive Entry"));
    detailText.field(QStringLiteral("RootPath"), QStringLiteral("%1").arg(entry.rootPath));
    detailText.field(QStringLiteral("DirectoryPath"), QStringLiteral("%1").arg(entry.directoryPath));
    detailText.field(QStringLiteral("ObjectName"), QStringLiteral("%1").arg(entry.objectName));
    detailText.field(QStringLiteral("ObjectType"), QStringLiteral("%1").arg(entry.objectType));
    detailText.field(QStringLiteral("FullPath"), QStringLiteral("%1").arg(entry.fullPath));
    detailText.field(QStringLiteral("Depth"), QStringLiteral("%1").arg(entry.depth));
    detailText.field(QStringLiteral("IsDirectory"), QStringLiteral("%1").arg(entry.isDirectory ? QStringLiteral("true") : QStringLiteral("false")));
    detailText.field(QStringLiteral("QuerySucceeded"), QStringLiteral("%1").arg(entry.querySucceeded ? QStringLiteral("true") : QStringLiteral("false")));
    detailText.field(QStringLiteral("Status"), QStringLiteral("%1").arg(entry.statusText));
    return detailText;
}
