#include "./McpUiController.h"
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QItemSelectionModel>
#include <QPersistentModelIndex>
#include <QTableView>
#include <QTreeView>
#include <QVariant>
#include <cmath>
#include <limits>

namespace ks::ui
{
namespace
{
// parentRows：树路径按当前已加载模型行号解析，不调用 fetchMore 或展开全部树。
bool parentRows(const QJsonObject& params, QList<int>* rows)
{
    const QJsonValue value = params.value(QStringLiteral("parent_rows"));
    if (value.isUndefined())
    {
        return true;
    }
    if (!value.isArray() || value.toArray().size() > 32)
    {
        return false;
    }
    for (const QJsonValue row : value.toArray())
    {
        const double number = row.toDouble(); // 不能以小数或字符串寻址树节点。
        if (!row.isDouble() || !std::isfinite(number) || std::floor(number) != number
            || number < 0 || number > std::numeric_limits<int>::max())
        {
            return false;
        }
        rows->append(static_cast<int>(number));
    }
    return true;
}

// cellValue：只输出标准 JSON 标量，复杂 QVariant 不展开；字符串有单格/总量预算。
QJsonValue cellValue(const QVariant& value, int* remaining, bool* truncated)
{
    if (!value.isValid() || value.isNull())
    {
        return QJsonValue(QJsonValue::Null);
    }
    if (value.metaType().id() == QMetaType::Bool)
    {
        return value.toBool();
    }
    const int type = value.metaType().id(); // DisplayRole 中的数值保留类型与有限性。
    if (type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::Double
        || type == QMetaType::Float || type == QMetaType::LongLong || type == QMetaType::ULongLong)
    {
        const double number = value.toDouble();
        if (std::isfinite(number) && std::abs(number) <= 9007199254740991.0)
        {
            return number;
        }
    }
    // 未知 QVariant 不调用自定义转换器；只接收字符串与原生数值的显示形式。
    const bool supported = type == QMetaType::QString || type == QMetaType::QChar
        || type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::Double
        || type == QMetaType::Float || type == QMetaType::LongLong || type == QMetaType::ULongLong;
    const QString text = supported ? value.toString() : QString{};
    const int amount = static_cast<int>(qMin<qsizetype>(qMin<qsizetype>(text.size(), 1024), *remaining));
    *remaining -= amount;
    *truncated = *truncated || amount < text.size();
    return text.left(amount);
}
}

// modelRevision：修订绑定 view/model/模型信号代次，排序、替换和结构改变都会失效。
QString McpUiController::modelRevision(QAbstractItemView* view)
{
    if (!view || !view->model())
    {
        return {};
    }
    QAbstractItemModel* model = view->model(); // 信号观察与视图访问都在 GUI 线程。
    if (!m_models.contains(model))
    {
        const QString identity = QString::number(++m_nextModel); // 模型销毁后不复用身份。
        m_models.insert(model, {model, 1, identity});
        const auto changed = [this, model]()
        {
            auto watch = m_models.find(model); // close/销毁后，旧连接不能重新创建记录。
            if (!m_closed && watch != m_models.end())
            {
                ++watch->generation;
            }
        };
        const auto begin = [this, model, changed]()
        {
            changed();
            auto watch = m_models.find(model);
            if (!m_closed && watch != m_models.end())
            {
                ++watch->transactionDepth;
            }
        };
        const auto end = [this, model, changed]()
        {
            changed();
            auto watch = m_models.find(model);
            if (!m_closed && watch != m_models.end())
            {
                watch->transactionDepth = qMax(0, watch->transactionDepth - 1);
            }
        };
        // 同时观察 aboutTo 和完成信号，拒绝在重置/布局事务中取得的旧行。
        connect(model, &QAbstractItemModel::dataChanged, this, changed);
        connect(model, &QAbstractItemModel::headerDataChanged, this, changed);
        connect(model, &QAbstractItemModel::modelAboutToBeReset, this, begin);
        connect(model, &QAbstractItemModel::modelReset, this, end);
        connect(model, &QAbstractItemModel::layoutAboutToBeChanged, this, begin);
        connect(model, &QAbstractItemModel::layoutChanged, this, end);
        connect(model, &QAbstractItemModel::rowsAboutToBeInserted, this, begin);
        connect(model, &QAbstractItemModel::rowsInserted, this, end);
        connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this, begin);
        connect(model, &QAbstractItemModel::rowsRemoved, this, end);
        connect(model, &QAbstractItemModel::rowsAboutToBeMoved, this, begin);
        connect(model, &QAbstractItemModel::rowsMoved, this, end);
        connect(model, &QAbstractItemModel::columnsAboutToBeInserted, this, begin);
        connect(model, &QAbstractItemModel::columnsInserted, this, end);
        connect(model, &QAbstractItemModel::columnsAboutToBeRemoved, this, begin);
        connect(model, &QAbstractItemModel::columnsRemoved, this, end);
        connect(model, &QAbstractItemModel::columnsAboutToBeMoved, this, begin);
        connect(model, &QAbstractItemModel::columnsMoved, this, end);
        connect(model, &QObject::destroyed, this, [this, model]()
        {
            m_models.remove(model);
        });
    }
    const ModelWatch watch = m_models.value(model); // 不持有可被信号回调失效的 QHash 引用。
    if (watch.transactionDepth != 0)
    {
        return {}; // 不在模型重排事务中生成可用行号修订。
    }
    // rootIndex 相等比较不调用模型虚函数，也不需要暴露 internalPointer。
    const QPersistentModelIndex root(view->rootIndex());
    if (!m_viewRoots.contains(view) || m_viewRoots.value(view) != root)
    {
        m_viewRoots.insert(view, root);
        m_rootIds.insert(view, QString::number(++m_nextRoot));
    }
    return registerObject(view) + QStringLiteral(":m:") + watch.identity
        + QStringLiteral(":g:") + QString::number(watch.generation)
        + QStringLiteral(":r:") + m_rootIds.value(view);
}

