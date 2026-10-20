#include "./ComplexReportModel.h"
#include "../Internationalization/LanguageManager.h"

#include <QHash>
#include <QThreadPool>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QRegularExpression>
#include <algorithm>
#include <utility>

namespace ks::ui
{
    // 索引缓存只保存文档路径和显示数据；不保存会因下一份快照失效的 FieldNode 指针。
    struct ComplexReportModel::State
    {
        struct Entry
        {
            int parent = -1; // 父节点的 preorder 索引，根节点使用 -1。
            int row = 0; // 在父节点实际可见子项中的位置。
            int root = -1; // Precomputed scope identity, never walk QModelIndex parents per row.
            int end = 0; // Exclusive preorder subtree bound.
            int depth = 0; // 绘制层级，根分区为零。
            ReportNodeKind kind = ReportNodeKind::Field; // 原分区/字段或合成的长值详情。
            QVector<int> children; // 子节点的稳定 preorder 索引。
            QVector<int> sourcePath; // 规范文档中的逐层下标，合成子项复用父字段路径。
            QString path; // 不依赖本地化和显示名裁短的节点身份。
            QString name; // 本地化后的短显示名。
            QString value; // 完整值，不截断复制、搜索或行内详情。
            QString search; // Local canonical/display context; ancestors are shared through parent IDs.
            QString summary; // 生产者指定的摘要短标题或原分区名。
            QString offset; // 来自规范 Offset/RVA 属性的独立偏移列。
            QString type; // 来自规范 Type 属性的独立类型列。
            FieldNode::ReportRole purpose = FieldNode::ReportRole::Content;
            int reportedCount = -1; // 资料声明的字段数，与直接属性数分开。
            bool preferredScope = false; // 当前业务对象的默认目录。
            FieldNode::Status status = FieldNode::Status::Neutral; // 只接受生产者的真实语义。
            int count = 0; // 规范文档直接子项数，不把合成长值行计入业务数量。
            bool expanded = false; // 首次展开策略，不在值刷新时重设视图状态。
        };

        explicit State(const FieldDocument& source, Translator translator, std::function<bool()> cancellation = {})
            : translate(std::move(translator)), cancelled(std::move(cancellation)), snapshot(source)
        {
            noteLabel = localized(QStringLiteral("说明"), true);
            countPattern = localized(QStringLiteral("%1 个字段"), true);
            for (const QString& header : {QStringLiteral("字段"), QStringLiteral("偏移"), QStringLiteral("类型 / 值")})
                headers.append(localized(header, true));
            titleSearch = localized(snapshot.title, true) + QLatin1Char('\n') + snapshot.title;
            appendNodes(snapshot.nodes, -1, {}, QString(), QString());
        }

        // 输入规范节点和翻译策略，返回完整本地化文本；业务数据不会经 HTML 或报告文本解析。
        QString localized(const QString& text, bool enabled) const
        {
            return enabled && translate ? translate(text) : text;
        }

        // 只有确有长值的叶内容增加详情子行，短地址和普通属性不出现无意义的展开箭头。
        static bool needsValueDetail(const FieldNode& node)
        {
            return node.kind != FieldNode::Kind::Section
                && (node.value.size() > 80 || node.value.contains(QLatin1Char('\n'))
                    || node.value.contains(QLatin1Char('\r')));
        }

        // 只裁短直接父类型的限定前缀；规范名称和稳定路径仍使用完整原名。
        static QString shortName(const QString& name, const QString& parentName)
        {
            if (!parentName.isEmpty())
            {
                for (const QString& separator : {QStringLiteral("."), QStringLiteral("::"), QStringLiteral(" / ")})
                {
                    const QString prefix = parentName + separator;
                    if (name.startsWith(prefix) && name.size() > prefix.size())
                    {
                        return name.mid(prefix.size());
                    }
                }
            }
            return name;
        }

