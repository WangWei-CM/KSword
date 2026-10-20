#include "RegistryDock.h"
#include "RegistryDocument.h"
#include "RegistryWorkbenchAccess.h"
#include "RegistryValueTransactions.h"
#include "../UI/AsyncOperation.h"

#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTimer>

ks::ui::AsyncOperation& RegistryDock::legacyMutationOperation()
{
    if (!m_legacyMutationOperation)
    {
        m_legacyMutationOperation = std::make_unique<ks::ui::AsyncOperation>(this);
        m_legacyMutationOperation->setStateChangedCallback([this] {
            if (!m_legacyMutationOperation->isBusy() && m_applyingChanges && !m_legacyMutationReceipt)
            {
                finishLegacyMutation();
                updateStatusBar(QStringLiteral("注册表操作未取得完整回执；请刷新后核对。"));
            }
        });
    }
    return *m_legacyMutationOperation;
}

void RegistryDock::finishLegacyMutation()
{
    m_applyingChanges = false;
    updatePendingChanges();
    updateStatusBar(QStringLiteral("状态: 就绪"));
}

void RegistryDock::createSubKey()
{
    const QPointer<RegistryDock> guarded(this);
    const QString parent = m_currentPath;
    const RegistryAccessContext context = accessContextForPath(parent);
    if (m_applyingChanges || !preserveEditorDraft() || !guarded) return;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("新建子键"),
        QStringLiteral("请输入子键名称："), QLineEdit::Normal, QStringLiteral("New Key"), &accepted);
    if (!guarded || !accepted) return;
    if (m_currentPath.compare(parent, Qt::CaseInsensitive) != 0 || m_viewBits != context.viewBits) return;
    if (name.isEmpty() || name.size() > 255 || name.contains(QLatin1Char('\\')) || name.contains(QChar(0)))
    {
        QMessageBox::warning(this, QStringLiteral("新建子键"), QStringLiteral("子键名称必须为 1 ～ 255 字符，不能包含反斜杠或 NUL。"));
        return;
    }
    const QString path = parent + QLatin1Char('\\') + name;
    struct Request { QString path; RegistryAccessContext context; };
    struct Result { bool ok = false; bool exists = false; QString error; };
    auto& operation = legacyMutationOperation();
    m_applyingChanges = true;
    m_legacyMutationReceipt = false;
    updatePendingChanges();
    updateStatusBar(QStringLiteral("正在处理，请稍候…"));
    operation.submit<Request, Result>({path, context}, [](const Request& request, const ks::ui::AsyncOperationToken&) {
        Result result;
        result.ok = RegistryWorkbenchAccess::keyExists(request.path, request.context, &result.exists, &result.error);
        return result;
    }, [this, parent, path, context](const Result& result, std::uint64_t) {
        m_legacyMutationReceipt = true;
        QTimer::singleShot(0, this, [this, parent, path, context, result] {
            finishLegacyMutation();
            if (m_currentPath.compare(parent, Qt::CaseInsensitive) != 0 || m_viewBits != context.viewBits) return;
            if (!result.ok || result.exists)
            {
                QMessageBox::warning(this, QStringLiteral("新建子键"), result.exists
                    ? QStringLiteral("Registry key already exists; it was not replaced.") : result.error);
                return;
            }
            RegistryDocument document;
            document.viewBits = context.viewBits;
            document.keys.append({path, false, {}});
            document.operationOrder.append({RegistryDocumentOperation::Kind::Key, 0});
            previewRegistryDocument(document, QStringLiteral("新建子键"), context.useR0);
        });
        return true;
    }, [this](std::exception_ptr) {
        m_legacyMutationReceipt = true;
        finishLegacyMutation();
        updateStatusBar(QStringLiteral("注册表操作未取得完整回执；请刷新后核对。"));
    });
}