// readTable：读取指定矩形和树父路径，固定工作量，不主动拉取尚未加载的数据。
QJsonObject McpUiController::readTable(const QJsonObject& params)
{
    int rowStart = 0; // 已加载父节点下的行起点。
    int rowCount = 0; // 单次行预算，默认 40，硬上限 200。
    int columnStart = 0; // 列起点，保留模型原始列序。
    int columnCount = 0; // 单次列预算，默认 16，硬上限 64。
    QList<int> path; // 树父节点路径，以列 0 逐级定位。
    if (!integer(params, QStringLiteral("row_start"), 0, 0, std::numeric_limits<int>::max(), &rowStart)
        || !integer(params, QStringLiteral("row_count"), 40, 1, 200, &rowCount)
        || !integer(params, QStringLiteral("column_start"), 0, 0, std::numeric_limits<int>::max(), &columnStart)
        || !integer(params, QStringLiteral("column_count"), 16, 1, 64, &columnCount)
        || rowCount * columnCount > 2048 || !parentRows(params, &path))
    {
        return fail(QStringLiteral("invalid_params"));
    }
    QObject* object = resolve(params.value(QStringLiteral("object_id")).toString());
    if (!object)
    {
        return fail(QStringLiteral("unknown_object"));
    }
    QPointer<QAbstractItemView> view = qobject_cast<QAbstractItemView*>(object);
    if (!view)
    {
        return fail(QStringLiteral("unsupported_control"));
    }
    if (!allowedByModal(view))
    {
        return fail(QStringLiteral("modal_blocked"));
    }
    QPointer<QAbstractItemModel> model(view->model()); // 不以裸模型指针跨虚函数调用。
    if (!model)
    {
        return fail(QStringLiteral("model_unavailable"));
    }
    const QString revision = modelRevision(view); // 读取前固定本次快照身份。
    if (revision.isEmpty())
    {
        return fail(QStringLiteral("model_busy"));
    }
    const QPointer<McpUiController> self(this); // 模型 data() 可能进入业务回调并关闭主窗口。
    const auto stable = [this, self, view, model, revision]()
    {
        return self && !m_closed && view && model && view->model() == model
            && modelRevision(view) == revision;
    };
    QPersistentModelIndex parent(view->rootIndex()); // persistent index 可检测结构销毁。
    for (int row : path)
    {
        const int count = model->rowCount(parent);
        if (!stable())
        {
            return fail(QStringLiteral("stale_revision"));
        }
        if (row >= count)
        {
            return fail(QStringLiteral("invalid_parent"));
        }
        const QPersistentModelIndex next(model->index(row, 0, parent));
        if (!stable() || !next.isValid())
        {
            return fail(QStringLiteral("stale_revision"));
        }
        parent = next;
    }
    const int rowsTotal = model->rowCount(parent);
    if (!stable())
    {
        return fail(QStringLiteral("stale_revision"));
    }
    const int columnsTotal = model->columnCount(parent);
    if (!stable() || rowsTotal < 0 || columnsTotal < 0)
    {
        return fail(QStringLiteral("stale_revision"));
    }
    const int rowsToRead = qMin(rowCount, qMax(0, rowsTotal - rowStart));
    const int columnsToRead = qMin(columnCount, qMax(0, columnsTotal - columnStart));
    QJsonArray headers; // 表头文字按同一总字符预算剪裁。
    QJsonArray rows; // 仅包含指定矩形，行号在 data.row_start 中说明。
    QJsonArray columnHidden; // 模型原始列序与 GUI 当前显隐分别表达。
    QJsonArray rowHidden; // 不以隐藏行冒充当前可见目标。
    int remaining = 131072; // 总输出文字码元上限，避免单格预算乘法膨胀。
    bool truncated = false; // 明确报告文本剪裁和矩形分页。
    for (int column = 0; column < columnsToRead; ++column)
    {
        const int actualColumn = columnStart + column;
        const QVariant value = model->headerData(actualColumn, Qt::Horizontal, Qt::DisplayRole);
        if (!stable())
        {
            return fail(QStringLiteral("stale_revision"));
        }
        headers.append(cellValue(value, &remaining, &truncated));
        const bool hidden = qobject_cast<QTableView*>(view)
            ? qobject_cast<QTableView*>(view)->isColumnHidden(actualColumn)
            : qobject_cast<QTreeView*>(view) && qobject_cast<QTreeView*>(view)->isColumnHidden(actualColumn);
        columnHidden.append(hidden);
    }
    for (int row = 0; row < rowsToRead; ++row)
    {
        const int actualRow = rowStart + row;
        QJsonArray values; // 每行只保存请求的列。
        for (int column = 0; column < columnsToRead; ++column)
        {
            const QPersistentModelIndex index(model->index(actualRow, columnStart + column, parent));
            if (!stable())
            {
                return fail(QStringLiteral("stale_revision"));
            }
            const QVariant value = index.isValid() ? model->data(index, Qt::DisplayRole) : QVariant{};
            if (!stable())
            {
                return fail(QStringLiteral("stale_revision"));
            }
            values.append(cellValue(value, &remaining, &truncated));
        }
        rows.append(values);
        const bool hidden = qobject_cast<QTableView*>(view)
            ? qobject_cast<QTableView*>(view)->isRowHidden(actualRow)
            : qobject_cast<QTreeView*>(view) && qobject_cast<QTreeView*>(view)->isRowHidden(actualRow, parent);
        rowHidden.append(hidden);
    }
    if (!stable())
    {
        return fail(QStringLiteral("stale_revision"));
    }
    return ok({{QStringLiteral("revision"), revision},
        {QStringLiteral("headers"), headers}, {QStringLiteral("rows"), rows},
        {QStringLiteral("row_hidden"), rowHidden}, {QStringLiteral("column_hidden"), columnHidden},
        {QStringLiteral("row_total"), rowsTotal}, {QStringLiteral("column_total"), columnsTotal},
        {QStringLiteral("row_start"), rowStart}, {QStringLiteral("column_start"), columnStart},
        {QStringLiteral("parent_rows"), params.value(QStringLiteral("parent_rows")).toArray()},
        {QStringLiteral("loaded_only"), true},
        {QStringLiteral("truncated"), truncated || rowsToRead < rowsTotal - rowStart
            || columnsToRead < columnsTotal - columnStart}});
}