        // PDB 字段组的折叠行只投影原 Offset/Type 属性值，复制与规范文档仍保留完整子树。
        QString fieldPropertySummary(const FieldNode& node) const
        {
            QStringList values; // 明确属性次序，不从显示标题或说明猜测字段含义。
            for (const QString& name : {QStringLiteral("Offset"), QStringLiteral("Type")})
            {
                for (const FieldNode& child : node.children)
                {
                    if (child.kind == FieldNode::Kind::Field && child.name == name)
                    {
                        const QString value = localized(child.value, child.translateValue);
                        if (!value.isEmpty())
                        {
                            values.append(value);
                        }
                        break;
                    }
                }
            }
            return values.join(QStringLiteral(" · "));
        }

        // 按原文档次序建立 preorder；同名同类兄弟使用序号消歧，不以翻译后的文字作身份。
        void appendNodes(const QVector<FieldNode>& nodes, int parentIndex, const QVector<int>& sourcePrefix,
            const QString& parentPath, const QString& parentName)
        {
            QHash<QString, int> occurrences; // 当前兄弟层的同名节点计数。
            for (int row = 0; row < nodes.size(); ++row)
            {
                if (cancelled && cancelled()) return;
                const FieldNode& node = nodes.at(row);
                const QString key = QString::number(static_cast<int>(node.kind)) + QLatin1Char(':') + node.name;
                const int occurrence = occurrences.value(key);
                occurrences.insert(key, occurrence + 1);
                Entry entry;
                entry.parent = parentIndex;
                entry.root = parentIndex < 0 ? static_cast<int>(entries.size()) : entries.at(parentIndex).root;
                entry.row = row;
                entry.depth = parentIndex < 0 ? 0 : entries.at(parentIndex).depth + 1;
                entry.kind = node.kind == FieldNode::Kind::Section
                    ? (parentIndex < 0 ? ReportNodeKind::Section : ReportNodeKind::Group)
                    : node.kind == FieldNode::Kind::Note ? ReportNodeKind::Note : ReportNodeKind::Field;
                entry.sourcePath = sourcePrefix;
                entry.sourcePath.append(row);
                entry.path = parentPath + QLatin1Char('/') + QString::number(static_cast<int>(node.kind))
                    + QLatin1Char(':') + QString::fromLatin1(QUrl::toPercentEncoding(node.name))
                    + QLatin1Char(':') + QString::number(occurrence);
                entry.name = shortName(localized(node.name, node.translateName), localized(parentName, node.translateName));
                entry.value = localized(node.value, node.translateValue || node.kind == FieldNode::Kind::Note);
                if (entry.kind == ReportNodeKind::Group && entry.depth >= 2)
                {
                    const QString properties = fieldPropertySummary(node);
                    if (!properties.isEmpty())
                    {
                        entry.value = properties; // 属性变化由同结构值比较通知父组，不改节点身份或状态。
                    }
                }
                entry.search = node.name + QLatin1Char('\n')
                    + localized(node.name, node.translateName) + QLatin1Char('\n') + node.value
                    + QLatin1Char('\n') + entry.value;
                entry.summary = node.summaryLabel.isEmpty()
                    ? localized(node.name, node.translateName) : localized(node.summaryLabel, true);
                entry.search += QLatin1Char('\n') + entry.summary; // Short labels participate in the same ancestor search context.
                entry.purpose = node.reportRole;
                entry.reportedCount = node.reportedCount;
                entry.preferredScope = node.preferredScope;
                if (node.kind == FieldNode::Kind::Section)
                {
                    for (const FieldNode& property : node.children)
                    {
                        if (property.kind != FieldNode::Kind::Field) continue;
                        if (property.name == QStringLiteral("Offset") || property.name == QStringLiteral("RVA"))
                            entry.offset = localized(property.value, property.translateValue);
                        else if (property.name == QStringLiteral("Type"))
                            entry.type = localized(property.value, property.translateValue);
                    }
                }
                entry.status = node.status;
                entry.count = static_cast<int>(node.children.size());
                entry.expanded = node.initiallyExpanded && !node.children.isEmpty();
                const int entryIndex = static_cast<int>(entries.size());
                entries.append(std::move(entry));
                paths.insert(entries.at(entryIndex).path, entryIndex);
                if (parentIndex < 0)
                {
                    roots.append(entryIndex);
                }
                else
                {
                    entries[parentIndex].children.append(entryIndex);
                }
                // 递归可能让 QVector 重分配，因此不跨递归保留任何 Entry 引用。
                const QVector<int> childSourcePrefix = entries.at(entryIndex).sourcePath;
                const QString childPath = entries.at(entryIndex).path;
                appendNodes(node.children, entryIndex, childSourcePrefix, childPath, node.name);
                if (needsValueDetail(node))
                {
                    Entry detail;
                    detail.parent = entryIndex;
                    detail.root = entries.at(entryIndex).root;
                    detail.row = static_cast<int>(entries.at(entryIndex).children.size());
                    detail.depth = entries.at(entryIndex).depth + 1;
                    detail.kind = ReportNodeKind::ValueDetail;
                    detail.sourcePath = childSourcePrefix;
                    detail.path = childPath + QStringLiteral("/value-detail");
                    detail.value = entries.at(entryIndex).value;
                    detail.end = static_cast<int>(entries.size()) + 1;
                    detail.status = node.status;
                    const int detailIndex = static_cast<int>(entries.size());
                    entries.append(std::move(detail));
                    entries[entryIndex].children.append(detailIndex);
                    paths.insert(entries.at(detailIndex).path, detailIndex);
                }
                entries[entryIndex].end = static_cast<int>(entries.size());
            }
        }

