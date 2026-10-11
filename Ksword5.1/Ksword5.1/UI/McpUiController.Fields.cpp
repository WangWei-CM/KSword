#include "./McpUiController.h"
#include <QAbstractItemModel>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QWidget>
#include <cmath>
#include <limits>

namespace ks::ui
{
namespace
{
// numberIndex：索引必须为真实 JSON 整数，不能将 null/字符串/小数当成第零项。
bool numberIndex(const QJsonValue& value, int* output)
{
    const double number = value.toDouble(); // 类型、有限性和 int 范围分别验证。
    if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number
        || number < 0 || number > std::numeric_limits<int>::max())
    {
        return false;
    }
    *output = static_cast<int>(number);
    return true;
}

// editLine：通过原生 insert 执行编辑，保留 validator/inputMask/textEdited 语义。
// 输入为完整替换值；submit 为 true 时再发送 Enter，原业务槽及确认继续生效。
QString editLine(QPointer<QLineEdit> edit, const QString& value, bool submit,
    const std::function<bool()>& continuing)
{
    if (!edit || edit->isReadOnly() || !edit->isEnabled() || !edit->isVisible())
    {
        return QStringLiteral("control_unavailable");
    }
    if (value.size() > edit->maxLength() || value.contains(QChar::LineFeed)
        || value.contains(QChar::CarriageReturn))
    {
        return QStringLiteral("invalid_value");
    }
    edit->selectAll(); // 选区回调也可能销毁字段，因此每次输入前重新判活。
    if (!edit || !continuing())
    {
        return QStringLiteral("object_destroyed");
    }
    edit->insert(value); // 不用 setText，它会绕过 validator 且不发 textEdited。
    if (!edit || !continuing())
    {
        return {}; // 原字段编辑已派发，不能把业务主动关窗误报为失败。
    }
    if (edit->text() != value)
    {
        return QStringLiteral("value_rejected");
    }
    if (submit)
    {
        const bool acceptable = edit->hasAcceptableInput(); // 自定义 validator 也可能重入。
        if (!edit || !continuing())
        {
            return {};
        }
        if (!acceptable)
        {
            return QStringLiteral("validation_rejected");
        }
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(edit, &enter); // 使用固定 Enter 语义，不能传任意按键。
    }
    return {};
}
}

