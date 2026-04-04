#include "StartupDock.Internal.h"

#include "../UI/CodeEditorWidget.h"

using namespace startup_dock_detail;

namespace
{
    // boolText：
    // - 作用：把布尔值统一转换成中文“是/否”；
    // - 调用：启动项详细信息文本拼装时复用。
    QString boolText(const bool value)
    {
        return value ? QStringLiteral("是") : QStringLiteral("否");
    }

    // buildEntryDetailText：
    // - 作用：把一条 StartupEntry 展开成详细信息文本；
    // - 调用：查看启动项详细信息时使用；
    // - 传入 entry：当前选中的启动项记录；
    // - 传出：返回可直接塞给 CodeEditorWidget 的纯文本。
    QString buildEntryDetailText(const StartupDock::StartupEntry& entry)
    {
        QString detailText;
        detailText += QStringLiteral("名称：%1\n").arg(entry.itemNameText);
        detailText += QStringLiteral("分类：%1\n").arg(entry.categoryText);
        detailText += QStringLiteral("发布者：%1\n").arg(entry.publisherText.isEmpty() ? QStringLiteral("<空>") : entry.publisherText);
        detailText += QStringLiteral("镜像路径：%1\n").arg(entry.imagePathText.isEmpty() ? QStringLiteral("<空>") : entry.imagePathText);
        detailText += QStringLiteral("命令：%1\n").arg(entry.commandText.isEmpty() ? QStringLiteral("<空>") : entry.commandText);
        detailText += QStringLiteral("来源位置：%1\n").arg(entry.locationText.isEmpty() ? QStringLiteral("<空>") : entry.locationText);
        detailText += QStringLiteral("分组位置：%1\n").arg(entry.locationGroupText.isEmpty() ? QStringLiteral("<空>") : entry.locationGroupText);
        detailText += QStringLiteral("注册表值名：%1\n").arg(entry.registryValueNameText.isEmpty() ? QStringLiteral("<空>") : entry.registryValueNameText);
        detailText += QStringLiteral("用户/上下文：%1\n").arg(entry.userText.isEmpty() ? QStringLiteral("<空>") : entry.userText);
        detailText += QStringLiteral("类型：%1\n").arg(entry.sourceTypeText.isEmpty() ? QStringLiteral("<空>") : entry.sourceTypeText);
        detailText += QStringLiteral("状态：%1\n").arg(buildStatusText(entry.enabled));
        detailText += QStringLiteral("补充说明：%1\n").arg(entry.detailText.isEmpty() ? QStringLiteral("<空>") : entry.detailText);
        detailText += QStringLiteral("可打开文件位置：%1\n").arg(boolText(entry.canOpenFileLocation));
        detailText += QStringLiteral("可打开注册表位置：%1\n").arg(boolText(entry.canOpenRegistryLocation));
        detailText += QStringLiteral("可删除：%1\n").arg(boolText(entry.canDelete));
        detailText += QStringLiteral("删除整棵注册表子键：%1\n").arg(boolText(entry.deleteRegistryTree));
        detailText += QStringLiteral("唯一标识：%1\n").arg(entry.uniqueIdText.isEmpty() ? QStringLiteral("<空>") : entry.uniqueIdText);
        return detailText;
    }

    // buildRegistryNodeDetailText：
    // - 作用：给注册表树的位置节点/占位节点生成详情文本；
    // - 调用：用户右键查看注册表位置节点详细信息时使用；
    // - 传入 treeItem：当前选中的树节点；
    // - 传出：返回可直接展示的文本内容。
    QString buildRegistryNodeDetailText(const QTreeWidgetItem* treeItem)
    {
        if (treeItem == nullptr)
        {
            return QStringLiteral("<空节点>");
        }

        const StartupTreeNodeKind nodeKind = static_cast<StartupTreeNodeKind>(
            treeItem->data(0, kStartupTreeNodeKindRole).toInt());
        const QString kindText =
            nodeKind == StartupTreeNodeKind::Group ? QStringLiteral("注册表位置节点")
            : (nodeKind == StartupTreeNodeKind::Placeholder ? QStringLiteral("占位节点") : QStringLiteral("条目节点"));

        QString detailText;
        detailText += QStringLiteral("节点类型：%1\n").arg(kindText);
        detailText += QStringLiteral("显示文本：%1\n").arg(treeItem->text(StartupDock::toStartupColumn(StartupDock::StartupColumn::Name)));
        detailText += QStringLiteral("注册表位置：%1\n").arg(treeItem->data(0, kStartupTreeLocationRole).toString());
        detailText += QStringLiteral("详情列：%1\n").arg(treeItem->text(StartupDock::toStartupColumn(StartupDock::StartupColumn::Detail)));
        detailText += QStringLiteral("子节点数量：%1\n").arg(treeItem->childCount());
        return detailText;
    }