        // 同结构判断只比较模型身份和拓扑；值、状态、展开偏好与摘要文字变化不重置索引。
        bool sameStructure(const State& other) const
        {
            if (roots != other.roots || entries.size() != other.entries.size())
            {
                return false;
            }
            for (int index = 0; index < entries.size(); ++index)
            {
                const Entry& first = entries.at(index);
                const Entry& second = other.entries.at(index);
                if (first.path != second.path || first.kind != second.kind
                    || first.parent != second.parent || first.row != second.row || first.children != second.children)
                {
                    return false;
                }
            }
            return true;
        }

        // 显示数据是否变化决定 dataChanged；不因重复提交相同快照而刷新整棵报告。
        static bool sameDisplay(const Entry& first, const Entry& second)
        {
            return first.name == second.name && first.value == second.value && first.search == second.search
                && first.summary == second.summary && first.status == second.status
                && first.offset == second.offset && first.type == second.type && first.purpose == second.purpose
                && first.reportedCount == second.reportedCount && first.preferredScope == second.preferredScope
                && first.count == second.count && first.expanded == second.expanded;
        }

        // 按快照路径定位唯一规范节点，供完整子树复制；合成详情不进入规范文档。
        const FieldNode* sourceNode(const Entry& entry) const
        {
            const QVector<FieldNode>* children = &snapshot.nodes;
            const FieldNode* result = nullptr;
            for (int index : entry.sourcePath)
            {
                if (index < 0 || index >= children->size())
                {
                    return nullptr;
                }
                result = &children->at(index);
                children = &result->children;
            }
            return result;
        }

        QString fullSearch(int item) const
        {
            QStringList context;
            for (; item >= 0; item = entries.at(item).parent) context.prepend(entries.at(item).search);
            context.prepend(titleSearch);
            return context.join(QLatin1Char('\n'));
        }

        QString noteLabel;
        QString countPattern;
        QStringList headers;
        QString titleSearch; // Shared document context, not copied into every descendant string.
        Translator translate; // Frozen value callback; production workers never consult LanguageManager.
        std::function<bool()> cancelled;
        FieldDocument snapshot; // 复制、导出与所有角色共用的完整规范文档快照。
        QVector<Entry> entries; // internalId 使用此 preorder 下标加一，零留给无效索引。
        QVector<int> roots; // 顶层原分区和字段的阅读次序。
        QHash<QString, int> paths; // 稳定路径到 preorder 下标的快速定位。
    };

