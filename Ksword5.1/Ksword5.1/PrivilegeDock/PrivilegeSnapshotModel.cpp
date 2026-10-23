#include "PrivilegeSnapshotModel.h"
#include "../Internationalization/LanguageManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QDateTime>
#include <QMap>
#include <QSet>

namespace ks::privilege
{
    namespace
    {
        constexpr int MaximumEntries = 200000;
        QString message(const QString& source) { return ks::i18n::sourceText(source); }

        template<typename Entries>
        bool collect(const QJsonObject& section, const QString& prefix,
            Entries* entries, QString* error)
        {
            if (!section.value(QStringLiteral("entries")).isArray()
                || !section.value(QStringLiteral("errors")).isArray())
            {
                if (error) *error = QStringLiteral("快照缺少条目或采集错误信息。");
                return false;
            }
            const QJsonArray rows = section.value(QStringLiteral("entries")).toArray();
            if (rows.size() > MaximumEntries)
            {
                if (error) *error = QStringLiteral("快照条目数量超过限制。");
                return false;
            }
            for (const QJsonValue& row : rows)
            {
                const QJsonObject object = row.toObject();
                const QString key = object.value(QStringLiteral("key")).toString();
                const QJsonValue value = object.value(QStringLiteral("value"));
                const QString state = object.value(QStringLiteral("state")).toString(QStringLiteral("read"));
                if (!row.isObject() || key.isEmpty() || key.size() > 4096
                    || value.isUndefined() || value.isNull()
                    || (!value.isString() && !value.isBool() && !value.isDouble())
                    || (object.contains(QStringLiteral("state")) && !object.value(QStringLiteral("state")).isString())
                    || (state != QLatin1String("read") && state != QLatin1String("absent") && state != QLatin1String("unreadable"))
                    || (value.isString() && value.toString().size() > 65536)
                    || entries->contains(prefix + key))
                {
                    if (error) *error = QStringLiteral("快照含无效或重复的条目。");
                    return false;
                }
                // Keep a type-tagged representation: string "1" and number 1 differ.
                entries->insert(prefix + key, {QString::fromUtf8(
                    QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)), state});
            }
            if (section.value(QStringLiteral("errors")).toArray().size() > 10000)
            {
                if (error) *error = QStringLiteral("快照采集错误信息格式无效。");
                return false;
            }
            for (const QJsonValue& diagnostic : section.value(QStringLiteral("errors")).toArray())
            {
                if (!diagnostic.isString() || diagnostic.toString().size() > 16384)
                {
                    if (error) *error = QStringLiteral("快照采集错误信息格式无效。");
                    return false;
                }
            }
            return true;
        }

