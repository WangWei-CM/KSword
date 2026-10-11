#pragma once

// McpUiController：主程序 GUI 线程中的固定控件协议，不开放任意反射调用。
// 调用方通过 request 传入方法与 JSON 参数；返回控件快照或排队调用回执。
#include "../Framework.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QQueue>
#include <QString>
#include <functional>

class QAbstractItemModel;
class QAbstractItemView;
class QWidget;

namespace ks::ui
{
class McpUiController final : public QObject
{
public:
    using PageList = std::function<QJsonArray()>; // 只列页面，不创建惰性内容。
    using PageOpen = std::function<QWidget*(const QString&)>; // 原导航返回页面根。
    using ReadAdapter = std::function<QJsonObject(QObject*, const QJsonObject&)>; // 自绘只读扩展。

    // host 为所有权边界；两个回调由主窗口提供。全部调用限定 GUI 线程。
    McpUiController(QWidget* host, PageList list, PageOpen open);
    ~McpUiController() override;
    void close(); // 取消排队调用并作废对象登记，避免关闭之后继续触发业务。
    QJsonObject request(const QString& method, const QJsonObject& params);
    // 应用在 GUI 线程显式登记类型；callback 返回统一 status/data/error 对象。
    void registerReadAdapter(const QByteArray& className, ReadAdapter adapter);

private:
    // 对象 ID 只在当前进程/桥实例有效；QPointer 拒绝销毁后地址复用。
    struct ObjectEntry
    {
        QPointer<QObject> object; // 当前活对象，销毁自动清空。
    };
    struct Operation
    {
        QString state; // queued/dispatching/dispatched/failed，仅表示控件调用。
        QString error; // 固定机器错误码，不记录字段值或业务敏感内容。
    };
    struct ModelWatch
    {
        QPointer<QAbstractItemModel> model; // 被观察模型，不延长生命周期。
        quint64 generation = 1; // 数据/布局/结构变化统一递增的修订代次。
        QString identity; // 防止模型地址复用的单调身份。
        int transactionDepth = 0; // aboutTo 与完成信号之间禁止行号定位。
    };

    // 统一结果、参数与范围检查；不会把调用成功推定成业务完成。
    static QJsonObject ok(const QJsonObject& data = {});
    static QJsonObject fail(const QString& code);
    static bool integer(const QJsonObject& params, const QString& key,
        int defaultValue, int minimum, int maximum, int* output);
    bool owned(QObject* object) const;
    bool allowedByModal(QObject* object) const;
    QObject* resolve(const QString& id) const;
    QString registerObject(QObject* object);
    QJsonObject describe(QObject* object, const QString& parentId);

    // 各固定入口分文件实现；snapshot 不会创建尚未初始化的页面。
    QJsonObject snapshot(const QJsonObject& params);
    QJsonObject action(const QJsonObject& params);
    QJsonObject setField(const QJsonObject& params);
    QJsonObject readText(const QJsonObject& params);
    QJsonObject readCustom(const QJsonObject& params); // 应用明确登记的只读扩展入口。
    ReadAdapter adapterFor(QObject* object) const; // 按真实元对象继承链查找。
    QJsonObject readTable(const QJsonObject& params);
    QJsonObject selectRow(const QJsonObject& params);
    QString modelRevision(QAbstractItemView* view);
    QJsonObject enqueue(QObject* target, std::function<QString()> dispatch);

    // 控制器状态全部由 GUI 线程读写；所有排队 lambda 以控制器作为 context。
    QPointer<QWidget> m_host; // 主窗口及其所属窗口的访问边界。
    PageList m_pageList; // 页面目录回调，不产生页面初始化副作用。
    PageOpen m_pageOpen; // 原有主窗口导航与惰性构建回调。
    QString m_epoch; // 每次桥构造的随机实例身份。
    quint64 m_nextObject = 0; // 对象编号单调增长，永不以指针作为外部 ID。
    quint64 m_nextOperation = 0; // 排队操作回执的单调编号。
    quint64 m_nextModel = 0; // 表模型身份的单调编号。
    quint64 m_nextRoot = 0; // 视图根改变的单调身份，不输出 QModelIndex 内部指针。
    bool m_closed = false; // close 后拒绝全部请求。
    QHash<QString, ObjectEntry> m_objects; // 有界对象身份登记。
    QHash<QObject*, QString> m_ids; // 活对象到身份的反向查询。
    QHash<QObject*, QPointer<QObject>> m_menuOwners; // 无父菜单的明确控件归属。
    QHash<QString, QPointer<QWidget>> m_pageRoots; // 仅记录显式 open_page 得到的根。
    QHash<QString, Operation> m_operations; // 有界调用回执，不保存业务结果。
    QQueue<QString> m_operationOrder; // 已结束历史回执的淘汰顺序。
    QHash<QAbstractItemModel*, ModelWatch> m_models; // 模型信号代次观察。
    QHash<QObject*, QPersistentModelIndex> m_viewRoots; // 原视图根的精确持久身份。
    QHash<QObject*, QString> m_rootIds; // 根身份改变时换代的无指针 token。
    QHash<QByteArray, ReadAdapter> m_readAdapters; // 明确登记的类型与只读回调。
};
}
