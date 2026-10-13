#include "./McpUiController.h"
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QWidget>
#include <cmath>

namespace ks::ui
{
namespace
{
// within：QObject 拥有链加上实际关联菜单/动作边；有界且拒绝循环归属。
bool within(QObject* object, QObject* boundary,
    const QHash<QObject*, QPointer<QObject>>& menuOwners)
{
    QSet<QObject*> visited; // 同一 QAction 可被多个窗口关联，避免边循环。
    QList<QPointer<QObject>> pending{object}; // 所有跨边引用均为弱引用。
    int edges = 0; // 归属判断总预算，不能被恶意/异常对象图拖成无限遍历。
    while (!pending.isEmpty() && edges++ < 512)
    {
        QObject* current = pending.takeLast();
        if (!current || visited.contains(current))
        {
            continue;
        }
        visited.insert(current);
        if (current == boundary)
        {
            return true;
        }
        if (current->parent())
        {
            pending.append(current->parent());
        }
        const auto owner = menuOwners.constFind(current);
        if (owner != menuOwners.cend())
        {
            QObject* anchor = owner.value(); // 关联被替换后，旧无父菜单不能继续借用宿主权限。
            bool associated = false;
            if (auto* tool = qobject_cast<QToolButton*>(anchor))
            {
                associated = tool->menu() == current;
            }
            else if (auto* push = qobject_cast<QPushButton*>(anchor))
            {
                associated = push->menu() == current;
            }
            else if (auto* actionOwner = qobject_cast<QAction*>(anchor))
            {
                associated = actionOwner->menu() == current;
            }
            if (associated)
            {
                pending.append(anchor);
            }
        }
        if (const auto* action = qobject_cast<QAction*>(current))
        {
            for (QObject* associated : action->associatedObjects())
            {
                if (qobject_cast<QWidget*>(associated) && pending.size() < 512)
                {
                    pending.append(associated);
                }
            }
        }
    }
    return false;
}

// shortText：快照名称有固定字数预算，避免超长日志或提示拖垮 GUI 协议。
QString shortText(const QString& text)
{
    return text.left(1024);
}
}

// 构造：回调只在 GUI 线程调用；控制器不拥有主窗口，关闭时自动作废弱引用。
McpUiController::McpUiController(QWidget* host, PageList list, PageOpen open)
    : QObject(host)
    , m_host(host)
    , m_pageList(std::move(list))
    , m_pageOpen(std::move(open))
    , m_epoch(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

McpUiController::~McpUiController()
{
    close();
}

// close：取消尚未执行的调用，不阻塞原有业务；历史 ID 从此不得再使用。
void McpUiController::close()
{
    if (m_closed)
    {
        return;
    }
    m_closed = true;
    // 未派发调用明确记录取消；嵌套事件循环中的调用只记中断，不推断业务结果。
    const auto operations = m_operations; // 日志回调期间不遍历可被关闭流程修改的原表。
    const QPointer<McpUiController> self(this); // 日志接收方也不能延长控制器生命周期。
    for (auto it = operations.cbegin(); it != operations.cend(); ++it)
    {
        if (it->state == QStringLiteral("queued") || it->state == QStringLiteral("dispatching"))
        {
            reportActivity(it.key(), it->state == QStringLiteral("queued")
                ? QStringLiteral("cancelled") : QStringLiteral("interrupted"));
            if (!self)
            {
                return;
            }
        }
    }
    m_objects.clear();
    m_ids.clear();
    m_menuOwners.clear();
    m_pageRoots.clear();
    m_operations.clear();
    m_operationOrder.clear();
    m_models.clear();
    m_viewRoots.clear();
    m_rootIds.clear();
    m_readAdapters.clear();
    m_pageList = {};
    m_pageOpen = {};
}

// 接收方由主程序显式登记；回调只收到生成的操作身份、状态和固定错误码。
void McpUiController::setActivitySink(ActivitySink sink)
{
    m_activitySink = std::move(sink);
}

void McpUiController::reportActivity(const QString& id, const QString& state, const QString& error)
{
    const ActivitySink sink = m_activitySink; // 副本保护回调栈，不依赖关闭期间的成员存活。
    if (sink)
    {
        sink(QStringLiteral("调用回执 %1 | 状态=%2 | 错误=%3"), {id, state, error});
    }
}

QJsonObject McpUiController::ok(const QJsonObject& data)
{
    return {{QStringLiteral("status"), QStringLiteral("ok")},
        {QStringLiteral("data"), data}};
}

QJsonObject McpUiController::fail(const QString& code)
{
    return {{QStringLiteral("status"), QStringLiteral("error")},
        {QStringLiteral("data"), QJsonObject{}},
        {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}}}};
}

