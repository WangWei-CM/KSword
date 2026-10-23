// Pure registry-value fixture. Exact production snapshot/detail/type excerpts are SHA-bound in meta.json.
#include "UI/StructuredFieldView.h"
#include "Internationalization/LanguageManager.h"
#include <QCoreApplication>
#include <QHash>
#include <QPair>
#include <QStringList>
#include <cstdio>
namespace ks::i18n {
struct LanguageManager::State {};
LanguageManager::LanguageManager() = default;
LanguageManager::~LanguageManager() = default;
LanguageManager& LanguageManager::instance() { static LanguageManager value; return value; }
QString LanguageManager::sourceText(const QString& value) const { return value; }
QString LanguageManager::displayText(const QString& value) const { return value; }
bool LanguageManager::eventFilter(QObject*, QEvent*) { return false; }
}
#include "shell_snapshot_value_helpers.inc"
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int checks = 0, failures = 0;
    const auto check = [&](bool ok) { ++checks; if (!ok) ++failures; };
    using namespace ks::misc;
    using Page = ContextMenuCleanerTab;
    for (const int count : {1000, 10000, 100000})
    {
        fixtureEntries.clear(); fixtureEntries.reserve(count);
        friendlyReads = 0; serverReads = 0;
        for (int i = 0; i < count; ++i)
        {
            Page::ContextMenuEntry entry;
            entry.area = Page::MenuArea::OpenWith;
            entry.rootLabel = i % 2 ? QStringLiteral("HKLM(32位)") : QStringLiteral("HKCU");
            entry.rootKey = reinterpret_cast<HKEY>(static_cast<std::uintptr_t>(i % 2 + 1));
            entry.viewFlag = i % 2 ? 0x200 : 0;
            entry.subKeyPath = QStringLiteral("Software\\Classes\\target_%1").arg(i);
            entry.itemName = QStringLiteral("item_%1").arg(i);
            entry.displayName = QStringLiteral("显示_%1").arg(i);
            entry.commandOrHandler = QStringLiteral("command_%1").arg(i);
            entry.clsidText = QStringLiteral("CLSID_%1").arg(i % 10);
            entry.deleteKind = Page::DeleteKind::RegistryValue;
            entry.valueName = i % 2 ? QString() : QStringLiteral("exact-name");
            entry.cleanupOpenWithMru = true;
            entry.canDelete = i % 2 == 0;
            entry.details.field(QStringLiteral("preserved"), QStringLiteral("tail_%1").arg(i));
            fixtureEntries.push_back(std::move(entry));
        }
        auto result = Page::enumerateAreaSnapshot(Page::MenuArea::OpenWith);
        check(result.size() == count);
        check(friendlyReads == 10 && serverReads == 10);
        check(result.last().rootKey == fixtureEntries.last().rootKey);
        check(result.last().viewFlag == fixtureEntries.last().viewFlag);
        check(result.last().cleanupOpenWithMru);
        check(result.last().deleteKind == Page::DeleteKind::RegistryValue);
        check(result.last().registryPathText.contains(QStringLiteral("[值: (默认)]")));
        check(result.first().registryPathText.contains(QStringLiteral("exact-name")));
        check(result.last().searchText.contains(QStringLiteral("tail_%1").arg(count - 1)));
        check(result.first().searchText.contains(QStringLiteral("显示_0")));
        check(result.last().detailText == fixtureEntries.last().details.toPlainText());
        check(fixtureEntries.first().clsidFriendlyName.isEmpty());
        const auto detail = Page::buildEntryDetailDocument(result.last()).toPlainText();
        check(detail.contains(QStringLiteral("friendly:CLSID_9")));
        check(detail.contains(QStringLiteral("server:CLSID_9")));
        check(detail.contains(QStringLiteral("tail_%1").arg(count - 1)));
        check(friendlyReads == 10 && serverReads == 10); // selection never calls the registry.
    }
    fixtureEntries = {Page::ContextMenuEntry{}};
    fixtureEntries[0].details.note(QString(1024 * 1024, QChar('X')) + QStringLiteral("TAIL_MARK"));
    auto large = Page::enumerateAreaSnapshot(Page::MenuArea::File);
    check(large.first().detailText.size() > 1024 * 1024);
    check(large.first().searchText.endsWith(QStringLiteral("tail_mark\n"))
        || large.first().searchText.contains(QStringLiteral("tail_mark")));
    check(Page::buildEntryDetailDocument(large.first()).toPlainText().contains(QStringLiteral("TAIL_MARK")));
    std::printf("SHELL_SNAPSHOT_VALUE_CHECKS=%d FAILURES=%d GUI=0 REGISTRY_IO=0\n", checks, failures);
    return failures ? 1 : 0;
}