// selectRow：行号只在 read_table 返回的修订上有效；投递前和执行时均复核。
QJsonObject McpUiController::selectRow(const QJsonObject& params)
{
    if (params.value(QStringLiteral("confirmed")) != QJsonValue(true))
    {
        return fail(QStringLiteral("confirmation_required"));
    }
    int row = 0; // 当前模型父节点下的目标行。
    QList<int> path; // 必须与客户端读取时的树路径一致。
    const QJsonValue revisionValue = params.value(QStringLiteral("revision"));
    if (!integer(params, QStringLiteral("row"), -1, 0, std::numeric_limits<int>::max(), &row)
        || row < 0 || !parentRows(params, &path) || !revisionValue.isString()
        || revisionValue.toString().isEmpty())
    {
        return fail(QStringLiteral("invalid_params"));
    }
    QObject* object = resolve(params.value(QStringLiteral("object_id")).toString());
    if (!object)
    {
        return fail(QStringLiteral("unknown_object"));
    }
    QPointer<QAbstractItemView> view = qobject_cast<QAbstractItemView*>(object);
    if (!view)
    {
        return fail(QStringLiteral("unsupported_control"));
    }
    const QString revision = revisionValue.toString(); // 必填，不能默认取当前修订。
    if (modelRevision(view) != revision)
    {
        return fail(QStringLiteral("stale_revision"));
    }
    const QPointer<McpUiController> self(this);
    return enqueue(view, [this, self, view, row, path, revision]() -> QString
    {
        if (!self || m_closed || !view || !view->model() || !view->selectionModel())
        {
            return QStringLiteral("model_unavailable");
        }
        QPointer<QAbstractItemModel> model(view->model());
        const auto stable = [this, self, view, model, revision]()
        {
            return self && !m_closed && view && model && view->model() == model
                && modelRevision(view) == revision;
        };
        if (!view->isEnabled() || !view->isVisible()
            || view->selectionMode() == QAbstractItemView::NoSelection)
        {
            return QStringLiteral("control_unavailable");
        }
        if (!stable())
        {
            return QStringLiteral("stale_revision");
        }
        QPersistentModelIndex parent(view->rootIndex());
        for (int parentRow : path)
        {
            const int count = model->rowCount(parent);
            if (!stable())
            {
                return QStringLiteral("stale_revision");
            }
            if (parentRow >= count)
            {
                return QStringLiteral("invalid_parent");
            }
            const QPersistentModelIndex next(model->index(parentRow, 0, parent));
            if (!stable() || !next.isValid())
            {
                return QStringLiteral("stale_revision");
            }
            parent = next;
        }
        const int count = model->rowCount(parent);
        if (!stable())
        {
            return QStringLiteral("stale_revision");
        }
        if (row >= count)
        {
            return QStringLiteral("invalid_value");
        }
        const QPersistentModelIndex index(model->index(row, 0, parent));
        if (!stable() || !index.isValid())
        {
            return QStringLiteral("stale_revision");
        }
        const Qt::ItemFlags flags = model->flags(index);
        if (!stable())
        {
            return QStringLiteral("stale_revision");
        }
        if (!(flags & Qt::ItemIsEnabled) || !(flags & Qt::ItemIsSelectable))
        {
            return QStringLiteral("control_unavailable");
        }
        // 隐藏行不能被远程选择；树的祖先是否展开继续由原视图负责揭示。
        const bool hidden = qobject_cast<QTableView*>(view)
            ? qobject_cast<QTableView*>(view)->isRowHidden(row)
            : qobject_cast<QTreeView*>(view) && qobject_cast<QTreeView*>(view)->isRowHidden(row, parent);
        if (hidden)
        {
            return QStringLiteral("control_unavailable");
        }
        QPointer<QItemSelectionModel> selection(view->selectionModel());
        selection->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        // currentChanged/selectionChanged 可销毁视图或重排模型，滚动前再次校验。
        if (stable() && selection && index.isValid())
        {
            view->scrollTo(index, QAbstractItemView::EnsureVisible);
        }
        return {}; // 回执只表示选行调用已派发，不承诺后续业务完成。
    });
}
}