// integer：JSON 小数、非有限数、字符串及越界值都不能悄悄转成行号或预算。
bool McpUiController::integer(const QJsonObject& params, const QString& key,
    int defaultValue, int minimum, int maximum, int* output)
{
    const QJsonValue value = params.value(key); // 未传时使用入口明确的默认值。
    if (value.isUndefined())
    {
        *output = defaultValue;
        return true;
    }
    const double number = value.toDouble(); // 先检查 JSON 类型，再使用数值。
    if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number
        || number < minimum || number > maximum)
    {
        return false;
    }
    *output = static_cast<int>(number);
    return true;
}

// owned：允许宿主所属 QWidget/QAction；无父菜单必须由已知按钮显式关联。
bool McpUiController::owned(QObject* object) const
{
    if (!object || !m_host)
    {
        return false;
    }
    return within(object, m_host, m_menuOwners);
}

// allowedByModal：当前模态窗口存在时，只允许该窗口拥有的对象处理新请求。
bool McpUiController::allowedByModal(QObject* object) const
{
    QWidget* modal = QApplication::activeModalWidget(); // 不自动点击任何业务确认。
    if (!modal)
    {
        return true;
    }
    if (!owned(modal))
    {
        return false;
    }
    return within(object, modal, m_menuOwners);
}

// resolve：不暴露裸指针；每次请求重新校验存活、拥有关系和实例 ID。
QObject* McpUiController::resolve(const QString& id) const
{
    const auto found = m_objects.constFind(id); // 对象销毁后登记会被移除。
    if (found == m_objects.cend() || !owned(found->object))
    {
        return nullptr;
    }
    return found->object;
}

QString McpUiController::registerObject(QObject* object)
{
    if (!owned(object))
    {
        return {};
    }
    const auto existing = m_ids.constFind(object); // 同一活对象使用同一 ID。
    if (existing != m_ids.cend())
    {
        return existing.value();
    }
    if (m_objects.size() >= 20000)
    {
        return {};
    }
    const QString id = m_epoch + QStringLiteral(":o:") + QString::number(++m_nextObject);
    m_objects.insert(id, ObjectEntry{object});
    m_ids.insert(object, id);
    connect(object, &QObject::destroyed, this, [this, object, id]()
    {
        m_objects.remove(id);
        m_ids.remove(object);
        m_menuOwners.remove(object);
        m_viewRoots.remove(object);
        m_rootIds.remove(object);
    });
    return id;
}

// request：固定方法白名单，未知方法与错误类型统一返回机器错误码。
QJsonObject McpUiController::request(const QString& method, const QJsonObject& params)
{
    if (!qApp || QThread::currentThread() != qApp->thread())
    {
        return fail(QStringLiteral("wrong_thread"));
    }
    if (m_closed || !m_host)
    {
        return fail(QStringLiteral("bridge_closed"));
    }
    if (method == QStringLiteral("pages"))
    {
        const PageList callback = m_pageList; // 复制回调，close 重入不能销毁正在调用的闭包。
        return ok({{QStringLiteral("pages"), callback ? callback() : QJsonArray{}}});
    }
    if (method == QStringLiteral("open_page"))
    {
        const QJsonValue id = params.value(QStringLiteral("page_id"));
        if (!id.isString() || id.toString().isEmpty())
        {
            return fail(QStringLiteral("invalid_params"));
        }
        if (QApplication::activeModalWidget())
        {
            return fail(QStringLiteral("modal_blocked"));
        }
        QPointer<McpUiController> self(this); // 导航回调可能触发同步销毁。
        const PageOpen callback = m_pageOpen; // 原导航可能重入 close，不能引用成员闭包。
        QPointer<QWidget> page = callback ? callback(id.toString()) : nullptr;
        if (!self || m_closed || !page || !owned(page))
        {
            return fail(QStringLiteral("page_unavailable"));
        }
        m_pageRoots.insert(id.toString(), page);
        return ok({{QStringLiteral("page_id"), id},
            {QStringLiteral("root_id"), registerObject(page)}});
    }
    if (method == QStringLiteral("snapshot"))
    {
        return snapshot(params);
    }
    if (method == QStringLiteral("operation_status"))
    {
        const QJsonValue id = params.value(QStringLiteral("operation_id"));
        if (!id.isString() || !m_operations.contains(id.toString()))
        {
            return fail(QStringLiteral("unknown_operation"));
        }
        const Operation operation = m_operations.value(id.toString());
        QJsonObject data{{QStringLiteral("operation_id"), id},
            {QStringLiteral("state"), operation.state},
            {QStringLiteral("business_completion"), QStringLiteral("unknown")}};
        if (!operation.error.isEmpty())
        {
            data.insert(QStringLiteral("error_code"), operation.error);
        }
        return ok(data);
    }
    // 所有对象入口要求非空字符串 ID；额外参数由各类型入口独立检查。
    const QJsonValue objectId = params.value(QStringLiteral("object_id"));
    if (!objectId.isString() || objectId.toString().isEmpty())
    {
        return fail(QStringLiteral("invalid_params"));
    }
    if (method == QStringLiteral("action"))
    {
        return action(params);
    }
    if (method == QStringLiteral("set_field"))
    {
        return setField(params);
    }
    if (method == QStringLiteral("read_table"))
    {
        return readTable(params);
    }
    if (method == QStringLiteral("select_row"))
    {
        return selectRow(params);
    }
    if (method == QStringLiteral("read_text"))
    {
        return readText(params);
    }
    if (method == QStringLiteral("read_custom"))
    {
        return readCustom(params);
    }
    return fail(QStringLiteral("unknown_method"));
}

