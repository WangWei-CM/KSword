using HKEY = void*;
using REGSAM = unsigned long;
namespace ks::misc {
class ContextMenuCleanerTab {
public:
        enum class MenuArea
        {
            InternetExplorer, // IE 右键菜单：Internet Explorer MenuExt。
            Desktop,          // 桌面右键菜单：DesktopBackground/Directory Background。
            File,             // 文件右键菜单：*、AllFilesystemObjects、Directory/Folder/Drive。
            UrlBinding,       // URL 绑定：协议注册和当前用户默认协议绑定。
            OpenWith,         // 打开方式：扩展名对应的候选应用与 ProgID。
            FormatMenu,       // 格式右键菜单：扩展名、ProgID 和 SystemFileAssociations。
            ExplorerHome      // 资源管理器主页：第三方或用户注册的 Shell 命名空间。
        };
        enum class DeleteKind
        {
            RegistryTree, // RegistryTree：删除 subKeyPath 指向的完整子树。
            RegistryValue // RegistryValue：仅删除 subKeyPath 下 valueName 指向的值。
        };
        struct ContextMenuEntry
        {
            MenuArea area = MenuArea::File;       // area：所属子页。
            HKEY rootKey = nullptr;               // rootKey：真实注册表根键，删除时复用。
            QString rootLabel;                    // rootLabel：显示用根键，如 HKCU/HKLM(64位)。
            QString subKeyPath;                   // subKeyPath：待删除的完整子键路径。
            REGSAM viewFlag = 0;                  // viewFlag：WOW64 视图标记，保证枚举/删除同一视图。
            QString sourceGroup;                  // sourceGroup：来源分类，如“文件 *”或“桌面背景”。
            QString entryKind;                    // entryKind：shell/shellex/IE MenuExt。
            QString itemName;                     // itemName：注册表子键名。
            QString displayName;                  // displayName：菜单显示名，缺失时回退子键名。
            QString commandOrHandler;             // commandOrHandler：命令、脚本路径、CLSID 或 COM Server。
            QString clsidFriendlyName;
            QString clsidServerPath;
            QString detailText;
            QString registryPathText;
            QString searchText;
            QString clsidText;                    // clsidText：COM 右键处理器 CLSID。
            ks::ui::FieldDocument details;                   // detailText：状态标记、Icon、AppliesTo 等补充信息。
            QString statusText;                   // statusText：启用/禁用/扩展菜单等状态。
            DeleteKind deleteKind = DeleteKind::RegistryTree; // deleteKind：当前行的精确删除粒度。
            QString valueName;                    // valueName：删除注册表值时使用的命名值。
            bool cleanupOpenWithMru = false;      // cleanupOpenWithMru：删除历史打开方式后同步 MRUList。
            bool canDelete = false;               // canDelete：当前行是否允许从 UI 发起删除。
        };
static QVector<ContextMenuEntry> enumerateEntriesForArea(MenuArea);
static QVector<ContextMenuEntry> enumerateAreaSnapshot(MenuArea);
static ks::ui::FieldDocument buildEntryDetailDocument(const ContextMenuEntry&);
static QString areaTitle(MenuArea);
};
static QVector<ContextMenuCleanerTab::ContextMenuEntry> fixtureEntries;
static int friendlyReads = 0, serverReads = 0;
namespace context_menu_cleaner_detail {
QString queryClsidFriendlyName(const QString& clsid) { ++friendlyReads; return QStringLiteral("friendly:") + clsid; }
QString queryClsidServerPath(const QString& clsid) { ++serverReads; return QStringLiteral("server:") + clsid; }
    QString rootPathText(const QString& rootLabel, const QString& subKeyPath)
    {
        return QStringLiteral("%1\\%2").arg(rootLabel, subKeyPath);
    }