        bool partial(const QJsonObject& section)
        {
            return !section.value(QStringLiteral("errors")).toArray().isEmpty();
        }
    }

    std::shared_ptr<const PreparedPermissionSnapshot> preparePermissionSnapshot(
        const QJsonObject& snapshot, QString* error)
    {
        if (error) error->clear();
        if (snapshot.value(QStringLiteral("schema")).toString() != QLatin1String("ksword.permission.snapshot")
            || snapshot.value(QStringLiteral("version")).toInt() != 1
            || snapshot.value(QStringLiteral("computer")).toString().isEmpty()
            || snapshot.value(QStringLiteral("computer")).toString().size() > 255
            || !snapshot.value(QStringLiteral("tokenIncluded")).isBool()
            || snapshot.value(QStringLiteral("capturedUtc")).toString().size() > 64
            || !QDateTime::fromString(snapshot.value(QStringLiteral("capturedUtc")).toString(), Qt::ISODateWithMs).isValid()
            || !snapshot.value(QStringLiteral("policy")).isObject()
            || !snapshot.value(QStringLiteral("token")).isObject())
        {
            if (error) *error = QStringLiteral("快照格式或版本不受支持。");
            return {};
        }
        auto prepared = std::make_shared<PreparedPermissionSnapshot>();
        const auto policy = snapshot.value(QStringLiteral("policy")).toObject();
        const auto token = snapshot.value(QStringLiteral("token")).toObject();
        if (!collect(policy, QStringLiteral("policy/"), &prepared->m_entries, error)
            || !collect(token, QStringLiteral("token/"), &prepared->m_entries, error)) return {};
        if (prepared->m_entries.size() > MaximumEntries)
        {
            if (error) *error = QStringLiteral("快照条目数量超过限制。");
            return {};
        }
        prepared->m_document = snapshot;
        prepared->m_partialPolicy = partial(policy);
        prepared->m_partialToken = partial(token);
        for (const auto& section : {policy, token})
            for (const auto& diagnostic : section.value(QStringLiteral("errors")).toArray())
                prepared->m_diagnostics.push_back(diagnostic.toString());
        prepared->m_diagnostics.removeDuplicates();
        return prepared;
    }

    QVector<SnapshotDifference> comparePreparedPermissionSnapshots(
        const PreparedPermissionSnapshot& before, const PreparedPermissionSnapshot& after, QString* error)
    {
        if (error) error->clear();
        QVector<SnapshotDifference> differences;
        if (before.m_document.value(QStringLiteral("computer")) != after.m_document.value(QStringLiteral("computer")))
        {
            if (error) *error = QStringLiteral("两份快照来自不同计算机，无法比较本地权限变化。");
            return differences;
        }
        if (before.m_document.value(QStringLiteral("tokenIncluded")) != after.m_document.value(QStringLiteral("tokenIncluded")))
        {
            if (error) *error = QStringLiteral("两份快照的令牌采集选项不同，请使用相同选项重新采集。");
            return differences;
        }
        // Both indexes are ordered at validation time. Merge once without revalidating,
        // reserializing evidence, collecting a key set or sorting a second time.
        auto a = before.m_entries.cbegin();
        auto b = after.m_entries.cbegin();
        while (a != before.m_entries.cend() || b != after.m_entries.cend())
        {
            const bool hasBefore = a != before.m_entries.cend()
                && (b == after.m_entries.cend() || a.key() <= b.key());
            const bool hasAfter = b != after.m_entries.cend()
                && (a == before.m_entries.cend() || b.key() <= a.key());
            const QString key = hasBefore ? a.key() : b.key();
            const auto av = hasBefore ? a.value() : PreparedPermissionSnapshot::Evidence{};
            const auto bv = hasAfter ? b.value() : PreparedPermissionSnapshot::Evidence{};
            if (hasBefore) ++a;
            if (hasAfter) ++b;
            if (hasBefore && hasAfter && ((av.state == QLatin1String("unreadable") && bv.state == QLatin1String("unreadable"))
                || (av.value == bv.value && av.state == bv.state))) continue;
            SnapshotDifference row{key, av.value, bv.value};
            const bool policy = key.startsWith(QLatin1String("policy/"));
            if (av.state == QLatin1String("unreadable") || bv.state == QLatin1String("unreadable"))
                row.kind = SnapshotDifference::Kind::Uncertain;
            else if (!hasBefore)
                row.kind = (policy ? before.m_partialPolicy : before.m_partialToken)
                    ? SnapshotDifference::Kind::Uncertain : SnapshotDifference::Kind::Added;
            else if (!hasAfter)
                row.kind = (policy ? after.m_partialPolicy : after.m_partialToken)
                    ? SnapshotDifference::Kind::Uncertain : SnapshotDifference::Kind::Removed;
            else if (av.state == QLatin1String("absent") && bv.state == QLatin1String("read"))
                row.kind = SnapshotDifference::Kind::Added;
            else if (av.state == QLatin1String("read") && bv.state == QLatin1String("absent"))
                row.kind = SnapshotDifference::Kind::Removed;
            else if (key.endsWith(QLatin1String("/enabled")))
            {
                const auto enabled = [](const QString& value) { return value == QLatin1String("[true]") || value == QLatin1String("[\"1\"]"); };
                const auto disabled = [](const QString& value) { return value == QLatin1String("[false]") || value == QLatin1String("[\"0\"]"); };
                if (disabled(av.value) && enabled(bv.value)) row.kind = SnapshotDifference::Kind::Enabled;
                else if (enabled(av.value) && disabled(bv.value)) row.kind = SnapshotDifference::Kind::Disabled;
            }
            differences.push_back(std::move(row));
        }
        return differences;
    }

    bool validatePermissionSnapshot(const QJsonObject& snapshot, QString* error)
    {
        QString sourceError;
        const bool valid = static_cast<bool>(preparePermissionSnapshot(snapshot, &sourceError));
        if (error) *error = sourceError.isEmpty() ? QString() : message(sourceError);
        return valid;
    }

    QVector<SnapshotDifference> comparePermissionSnapshots(
        const QJsonObject& before, const QJsonObject& after, QString* error)
    {
        QString sourceError;
        const auto a = preparePermissionSnapshot(before, &sourceError);
        const auto b = a ? preparePermissionSnapshot(after, &sourceError) : nullptr;
        auto differences = a && b ? comparePreparedPermissionSnapshots(*a, *b, &sourceError) : QVector<SnapshotDifference>{};
        if (error) *error = sourceError.isEmpty() ? QString() : message(sourceError);
        return differences;
    }
}