// describe：只读取标准控件语义；密码字段既不返回内容，也不返回长度。
QJsonObject McpUiController::describe(QObject* object, const QString& parentId)
{
    QJsonObject node{{QStringLiteral("id"), registerObject(object)},
        {QStringLiteral("parent_id"), parentId},
        {QStringLiteral("class"), QString::fromLatin1(object->metaObject()->className())},
        {QStringLiteral("object_name"), shortText(object->objectName())}};
    QJsonArray actions; // 仅列固定的类型适配动作。
    QString type = QStringLiteral("object"); // 未适配的对象只保留可发现性。
    QString name; // 描述文字只作为辅助，不能替代 ID。
    if (auto* widget = qobject_cast<QWidget*>(object))
    {
        node.insert(QStringLiteral("visible"), widget->isVisible());
        node.insert(QStringLiteral("enabled"), widget->isEnabled());
        node.insert(QStringLiteral("accessible_id"), shortText(widget->accessibleIdentifier()));
        const QString description = widget->accessibleDescription().isEmpty()
            ? widget->toolTip() : widget->accessibleDescription(); // 图标按钮保留原有悬停释义。
        node.insert(QStringLiteral("description"), shortText(description));
        name = widget->accessibleName();
        if (name.isEmpty())
        {
            name = widget->windowTitle();
        }
        type = QStringLiteral("widget");
    }
    if (auto* button = qobject_cast<QAbstractButton*>(object))
    {
        type = QStringLiteral("button");
        if (name.isEmpty())
        {
            name = button->text();
        }
        actions.append(QStringLiteral("press"));
        if (button->isCheckable())
        {
            actions.append(QStringLiteral("toggle"));
            actions.append(QStringLiteral("set_field")); // 明确宣告布尔字段，agent可指定状态而非盲目反转。
            node.insert(QStringLiteral("field_type"), QStringLiteral("boolean"));
            node.insert(QStringLiteral("checked"), button->isChecked());
            node.insert(QStringLiteral("value"), button->isChecked()); // 与set_field对应的真实值，旧checked字段保留。
        }
        QMenu* menu = nullptr; // 只登记实际按钮菜单，不能任意授予外部菜单归属。
        if (auto* tool = qobject_cast<QToolButton*>(button))
        {
            menu = tool->menu();
        }
        else if (auto* push = qobject_cast<QPushButton*>(button))
        {
            menu = push->menu();
        }
        if (menu)
        {
            m_menuOwners.insert(menu, button);
            actions.append(QStringLiteral("show_menu"));
            node.insert(QStringLiteral("menu_id"), registerObject(menu));
        }
    }
    else if (auto* action = qobject_cast<QAction*>(object))
    {
        type = QStringLiteral("action");
        name = action->text();
        node.insert(QStringLiteral("visible"), action->isVisible());
        node.insert(QStringLiteral("enabled"), action->isEnabled());
        node.insert(QStringLiteral("checked"), action->isChecked());
        node.insert(QStringLiteral("description"), shortText(action->toolTip()));
        actions.append(QStringLiteral("press"));
        if (action->isCheckable())
        {
            actions.append(QStringLiteral("toggle"));
        }
        if (action->menu())
        {
            m_menuOwners.insert(action->menu(), action);
            actions.append(QStringLiteral("show_menu"));
            node.insert(QStringLiteral("menu_id"), registerObject(action->menu()));
        }
    }
    else if (auto* edit = qobject_cast<QLineEdit*>(object))
    {
        type = QStringLiteral("line_edit");
        const bool sensitive = edit->echoMode() != QLineEdit::Normal;
        node.insert(QStringLiteral("sensitive"), sensitive);
        node.insert(QStringLiteral("read_only"), edit->isReadOnly());
        if (!sensitive)
        {
            node.insert(QStringLiteral("value"), shortText(edit->text()));
            node.insert(QStringLiteral("placeholder"), shortText(edit->placeholderText()));
        }
        if (!edit->isReadOnly())
        {
            actions.append(QStringLiteral("set_field"));
        }
        if (!sensitive)
        {
            actions.append(QStringLiteral("read_text"));
        }
    }
    else if (auto* combo = qobject_cast<QComboBox*>(object))
    {
        const QPointer<QComboBox> guarded(combo); // 从第一个模型 getter 前就建立弱引用。
        const QPointer<McpUiController> self(this); // 模型可重入 close/主窗口析构。
        type = QStringLiteral("combo_box");
        node.insert(QStringLiteral("current_index"), combo->currentIndex());
        const bool sensitive = combo->isEditable() && combo->lineEdit()
            && combo->lineEdit()->echoMode() != QLineEdit::Normal;
        node.insert(QStringLiteral("sensitive"), sensitive);
        if (!sensitive)
        {
            const QString currentText = combo->currentText();
            if (!guarded || !self || m_closed)
            {
                return {};
            }
            node.insert(QStringLiteral("value"), shortText(currentText));
        }
        node.insert(QStringLiteral("editable"), combo->isEditable());
        const int count = combo->count(); // count 也可能经由业务模型虚函数。
        if (!guarded || !self || m_closed)
        {
            return {};
        }
        node.insert(QStringLiteral("item_count"), count);
        QJsonArray choices; // 大目录只列前 64 项，不强制加载模型其它数据。
        for (int index = 0; !sensitive && index < count && index < 64; ++index)
        {
            const QString text = combo->itemText(index);
            if (!guarded || !self || m_closed)
            {
                return {}; // snapshot 会通过节点 QPointer 丢弃已销毁对象。
            }
            choices.append(shortText(text));
        }
        node.insert(QStringLiteral("choices"), choices);
        actions.append(QStringLiteral("set_field"));
    }
    else if (auto* spin = qobject_cast<QSpinBox*>(object))
    {
        type = QStringLiteral("spin_box");
        node.insert(QStringLiteral("value"), spin->value());
        node.insert(QStringLiteral("minimum"), spin->minimum());
        node.insert(QStringLiteral("maximum"), spin->maximum());
        node.insert(QStringLiteral("read_only"), spin->isReadOnly());
        if (!spin->isReadOnly())
        {
            actions.append(QStringLiteral("set_field"));
        }
    }
    else if (auto* doubleSpin = qobject_cast<QDoubleSpinBox*>(object))
    {
        type = QStringLiteral("double_spin_box");
        node.insert(QStringLiteral("value"), doubleSpin->value());
        node.insert(QStringLiteral("minimum"), doubleSpin->minimum());
        node.insert(QStringLiteral("maximum"), doubleSpin->maximum());
        node.insert(QStringLiteral("read_only"), doubleSpin->isReadOnly());
        if (!doubleSpin->isReadOnly())
        {
            actions.append(QStringLiteral("set_field"));
        }
    }
    else if (auto* tabs = qobject_cast<QTabWidget*>(object))
    {
        type = QStringLiteral("tab_widget");
        node.insert(QStringLiteral("current_index"), tabs->currentIndex());
        QJsonArray titles; // 原始页序保留，调用端用整数索引选择。
        for (int index = 0; index < tabs->count() && index < 64; ++index)
        {
            titles.append(shortText(tabs->tabText(index)));
        }
        node.insert(QStringLiteral("tabs"), titles);
        actions.append(QStringLiteral("set_field"));
    }
    else if (auto* tabBar = qobject_cast<QTabBar*>(object))
    {
        type = QStringLiteral("tab_bar");
        node.insert(QStringLiteral("current_index"), tabBar->currentIndex());
        QJsonArray titles; // 独立自建导航栏也支持原始索引，不重建页面。
        for (int index = 0; index < tabBar->count() && index < 64; ++index)
        {
            titles.append(shortText(tabBar->tabText(index)));
        }
        node.insert(QStringLiteral("tabs"), titles);
        actions.append(QStringLiteral("set_field"));
    }
    else if (auto* view = qobject_cast<QAbstractItemView*>(object))
    {
        type = QStringLiteral("item_view");
        node.insert(QStringLiteral("revision"), modelRevision(view));
        actions.append(QStringLiteral("read_table"));
        actions.append(QStringLiteral("select_row"));
        if (view->contextMenuPolicy() != Qt::NoContextMenu
            && view->contextMenuPolicy() != Qt::PreventContextMenu)
        {
            actions.append(QStringLiteral("show_menu"));
        }
    }
    else if (auto* label = qobject_cast<QLabel*>(object))
    {
        type = QStringLiteral("label");
        name = label->text();
        actions.append(QStringLiteral("read_text"));
    }
    else if (auto* group = qobject_cast<QGroupBox*>(object))
    {
        name = group->title();
    }
    else if (qobject_cast<QPlainTextEdit*>(object) || qobject_cast<QTextEdit*>(object))
    {
        type = QStringLiteral("text_edit");
        actions.append(QStringLiteral("read_text"));
        const bool readOnly = qobject_cast<QPlainTextEdit*>(object)
            ? qobject_cast<QPlainTextEdit*>(object)->isReadOnly()
            : qobject_cast<QTextEdit*>(object)->isReadOnly();
        node.insert(QStringLiteral("read_only"), readOnly);
        if (!readOnly)
        {
            actions.append(QStringLiteral("set_field"));
        }
    }
    node.insert(QStringLiteral("type"), type);
    if (auto* widget = qobject_cast<QWidget*>(object); type == QStringLiteral("widget")
        && widget->contextMenuPolicy() != Qt::NoContextMenu
        && widget->contextMenuPolicy() != Qt::PreventContextMenu)
    {
        actions.append(QStringLiteral("show_menu")); // 自绘控件仍可复用原右键入口。
    }
    if (name.isEmpty())
    {
        if (auto* namedWidget = qobject_cast<QWidget*>(object))
        {
            name = namedWidget->toolTip(); // 无文本图标控件仍能由调用方识别用途。
        }
    }
    node.insert(QStringLiteral("name"), shortText(name));
    if (adapterFor(object))
    {
        actions.append(QStringLiteral("read_custom")); // 能力发现不执行数据读取回调。
    }
    node.insert(QStringLiteral("actions"), actions);
    return node;
}