    namespace
    {
        // The complete canonical document and index may be large, including on close/reset.
        // Snapshot ownership is thread independent; its final release always retires off GUI.
        void retireState(const ComplexReportModel::State* state)
        {
            QThreadPool::globalInstance()->start([state]() { delete state; });
        }
    }

    ComplexReportModel::ComplexReportModel(QObject* parent)
        : QAbstractItemModel(parent), m_state(new State(FieldDocument{}, Translator{}), retireState)
    {
    }

    ComplexReportModel::~ComplexReportModel() = default;

    ComplexReportModel::PreparedDocument ComplexReportModel::prepareDocument(const FieldDocument& document,
        const Translator& translate, Snapshot previous, const std::function<bool()>& cancelled)
    {
        PreparedDocument result;
        result.base = std::move(previous);
        auto next = std::make_unique<State>(document, translate, cancelled);
        if (cancelled && cancelled()) return result;
        result.reset = !result.base || !result.base->sameStructure(*next);
        if (!result.reset)
        {
            QHash<int, QVector<int>> changedRows;
            QBitArray contextChanged(next->entries.size());
            const bool titleChanged = result.base->titleSearch != next->titleSearch;
            for (int item = 0; item < next->entries.size(); ++item)
            {
                if ((item & 255) == 0 && cancelled && cancelled()) return result;
                const State::Entry& entry = next->entries.at(item);
                const State::Entry& previousEntry = result.base->entries.at(item);
                const bool inheritedChange = titleChanged || (entry.parent >= 0 && contextChanged.testBit(entry.parent));
                contextChanged.setBit(item, inheritedChange || previousEntry.search != entry.search);
                const bool labelChanged = (entry.kind == ReportNodeKind::Note && result.base->noteLabel != next->noteLabel)
                    || (entry.reportedCount >= 0 && result.base->countPattern != next->countPattern);
                if (inheritedChange || labelChanged || !State::sameDisplay(previousEntry, entry))
                    changedRows[entry.parent].append(entry.row);
            }
            for (auto iterator = changedRows.constBegin(); iterator != changedRows.constEnd(); ++iterator)
            {
                QVector<int> rows = iterator.value();
                std::sort(rows.begin(), rows.end());
                for (int offset = 0; offset < rows.size();)
                {
                    const int first = rows.at(offset);
                    int last = first;
                    while (++offset < rows.size() && rows.at(offset) == last + 1) last = rows.at(offset);
                    result.changes.append({iterator.key(), first, last});
                }
            }
        }
        // Cancellation closures belong to a request, not the installed immutable value.
        next->cancelled = {};
        result.next = Snapshot(next.release(), retireState);
        return result;
    }

    ComplexReportModel::InstallResult ComplexReportModel::installPrepared(const PreparedDocument& prepared)
    {
        // A reentrant latest query can reuse the exact State that an earlier delivery installed.
        if (prepared.next && prepared.next == m_state) return InstallResult::Updated;
        if (!prepared.next || prepared.base != m_state) return InstallResult::Rejected;
        const QPointer<ComplexReportModel> alive(this);
        const bool reset = applyPrepared(prepared);
        if (!alive || m_state != prepared.next) return InstallResult::Rejected;
        return reset ? InstallResult::Reset : InstallResult::Updated;
    }

    // GUI installs precomputed topology/display decisions; stable integer IDs retain persistent indexes.
    bool ComplexReportModel::applyPrepared(const PreparedDocument& prepared)
    {
        if (!prepared.next || prepared.base != m_state) return false;
        const bool headersChanged = m_state->headers != prepared.next->headers;
        const QPointer<ComplexReportModel> alive(this);
        if (prepared.reset) beginResetModel();
        if (!alive) return false;
        m_state = prepared.next;
        if (prepared.reset) endResetModel();
        if (!alive || m_state != prepared.next) return false;
        if (!prepared.reset) for (const ChangedRange& range : prepared.changes)
        {
            const QModelIndex parentIndex = range.parent < 0 ? QModelIndex()
                : indexForPath(m_state->entries.at(range.parent).path);
            emit dataChanged(index(range.first, 0, parentIndex), index(range.last, 2, parentIndex));
            if (!alive || m_state != prepared.next) return false;
        }
        if (headersChanged) emit headerDataChanged(Qt::Horizontal, 0, 2);
        return prepared.reset;
    }

