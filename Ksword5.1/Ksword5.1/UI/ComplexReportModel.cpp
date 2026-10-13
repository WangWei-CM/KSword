#include "./ComplexReportModel.h"
#include "../Internationalization/LanguageManager.h"

#include <QHash>
#include <QStringList>
#include <QUrl>
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
            int depth = 0; // 绘制层级，根分区为零。
            ReportNodeKind kind = ReportNodeKind::Field; // 原分区/字段或合成的长值详情。
            QVector<int> children; // 子节点的稳定 preorder 索引。
            QVector<int> sourcePath; // 规范文档中的逐层下标，合成子项复用父字段路径。
            QString path; // 不依赖本地化和显示名裁短的节点身份。
            QString name; // 本地化后的短显示名。
            QString value; // 完整值，不截断复制、搜索或行内详情。
            QString search; // 原名、完整值和祖先上下文。
            QString summary; // 生产者指定的摘要短标题或原分区名。
            FieldNode::Status status = FieldNode::Status::Neutral; // 只接受生产者的真实语义。
            int count = 0; // 规范文档直接子项数，不把合成长值行计入业务数量。
            bool expanded = false; // 首次展开策略，不在值刷新时重设视图状态。
        };

        explicit State(const FieldDocument& source) : snapshot(source)
        {
            const QString context = localized(snapshot.title, true) + QLatin1Char('\n') + snapshot.title;
            appendNodes(snapshot.nodes, -1, {}, QString(), context, QString());
        }

        // 输入规范节点和翻译策略，返回完整本地化文本；业务数据不会经 HTML 或报告文本解析。
        static QString localized(const QString& text, bool translate)
        {
            return translate ? ks::i18n::sourceText(text) : text;
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
        static QString fieldPropertySummary(const FieldNode& node)
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
            const QString& parentPath, const QString& context, const QString& parentName)
        {
            QHash<QString, int> occurrences; // 当前兄弟层的同名节点计数。
            for (int row = 0; row < nodes.size(); ++row)
            {
                const FieldNode& node = nodes.at(row);
                const QString key = QString::number(static_cast<int>(node.kind)) + QLatin1Char(':') + node.name;
                const int occurrence = occurrences.value(key);
                occurrences.insert(key, occurrence + 1);
                Entry entry;
                entry.parent = parentIndex;
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
                entry.search = context + QLatin1Char('\n') + node.name + QLatin1Char('\n')
                    + localized(node.name, node.translateName) + QLatin1Char('\n') + node.value
                    + QLatin1Char('\n') + entry.value;
                entry.summary = node.summaryLabel.isEmpty()
                    ? localized(node.name, node.translateName) : localized(node.summaryLabel, true);
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
                const QString childContext = entries.at(entryIndex).search;
                appendNodes(node.children, entryIndex, childSourcePrefix, childPath, childContext, node.name);
                if (needsValueDetail(node))
                {
                    Entry detail;
                    detail.parent = entryIndex;
                    detail.row = static_cast<int>(entries.at(entryIndex).children.size());
                    detail.depth = entries.at(entryIndex).depth + 1;
                    detail.kind = ReportNodeKind::ValueDetail;
                    detail.sourcePath = childSourcePrefix;
                    detail.path = childPath + QStringLiteral("/value-detail");
                    detail.value = entries.at(entryIndex).value;
                    detail.search = childContext;
                    detail.status = node.status;
                    const int detailIndex = static_cast<int>(entries.size());
                    entries.append(std::move(detail));
                    entries[entryIndex].children.append(detailIndex);
                    paths.insert(entries.at(detailIndex).path, detailIndex);
                }
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

        FieldDocument snapshot; // 复制、导出与所有角色共用的完整规范文档快照。
        QVector<Entry> entries; // internalId 使用此 preorder 下标加一，零留给无效索引。
        QVector<int> roots; // 顶层原分区和字段的阅读次序。
        QHash<QString, int> paths; // 稳定路径到 preorder 下标的快速定位。
    };

    ComplexReportModel::ComplexReportModel(QObject* parent)
        : QAbstractItemModel(parent), m_state(std::make_unique<State>(FieldDocument{}))
    {
    }

    ComplexReportModel::~ComplexReportModel() = default;

    // 新快照只在拓扑变化时 reset；普通值更新按同父节点的连续区间合并通知。
    bool ComplexReportModel::setDocument(const FieldDocument& document)
    {
        auto next = std::make_unique<State>(document);
        if (!m_state->sameStructure(*next))
        {
            beginResetModel();
            m_state = std::move(next);
            endResetModel();
            return true;
        }
        QHash<int, QVector<int>> changedRows; // 每个父节点中真正变化的行号。
        for (int index = 0; index < next->entries.size(); ++index)
        {
            const State::Entry& entry = next->entries.at(index);
            if (!State::sameDisplay(m_state->entries.at(index), entry))
            {
                changedRows[entry.parent].append(entry.row);
            }
        }
        m_state = std::move(next); // integer internalId 和所有持久索引仍指向同一节点身份。
        for (auto iterator = changedRows.constBegin(); iterator != changedRows.constEnd(); ++iterator)
        {
            QVector<int> rows = iterator.value();
            std::sort(rows.begin(), rows.end());
            const QModelIndex parentIndex = iterator.key() < 0 ? QModelIndex()
                : indexForPath(m_state->entries.at(iterator.key()).path);
            for (int offset = 0; offset < rows.size();)
            {
                const int first = rows.at(offset);
                int last = first;
                while (++offset < rows.size() && rows.at(offset) == last + 1)
                {
                    last = rows.at(offset);
                }
                emit dataChanged(index(first, 0, parentIndex), index(last, 0, parentIndex));
            }
        }
        return false;
    }

    // 语言变化只重算显示缓存；规范名称、路径及层级不随语言变化。
    void ComplexReportModel::relocalize()
    {
        setDocument(m_state->snapshot);
    }

    QModelIndex ComplexReportModel::index(int row, int column, const QModelIndex& parent) const
    {
        if (row < 0 || column != 0 || (parent.isValid() && (parent.model() != this || parent.column() != 0)))
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
        // 委托绘制同一完整行；外部模型或非首列不能成为本报告的子树宿主。
        return parent.isValid() && (parent.model() != this || parent.column() != 0) ? 0 : 1;
    }

    QVariant ComplexReportModel::data(const QModelIndex& index, int role) const
    {
        if (!index.isValid() || index.model() != this || index.column() != 0 || index.internalId() == 0
            || index.internalId() > static_cast<quintptr>(m_state->entries.size()))
        {
            return {};
        }
        const State::Entry& entry = m_state->entries.at(static_cast<int>(index.internalId() - 1));
        switch (role)
        {
        case Qt::DisplayRole:
        case ReportNameRole: return entry.name;
        case Qt::ToolTipRole:
        case Qt::AccessibleTextRole: return entry.name + QLatin1Char('\n') + entry.value;
        case ReportKindRole: return static_cast<int>(entry.kind);
        case ReportValueRole: return entry.value;
        case ReportDepthRole: return entry.depth;
        case ReportPathRole: return entry.path;
        case ReportCountRole: return entry.count;
        case ReportInitiallyExpandedRole: return entry.expanded;
        case ReportStatusRole: return static_cast<int>(entry.status);
        case ReportSearchRole: return entry.search;
        case ReportSummaryRole: return entry.summary;
        default: return {};
        }
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
}