// snapshot：有界深度优先遍历，返回平铺节点；动态窗口和菜单沿真实归属发现。
QJsonObject McpUiController::snapshot(const QJsonObject& params)
{
    int maxNodes = 0; // 外部请求的节点预算，硬上限 2000。
    int maxDepth = 0; // 输出层级预算，硬上限 32。
    if (!integer(params, QStringLiteral("max_nodes"), 400, 1, 2000, &maxNodes)
        || !integer(params, QStringLiteral("depth"), 12, 0, 32, &maxDepth))
    {
        return fail(QStringLiteral("invalid_params"));
    }
    const QJsonValue rootValue = params.value(QStringLiteral("root_id"));
    const QJsonValue pageValue = params.value(QStringLiteral("page_id"));
    if ((!rootValue.isUndefined() && !rootValue.isString())
        || (!pageValue.isUndefined() && !pageValue.isString())
        || (!rootValue.toString().isEmpty() && !pageValue.toString().isEmpty()))
    {
        return fail(QStringLiteral("invalid_params"));
    }
    QPointer<QObject> root = m_host; // 默认根，模态窗口出现时缩到模态树。
    if (!rootValue.toString().isEmpty())
    {
        root = resolve(rootValue.toString());
    }
    else if (!pageValue.toString().isEmpty())
    {
        root = m_pageRoots.value(pageValue.toString());
        if (!root)
        {
            return fail(QStringLiteral("page_not_opened"));
        }
    }
    else if (QApplication::activeModalWidget())
    {
        root = QApplication::activeModalWidget();
    }
    else if (QApplication::activePopupWidget())
    {
        root = QApplication::activePopupWidget(); // 已打开临时菜单直接成为默认快照根。
    }
    if (!root || !owned(root))
    {
        return fail(QStringLiteral("unknown_object"));
    }
    if (!allowedByModal(root))
    {
        return fail(QStringLiteral("modal_blocked"));
    }
    // 访问预算也限制非输出的 QObject，避免内部庞大对象树绕过节点上限。
    struct Pending
    {
        QPointer<QObject> object; // 防止获取模型语义时同步销毁下一节点。
        QString parentId; // 最近一个输出节点的身份。
        int depth = 0; // QObject 归属树的深度。
    };
    QList<Pending> pending{{root, {}, 0}};
    QSet<QObject*> visited; // QWidget action 与 QObject child 可能重复。
    QJsonArray nodes; // 受 maxNodes 限制的平铺快照。
    int visits = 0; // 不输出的内部对象也计入工作预算。
    bool truncated = false; // 任一预算被耗尽都明确标记。
    while (!pending.isEmpty() && nodes.size() < maxNodes && visits < maxNodes * 4)
    {
        const Pending item = pending.takeLast(); // 值复制不持有容器内引用。
        QObject* object = item.object;
        if (!object || visited.contains(object) || !owned(object))
        {
            continue;
        }
        visited.insert(object);
        ++visits;
        QString parentId = item.parentId;
        const bool output = qobject_cast<QWidget*>(object) || qobject_cast<QAction*>(object);
        if (output)
        {
            const QPointer<McpUiController> self(this); // 描述里的模型读取可重入关窗。
            const QJsonObject node = describe(object, parentId);
            if (!self || m_closed)
            {
                return fail(QStringLiteral("bridge_closed"));
            }
            if (!item.object)
            {
                continue;
            }
            parentId = node.value(QStringLiteral("id")).toString();
            if (parentId.isEmpty())
            {
                truncated = true;
                continue;
            }
            nodes.append(node);
        }
        if (item.depth >= maxDepth)
        {
            truncated = truncated || !object->children().isEmpty();
            continue;
        }
        QList<QObject*> children = object->children(); // 拷贝后用 QPointer 抵御后续销毁。
        if (auto* widget = qobject_cast<QWidget*>(object))
        {
            for (QAction* action : widget->actions())
            {
                children.append(action);
            }
        }
        if (auto* menu = qobject_cast<QMenu*>(object))
        {
            for (QAction* action : menu->actions())
            {
                children.append(action);
            }
        }
        if (output)
        {
            const QString menuId = nodes.last().toObject().value(QStringLiteral("menu_id")).toString();
            if (!menuId.isEmpty())
            {
                children.append(resolve(menuId));
            }
        }
        for (auto child = children.crbegin(); child != children.crend(); ++child)
        {
            if (pending.size() >= maxNodes * 4)
            {
                truncated = true;
                break;
            }
            pending.append(Pending{*child, parentId, item.depth + 1});
        }
    }
    truncated = truncated || !pending.isEmpty();
    return ok({{QStringLiteral("nodes"), nodes},
        {QStringLiteral("root_ids"), QJsonArray{registerObject(root)}},
        {QStringLiteral("truncated"), truncated},
        {QStringLiteral("epoch"), m_epoch}});
}