    // Synchronous compatibility entry for QtCore callers. The report widget uses the worker API.
    bool ComplexReportModel::setDocument(const FieldDocument& document)
    {
        return applyPrepared(prepareDocument(document,
            [](const QString& source) { return ks::i18n::sourceText(source); }, m_state));
    }

    // 语言变化只重算显示缓存；规范名称、路径及层级不随语言变化。
    void ComplexReportModel::relocalize()
    {
        setDocument(m_state->snapshot);
    }

    QModelIndex ComplexReportModel::index(int row, int column, const QModelIndex& parent) const
    {
        if (row < 0 || column < 0 || column >= 3 || (parent.isValid() && (parent.model() != this || parent.column() != 0)))
        {
            return {};
        }
        const QVector<int>* children = &m_state->roots;
        if (parent.isValid())
        {
            const quintptr id = parent.internalId();
            if (id == 0 || id > static_cast<quintptr>(m_state->entries.size()))
            {
                return {};
            }
            children = &m_state->entries.at(static_cast<int>(id - 1)).children;
        }
        return row < children->size() ? createIndex(row, column, static_cast<quintptr>(children->at(row) + 1)) : QModelIndex();
    }

    QModelIndex ComplexReportModel::parent(const QModelIndex& child) const
    {
        if (!child.isValid() || child.model() != this || child.internalId() == 0
            || child.internalId() > static_cast<quintptr>(m_state->entries.size()))
        {
            return {};
        }
        const int parentIndex = m_state->entries.at(static_cast<int>(child.internalId() - 1)).parent;
        if (parentIndex < 0)
        {
            return {};
        }
        const State::Entry& entry = m_state->entries.at(parentIndex);
        return createIndex(entry.row, 0, static_cast<quintptr>(parentIndex + 1));
    }

    int ComplexReportModel::rowCount(const QModelIndex& parent) const
    {
        if (!parent.isValid())
        {
            return static_cast<int>(m_state->roots.size());
        }
        if (parent.model() != this || parent.column() != 0 || parent.internalId() == 0
            || parent.internalId() > static_cast<quintptr>(m_state->entries.size()))
        {
            return 0;
        }
        return static_cast<int>(m_state->entries.at(static_cast<int>(parent.internalId() - 1)).children.size());
    }

    int ComplexReportModel::columnCount(const QModelIndex& parent) const
    {
        // 字段、偏移、类型/值是可分别选择复制的真实列；只有首列拥有子树。
        return parent.isValid() && (parent.model() != this || parent.column() != 0) ? 0 : 3;
    }

    QVariant ComplexReportModel::data(const QModelIndex& index, int role) const
    {
        if (!index.isValid() || index.model() != this || index.column() < 0 || index.column() >= 3 || index.internalId() == 0
            || index.internalId() > static_cast<quintptr>(m_state->entries.size()))
        {
            return {};
        }
        const State::Entry& entry = m_state->entries.at(static_cast<int>(index.internalId() - 1));
        switch (role)
        {
        case Qt::DisplayRole:
            if (index.column() == 0)
            {
                if (entry.kind == ReportNodeKind::Note) return m_state->noteLabel;
                return entry.kind == ReportNodeKind::Section && !entry.summary.isEmpty() ? entry.summary : entry.name;
            }
            if (index.column() == 1) return entry.offset;
            if (!entry.type.isEmpty()) return entry.type;
            if (entry.reportedCount >= 0)
                return m_state->countPattern.arg(entry.reportedCount);
            return entry.value;
        case ReportNameRole: return entry.name;
        case Qt::ToolTipRole:
            return entry.name + QLatin1Char('\n') + entry.value;
        case Qt::AccessibleTextRole: return data(index, Qt::DisplayRole); // 每个实际单元格只朗读自身内容。
        case ReportKindRole: return static_cast<int>(entry.kind);
        case ReportValueRole: return entry.value;
        case ReportDepthRole: return entry.depth;
        case ReportPathRole: return entry.path;
        case ReportCountRole: return entry.count;
        case ReportInitiallyExpandedRole: return entry.expanded;
        case ReportStatusRole: return static_cast<int>(entry.status);
        case ReportSearchRole: return m_state->fullSearch(static_cast<int>(index.internalId() - 1));
        case ReportSummaryRole: return entry.summary;
        case ReportPurposeRole: return static_cast<int>(entry.purpose);
        case ReportReportedCountRole: return entry.reportedCount;
        case ReportPreferredScopeRole: return entry.preferredScope;
        default: return {};
        }
    }