    // registryTargetPathText：
    // - 输入 rootLabel/subKeyPath/valueTarget/valueName：删除目标组成部分；
    // - 处理：子树沿用普通路径，注册表值追加清晰的“值”标记；
    // - 返回：不会被误解为整棵子树的精确 UI/剪贴板路径。
    QString registryTargetPathText(
        const QString& rootLabel,
        const QString& subKeyPath,
        const bool valueTarget,
        const QString& valueName)
    {
        const QString keyPath = rootPathText(rootLabel, subKeyPath);
        if (!valueTarget)
        {
            return keyPath;
        }
        const QString shownValueName = valueName.isEmpty()
            ? QStringLiteral("(默认)")
            : valueName;
        return QStringLiteral("%1 [值: %2]").arg(keyPath, shownValueName);
    }


}
using namespace context_menu_cleaner_detail;
QVector<ContextMenuCleanerTab::ContextMenuEntry> ContextMenuCleanerTab::enumerateEntriesForArea(MenuArea) { return fixtureEntries; }
QVector<ContextMenuCleanerTab::ContextMenuEntry> ContextMenuCleanerTab::enumerateAreaSnapshot(const MenuArea area)
{
    QVector<ContextMenuEntry> entries = enumerateEntriesForArea(area);
    QHash<QString, QPair<QString, QString>> clsidCache;
    for (ContextMenuEntry& entry : entries)
    {
        if (!entry.clsidText.isEmpty())
        {
            auto it = clsidCache.constFind(entry.clsidText);
            if (it == clsidCache.cend())
                it = clsidCache.insert(entry.clsidText,
                    qMakePair(queryClsidFriendlyName(entry.clsidText), queryClsidServerPath(entry.clsidText)));
            entry.clsidFriendlyName = it->first;
            entry.clsidServerPath = it->second;
        }
        entry.detailText = entry.details.toPlainText();
        entry.registryPathText = registryTargetPathText(entry.rootLabel, entry.subKeyPath,
            entry.deleteKind == DeleteKind::RegistryValue, entry.valueName);
        entry.searchText = QStringList{entry.itemName, entry.displayName, entry.entryKind,
            entry.sourceGroup, entry.commandOrHandler, entry.registryPathText,
            entry.statusText, entry.detailText, entry.clsidText}.join('\n').toLower();
    }
    return entries;
}

ks::ui::FieldDocument ContextMenuCleanerTab::buildEntryDetailDocument(const ContextMenuEntry& entry)
{
    // 只用枚举阶段已经采集到的字段拼装文本，不新增数据来源，也不改表格列；
    // CLSID 友好名与服务器路径复用 Internal.h 里既有的 HKCR 查询 helper。
    ks::ui::FieldDocument lines;
    lines.note(QStringLiteral("【%1】").arg(
        entry.displayName.isEmpty() ? entry.itemName : entry.displayName));

    lines.field(QStringLiteral("分类"), QStringLiteral("%1").arg(areaTitle(entry.area)));
    lines.field(QStringLiteral("名称"), QStringLiteral("%1").arg(entry.itemName));
    lines.field(QStringLiteral("显示名"), QStringLiteral("%1").arg(entry.displayName));
    lines.field(QStringLiteral("类型"), QStringLiteral("%1").arg(entry.entryKind));
    lines.field(QStringLiteral("来源"), QStringLiteral("%1").arg(entry.sourceGroup));
    lines.field(QStringLiteral("根键"), QStringLiteral("%1").arg(entry.rootLabel));
    lines.field(QStringLiteral("注册表位置"), QStringLiteral("%1").arg(registryTargetPathText(
        entry.rootLabel,
        entry.subKeyPath,
        entry.deleteKind == DeleteKind::RegistryValue,
        entry.valueName)));
    lines.field(QStringLiteral("命令/处理器"), QStringLiteral("%1").arg(entry.commandOrHandler));
    lines.field(QStringLiteral("状态"), QStringLiteral("%1").arg(entry.statusText));

    if (!entry.clsidText.isEmpty())
    {

        lines.field(QStringLiteral("CLSID"), QStringLiteral("%1").arg(entry.clsidText));
        const QString& friendlyName = entry.clsidFriendlyName;
        if (!friendlyName.isEmpty())
        {
            lines.field(QStringLiteral("CLSID 名称"), QStringLiteral("%1").arg(friendlyName));
        }
        const QString& serverPath = entry.clsidServerPath;
        if (!serverPath.isEmpty())
        {
            lines.field(QStringLiteral("服务器"), QStringLiteral("%1").arg(serverPath));
        }
    }

    if (!entry.details.isEmpty())
    {

        lines.field(QStringLiteral("补充详情"), QStringLiteral(""));
        for (const auto& node : entry.details.nodes) lines.nodes.push_back(node);
    }


    lines.field(QStringLiteral("删除粒度"), QStringLiteral("%1").arg(
        entry.deleteKind == DeleteKind::RegistryValue
            ? QStringLiteral("仅删除注册表值 %1").arg(entry.valueName)
            : QStringLiteral("删除整棵子键树")));
    lines.field(QStringLiteral("本页可删除"), QStringLiteral("%1").arg(
        entry.canDelete
            ? QStringLiteral("是")
            : QStringLiteral("否（系统保护项或受策略限制）")));

    return lines;
}
QString ContextMenuCleanerTab::areaTitle(const MenuArea area)
{
    switch (area)
    {
    case MenuArea::InternetExplorer:
        return QStringLiteral("IE右键菜单");
    case MenuArea::Desktop:
        return QStringLiteral("桌面右键菜单");
    case MenuArea::File:
        return QStringLiteral("文件右键菜单");
    case MenuArea::UrlBinding:
        return QStringLiteral("URL 绑定");
    case MenuArea::OpenWith:
        return QStringLiteral("文件打开方式");
    case MenuArea::FormatMenu:
        return QStringLiteral("格式右键菜单");
    case MenuArea::ExplorerHome:
        return QStringLiteral("资源管理器主页第三方程序");
    }
    return QStringLiteral("文件右键菜单");
}


}