// enqueue：动作回执在排队时即可返回；嵌套业务确认保留为原有 UI 交互。
QJsonObject McpUiController::enqueue(QObject* target, std::function<QString()> dispatch)
{
    if (!target || !owned(target))
    {
        return fail(QStringLiteral("unknown_object"));
    }
    if (!allowedByModal(target))
    {
        return fail(QStringLiteral("modal_blocked"));
    }
    if (QWidget* popup = QApplication::activePopupWidget(); popup && !within(target, popup, m_menuOwners))
    {
        return fail(QStringLiteral("popup_blocked")); // 菜单打开时不向背景控件派发动作。
    }
    // 只淘汰已经终止的旧回执，永不丢弃排队中或嵌套事件循环中的操作。
    while (m_operations.size() >= 256 && !m_operationOrder.isEmpty())
    {
        const QString oldest = m_operationOrder.head();
        const QString state = m_operations.value(oldest).state;
        if (state != QStringLiteral("dispatched") && state != QStringLiteral("failed"))
        {
            return fail(QStringLiteral("operation_limit"));
        }
        m_operationOrder.dequeue();
        m_operations.remove(oldest);
    }
    const QString id = m_epoch + QStringLiteral(":op:") + QString::number(++m_nextOperation);
    m_operations.insert(id, {QStringLiteral("queued"), {}});
    m_operationOrder.enqueue(id);
    const QPointer<QObject> guardedTarget(target); // 跨事件循环不捕获裸目标指针。
    QTimer::singleShot(0, this, [this, id, guardedTarget, dispatch = std::move(dispatch)]()
    {
        if (m_closed || !m_operations.contains(id))
        {
            return;
        }
        QString error; // 仅机器码，不将字段值写入错误或日志。
        if (!guardedTarget || !owned(guardedTarget))
        {
            error = QStringLiteral("object_destroyed");
        }
        else if (!allowedByModal(guardedTarget))
        {
            error = QStringLiteral("modal_blocked");
        }
        else if (QWidget* popup = QApplication::activePopupWidget();
            popup && !within(guardedTarget, popup, m_menuOwners))
        {
            error = QStringLiteral("popup_blocked");
        }
        if (!error.isEmpty())
        {
            m_operations[id] = {QStringLiteral("failed"), error};
            reportActivity(id, QStringLiteral("failed"), error);
            return;
        }
        m_operations[id].state = QStringLiteral("dispatching");
        QPointer<McpUiController> self(this); // 控件槽可关闭主窗口并销毁桥。
        reportActivity(id, QStringLiteral("dispatching"));
        if (!self || m_closed)
        {
            return;
        }
        error = dispatch();
        if (!self || m_closed || !m_operations.contains(id))
        {
            return;
        }
        m_operations[id] = {error.isEmpty() ? QStringLiteral("dispatched")
            : QStringLiteral("failed"), error};
        reportActivity(id, m_operations[id].state, error);
    });
    return {{QStringLiteral("status"), QStringLiteral("accepted")},
        {QStringLiteral("data"), QJsonObject{{QStringLiteral("operation_id"), id},
            {QStringLiteral("state"), QStringLiteral("queued")},
            {QStringLiteral("business_completion"), QStringLiteral("unknown")}}}};
}