    // 三列分别承担定位、比较偏移和读取类型/值，避免把多个值塞在一条不对齐的预览中。
    QVariant ComplexReportModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        return section >= 0 && section < m_state->headers.size() ? m_state->headers.at(section) : QVariant();
    }

    Qt::ItemFlags ComplexReportModel::flags(const QModelIndex& index) const
    {
        return index.isValid() && index.model() == this
            ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
    }

    QModelIndex ComplexReportModel::indexForPath(const QString& path) const
    {
        const auto found = m_state->paths.constFind(path);
        if (found == m_state->paths.constEnd())
        {
            return {};
        }
        const State::Entry& entry = m_state->entries.at(found.value());
        return createIndex(entry.row, 0, static_cast<quintptr>(found.value() + 1));
    }

    QVector<QModelIndex> ComplexReportModel::initialExpansion() const
    {
        QVector<QModelIndex> result;
        for (const State::Entry& entry : m_state->entries)
        {
            if (entry.expanded && !entry.children.isEmpty())
            {
                result.append(indexForPath(entry.path));
            }
        }
        return result;
    }

    QVector<QModelIndex> ComplexReportModel::sectionIndexes() const
    {
        QVector<QModelIndex> result;
        for (int rootIndex : m_state->roots)
        {
            const State::Entry& entry = m_state->entries.at(rootIndex);
            if (entry.kind == ReportNodeKind::Section)
            {
                result.append(indexForPath(entry.path));
            }
        }
        return result;
    }

    // 完整复制回到规范节点；长值的合成详情返回原值，不把同一内容重复拼入子树。
    QString ComplexReportModel::textForIndex(const QModelIndex& index) const
    {
        if (!index.isValid())
        {
            return m_state->snapshot.toPlainText(true);
        }
        if (index.model() != this || index.internalId() == 0
            || index.internalId() > static_cast<quintptr>(m_state->entries.size()))
        {
            return {};
        }
        const State::Entry& entry = m_state->entries.at(static_cast<int>(index.internalId() - 1));
        if (entry.kind == ReportNodeKind::ValueDetail)
        {
            return entry.value;
        }
        const FieldNode* source = m_state->sourceNode(entry);
        FieldDocument subtree;
        if (source)
        {
            subtree.nodes.append(*source);
        }
        return subtree.toPlainText(true);
    }

    const FieldDocument& ComplexReportModel::document() const
    {
        return m_state->snapshot;
    }

    bool ComplexReportModel::containsPath(const Snapshot& source, const QString& path)
    {
        return source && source->paths.contains(path);
    }

    QString ComplexReportModel::preferredScopePath(const Snapshot& source)
    {
        QString first;
        if (source) for (int item : source->roots)
        {
            const State::Entry& entry = source->entries.at(item);
            if (entry.purpose != FieldNode::ReportRole::Catalog) continue;
            if (entry.preferredScope) return entry.path;
            if (first.isEmpty()) first = entry.path;
        }
        return first;
    }

    ComplexReportModel::FilterSnapshot ComplexReportModel::prepareFilter(Snapshot source,
        const QString& query, const QString& scopePath, bool diagnosticsOnly, const std::function<bool()>& cancelled)
    {
        if (!source || (cancelled && cancelled())) return {};
        auto result = std::make_unique<FilterState>();
        result->source = std::move(source);
        result->query = query.trimmed();
        result->diagnosticsOnly = diagnosticsOnly;
        const State& state = *result->source;
        const int size = static_cast<int>(state.entries.size());
        const int scope = state.paths.value(scopePath, -1);
        const bool missingScope = !scopePath.isEmpty() && scope < 0;
        result->scopePath = scopePath;
        result->accepted.resize(size);
        result->matchesBelow.fill(0, size);
        result->childRows.fill(0, size);
        QBitArray matched(size, true);
        const QStringList terms = result->query.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        // AND terms can match different ancestors. Each term propagates one bit in preorder;
        // no descendant stores or scans a copied full ancestor document.
        for (const QString& term : terms)
        {
            QBitArray inherited(size);
            const bool title = state.titleSearch.contains(term, Qt::CaseInsensitive);
            for (int item = 0; item < size; ++item)
            {
                if ((item & 255) == 0 && cancelled && cancelled()) return {};
                const State::Entry& entry = state.entries.at(item);
                const bool hit = title || (entry.parent >= 0 && inherited.testBit(entry.parent))
                    || entry.search.contains(term, Qt::CaseInsensitive);
                inherited.setBit(item, hit);
                if (!hit) matched.clearBit(item);
            }
        }
        QBitArray eligible(size);
        for (int item = 0; item < size; ++item)
        {
            if ((item & 255) == 0 && cancelled && cancelled()) return {};
            const State::Entry& entry = state.entries.at(item);
            const bool inScope = scope < 0 || (item >= scope && item < state.entries.at(scope).end)
                || (scope >= item && scope < entry.end);
            const bool allowed = !missingScope && inScope && entry.purpose != FieldNode::ReportRole::Metadata
                && entry.kind != ReportNodeKind::ValueDetail
                && (!diagnosticsOnly || state.entries.at(entry.root).purpose != FieldNode::ReportRole::Catalog);
            eligible.setBit(item, allowed);
            result->accepted.setBit(item, allowed && matched.testBit(item));
        }
        // Close accepted ancestors once on the worker; Qt recursive proxy filtering stays disabled.
        for (int item = size - 1; item >= 0; --item)
        {
            if ((item & 255) == 0 && cancelled && cancelled()) return {};
            const State::Entry& entry = state.entries.at(item);
            if (result->accepted.testBit(item) && entry.parent >= 0 && eligible.testBit(entry.parent))
                result->accepted.setBit(entry.parent);
        }
        for (int item = size - 1; item >= 0; --item)
        {
            if ((item & 255) == 0 && cancelled && cancelled()) return {};
            if (!result->accepted.testBit(item)) continue;
            const State::Entry& entry = state.entries.at(item);
            const int count = entry.purpose == FieldNode::ReportRole::Record || result->childRows.at(item) == 0
                ? 1 : result->matchesBelow.at(item);
            if (entry.parent < 0)
            {
                ++result->rootRows;
                result->rootMatches += count;
            }
            else
            {
                ++result->childRows[entry.parent];
                result->matchesBelow[entry.parent] += count;
            }
        }
        for (int item = 0; item < size; ++item)
        {
            const State::Entry& entry = state.entries.at(item);
            if (result->accepted.testBit(item) && (entry.purpose == FieldNode::ReportRole::Collection
                || entry.kind == ReportNodeKind::Section)) result->expansionPaths.append(entry.path);
        }
        return FilterSnapshot(result.release(), [](const FilterState* retired)
        { QThreadPool::globalInstance()->start([retired]() { delete retired; }); });
    }

    ComplexReportFilter::ComplexReportFilter(QObject* parent) : QSortFilterProxyModel(parent)
    {
        setRecursiveFilteringEnabled(false); // Prepared bitmap already includes the exact ancestor closure.
        setAutoAcceptChildRows(false);
    }

    void ComplexReportFilter::setSourceModel(QAbstractItemModel* model)
    {
        for (const auto& connection : m_sourceConnections) disconnect(connection);
        m_sourceConnections.clear();
        m_prepared.reset();
        m_staged.reset();
        QSortFilterProxyModel::setSourceModel(model);
        if (model)
        {
            const auto refresh = [this]() { if (!m_preparedSearch) rebuildPrepared(); };
            m_sourceConnections.append(connect(model, &QAbstractItemModel::modelReset, this, refresh));
            m_sourceConnections.append(connect(model, &QAbstractItemModel::dataChanged, this, refresh));
        }
        if (!m_preparedSearch) rebuildPrepared();
    }

    void ComplexReportFilter::setPreparedSearchEnabled(bool enabled)
    {
        m_preparedSearch = enabled;
        setDynamicSortFilter(!enabled);
        if (!enabled) rebuildPrepared();
    }

    void ComplexReportFilter::stagePrepared(ComplexReportModel::FilterSnapshot prepared)
    {
        m_staged = std::move(prepared);
    }

    void ComplexReportFilter::applyPrepared(ComplexReportModel::FilterSnapshot prepared)
    {
        const auto* model = dynamic_cast<const ComplexReportModel*>(sourceModel());
        if (!prepared || !model || prepared->source != model->snapshot()) return;
        m_query = prepared->query;
        m_scopePath = prepared->scopePath;
        m_diagnosticsOnly = prepared->diagnosticsOnly;
        m_prepared = std::move(prepared);
        m_staged.reset();
        invalidateFilter();
    }

    const ComplexReportModel::FilterState* ComplexReportFilter::activePrepared() const
    {
        const auto* model = dynamic_cast<const ComplexReportModel*>(sourceModel());
        if (!model) return nullptr;
        const auto current = model->snapshot();
        if (m_staged && m_staged->source == current) return m_staged.get();
        return m_prepared && m_prepared->source == current ? m_prepared.get() : nullptr;
    }

    void ComplexReportFilter::rebuildPrepared()
    {
        if (const auto* model = dynamic_cast<const ComplexReportModel*>(sourceModel()))
            applyPrepared(ComplexReportModel::prepareFilter(model->snapshot(), m_query, m_scopePath, m_diagnosticsOnly));
    }

    void ComplexReportFilter::setQuery(const QString& query)
    {
        if (m_query == query.trimmed()) return;
        m_query = query.trimmed();
        if (!m_preparedSearch) rebuildPrepared();
    }

    void ComplexReportFilter::setDiagnosticsOnly(bool enabled)
    {
        if (m_diagnosticsOnly == enabled) return;
        m_diagnosticsOnly = enabled;
        if (!m_preparedSearch) rebuildPrepared();
    }

    void ComplexReportFilter::setScopePath(const QString& path)
    {
        if (m_scopePath == path) return;
        m_scopePath = path;
        if (!m_preparedSearch) rebuildPrepared();
    }

    bool ComplexReportFilter::filterAcceptsRow(int row, const QModelIndex& parent) const
    {
        const auto* prepared = activePrepared();
        if (!prepared) return false;
        const QModelIndex index = sourceModel()->index(row, 0, parent);
        const int item = static_cast<int>(index.internalId()) - 1;
        return item >= 0 && item < prepared->accepted.size() && prepared->accepted.testBit(item);
    }

    int ComplexReportFilter::matchCount(const QModelIndex& proxyRoot) const
    {
        const auto* prepared = activePrepared();
        if (!prepared) return 0;
        if (!proxyRoot.isValid()) return prepared->rootMatches;
        const int item = static_cast<int>(mapToSource(proxyRoot).internalId()) - 1;
        return item >= 0 && item < prepared->matchesBelow.size() ? prepared->matchesBelow.at(item) : 0;
    }

    QStringList ComplexReportFilter::expansionPaths() const
    {
        const auto* prepared = activePrepared();
        return prepared ? prepared->expansionPaths : QStringList();
    }
}
