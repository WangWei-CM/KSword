#include "./McpUiController.h"
#include <QApplication>
#include <QMetaObject>
#include <QThread>

namespace ks::ui
{
// registerReadAdapter：只有主程序代码能登记适配，客户端不能选择类名或函数名。
// 空 callback 表示取消该类型登记；错误线程或已关闭实例不接受登记。
void McpUiController::registerReadAdapter(const QByteArray& className, ReadAdapter adapter)
{
    if (m_closed || !qApp || QThread::currentThread() != qApp->thread()
        || className.isEmpty() || className.size() > 256)
    {
        return;
    }
    if (!adapter)
    {
        m_readAdapters.remove(className);
        return;
    }
    m_readAdapters.insert(className, std::move(adapter));
}

// adapterFor：派生类的明确适配优先；没有登记时可沿真实元对象继承链回退。
McpUiController::ReadAdapter McpUiController::adapterFor(QObject* object) const
{
    if (!object)
    {
        return {};
    }
    for (const QMetaObject* meta = object->metaObject(); meta; meta = meta->superClass())
    {
        const auto found = m_readAdapters.constFind(QByteArray(meta->className()));
        if (found != m_readAdapters.cend())
        {
            return found.value(); // 复制闭包，不跨重入持有 QHash 内部引用。
        }
    }
    return {};
}

// readCustom：固定只读扩展入口。输入 object_id/options；输出统一响应对象。
// 适配内部必须遵守原控件的只读约束，不能以此次扩展绕过写操作确认。
QJsonObject McpUiController::readCustom(const QJsonObject& params)
{
    const QJsonValue options = params.value(QStringLiteral("options"));
    if (!options.isUndefined() && !options.isObject())
    {
        return fail(QStringLiteral("invalid_params"));
    }
    QPointer<QObject> target = resolve(params.value(QStringLiteral("object_id")).toString());
    if (!target)
    {
        return fail(QStringLiteral("unknown_object"));
    }
    if (!allowedByModal(target))
    {
        return fail(QStringLiteral("modal_blocked"));
    }
    const ReadAdapter callback = adapterFor(target); // close/重新登记不影响当前回调对象存活。
    if (!callback)
    {
        return fail(QStringLiteral("unsupported_control"));
    }
    const QPointer<McpUiController> self(this); // 自绘读回调也可能重入窗口关闭。
    const QJsonObject result = callback(target, options.toObject());
    if (!self || m_closed)
    {
        return fail(QStringLiteral("bridge_closed"));
    }
    if (!target || !owned(target))
    {
        return fail(QStringLiteral("object_destroyed"));
    }
    // 限制只读回调的响应形状，禁止用 accepted 宣称业务任务已完成。
    const QString status = result.value(QStringLiteral("status")).toString();
    if (!result.value(QStringLiteral("data")).isObject()
        || (status != QStringLiteral("ok") && status != QStringLiteral("error"))
        || (status == QStringLiteral("error")
            && result.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString().isEmpty()))
    {
        return fail(QStringLiteral("adapter_invalid_response"));
    }
    return result;
}
}