// action：只允许标准按钮与 QAction，触发已有连接，不代替业务确认或目标复核。
QJsonObject McpUiController::action(const QJsonObject& params)
{
    if (params.value(QStringLiteral("confirmed")) != QJsonValue(true))
    {
        return fail(QStringLiteral("confirmation_required"));
    }
    const QJsonValue value = params.value(QStringLiteral("action"));
    if (!value.isUndefined() && !value.isString())
    {
        return fail(QStringLiteral("invalid_params"));
    }
    const QString name = value.isUndefined() ? QStringLiteral("press") : value.toString();
    if (name != QStringLiteral("press") && name != QStringLiteral("toggle")
        && name != QStringLiteral("show_menu"))
    {
        return fail(QStringLiteral("unsupported_action"));
    }
    QPointer<QObject> target = resolve(params.value(QStringLiteral("object_id")).toString());
    if (!target)
    {
        return fail(QStringLiteral("unknown_object"));
    }
    if (!qobject_cast<QAbstractButton*>(target) && !qobject_cast<QAction*>(target)
        && (name != QStringLiteral("show_menu") || !qobject_cast<QWidget*>(target)))
    {
        return fail(QStringLiteral("unsupported_control"));
    }
    // 表格右键动作绑定当前选中项目和修订，不能在排队期间改用刷新后的同号行。
    QPointer<QAbstractItemView> view = qobject_cast<QAbstractItemView*>(target);
    QString revision;
    QPersistentModelIndex current;
    if (view && name == QStringLiteral("show_menu"))
    {
        revision = modelRevision(view);
        current = view->currentIndex();
        if (revision.isEmpty() || !current.isValid() || !view->selectionModel()
            || !view->selectionModel()->isSelected(current))
        {
            return fail(QStringLiteral("selection_required"));
        }
    }
    const QPointer<McpUiController> self(this); // 原 contextMenuEvent 可同步进入菜单 exec。
    return enqueue(target, [this, self, target, name, view, revision, current]() -> QString
    {
        // 通用控件右键复用原事件处理器，保留每个页面已有的菜单与目标复核。
        if (name == QStringLiteral("show_menu") && !qobject_cast<QAbstractButton*>(target)
            && !qobject_cast<QAction*>(target))
        {
            QPointer<QWidget> widget = qobject_cast<QWidget*>(target);
            if (!widget || !widget->isEnabled() || !widget->isVisible()
                || widget->contextMenuPolicy() == Qt::NoContextMenu
                || widget->contextMenuPolicy() == Qt::PreventContextMenu)
            {
                return QStringLiteral("control_unavailable");
            }
            QPointer<QWidget> receiver = widget; // item view 的右键位置使用原 viewport。
            QPoint local = widget->rect().center(); // 普通控件默认在本体中心发起右键。
            if (view)
            {
                if (!self || m_closed || !current.isValid() || modelRevision(view) != revision
                    || !view->selectionModel() || !view->selectionModel()->isSelected(current))
                {
                    return QStringLiteral("stale_revision");
                }
                const QRect rectangle = view->visualRect(current); // 模型虚函数可重入。
                if (!self || m_closed || !view || !current.isValid()
                    || modelRevision(view) != revision)
                {
                    return QStringLiteral("stale_revision");
                }
                receiver = view->viewport();
                local = rectangle.center();
                if (!receiver || rectangle.isEmpty() || !receiver->rect().contains(local))
                {
                    return QStringLiteral("control_unavailable");
                }
            }
            QContextMenuEvent context(QContextMenuEvent::Mouse,
                local, receiver->mapToGlobal(local));
            QApplication::sendEvent(receiver, &context);
            return context.isAccepted() ? QString{} : QStringLiteral("unsupported_action");
        }
        if (auto* button = qobject_cast<QAbstractButton*>(target))
        {
            if (!button->isEnabled() || !button->isVisible())
            {
                return QStringLiteral("control_unavailable");
            }
            if (name == QStringLiteral("toggle") && !button->isCheckable())
            {
                return QStringLiteral("unsupported_action");
            }
            if (name != QStringLiteral("show_menu"))
            {
                button->click(); // click 保留 clicked/toggled 与原连接语义。
                return {};
            }
            if (auto* tool = qobject_cast<QToolButton*>(button); tool && tool->menu())
            {
                tool->showMenu(); // 原 showMenu 可进入嵌套事件循环，回执仍为 dispatching。
                return {};
            }
            if (auto* push = qobject_cast<QPushButton*>(button); push && push->menu())
            {
                push->showMenu();
                return {};
            }
            return QStringLiteral("unsupported_action");
        }
        auto* action = qobject_cast<QAction*>(target); // 已知支持对象仍须再次判活。
        if (!action || !action->isEnabled() || !action->isVisible())
        {
            return QStringLiteral("control_unavailable");
        }
        // 临时菜单关闭之后不能执行旧动作。共享工具栏动作按当前可见关联检查。
        bool menuAssociated = qobject_cast<QMenu*>(action->parent()) != nullptr;
        bool visibleAssociated = false;
        QPointer<QMenu> visibleMenu; // 当前菜单动作须走原生选择，令 exec 返回正确 QAction。
        for (QObject* associated : action->associatedObjects())
        {
            if (auto* widget = qobject_cast<QWidget*>(associated))
            {
                menuAssociated = menuAssociated || qobject_cast<QMenu*>(widget);
                visibleAssociated = visibleAssociated || widget->isVisible();
                if (auto* menu = qobject_cast<QMenu*>(widget); menu && menu->isVisible())
                {
                    visibleMenu = menu;
                }
            }
        }
        if (menuAssociated && !visibleAssociated)
        {
            return QStringLiteral("control_unavailable");
        }
        if (QWidget* popup = QApplication::activePopupWidget(); popup && !within(action, popup, {}))
        {
            return QStringLiteral("popup_blocked");
        }
        if (name == QStringLiteral("toggle") && !action->isCheckable())
        {
            return QStringLiteral("unsupported_action");
        }
        if (name == QStringLiteral("show_menu") && !action->menu())
        {
            return QStringLiteral("unsupported_action");
        }
        if (visibleMenu)
        {
            visibleMenu->setActiveAction(action); // 保留原菜单 hovered/activeAction 语义。
            if (!self || m_closed || !target || !visibleMenu || !visibleMenu->isVisible()
                || visibleMenu->activeAction() != target)
            {
                return QStringLiteral("control_unavailable");
            }
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(visibleMenu, &enter); // 原生菜单回写同步选择并关闭 exec。
            return {};
        }
        if (name == QStringLiteral("show_menu"))
        {
            action->menu()->popup(QCursor::pos()); // 非菜单容器中的顶级菜单可以直接展示。
            return {};
        }
        action->trigger(); // 包括原有勾选切换，不能只写 checked 属性。
        return {};
    });
}
}