    // showStartupDetailDialog：
    // - 作用：统一弹出启动项/注册表节点详细信息窗口；
    // - 调用：右键菜单“查看启动项详细信息”动作触发；
    // - 传入 titleText/detailText：窗口标题与正文；
    // - 传出：无，直接模态显示。
    void showStartupDetailDialog(QWidget* parentWidget, const QString& titleText, const QString& detailText)
    {
        QDialog detailDialog(parentWidget);
        detailDialog.setWindowTitle(titleText);
        detailDialog.resize(860, 620);

        QVBoxLayout* layout = new QVBoxLayout(&detailDialog);
        CodeEditorWidget* detailEditor = new CodeEditorWidget(&detailDialog);
        detailEditor->setReadOnly(true);
        detailEditor->setText(detailText);

        QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &detailDialog);
        QObject::connect(buttonBox, &QDialogButtonBox::rejected, &detailDialog, &QDialog::reject);
        QObject::connect(buttonBox, &QDialogButtonBox::accepted, &detailDialog, &QDialog::accept);

        layout->addWidget(detailEditor, 1);
        layout->addWidget(buttonBox, 0);
        detailDialog.exec();
    }

    // openFilePropertiesByPath：
    // - 作用：打开文件属性对话框；
    // - 调用：右键菜单“转到文件属性”动作触发；
    // - 传入 filePathText：目标文件路径；
    // - 传出：返回 true 表示 ShellExecute 成功触发。
    bool openFilePropertiesByPath(const QString& filePathText, QString* errorTextOut)
    {
        if (filePathText.trimmed().isEmpty())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("文件路径为空。");
            }
            return false;
        }

        const HINSTANCE shellResult = ::ShellExecuteW(
            nullptr,
            L"properties",
            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(filePathText).utf16()),
            nullptr,
            nullptr,
            SW_SHOW);
        if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("ShellExecute(properties) 失败，返回值=%1")
                    .arg(reinterpret_cast<INT_PTR>(shellResult));
            }
            return false;
        }
        return true;
    }
}

void StartupDock::showSelectedEntryDetails(const StartupCategory category, QTableWidget* tableWidget)
{
    if (category == StartupCategory::Registry)
    {
        QTreeWidgetItem* currentItem = (m_registryTree != nullptr) ? m_registryTree->currentItem() : nullptr;
        if (currentItem == nullptr)
        {
            return;
        }

        const int entryIndex = findEntryIndexByRegistryTreeItem(currentItem);
        if (entryIndex >= 0 && entryIndex < static_cast<int>(m_entryList.size()))
        {
            const StartupEntry& entry = m_entryList[static_cast<std::size_t>(entryIndex)];
            showStartupDetailDialog(
                this,
                QStringLiteral("启动项详细信息 - %1").arg(entry.itemNameText),
                buildEntryDetailText(entry));
        }
        else
        {
            showStartupDetailDialog(
                this,
                QStringLiteral("注册表位置详细信息"),
                buildRegistryNodeDetailText(currentItem));
        }
        return;
    }

    if (tableWidget == nullptr || tableWidget->currentRow() < 0)
    {
        return;
    }

    const int entryIndex = findEntryIndexByTableRow(category, tableWidget->currentRow());
    if (entryIndex < 0 || entryIndex >= static_cast<int>(m_entryList.size()))
    {
        return;
    }

    const StartupEntry& entry = m_entryList[static_cast<std::size_t>(entryIndex)];
    showStartupDetailDialog(
        this,
        QStringLiteral("启动项详细信息 - %1").arg(entry.itemNameText),
        buildEntryDetailText(entry));
}

void StartupDock::openSelectedFileProperties(const StartupCategory category, QTableWidget* tableWidget)
{
    int entryIndex = -1;
    if (category == StartupCategory::Registry)
    {
        entryIndex = findEntryIndexByRegistryTreeItem(
            m_registryTree != nullptr ? m_registryTree->currentItem() : nullptr);
    }
    else if (tableWidget != nullptr && tableWidget->currentRow() >= 0)
    {
        entryIndex = findEntryIndexByTableRow(category, tableWidget->currentRow());
    }
    if (entryIndex < 0 || entryIndex >= static_cast<int>(m_entryList.size()))
    {
        return;
    }

    const StartupEntry& entry = m_entryList[static_cast<std::size_t>(entryIndex)];
    if (!entry.canOpenFileLocation || entry.imagePathText.trimmed().isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("启动项"), QStringLiteral("该条目没有可查看属性的文件路径。"));
        return;
    }

    QString errorText;
    if (!openFilePropertiesByPath(entry.imagePathText, &errorText))
    {
        QMessageBox::warning(this, QStringLiteral("启动项"), QStringLiteral("打开文件属性失败：%1").arg(errorText));
    }
}
