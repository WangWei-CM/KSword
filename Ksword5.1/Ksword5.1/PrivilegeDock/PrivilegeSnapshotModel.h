#pragma once

#include <QJsonObject>
#include <QMap>
#include <memory>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ks::privilege
{
    // Persistent snapshots contain evidence, never executable actions or credentials.
    struct SnapshotDifference
    {
        QString key;
        QString before;
        QString after;
        enum class Kind { Added, Removed, Modified, Enabled, Disabled, Uncertain } kind = Kind::Modified;
    };

    // A successful preparation owns the complete original document and the validated,
    // type-tagged comparison index. It contains no QObject or language singleton.
    class PreparedPermissionSnapshot
    {
    public:
        const QJsonObject& document() const { return m_document; }
        const QStringList& diagnostics() const { return m_diagnostics; }
        qsizetype entryCount() const { return m_entries.size(); }

    private:
        struct Evidence { QString value; QString state; };
        QJsonObject m_document;
        QMap<QString, Evidence> m_entries;
        QStringList m_diagnostics;
        bool m_partialPolicy = false;
        bool m_partialToken = false;
        friend std::shared_ptr<const PreparedPermissionSnapshot> preparePermissionSnapshot(const QJsonObject&, QString*);
        friend QVector<SnapshotDifference> comparePreparedPermissionSnapshots(
            const PreparedPermissionSnapshot&, const PreparedPermissionSnapshot&, QString*);
    };

    // Pure worker-safe value APIs. Errors are source text; callers translate at the GUI boundary.
    std::shared_ptr<const PreparedPermissionSnapshot> preparePermissionSnapshot(
        const QJsonObject& snapshot, QString* sourceError);
    QVector<SnapshotDifference> comparePreparedPermissionSnapshots(
        const PreparedPermissionSnapshot& before, const PreparedPermissionSnapshot& after, QString* sourceError);

    bool validatePermissionSnapshot(const QJsonObject& snapshot, QString* error);
    QVector<SnapshotDifference> comparePermissionSnapshots(
        const QJsonObject& before, const QJsonObject& after, QString* error);
}
