// Standalone QtCore-only fixture. Helpers are exact production excerpts, SHA-bound in meta.json.
#include "UI/FlatTableModel.h"
#include "MiscDock/DiskEditor/DiskRawFileSystemBrowser.h"
#include "MiscDock/DiskEditor/DiskDeletedEntryForensics.h"
#include <QCoreApplication>
#include <QAbstractProxyModel>
#include <QSortFilterProxyModel>
#include <QPersistentModelIndex>
#include <cstdio>
namespace ks::i18n {
// Translation adapter only; this fixture never loads language packs or creates widgets.
struct LanguageManager::State {};
LanguageManager::LanguageManager() = default;
LanguageManager::~LanguageManager() = default;
LanguageManager& LanguageManager::instance() { static LanguageManager value; return value; }
QString LanguageManager::sourceText(const QString& value) const { return value; }
QString LanguageManager::displayText(const QString& value) const { return value; }
bool LanguageManager::eventFilter(QObject*, QEvent*) { return false; }
}
#include "disk_forensics_value_helpers.inc"
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int checks = 0, failures = 0;
    const auto check = [&](bool ok) { ++checks; if (!ok) ++failures; };
    using namespace ks::misc;
    using Model = ks::ui::FlatTableModel<RawFileEntry>;
    std::vector<Model::ColumnSpec> columns(8);
    int calls = 0;
    Model model(columns, [&](const RawFileEntry& row, int col, int role) {
        ++calls; return rawCell(row, col, role);
    });
    std::vector<RawFileEntry> rows(100000);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        rows[i].name = QStringLiteral("name_%1").arg(100000 - i, 6, 10, QChar('0'));
        rows[i].objectId = i + 10;
        rows[i].fullPath = QStringLiteral("\\all\\") + rows[i].name;
    }
    const RawFileEntry* originalStorage = rows.data();
    model.setRows(std::move(rows));
    check(model.rowCount() == 100000);
    check(calls == 0); // No eager formatting or per-cell object creation during adoption.
    check(model.rows().data() == originalStorage);
    check(model.rowAt(-1) == nullptr && model.rowAt(100000) == nullptr);
    check(model.index(99999, 4).data().toString() == QStringLiteral("100009"));
    QSortFilterProxyModel proxy;
    proxy.setSourceModel(&model);
    proxy.sort(0, Qt::AscendingOrder);
    const QModelIndex first = proxy.index(0, 0);
    const RawFileEntry* selected = valueAt(&model, first);
    check(selected != nullptr && selected->objectId == 100009);
    check(selected != nullptr && selected->fullPath.endsWith(QStringLiteral("name_000001")));
    QSortFilterProxyModel nested;
    nested.setSourceModel(&proxy);
    nested.sort(0, Qt::DescendingOrder);
    check(valueAt(&model, nested.index(0, 0))->objectId == 10);
    check(valueAt(&model, QModelIndex()) == nullptr);
    Model unrelated(columns, rawCell);
    unrelated.setRows({RawFileEntry{}});
    check(valueAt(&model, unrelated.index(0, 0)) == nullptr);
    QPersistentModelIndex oldSelection(first);
    model.clearRows();
    check(!oldSelection.isValid());
    check(model.rowCount() == 0 && proxy.rowCount() == 0);
    RawFileEntry entry;
    entry.name = QStringLiteral("target"); entry.fileSizeBytes = 5000;
    entry.allocatedSizeBytes = 8192; entry.objectId = 87; entry.metadataOffset = 0xCAFE;
    entry.extents = {RawFileExtent{}, RawFileExtent{}, RawFileExtent{}};
    entry.extents[0].sparse = true; entry.extents[0].lengthBytes = 123;
    entry.extents[1].physicalMappingExact = true; entry.extents[1].absoluteOffset = 0xF000;
    entry.extents[1].lengthBytes = 456; entry.extents[1].unwritten = true;
    entry.extents[2].physicalMappingExact = true; entry.extents[2].absoluteOffset = 0x10000;
    entry.extents[2].lengthBytes = 789; entry.extents[2].compressed = true;
    entry.extentsTruncated = true;
    check(rawCell(entry, 0, Qt::DisplayRole).toString() == entry.name);
    check(rawCell(entry, 2, Qt::DisplayRole).toString() == QStringLiteral("5000"));
    check(rawCell(entry, 3, Qt::DisplayRole).toString() == QStringLiteral("8192"));
    check(rawCell(entry, 4, Qt::DisplayRole).toString() == QStringLiteral("87"));
    check(rawCell(entry, 5, Qt::UserRole).toULongLong() == 0xCAFE);
    check(rawCell(entry, 6, Qt::UserRole).toULongLong() == 0xF000);
    check(rawCell(entry, 6, Qt::UserRole + 1).toBool());
    const QString extents = rawCell(entry, 7, Qt::DisplayRole).toString();
    check(extents.contains(QStringLiteral("[稀疏]")) && extents.contains(QStringLiteral("[未写入]")));
    check(extents.contains(QStringLiteral("[压缩]")) && extents.endsWith(QStringLiteral("…[物理映射未完整展开]")));
    entry.extents.clear(); entry.extentsTruncated = false;
    check(!rawCell(entry, 6, Qt::UserRole + 1).toBool());
    check(rawCell(entry, 7, Qt::DisplayRole).toString() == QStringLiteral("-"));
    check(!rawCell(entry, 0, Qt::ForegroundRole).isValid());
    PhysicalFileExtent extent; extent.physicalOffset = 1234;
    check(!extentCell(extent, 4, Qt::UserRole + 1).toBool());
    extent.physicalMappingExact = true;
    check(extentCell(extent, 4, Qt::UserRole).toULongLong() == 1234);
    check(extentCell(extent, 4, Qt::UserRole + 1).toBool());
    DeletedDirectoryEntry deleted; deleted.exactExtents = true; deleted.clustersCurrentlyFree = true;
    deleted.evidenceText = QString(1000000, QChar('Z'));
    check(deletedCell(deleted, 6, Qt::DisplayRole).toString() == QStringLiteral("离线磁盘可擦除"));
    deleted.directory = true;
    check(deletedCell(deleted, 6, Qt::DisplayRole).toString() == QStringLiteral("只读取证"));
    check(deletedCell(deleted, 7, Qt::DisplayRole).toString().size() == 1000000);
    ReverseClusterEntry reverse; reverse.cluster = 77; reverse.clusterCount = 9;
    reverse.streamPath = QStringLiteral("\\identity\\stream:$DATA");
    check(reverseCell(reverse, 0, Qt::DisplayRole).toString() == QStringLiteral("77"));
    check(reverseCell(reverse, 1, Qt::DisplayRole).toString() == QStringLiteral("9"));
    check(reverseCell(reverse, 2, Qt::DisplayRole).toString() == reverse.streamPath);
    std::printf("DISK_VALUE_MODEL_CHECKS=%d FAILURES=%d GUI=0 BUSINESS_IO=0\n", checks, failures);
    return failures ? 1 : 0;
}
