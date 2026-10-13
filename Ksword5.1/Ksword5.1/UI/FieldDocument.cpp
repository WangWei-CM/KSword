#include "./StructuredFieldView.h"
#include "../Internationalization/LanguageManager.h"

#include <QStringList>

namespace ks::ui
{
    namespace
    {
        // 文档本地化沿用原唯一翻译入口，与控件显示和报告模型使用相同策略。
        QString translated(const QString& source)
        {
            return ks::i18n::sourceText(source);
        }

        // 递归输出规范节点，保留原分区括号、字段缩进和 Note 翻译规则，不读取任何控件。
        void appendText(const FieldNode& node, QStringList& lines, const int depth,
            const bool localize)
        {
            const auto name = localize && node.translateName ? translated(node.name) : node.name;
            const auto value = localize && node.translateValue ? translated(node.value) : node.value;
            const QString indentation(depth * 2, QLatin1Char(' '));
            if (node.kind == FieldNode::Kind::Section)
                lines.append(indentation + QLatin1Char('[') + name + QLatin1Char(']'));
            else if (node.kind == FieldNode::Kind::Note)
                lines.append(indentation + (localize ? translated(node.value) : node.value));
            else
                lines.append(indentation + name + QStringLiteral(": ") + value);
            for (const auto& child : node.children)
                appendText(child, lines, depth + (node.kind == FieldNode::Kind::Field ? 1 : 0), localize);
        }
    }

    // 新分区成为后续字段的正文宿主；接口与原 StructuredFieldView.cpp 中实现完全一致。
    FieldDocument& FieldDocument::section(const QString& sectionTitle)
    {
        FieldNode node;
        node.kind = FieldNode::Kind::Section;
        node.name = sectionTitle;
        nodes.append(node);
        return *this;
    }

    // 字段附到最近的顶层分区，没有分区时保留在根，返回同一文档支持链式构造。
    FieldDocument& FieldDocument::field(const QString& name, const QString& value,
        const bool translateValue)
    {
        FieldNode node;
        node.kind = FieldNode::Kind::Field;
        node.name = name;
        node.value = value;
        node.translateValue = translateValue;
        if (!nodes.isEmpty() && nodes.last().kind == FieldNode::Kind::Section)
            nodes.last().children.append(node);
        else nodes.append(node);
        return *this;
    }

    // 说明与字段使用同一分区归属；不把说明解析成额外状态或属性。
    FieldDocument& FieldDocument::note(const QString& body)
    {
        FieldNode node;
        node.kind = FieldNode::Kind::Note;
        node.value = body;
        if (!nodes.isEmpty() && nodes.last().kind == FieldNode::Kind::Section)
            nodes.last().children.append(node);
        else nodes.append(node);
        return *this;
    }

    // 完整文档的复制/导出格式继续来自规范树，同一实现可在 QCoreApplication 下验证。
    QString FieldDocument::toPlainText(const bool localize) const
    {
        QStringList lines;
        if (!title.isEmpty()) lines.append(localize ? translated(title) : title);
        for (const auto& node : nodes) appendText(node, lines, 0, localize);
        return lines.join(QLatin1Char('\n'));
    }
}