// setField：只适配明确的标准字段类型，参数值不会写进日志或回执。
QJsonObject McpUiController::setField(const QJsonObject& params)
{
    if (params.value(QStringLiteral("confirmed")) != QJsonValue(true))
    {
        return fail(QStringLiteral("confirmation_required"));
    }
    const QJsonValue value = params.value(QStringLiteral("value"));
    const QJsonValue submitValue = params.value(QStringLiteral("submit"));
    if (value.isUndefined() || (!submitValue.isUndefined() && !submitValue.isBool())
        || (value.isString() && value.toString().size() > 16000))
    {
        return fail(QStringLiteral("invalid_params"));
    }
    const bool submit = submitValue.toBool(false); // 未传 submit 时只进行编辑。
    QPointer<QObject> target = resolve(params.value(QStringLiteral("object_id")).toString());
    if (!target)
    {
        return fail(QStringLiteral("unknown_object"));
    }
    const bool stringControl = qobject_cast<QLineEdit*>(target)
        || qobject_cast<QPlainTextEdit*>(target) || qobject_cast<QTextEdit*>(target);
    const bool numericControl = qobject_cast<QSpinBox*>(target)
        || qobject_cast<QDoubleSpinBox*>(target) || qobject_cast<QTabWidget*>(target)
        || qobject_cast<QTabBar*>(target);
    if ((stringControl && !value.isString()) || (numericControl && !value.isDouble()))
    {
        return fail(QStringLiteral("invalid_value"));
    }
    if (!stringControl && !numericControl && !qobject_cast<QComboBox*>(target))
    {
        return fail(QStringLiteral("unsupported_control"));
    }
    if (submit && (qobject_cast<QPlainTextEdit*>(target) || qobject_cast<QTextEdit*>(target)
        || qobject_cast<QTabWidget*>(target) || qobject_cast<QTabBar*>(target)))
    {
        return fail(QStringLiteral("unsupported_submit"));
    }
    const QPointer<McpUiController> self(this); // 任一原控件信号都可能关闭桥。
    return enqueue(target, [this, self, target, value, submit]() -> QString
    {
        const auto continuing = [this, self]()
        {
            return self && !m_closed;
        };
        const auto* widget = qobject_cast<QWidget*>(target); // 排队之后重新检查现场可用性。
        if (!widget || !widget->isEnabled() || !widget->isVisible())
        {
            return QStringLiteral("control_unavailable");
        }
        if (auto* line = qobject_cast<QLineEdit*>(target))
        {
            return editLine(line, value.toString(), submit, continuing);
        }
        if (auto* combo = qobject_cast<QComboBox*>(target))
        {
            QPointer<QComboBox> guarded(combo); // model 与当前项目变化都可能重入业务。
            int index = -1; // 整数选择原索引，字符串匹配原有项目。
            if (value.isDouble())
            {
                if (!numberIndex(value, &index))
                {
                    return QStringLiteral("invalid_value");
                }
                const int count = combo->count();
                if (!guarded || !continuing())
                {
                    return QStringLiteral("object_destroyed");
                }
                if (index >= count)
                {
                    return QStringLiteral("invalid_value");
                }
            }
            else if (value.isString())
            {
                index = combo->findText(value.toString(), Qt::MatchExactly | Qt::MatchCaseSensitive);
                if (!guarded || !continuing())
                {
                    return QStringLiteral("object_destroyed");
                }
                if (index < 0)
                {
                    if (!combo->isEditable() || !combo->lineEdit())
                    {
                        return QStringLiteral("invalid_value");
                    }
                    return editLine(combo->lineEdit(), value.toString(), submit, continuing);
                }
            }
            else
            {
                return QStringLiteral("invalid_value");
            }
            // 项目被业务禁用时不能通过 setCurrentIndex 绕过原菜单的选择限制。
            QPointer<QAbstractItemModel> model(combo->model());
            if (!model)
            {
                return QStringLiteral("control_unavailable");
            }
            const QModelIndex item = model->index(index, combo->modelColumn(), combo->rootModelIndex());
            if (!guarded || !model || !continuing())
            {
                return QStringLiteral("object_destroyed");
            }
            const Qt::ItemFlags flags = model->flags(item);
            if (!guarded || !model || !continuing()
                || !(flags & Qt::ItemIsEnabled) || !(flags & Qt::ItemIsSelectable))
            {
                return QStringLiteral("control_unavailable");
            }
            combo->setCurrentIndex(index); // currentIndexChanged/currentTextChanged 保留。
            if (!guarded || !continuing())
            {
                return {};
            }
            // activated 仅由交互触发，固定信号适配补齐业务监听，绝不接受方法名参数。
            emit combo->activated(index);
            if (!guarded || !continuing())
            {
                return {};
            }
            const QString text = combo->currentText(); // 使用当前值，尊重 activated 槽的调整。
            if (!guarded || !continuing())
            {
                return {};
            }
            emit combo->textActivated(text);
            if (submit && guarded && continuing() && combo->isEditable() && combo->lineEdit())
            {
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QApplication::sendEvent(combo->lineEdit(), &enter);
            }
            return {};
        }
        if (auto* spin = qobject_cast<QSpinBox*>(target))
        {
            const double number = value.toDouble(); // signed int 值与组合框索引分别校验。
            if (!std::isfinite(number) || std::floor(number) != number
                || number < spin->minimum() || number > spin->maximum() || spin->isReadOnly())
            {
                return QStringLiteral("invalid_value");
            }
            QPointer<QSpinBox> guarded(spin);
            spin->setValue(static_cast<int>(number)); // 原 valueChanged/textChanged 仍由 Qt 发出。
            if (submit && guarded && continuing())
            {
                emit spin->editingFinished();
            }
            return {};
        }
        if (auto* spin = qobject_cast<QDoubleSpinBox*>(target))
        {
            const double number = value.toDouble(); // 不让非有限数进入 Qt 的值与格式化链路。
            if (!std::isfinite(number) || number < spin->minimum() || number > spin->maximum()
                || spin->isReadOnly())
            {
                return QStringLiteral("invalid_value");
            }
            QPointer<QDoubleSpinBox> guarded(spin);
            spin->setValue(number);
            if (submit && guarded && continuing())
            {
                emit spin->editingFinished();
            }
            return {};
        }
        if (auto* tabs = qobject_cast<QTabWidget*>(target))
        {
            int index = -1; // 不允许激活业务禁用或显式隐藏的页签。
            if (!numberIndex(value, &index) || index >= tabs->count()
                || !tabs->isTabEnabled(index) || !tabs->isTabVisible(index))
            {
                return QStringLiteral("invalid_value");
            }
            tabs->setCurrentIndex(index); // 保留 currentChanged 与原有惰性创建槽。
            return {};
        }
        if (auto* tabs = qobject_cast<QTabBar*>(target))
        {
            int index = -1;
            if (!numberIndex(value, &index) || index >= tabs->count()
                || !tabs->isTabEnabled(index) || !tabs->isTabVisible(index))
            {
                return QStringLiteral("invalid_value");
            }
            tabs->setCurrentIndex(index);
            return {};
        }
        // 多行文本按原 QTextCursor 的编辑路径写入，不能开放任意 HTML 或属性写入。
        if (auto* edit = qobject_cast<QPlainTextEdit*>(target))
        {
            if (edit->isReadOnly())
            {
                return QStringLiteral("control_unavailable");
            }
            QPointer<QPlainTextEdit> guarded(edit);
            edit->selectAll();
            if (guarded && continuing())
            {
                edit->insertPlainText(value.toString());
            }
            return {};
        }
        if (auto* edit = qobject_cast<QTextEdit*>(target))
        {
            if (edit->isReadOnly())
            {
                return QStringLiteral("control_unavailable");
            }
            QPointer<QTextEdit> guarded(edit);
            edit->selectAll();
            if (guarded && continuing())
            {
                edit->insertPlainText(value.toString());
            }
            return {};
        }
        return QStringLiteral("unsupported_control");
    });
}