void RegistryDock::renameSelectedObject()
{
    const QPointer<RegistryDock> guarded(this);
    const QString path = m_currentPath;
    const RegistryAccessContext context = accessContextForPath(path);
    if (m_applyingChanges || !preserveEditorDraft() || !guarded) return;
    auto& operation = legacyMutationOperation();
    if ((m_valuesActive || m_valueTable->hasFocus()) && m_valueTable->currentRow() >= 0)
    {
        const auto* item = m_valueTable->item(m_valueTable->currentRow(), 0);
        if (!item) return;
        const QString oldName = item->data(Qt::UserRole).toString();
        if (oldName.isEmpty())
        {
            QMessageBox::information(this, QStringLiteral("重命名"), QStringLiteral("默认值不支持重命名。"));
            return;
        }
        struct ReadRequest { QString path; QString name; RegistryAccessContext context; };
        struct ReadResult { RegistryValueState original; QString error; bool ok = false; };
        m_applyingChanges = true;
        m_legacyMutationReceipt = false;
        updatePendingChanges();
        updateStatusBar(QStringLiteral("正在处理，请稍候…"));
        operation.submit<ReadRequest, ReadResult>({path, oldName, context},
            [](const ReadRequest& request, const ks::ui::AsyncOperationToken&) {
                ReadResult result;
                result.ok = RegistryWorkbenchAccess::read(request.path, request.name, request.context, &result.original, &result.error);
                return result;
            }, [this, path, oldName, context](const ReadResult& read, std::uint64_t) {
                m_legacyMutationReceipt = true;
                // Do not enter a modal loop while the controller is applying its own completion.
                QTimer::singleShot(0, this, [this, path, oldName, context, read] {
                    const QPointer<RegistryDock> alive(this);
                    if (!read.ok || !read.original.exists || !read.original.complete)
                    {
                        finishLegacyMutation();
                        QMessageBox::warning(this, QStringLiteral("重命名值"), read.error.isEmpty()
                            ? QStringLiteral("原值不存在或数据不完整。") : read.error);
                        return;
                    }
                    if (m_currentPath.compare(path, Qt::CaseInsensitive) != 0 || m_viewBits != context.viewBits)
                    { finishLegacyMutation(); return; }
                    bool accepted = false;
                    const QString newName = QInputDialog::getText(this, QStringLiteral("重命名值"),
                        QStringLiteral("新名称："), QLineEdit::Normal, oldName, &accepted);
                    if (!alive) return;
                    if (!accepted || newName.isEmpty() || newName.compare(oldName, Qt::CaseInsensitive) == 0
                        || m_currentPath.compare(path, Qt::CaseInsensitive) != 0 || m_viewBits != context.viewBits)
                    { finishLegacyMutation(); return; }
                    struct Request { QString path; QString oldName; QString newName; RegistryValueState original; RegistryAccessContext context; };
                    struct Result { bool renamed = false; RegistryValueRenameResult receipt; };
                    m_legacyMutationReceipt = false;
                    updateStatusBar(QStringLiteral("正在处理，请稍候…"));
                    const auto ticket = legacyMutationOperation().submitTransaction<Request, Result>({path, oldName, newName, read.original, context},
                        [](const Request& request) {
                            Result result;
                            result.renamed = RegistryValueTransactions::rename(request.path, request.oldName, request.newName,
                                {true, request.original.type, request.original.data}, request.context, result.receipt);
                            return result;
                        }, [this, path, oldName, newName, context](const Result& result, std::uint64_t) {
                            m_legacyMutationReceipt = true;
                            QTimer::singleShot(0, this, [this, path, oldName, newName, context, result] {
                                finishLegacyMutation();
                                if (m_currentPath.compare(path, Qt::CaseInsensitive) == 0 && m_viewBits == context.viewBits) refreshValueTable();
                                if (!result.renamed)
                                { QMessageBox::warning(this, QStringLiteral("重命名值"), path + QLatin1Char('\n') + result.receipt.error); return; }
                                kLogEvent event;
                                info << event << "[RegistryDock] 重命名值已回读验证, path=" << path.toStdString()
                                    << ", oldName=" << oldName.toStdString() << ", newName=" << newName.toStdString() << eol;
                            });
                            return true;
                        }, [this](std::exception_ptr) {
                            m_legacyMutationReceipt = true;
                            finishLegacyMutation();
                            updateStatusBar(QStringLiteral("注册表操作未取得完整回执；请刷新后核对。"));
                        });
                    if (!ticket) { finishLegacyMutation(); updateStatusBar(QStringLiteral("注册表操作未取得完整回执；请刷新后核对。")); }
                });
                return true;
            }, [this](std::exception_ptr) {
                m_legacyMutationReceipt = true;
                finishLegacyMutation();
                updateStatusBar(QStringLiteral("注册表操作未取得完整回执；请刷新后核对。"));
            });
        return;
    }
    const qsizetype separator = path.lastIndexOf(QLatin1Char('\\'));
    if (separator < 0)
    { QMessageBox::information(this, QStringLiteral("重命名键"), QStringLiteral("根键不可重命名。")); return; }
    const QString oldName = path.mid(separator + 1);
    bool accepted = false;
    const QString newName = QInputDialog::getText(this, QStringLiteral("重命名键"),
        QStringLiteral("新键名："), QLineEdit::Normal, oldName, &accepted);
    if (!guarded || !accepted || newName.isEmpty() || newName.compare(oldName, Qt::CaseInsensitive) == 0) return;
    if (m_currentPath.compare(path, Qt::CaseInsensitive) != 0 || m_viewBits != context.viewBits) return;
    struct Request { QString path; QString newName; RegistryAccessContext context; };
    struct Result { bool renamed = false; QString newPath; QString error; };
    m_applyingChanges = true;
    m_legacyMutationReceipt = false;
    updatePendingChanges();
    updateStatusBar(QStringLiteral("正在处理，请稍候…"));
    const auto ticket = operation.submitTransaction<Request, Result>({path, newName, context}, [](const Request& request) {
        Result result;
        result.renamed = RegistryWorkbenchAccess::renameKey(request.path, request.newName, request.context, &result.newPath, &result.error);
        return result;
    }, [this, path, context](const Result& result, std::uint64_t) {
        m_legacyMutationReceipt = true;
        QTimer::singleShot(0, this, [this, path, context, result] {
            finishLegacyMutation();
            if (!result.renamed)
            { QMessageBox::warning(this, QStringLiteral("重命名键"), path + QLatin1Char('\n') + result.error); return; }
            if (m_currentPath.compare(path, Qt::CaseInsensitive) == 0 && m_viewBits == context.viewBits) navigateToPath(result.newPath, true);
        });
        return true;
    }, [this](std::exception_ptr) {
        m_legacyMutationReceipt = true;
        finishLegacyMutation();
        updateStatusBar(QStringLiteral("注册表操作未取得完整回执；请刷新后核对。"));
    });
    if (!ticket) { finishLegacyMutation(); updateStatusBar(QStringLiteral("注册表操作未取得完整回执；请刷新后核对。")); }
}

// 保留既有树删除预览/备份/Apply 确认；搜索行目标和实际通道不随当前树变化。
void RegistryDock::deleteSearchResultKey(const QString& inputPath,
    const RegistryAccessContext* capturedContext)
{
    const QPointer<RegistryDock> guarded(this); // 暂存确认返回后检查生命周期。
    const QString path = inputPath; // 输入可能借用 m_currentPath，交互前必须复制。
    const RegistryAccessContext context = capturedContext ? *capturedContext
        : accessContextForPath(path); // 搜索行保留旧来源，普通树行使用启动时来源。
    if (m_applyingChanges || !preserveEditorDraft() || !guarded) return;
    if (path.isEmpty() || !path.contains(QLatin1Char('\\')))
    {
        QMessageBox::warning(this, QStringLiteral("删除键"), QStringLiteral("不能删除根键。"));
        return;
    }
    RegistryDocument document; // 准确的原始行路径，不取当前树键替代。
    document.viewBits = context.viewBits;
    document.keys.append({path, true, {}});
    document.operationOrder.append({RegistryDocumentOperation::Kind::Key, 0});
    previewRegistryDocument(document, QStringLiteral("删除子树预览"), context.useR0);
}