// readText：按 UTF-16 offset/length 读取标准文本；密码/NoEcho 绝不返回内容。
QJsonObject McpUiController::readText(const QJsonObject& params)
{
    int offset = 0; // UTF-16 码元起点，与 Qt QString/QTextCursor 位置一致。
    int length = 0; // 单次读取预算，最多 65536 个码元。
    if (!integer(params, QStringLiteral("offset"), 0, 0, std::numeric_limits<int>::max(), &offset)
        || !integer(params, QStringLiteral("length"), 4096, 1, 65536, &length))
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
    QString text; // 只保存当前分页片段，不能复制整个巨型编辑器文档。
    int total = 0; // 原文 UTF-16 码元总数，密码字段不会提供此值。
    QPointer<QTextDocument> document; // 编辑器原文的弱引用，读取不改变选区。
    if (auto* edit = qobject_cast<QLineEdit*>(target))
    {
        if (edit->echoMode() != QLineEdit::Normal)
        {
            return fail(QStringLiteral("sensitive_field"));
        }
        total = static_cast<int>(qMin<qsizetype>(edit->text().size(), std::numeric_limits<int>::max()));
        text = edit->text().mid(offset, length);
    }
    else if (auto* label = qobject_cast<QLabel*>(target))
    {
        total = static_cast<int>(qMin<qsizetype>(label->text().size(), std::numeric_limits<int>::max()));
        text = label->text().mid(offset, length);
    }
    else if (auto* plainEdit = qobject_cast<QPlainTextEdit*>(target))
    {
        document = plainEdit->document();
    }
    else if (auto* richEdit = qobject_cast<QTextEdit*>(target))
    {
        document = richEdit->document();
    }
    else
    {
        return fail(QStringLiteral("unsupported_control"));
    }
    if (document)
    {
        total = qMax(0, document->characterCount() - 1); // 最末尾的隐含段落分隔不算内容。
        const int start = qMin(offset, total);
        const int amount = qMin(length, total - start); // 避免 offset+length 整数溢出。
        QTextCursor cursor(document);
        cursor.setPosition(start);
        cursor.setPosition(start + amount, QTextCursor::KeepAnchor);
        text = cursor.selectedText();
        text.replace(QChar::ParagraphSeparator, QChar::LineFeed);
    }
    return ok({{QStringLiteral("text"), text},
        {QStringLiteral("offset"), offset},
        {QStringLiteral("length"), text.size()},
        {QStringLiteral("total_length"), total},
        {QStringLiteral("truncated"), offset < total && text.size() < total - offset}});
}
}
