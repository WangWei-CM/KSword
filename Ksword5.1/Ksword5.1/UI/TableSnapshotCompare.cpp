#include "TableSnapshotCompare.h"

#include "../Internationalization/LanguageManager.h"
#include "../theme.h"
#include "TableFreezeSupport.h"

#include <QAbstractItemModel>
#include <QBitArray>
#include <QByteArray>
#include <QByteArrayView>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QHash>
#include <QHeaderView>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QRegularExpression>
#include <QTableView>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <limits>

namespace
{
    constexpr char kSnapshotKeyColumnsProperty[] = "kswordSnapshotKeyColumns";

    quint64 saturatingAdd(const quint64 left, const quint64 right)
    {
        return right > std::numeric_limits<quint64>::max() - left
            ? std::numeric_limits<quint64>::max()
            : left + right;
    }

    quint64 estimatedTextStorageBytes(const QString& text)
    {
        const quint64 characterCount = static_cast<quint64>(std::max<qsizetype>(0, text.size()));
        return characterCount > std::numeric_limits<quint64>::max() / sizeof(QChar)
            ? std::numeric_limits<quint64>::max()
            : characterCount * sizeof(QChar);
    }

    quint64 estimatedColumnStorageBytes(const ks::ui::TableSnapshotColumn& column)
    {
        return saturatingAdd(
            sizeof(ks::ui::TableSnapshotColumn),
            estimatedTextStorageBytes(column.headerText));
    }

    quint64 estimatedRowStorageBytes(const ks::ui::TableSnapshotRow& row)
    {
        quint64 bytes = saturatingAdd(
            sizeof(ks::ui::TableSnapshotRow),
            static_cast<quint64>(row.values.size()) * sizeof(QString));
        for (const QString& value : row.values)
        {
            bytes = saturatingAdd(bytes, estimatedTextStorageBytes(value));
        }
        return bytes;
    }

    quint64 estimatedSnapshotStorageBytes(const ks::ui::TableSnapshot& snapshot)
    {
        quint64 bytes = saturatingAdd(
            sizeof(ks::ui::TableSnapshot),
            estimatedTextStorageBytes(snapshot.label));
        for (const ks::ui::TableSnapshotColumn& column : snapshot.visibleColumns)
        {
            bytes = saturatingAdd(bytes, estimatedColumnStorageBytes(column));
        }
        for (const ks::ui::TableSnapshotRow& row : snapshot.rows)
        {
            bytes = saturatingAdd(bytes, estimatedRowStorageBytes(row));
        }
        return saturatingAdd(
            bytes,
            static_cast<quint64>(snapshot.keyColumns.size()) * sizeof(int));
    }

    bool canRetainEstimatedBytes(
        const quint64 currentBytes,
        const quint64 additionalBytes,
        const quint64 byteLimit)
    {
        return currentBytes <= byteLimit && additionalBytes <= byteLimit - currentBytes;
    }

    struct BoundedDisplayText
    {
        QString text;
        bool truncated = false;
        bool supported = true;
    };

    BoundedDisplayText boundedDisplayText(const QVariant& value, const int maximumCharacters)
    {
        BoundedDisplayText result;
        if (!value.isValid() || value.isNull())
        {
            return result;
        }

        const int safeMaximumCharacters = std::max(0, maximumCharacters);
        switch (value.metaType().id())
        {
        case QMetaType::QString:
        {
            const QString sourceText = value.value<QString>();
            const qsizetype retainedCharacters = std::min(
                sourceText.size(),
                static_cast<qsizetype>(safeMaximumCharacters));
            result.text = QString(sourceText.constData(), retainedCharacters);
            result.truncated = retainedCharacters < sourceText.size();
            break;
        }
        case QMetaType::QByteArray:
        {
            const QByteArray bytes = value.value<QByteArray>();
            const qsizetype maximumInputBytes = static_cast<qsizetype>(
                std::min<quint64>(
                    static_cast<quint64>(safeMaximumCharacters) * 4ULL + 4ULL,
                    static_cast<quint64>(std::numeric_limits<qsizetype>::max())));
            const qsizetype bytesToDecode = std::min(bytes.size(), maximumInputBytes);
            result.text = QString::fromUtf8(bytes.constData(), bytesToDecode);
            result.truncated = bytesToDecode < bytes.size();
            break;
        }
        case QMetaType::Bool:
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
        case QMetaType::Double:
        case QMetaType::QChar:
        case QMetaType::QDate:
        case QMetaType::QTime:
        case QMetaType::QDateTime:
        case QMetaType::QUuid:
        case QMetaType::Long:
        case QMetaType::Short:
        case QMetaType::Char:
        case QMetaType::ULong:
        case QMetaType::UShort:
        case QMetaType::UChar:
        case QMetaType::Float:
        case QMetaType::SChar:
            result.text = value.toString();
            break;
        default:
            result.supported = false;
            return result;
        }

        if (result.text.size() > safeMaximumCharacters)
        {
            result.text.truncate(safeMaximumCharacters);
            result.truncated = true;
        }
        // Never retain a model-owned implicit-sharing allocation whose capacity can
        // be much larger than the bounded logical value accounted above.
        result.text.squeeze();
        return result;
    }

    void recordBoundedDisplayValue(
        ks::ui::TableSnapshot& snapshot,
        const BoundedDisplayText& value,
        const bool headerValue)
    {
        if (!value.supported)
        {
            snapshot.truncatedByValueLimit = true;
            ++snapshot.unsupportedDisplayValueCount;
            return;
        }
        if (!value.truncated)
        {
            return;
        }

        snapshot.truncatedByValueLimit = true;
        if (headerValue)
        {
            ++snapshot.truncatedHeaderValueCount;
        }
        else
        {
            ++snapshot.truncatedCellValueCount;
        }
    }

    struct HeaderCaptureState
    {
        QVector<int> logicalIndexes;
        QVector<int> sectionSizes;
        QBitArray hiddenSections;
        int sectionCount = 0;
        int sortSection = -1;
        Qt::SortOrder sortOrder = Qt::AscendingOrder;
        bool headerHidden = false;
    };

    HeaderCaptureState captureHeaderState(QHeaderView* header, const int visualSectionsToTrack)
    {
        HeaderCaptureState state;
        if (header == nullptr)
        {
            return state;
        }

        state.sectionCount = header->count();
        state.sortSection = header->sortIndicatorSection();
        state.sortOrder = header->sortIndicatorOrder();
        state.headerHidden = header->isHidden();
        const int sectionCount = std::min(
            std::max(0, visualSectionsToTrack),
            std::max(0, header->count()));
        state.logicalIndexes.reserve(sectionCount);
        state.sectionSizes.reserve(sectionCount);
        state.hiddenSections.resize(sectionCount);
        for (int visualIndex = 0; visualIndex < sectionCount; ++visualIndex)
        {
            const int logicalIndex = header->logicalIndex(visualIndex);
            state.logicalIndexes.push_back(logicalIndex);
            state.sectionSizes.push_back(
                logicalIndex >= 0 ? header->sectionSize(logicalIndex) : -1);
            state.hiddenSections.setBit(
                visualIndex,
                logicalIndex >= 0 && header->isSectionHidden(logicalIndex));
        }
        return state;
    }

    bool headerStateMatches(QHeaderView* header, const HeaderCaptureState& expected)
    {
        if (header == nullptr ||
            header->count() != expected.sectionCount ||
            header->sortIndicatorSection() != expected.sortSection ||
            header->sortIndicatorOrder() != expected.sortOrder ||
            header->isHidden() != expected.headerHidden ||
            expected.logicalIndexes.size() != expected.sectionSizes.size() ||
            expected.logicalIndexes.size() != expected.hiddenSections.size())
        {
            return false;
        }

        for (int visualIndex = 0; visualIndex < expected.logicalIndexes.size(); ++visualIndex)
        {
            const int logicalIndex = expected.logicalIndexes.at(visualIndex);
            if (header->logicalIndex(visualIndex) != logicalIndex ||
                logicalIndex < 0 ||
                header->sectionSize(logicalIndex) != expected.sectionSizes.at(visualIndex) ||
                header->isSectionHidden(logicalIndex) != expected.hiddenSections.testBit(visualIndex))
            {
                return false;
            }
        }
        return true;
    }

    void discardInvalidatedSnapshot(ks::ui::TableSnapshot& snapshot)
    {
        snapshot.sourceInvalidated = true;
        snapshot.visibleColumns.clear();
        snapshot.rows.clear();
        snapshot.keyColumns.clear();
        snapshot.estimatedBytes = estimatedSnapshotStorageBytes(snapshot);
    }

    struct SnapshotColumnLookup
    {
        explicit SnapshotColumnLookup(const ks::ui::TableSnapshot& snapshot)
        {
            valueIndexBySourceColumn.reserve(snapshot.visibleColumns.size());
            for (int valueIndex = 0; valueIndex < snapshot.visibleColumns.size(); ++valueIndex)
            {
                const int sourceColumn = snapshot.visibleColumns.at(valueIndex).sourceColumn;
                if (sourceColumn >= 0 && !valueIndexBySourceColumn.contains(sourceColumn))
                {
                    valueIndexBySourceColumn.insert(sourceColumn, valueIndex);
                }
            }
        }

        int valueIndex(const int sourceColumn) const
        {
            const auto iterator = valueIndexBySourceColumn.constFind(sourceColumn);
            return iterator == valueIndexBySourceColumn.cend() ? -1 : iterator.value();
        }

        QHash<int, int> valueIndexBySourceColumn;
    };

    const QString& valueForSourceColumn(
        const ks::ui::TableSnapshotRow& row,
        const SnapshotColumnLookup& lookup,
        const int sourceColumn)
    {
        static const QString emptyValue;
        const int valueIndex = lookup.valueIndex(sourceColumn);
        return valueIndex >= 0 && valueIndex < row.values.size()
            ? row.values.at(valueIndex)
            : emptyValue;
    }

    QVector<int> valueIndexesForColumns(
        const SnapshotColumnLookup& lookup,
        const QVector<ks::ui::TableSnapshotColumn>& columns)
    {
        QVector<int> indexes;
        indexes.reserve(columns.size());
        for (const ks::ui::TableSnapshotColumn& column : columns)
        {
            indexes.push_back(lookup.valueIndex(column.sourceColumn));
        }
        return indexes;
    }

    QVector<ks::ui::TableSnapshotColumn> combinedColumns(
        const ks::ui::TableSnapshot& earlier,
        const ks::ui::TableSnapshot& later)
    {
        QVector<ks::ui::TableSnapshotColumn> columns;
        QSet<int> seenColumns;
        const auto appendMissingColumns = [&columns, &seenColumns](const QVector<ks::ui::TableSnapshotColumn>& source)
            {
                for (const ks::ui::TableSnapshotColumn& column : source)
                {
                    if (column.sourceColumn >= 0 && !seenColumns.contains(column.sourceColumn))
                    {
                        columns.push_back(column);
                        seenColumns.insert(column.sourceColumn);
                    }
                }
            };
        appendMissingColumns(earlier.visibleColumns);
        appendMissingColumns(later.visibleColumns);
        return columns;
    }

    struct ComparisonWorkState
    {
        const ks::ui::TableSnapshotComparisonLimits& limits;
        const std::function<bool()>& shouldCancel;
        ks::ui::TableComparisonResult& result;
        quint64 temporaryEstimatedBytes = 0;
        quint64 processedRows = 0;

        bool consumeWork(const quint64 units = 1)
        {
            const quint64 updated = saturatingAdd(result.workUnits, units);
            if (updated > limits.maximumWorkUnits)
            {
                result.truncatedByWorkLimit = true;
                return false;
            }
            result.workUnits = updated;
            return true;
        }

        bool reserveTemporary(const quint64 bytes)
        {
            const quint64 updated = saturatingAdd(temporaryEstimatedBytes, bytes);
            if (updated > limits.maximumTemporaryEstimatedBytes)
            {
                result.truncatedByTemporaryByteLimit = true;
                return false;
            }
            temporaryEstimatedBytes = updated;
            result.temporaryPeakEstimatedBytes = std::max(
                result.temporaryPeakEstimatedBytes,
                temporaryEstimatedBytes);
            return true;
        }

        bool checkpointAfterRow()
        {
            ++processedRows;
            const int yieldRows = std::max(1, limits.eventLoopYieldRows);
            if (processedRows % static_cast<quint64>(yieldRows) != 0)
            {
                return true;
            }
            if (QCoreApplication::instance() != nullptr)
            {
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 5);
            }
            if (shouldCancel && shouldCancel())
            {
                result.cancelled = true;
                return false;
            }
            return true;
        }

        bool canContinue() const
        {
            return !result.isTruncated();
        }
    };

    void addHashInteger(QCryptographicHash& hash, const qint64 value)
    {
        hash.addData(
            QByteArrayView(
                reinterpret_cast<const char*>(&value),
                static_cast<qsizetype>(sizeof(value))));
    }

    bool isBlankKeyValue(const QString& value)
    {
        for (const QChar character : value)
        {
            if (!character.isSpace())
            {
                return false;
            }
        }
        return true;
    }

    QByteArray rowHash(
        const ks::ui::TableSnapshotRow& row,
        const SnapshotColumnLookup& lookup,
        const QVector<ks::ui::TableSnapshotColumn>& columns,
        const QSet<int>& ignoredColumns,
        ComparisonWorkState& work)
    {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (const ks::ui::TableSnapshotColumn& column : columns)
        {
            if (ignoredColumns.contains(column.sourceColumn))
            {
                continue;
            }
            if (!work.consumeWork())
            {
                return {};
            }
            const QString& value = valueForSourceColumn(row, lookup, column.sourceColumn);
            addHashInteger(hash, column.sourceColumn);
            addHashInteger(hash, value.size());
            hash.addData(
                QByteArrayView(
                    reinterpret_cast<const char*>(value.constData()),
                    value.size() * static_cast<qsizetype>(sizeof(QChar))));
        }
        return hash.result();
    }

    QByteArray rowKeyHash(
        const ks::ui::TableSnapshotRow& row,
        const SnapshotColumnLookup& lookup,
        const QVector<int>& keyColumns,
        ComparisonWorkState& work)
    {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (const int sourceColumn : keyColumns)
        {
            if (!work.consumeWork())
            {
                return {};
            }
            const QString& value = valueForSourceColumn(row, lookup, sourceColumn);
            if (isBlankKeyValue(value))
            {
                return {};
            }
            addHashInteger(hash, sourceColumn);
            addHashInteger(hash, value.size());
            hash.addData(
                QByteArrayView(
                    reinterpret_cast<const char*>(value.constData()),
                    value.size() * static_cast<qsizetype>(sizeof(QChar))));
        }
        return hash.result();
    }

    QVector<int> usableStableKeyColumns(
        const ks::ui::TableSnapshot& earlier,
        const ks::ui::TableSnapshot& later,
        const SnapshotColumnLookup& earlierLookup,
        const SnapshotColumnLookup& laterLookup,
        const QVector<int>& requestedKeyColumns,
        const QSet<int>& ignoredColumns)
    {
        QVector<int> keyColumns;
        const QVector<int>& candidates = requestedKeyColumns.isEmpty()
            ? earlier.keyColumns
            : requestedKeyColumns;
        for (const int sourceColumn : candidates)
        {
            if (!ignoredColumns.contains(sourceColumn) &&
                (requestedKeyColumns.isEmpty() ? later.keyColumns.contains(sourceColumn) : true) &&
                earlierLookup.valueIndex(sourceColumn) >= 0 &&
                laterLookup.valueIndex(sourceColumn) >= 0)
            {
                keyColumns.push_back(sourceColumn);
            }
        }
        return keyColumns;
    }

    bool buildUniqueRowKeys(
        const ks::ui::TableSnapshot& snapshot,
        const SnapshotColumnLookup& lookup,
        const QVector<int>& keyColumns,
        QHash<QByteArray, int>& rowByKey,
        ComparisonWorkState& work)
    {
        rowByKey.clear();
        if (!work.reserveTemporary(
                static_cast<quint64>(snapshot.rows.size()) * (sizeof(int) + 96ULL)))
        {
            return false;
        }
        rowByKey.reserve(snapshot.rows.size());
        for (int rowIndex = 0; rowIndex < snapshot.rows.size(); ++rowIndex)
        {
            const QByteArray key = rowKeyHash(snapshot.rows.at(rowIndex), lookup, keyColumns, work);
            if (!work.canContinue())
            {
                return false;
            }
            if (key.isEmpty() || rowByKey.contains(key))
            {
                rowByKey.clear();
                return false;
            }
            rowByKey.insert(key, rowIndex);
            if (!work.checkpointAfterRow())
            {
                return false;
            }
        }
        return true;
    }

    quint64 logicalSnapshotBytes(const ks::ui::TableSnapshot& snapshot)
    {
        return std::max(
            snapshot.estimatedBytes,
            estimatedSnapshotStorageBytes(snapshot));
    }

    bool appendComparisonRow(
        ks::ui::TableComparisonResult& result,
        const ks::ui::TableSnapshotComparisonLimits& limits,
        const ks::ui::TableComparisonSource source,
        const int earlierRowIndex,
        const int laterRowIndex,
        const bool displayLater,
        QSet<int> changedSourceColumns = QSet<int>())
    {
        quint64 rowBytes = sizeof(ks::ui::TableComparisonRow);
        rowBytes = saturatingAdd(
            rowBytes,
            static_cast<quint64>(changedSourceColumns.size()) * 64ULL);
        if (!canRetainEstimatedBytes(
                result.estimatedBytes,
                rowBytes,
                limits.maximumEstimatedBytes))
        {
            result.truncatedByResultByteLimit = true;
            return false;
        }

        ks::ui::TableComparisonRow row;
        row.source = source;
        row.earlierRowIndex = earlierRowIndex;
        row.laterRowIndex = laterRowIndex;
        row.displayLater = displayLater;
        row.changedSourceColumns = std::move(changedSourceColumns);
        result.rows.push_back(std::move(row));
        result.estimatedBytes += rowBytes;
        return true;
    }

    bool headerMatchesKeyword(const QString& header, const QString& keyword)
    {
        const QString trimmedKeyword = keyword.trimmed();
        return !trimmedKeyword.isEmpty() &&
            header.contains(trimmedKeyword, Qt::CaseInsensitive);
    }

    QVector<int> declaredKeyColumns(
        QTableView* tableView,
        const ks::ui::TableSnapshot& snapshot,
        const int maximumEntries,
        const int maximumEntryCharacters)
    {
        QVector<int> result;
        if (tableView == nullptr || tableView->model() == nullptr)
        {
            return result;
        }

        const QVariant rawProperty = tableView->property(kSnapshotKeyColumnsProperty);
        if (!rawProperty.isValid() || rawProperty.isNull())
        {
            return result;
        }

        QVariantList entryList;
        if (rawProperty.metaType().id() == QMetaType::QVariantList)
        {
            const QVariantList sourceEntries = rawProperty.toList();
            const int entryCount = std::min(
                std::max(0, maximumEntries),
                static_cast<int>(sourceEntries.size()));
            entryList.reserve(entryCount);
            for (int index = 0; index < entryCount; ++index)
            {
                entryList.push_back(sourceEntries.at(index));
            }
        }
        else if (rawProperty.metaType().id() == QMetaType::QStringList)
        {
            const QStringList stringList = rawProperty.toStringList();
            const int entryCount = std::min(
                std::max(0, maximumEntries),
                static_cast<int>(stringList.size()));
            entryList.reserve(entryCount);
            for (int index = 0; index < entryCount; ++index)
            {
                entryList.push_back(stringList.at(index));
            }
        }
        else
        {
            const BoundedDisplayText propertyText = boundedDisplayText(
                rawProperty,
                std::max(0, maximumEntries) * std::max(0, maximumEntryCharacters));
            const QString& text = propertyText.text;
            const QStringList stringList = text.split(
                QRegularExpression(QStringLiteral("[,;|]")),
                Qt::SkipEmptyParts);
            const int entryCount = std::min(
                std::max(0, maximumEntries),
                static_cast<int>(stringList.size()));
            entryList.reserve(entryCount);
            for (int index = 0; index < entryCount; ++index)
            {
                entryList.push_back(stringList.at(index));
            }
        }

        const QAbstractItemModel* model = tableView->model();
        for (const QVariant& entry : entryList)
        {
            bool numericColumn = false;
            const int sourceColumn = entry.toInt(&numericColumn);
            if (numericColumn && sourceColumn >= 0 && sourceColumn < model->columnCount())
            {
                if (!result.contains(sourceColumn))
                {
                    result.push_back(sourceColumn);
                }
                continue;
            }

            const BoundedDisplayText boundedEntry = boundedDisplayText(
                entry,
                maximumEntryCharacters);
            const QString headerName = boundedEntry.text.trimmed();
            if (headerName.isEmpty())
            {
                continue;
            }
            for (const ks::ui::TableSnapshotColumn& column : snapshot.visibleColumns)
            {
                if (column.headerText.compare(headerName, Qt::CaseInsensitive) == 0 &&
                    !result.contains(column.sourceColumn))
                {
                    result.push_back(column.sourceColumn);
                    break;
                }
            }
        }
        return result;
    }

    QColor earlierOnlyColor()
    {
        return KswordTheme::WithAlpha(KswordTheme::ErrorColor(), 96);
    }

    QColor laterOnlyColor()
    {
        return KswordTheme::WithAlpha(KswordTheme::SuccessColor(), 96);
    }

    QColor changedFieldColor()
    {
        return KswordTheme::WithAlpha(KswordTheme::WarningColor(), 96);
    }
}

namespace ks::ui
{
    TableSnapshot TableSnapshotCompareEngine::capture(
        QTableView* tableView,
        const QString& label,
        const quint64 sequence,
        const TableSnapshotCaptureLimits& limits)
    {
        TableSnapshot snapshot;
        snapshot.label = label;
        snapshot.capturedAt = QDateTime::currentDateTime();
        snapshot.sequence = sequence;
        if (tableView == nullptr || tableView->model() == nullptr)
        {
            return snapshot;
        }

        QAbstractItemModel* model = tableView->model();
        snapshot.estimatedBytes = estimatedSnapshotStorageBytes(snapshot);
        if (QThread::currentThread() != tableView->thread() ||
            QThread::currentThread() != model->thread())
        {
            snapshot.sourceInvalidated = true;
            return snapshot;
        }

        const int maximumRows = std::max(0, limits.maximumRows);
        const int maximumColumns = std::max(0, limits.maximumColumns);
        const int maximumCellCharacters = std::max(0, limits.maximumCellCharacters);
        const int maximumHeaderCharacters = std::max(0, limits.maximumHeaderCharacters);
        const int eventLoopYieldRows = std::max(1, limits.eventLoopYieldRows);
        const quint64 maximumEstimatedBytes = std::max(
            limits.maximumEstimatedBytes,
            snapshot.estimatedBytes);
        QPointer<QTableView> tableGuard(tableView);
        QPointer<QAbstractItemModel> modelGuard(model);
        QPointer<QHeaderView> horizontalHeader(tableView->horizontalHeader());
        QPointer<QHeaderView> verticalHeader(tableView->verticalHeader());

        QObject epochObserver;
        quint64 sourceEpoch = 0;
        const auto markSourceChanged = [&sourceEpoch]()
            {
                ++sourceEpoch;
            };
        const auto observeModelSignal = [&](auto signal)
            {
                QObject::connect(model, signal, &epochObserver, markSourceChanged);
            };
        observeModelSignal(&QAbstractItemModel::dataChanged);
        observeModelSignal(&QAbstractItemModel::headerDataChanged);
        observeModelSignal(&QAbstractItemModel::layoutAboutToBeChanged);
        observeModelSignal(&QAbstractItemModel::layoutChanged);
        observeModelSignal(&QAbstractItemModel::modelAboutToBeReset);
        observeModelSignal(&QAbstractItemModel::modelReset);
        observeModelSignal(&QAbstractItemModel::rowsAboutToBeInserted);
        observeModelSignal(&QAbstractItemModel::rowsInserted);
        observeModelSignal(&QAbstractItemModel::rowsAboutToBeRemoved);
        observeModelSignal(&QAbstractItemModel::rowsRemoved);
        observeModelSignal(&QAbstractItemModel::rowsAboutToBeMoved);
        observeModelSignal(&QAbstractItemModel::rowsMoved);
        observeModelSignal(&QAbstractItemModel::columnsAboutToBeInserted);
        observeModelSignal(&QAbstractItemModel::columnsInserted);
        observeModelSignal(&QAbstractItemModel::columnsAboutToBeRemoved);
        observeModelSignal(&QAbstractItemModel::columnsRemoved);
        observeModelSignal(&QAbstractItemModel::columnsAboutToBeMoved);
        observeModelSignal(&QAbstractItemModel::columnsMoved);
        QObject::connect(model, &QObject::destroyed, &epochObserver, markSourceChanged);
        QObject::connect(tableView, &QObject::destroyed, &epochObserver, markSourceChanged);

        const auto observeHeader = [&](QHeaderView* header)
            {
                if (header == nullptr)
                {
                    return;
                }
                QObject::connect(header, &QHeaderView::geometriesChanged, &epochObserver, markSourceChanged);
                QObject::connect(header, &QHeaderView::sectionMoved, &epochObserver, markSourceChanged);
                QObject::connect(header, &QHeaderView::sectionResized, &epochObserver, markSourceChanged);
                QObject::connect(header, &QHeaderView::sectionCountChanged, &epochObserver, markSourceChanged);
                QObject::connect(header, &QHeaderView::sortIndicatorChanged, &epochObserver, markSourceChanged);
                QObject::connect(header, &QObject::destroyed, &epochObserver, markSourceChanged);
            };
        observeHeader(horizontalHeader.data());
        observeHeader(verticalHeader.data());

        const int columnCount = model->columnCount();
        const int rowCount = model->rowCount();
        snapshot.sourceColumnCount = columnCount;
        snapshot.sourceRowCount = rowCount;
        const int columnsToVisit = std::min(columnCount, maximumColumns);
        const int rowsToVisit = std::min(rowCount, maximumRows);
        const HeaderCaptureState horizontalState =
            captureHeaderState(horizontalHeader.data(), columnsToVisit);
        const HeaderCaptureState verticalState =
            captureHeaderState(verticalHeader.data(), rowsToVisit);
        const quint64 captureEpoch = sourceEpoch;
        const quint64 filterRevision = TableRowFilterIdentityRevision(tableView);

        const auto sourceIdentityIsStable = [&]()
            {
                return sourceEpoch == captureEpoch &&
                    !tableGuard.isNull() &&
                    !modelGuard.isNull() &&
                    tableGuard->model() == modelGuard.data() &&
                    TableRowFilterIdentityRevision(tableGuard.data()) == filterRevision &&
                    tableGuard->horizontalHeader() == horizontalHeader.data() &&
                    tableGuard->verticalHeader() == verticalHeader.data() &&
                    modelGuard->rowCount() == rowCount &&
                    modelGuard->columnCount() == columnCount;
            };
        const auto sourceStateIsStable = [&]()
            {
                return sourceIdentityIsStable() &&
                    headerStateMatches(horizontalHeader.data(), horizontalState) &&
                    headerStateMatches(verticalHeader.data(), verticalState);
            };

        if (!sourceStateIsStable())
        {
            discardInvalidatedSnapshot(snapshot);
            return snapshot;
        }

        for (int visualColumn = 0; visualColumn < columnsToVisit; ++visualColumn)
        {
            ++snapshot.visitedSourceColumns;
            const int sourceColumn = visualColumn < horizontalState.logicalIndexes.size()
                ? horizontalState.logicalIndexes.at(visualColumn)
                : visualColumn;
            // 冻结列在源表里也是"隐藏"的，但那是用户特意钉住的列，
            // 必须留在快照里，否则冻结期间的比对基线会凭空少列。
            if (sourceColumn < 0 ||
                sourceColumn >= columnCount ||
                (visualColumn < horizontalState.hiddenSections.size() &&
                    horizontalState.hiddenSections.testBit(visualColumn) &&
                    !isColumnHiddenByFreeze(tableView, sourceColumn)))
            {
                continue;
            }

            const BoundedDisplayText headerText = boundedDisplayText(
                modelGuard->headerData(sourceColumn, Qt::Horizontal, Qt::DisplayRole),
                maximumHeaderCharacters);
            recordBoundedDisplayValue(snapshot, headerText, true);
            if (!sourceIdentityIsStable())
            {
                discardInvalidatedSnapshot(snapshot);
                return snapshot;
            }
            TableSnapshotColumn column{ sourceColumn, headerText.text };
            const quint64 columnBytes = estimatedColumnStorageBytes(column);
            if (!canRetainEstimatedBytes(
                    snapshot.estimatedBytes,
                    columnBytes,
                    maximumEstimatedBytes))
            {
                snapshot.truncatedByByteLimit = true;
                break;
            }
            snapshot.visibleColumns.push_back(std::move(column));
            snapshot.estimatedBytes += columnBytes;
        }
        if (!snapshot.truncatedByByteLimit && columnsToVisit < columnCount)
        {
            snapshot.truncatedByColumnLimit = true;
        }

        for (int visualRow = 0;
             visualRow < rowsToVisit && !snapshot.truncatedByByteLimit;
             ++visualRow)
        {
            if (visualRow > 0 && visualRow % eventLoopYieldRows == 0)
            {
                if (QCoreApplication::instance() != nullptr)
                {
                    QCoreApplication::processEvents(
                        QEventLoop::ExcludeUserInputEvents,
                        5);
                }
                if (!sourceStateIsStable())
                {
                    discardInvalidatedSnapshot(snapshot);
                    break;
                }
            }

            ++snapshot.visitedSourceRows;
            const int sourceRow = visualRow < verticalState.logicalIndexes.size()
                ? verticalState.logicalIndexes.at(visualRow)
                : visualRow;
            // 同上：冻结行是用户钉住的证据行，不能因为源表隐藏就从快照里漏掉。
            if (sourceRow < 0 ||
                sourceRow >= rowCount ||
                IsTableRowExcludedByFilter(tableView, sourceRow) ||
                (visualRow < verticalState.hiddenSections.size() &&
                    verticalState.hiddenSections.testBit(visualRow) &&
                    !isRowHiddenByFreeze(tableView, sourceRow)))
            {
                continue;
            }

            TableSnapshotRow row;
            row.sourceRow = sourceRow;
            row.values.reserve(snapshot.visibleColumns.size());
            quint64 rowBytes = saturatingAdd(
                sizeof(TableSnapshotRow),
                static_cast<quint64>(snapshot.visibleColumns.size()) * sizeof(QString));
            if (!canRetainEstimatedBytes(
                    snapshot.estimatedBytes,
                    rowBytes,
                    maximumEstimatedBytes))
            {
                snapshot.truncatedByByteLimit = true;
                break;
            }

            for (const TableSnapshotColumn& column : snapshot.visibleColumns)
            {
                const BoundedDisplayText value = boundedDisplayText(
                    modelGuard->data(
                        modelGuard->index(sourceRow, column.sourceColumn),
                        Qt::DisplayRole),
                    maximumCellCharacters);
                recordBoundedDisplayValue(snapshot, value, false);
                if (!sourceIdentityIsStable())
                {
                    discardInvalidatedSnapshot(snapshot);
                    break;
                }
                rowBytes = saturatingAdd(rowBytes, estimatedTextStorageBytes(value.text));
                if (!canRetainEstimatedBytes(
                        snapshot.estimatedBytes,
                        rowBytes,
                        maximumEstimatedBytes))
                {
                    snapshot.truncatedByByteLimit = true;
                    break;
                }
                row.values.push_back(value.text);
            }
            if (snapshot.truncatedByByteLimit || snapshot.sourceInvalidated)
            {
                break;
            }
            snapshot.rows.push_back(std::move(row));
            snapshot.estimatedBytes += rowBytes;
        }

        if (!snapshot.truncatedByByteLimit &&
            !snapshot.sourceInvalidated &&
            rowsToVisit < rowCount)
        {
            snapshot.truncatedByRowLimit = true;
        }
        if (horizontalHeader != nullptr)
        {
            snapshot.sortColumn = horizontalState.sortSection;
            snapshot.sortOrder = horizontalState.sortOrder;
        }
        if (!snapshot.sourceInvalidated && !sourceStateIsStable())
        {
            discardInvalidatedSnapshot(snapshot);
        }
        if (!tableGuard.isNull() && !snapshot.sourceInvalidated)
        {
            const QVector<int> keyColumns = declaredKeyColumns(
                tableGuard.data(),
                snapshot,
                maximumColumns,
                maximumHeaderCharacters);
            if (!sourceStateIsStable())
            {
                discardInvalidatedSnapshot(snapshot);
                return snapshot;
            }
            const quint64 keyColumnBytes =
                static_cast<quint64>(keyColumns.size()) * sizeof(int);
            if (canRetainEstimatedBytes(
                    snapshot.estimatedBytes,
                    keyColumnBytes,
                    maximumEstimatedBytes))
            {
                snapshot.keyColumns = keyColumns;
                snapshot.estimatedBytes += keyColumnBytes;
            }
            else
            {
                snapshot.truncatedByByteLimit = true;
            }
        }
        snapshot.visibleColumns.squeeze();
        snapshot.rows.squeeze();
        snapshot.keyColumns.squeeze();
        snapshot.estimatedBytes = estimatedSnapshotStorageBytes(snapshot);
        return snapshot;
    }

    quint64 TableSnapshotCompareEngine::totalEstimatedBytes(
        const QVector<TableSnapshot>& snapshots)
    {
        quint64 totalBytes = 0;
        for (const TableSnapshot& snapshot : snapshots)
        {
            totalBytes = saturatingAdd(
                totalBytes,
                snapshot.estimatedBytes != 0
                    ? snapshot.estimatedBytes
                    : estimatedSnapshotStorageBytes(snapshot));
        }
        return totalBytes;
    }

    TableSnapshotRetentionResult TableSnapshotCompareEngine::enforceRetentionLimits(
        QVector<TableSnapshot>& snapshots,
        const TableSnapshotRetentionLimits& limits)
    {
        for (TableSnapshot& snapshot : snapshots)
        {
            if (snapshot.estimatedBytes == 0)
            {
                snapshot.estimatedBytes = estimatedSnapshotStorageBytes(snapshot);
            }
        }

        TableSnapshotRetentionResult result;
        result.remainingEstimatedBytes = totalEstimatedBytes(snapshots);
        const int maximumSnapshots = std::max(0, limits.maximumSnapshots);
        while (!snapshots.isEmpty() &&
               (snapshots.size() > maximumSnapshots ||
                result.remainingEstimatedBytes > limits.maximumEstimatedBytes))
        {
            const TableSnapshot& oldestSnapshot = snapshots.front();
            result.evictedLabels.push_back(oldestSnapshot.label);
            result.remainingEstimatedBytes =
                oldestSnapshot.estimatedBytes >= result.remainingEstimatedBytes
                ? 0
                : result.remainingEstimatedBytes - oldestSnapshot.estimatedBytes;
            snapshots.erase(snapshots.begin());
        }
        return result;
    }

    QStringList TableSnapshotCompareEngine::defaultIgnoredColumnKeywords()
    {
        return {
            QStringLiteral("序号"),
            QStringLiteral("行号"),
            QStringLiteral("时间"),
            QStringLiteral("日期"),
            QStringLiteral("Index"),
            QStringLiteral("No."),
            QStringLiteral("Sequence"),
            QStringLiteral("Time"),
            QStringLiteral("Timestamp"),
            QStringLiteral("Date")
        };
    }

    QSet<int> TableSnapshotCompareEngine::defaultIgnoredColumnIndexes(const TableSnapshot& snapshot)
    {
        QSet<int> ignoredColumns;
        const QStringList keywords = defaultIgnoredColumnKeywords();
        for (const TableSnapshotColumn& column : snapshot.visibleColumns)
        {
            for (const QString& keyword : keywords)
            {
                if (headerMatchesKeyword(column.headerText, keyword))
                {
                    ignoredColumns.insert(column.sourceColumn);
                    break;
                }
            }
        }
        return ignoredColumns;
    }

    QSet<int> TableSnapshotCompareEngine::ignoredColumnIndexesForKeywords(
        const TableSnapshot& earlier,
        const TableSnapshot& later,
        const QStringList& keywords)
    {
        QSet<int> ignoredColumns;
        const auto appendMatchingColumns = [&ignoredColumns, &keywords](
            const QVector<TableSnapshotColumn>& columns)
        {
            for (const TableSnapshotColumn& column : columns)
            {
                for (const QString& keyword : keywords)
                {
                    if (headerMatchesKeyword(column.headerText, keyword))
                    {
                        ignoredColumns.insert(column.sourceColumn);
                        break;
                    }
                }
            }
        };
        appendMatchingColumns(earlier.visibleColumns);
        appendMatchingColumns(later.visibleColumns);
        return ignoredColumns;
    }

    TableComparisonResult TableSnapshotCompareEngine::compare(
        const TableSnapshot& earlier,
        const TableSnapshot& later,
        const QSet<int>& ignoredOriginalColumnIndexes,
        const QVector<int>& keyColumnIndexes,
        const TableSnapshotComparisonLimits& limits,
        const std::function<bool()>& shouldCancel)
    {
        TableComparisonResult result;
        result.earlierSnapshot = earlier;
        result.laterSnapshot = later;
        result.columns = combinedColumns(earlier, later);
        result.ignoredSourceColumns = ignoredOriginalColumnIndexes;

        const SnapshotColumnLookup earlierLookup(result.earlierSnapshot);
        const SnapshotColumnLookup laterLookup(result.laterSnapshot);
        result.earlierValueIndexes = valueIndexesForColumns(earlierLookup, result.columns);
        result.laterValueIndexes = valueIndexesForColumns(laterLookup, result.columns);
        result.estimatedBytes = sizeof(TableComparisonResult);
        result.estimatedBytes = saturatingAdd(
            result.estimatedBytes,
            logicalSnapshotBytes(result.earlierSnapshot));
        result.estimatedBytes = saturatingAdd(
            result.estimatedBytes,
            logicalSnapshotBytes(result.laterSnapshot));
        result.estimatedBytes = saturatingAdd(
            result.estimatedBytes,
            static_cast<quint64>(result.columns.size()) *
                (sizeof(TableSnapshotColumn) + sizeof(int) * 2ULL));
        if (result.estimatedBytes > limits.maximumEstimatedBytes)
        {
            result.truncatedByResultByteLimit = true;
            return result;
        }
        if (shouldCancel && shouldCancel())
        {
            result.cancelled = true;
            return result;
        }

        ComparisonWorkState work{ limits, shouldCancel, result };
        if (!work.reserveTemporary(
                static_cast<quint64>(
                    earlierLookup.valueIndexBySourceColumn.size() +
                    laterLookup.valueIndexBySourceColumn.size()) * 64ULL))
        {
            return result;
        }

        const QVector<int> keyColumns = usableStableKeyColumns(
            earlier,
            later,
            earlierLookup,
            laterLookup,
            keyColumnIndexes,
            result.ignoredSourceColumns);
        QHash<QByteArray, int> earlierRowsByKey;
        QHash<QByteArray, int> laterRowsByKey;
        const bool canUseStableKeys = !keyColumns.isEmpty() &&
            buildUniqueRowKeys(earlier, earlierLookup, keyColumns, earlierRowsByKey, work) &&
            buildUniqueRowKeys(later, laterLookup, keyColumns, laterRowsByKey, work);
        if (!work.canContinue())
        {
            return result;
        }

        if (canUseStableKeys)
        {
            result.usesStableKeys = true;
            if (!work.reserveTemporary(
                    static_cast<quint64>(later.rows.size()) * sizeof(bool)))
            {
                return result;
            }
            QVector<bool> matchedLaterRows(later.rows.size(), false);
            for (int earlierIndex = 0; earlierIndex < earlier.rows.size(); ++earlierIndex)
            {
                const TableSnapshotRow& earlierRow = earlier.rows.at(earlierIndex);
                const QByteArray key = rowKeyHash(earlierRow, earlierLookup, keyColumns, work);
                if (!work.canContinue())
                {
                    return result;
                }
                const auto laterIterator = laterRowsByKey.constFind(key);
                if (laterIterator == laterRowsByKey.cend())
                {
                    if (!appendComparisonRow(
                            result,
                            limits,
                            TableComparisonSource::EarlierOnly,
                            earlierIndex,
                            -1,
                            false))
                    {
                        return result;
                    }
                    if (!work.checkpointAfterRow())
                    {
                        return result;
                    }
                    continue;
                }

                const int laterIndex = laterIterator.value();
                matchedLaterRows[laterIndex] = true;
                const TableSnapshotRow& laterRow = later.rows.at(laterIndex);
                QSet<int> changedColumns;
                for (const TableSnapshotColumn& column : result.columns)
                {
                    if (!work.consumeWork())
                    {
                        return result;
                    }
                    if (!result.ignoredSourceColumns.contains(column.sourceColumn) &&
                        valueForSourceColumn(earlierRow, earlierLookup, column.sourceColumn) !=
                            valueForSourceColumn(laterRow, laterLookup, column.sourceColumn))
                    {
                        changedColumns.insert(column.sourceColumn);
                    }
                }
                if (!appendComparisonRow(
                        result,
                        limits,
                        TableComparisonSource::Both,
                        earlierIndex,
                        laterIndex,
                        true,
                        std::move(changedColumns)))
                {
                    return result;
                }
                if (!work.checkpointAfterRow())
                {
                    return result;
                }
            }
            for (int laterIndex = 0; laterIndex < later.rows.size(); ++laterIndex)
            {
                if (!matchedLaterRows.at(laterIndex) &&
                    !appendComparisonRow(
                        result,
                        limits,
                        TableComparisonSource::LaterOnly,
                        -1,
                        laterIndex,
                        true))
                {
                    return result;
                }
                if (!work.checkpointAfterRow())
                {
                    return result;
                }
            }
            return result;
        }

        if (!work.reserveTemporary(
                static_cast<quint64>(later.rows.size()) * 112ULL))
        {
            return result;
        }
        QHash<QByteArray, QVector<int>> laterRowsBySignature;
        laterRowsBySignature.reserve(later.rows.size());
        for (int laterIndex = 0; laterIndex < later.rows.size(); ++laterIndex)
        {
            const QByteArray signature = rowHash(
                later.rows.at(laterIndex),
                laterLookup,
                result.columns,
                result.ignoredSourceColumns,
                work);
            if (!work.canContinue())
            {
                return result;
            }
            laterRowsBySignature[signature].push_back(laterIndex);
            if (!work.checkpointAfterRow())
            {
                return result;
            }
        }

        if (!work.reserveTemporary(
                static_cast<quint64>(later.rows.size()) * (sizeof(bool) + 96ULL)))
        {
            return result;
        }
        QHash<QByteArray, int> matchedPerSignature;
        QVector<bool> matchedLaterRows(later.rows.size(), false);
        for (int earlierIndex = 0; earlierIndex < earlier.rows.size(); ++earlierIndex)
        {
            const TableSnapshotRow& earlierRow = earlier.rows.at(earlierIndex);
            const QByteArray signature = rowHash(
                earlierRow,
                earlierLookup,
                result.columns,
                result.ignoredSourceColumns,
                work);
            if (!work.canContinue())
            {
                return result;
            }
            const auto matchesIterator = laterRowsBySignature.constFind(signature);
            const int matchOffset = matchedPerSignature.value(signature);
            if (matchesIterator != laterRowsBySignature.cend() &&
                matchOffset < matchesIterator.value().size())
            {
                const int laterIndex = matchesIterator.value().at(matchOffset);
                matchedPerSignature.insert(signature, matchOffset + 1);
                matchedLaterRows[laterIndex] = true;
                if (!appendComparisonRow(
                        result,
                        limits,
                        TableComparisonSource::Both,
                        earlierIndex,
                        laterIndex,
                        false))
                {
                    return result;
                }
            }
            else
            {
                if (!appendComparisonRow(
                        result,
                        limits,
                        TableComparisonSource::EarlierOnly,
                        earlierIndex,
                        -1,
                        false))
                {
                    return result;
                }
            }
            if (!work.checkpointAfterRow())
            {
                return result;
            }
        }
        for (int laterIndex = 0; laterIndex < later.rows.size(); ++laterIndex)
        {
            if (!matchedLaterRows.at(laterIndex) &&
                !appendComparisonRow(
                    result,
                    limits,
                    TableComparisonSource::LaterOnly,
                    -1,
                    laterIndex,
                    true))
            {
                return result;
            }
            if (!work.checkpointAfterRow())
            {
                return result;
            }
        }
        return result;
    }

    QString TableSnapshotCompareEngine::snapshotLabelForOrdinal(quint64 ordinal)
    {
        QString label;
        quint64 current = ordinal + 1;
        while (current > 0)
        {
            const quint64 remainder = (current - 1) % 26;
            label.prepend(QChar(QLatin1Char('A').unicode() + static_cast<ushort>(remainder)));
            current = (current - 1) / 26;
        }
        return label;
    }

    TableComparisonModel::TableComparisonModel(QObject* parent)
        : QAbstractTableModel(parent)
    {
    }

    TableComparisonModel::TableComparisonModel(TableComparisonResult comparison, QObject* parent)
        : QAbstractTableModel(parent)
        , m_comparison(std::move(comparison))
    {
        rebuildVisibleRows();
    }

    void TableComparisonModel::setComparison(TableComparisonResult comparison)
    {
        beginResetModel();
        m_comparison = std::move(comparison);
        rebuildVisibleRows();
        endResetModel();
    }

    const TableComparisonResult& TableComparisonModel::comparison() const
    {
        return m_comparison;
    }

    void TableComparisonModel::setShowDifferencesOnly(const bool enabled)
    {
        if (m_showDifferencesOnly == enabled)
        {
            return;
        }
        beginResetModel();
        m_showDifferencesOnly = enabled;
        rebuildVisibleRows();
        endResetModel();
    }

    bool TableComparisonModel::showDifferencesOnly() const
    {
        return m_showDifferencesOnly;
    }

    int TableComparisonModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : m_visibleRows.size();
    }

    int TableComparisonModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : m_comparison.columns.size() + 1;
    }

    QVariant TableComparisonModel::data(const QModelIndex& index, const int role) const
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_visibleRows.size())
        {
            return {};
        }

        const TableComparisonRow& row = m_comparison.rows.at(m_visibleRows.at(index.row()));
        if (role == Qt::DisplayRole)
        {
            if (index.column() == 0)
            {
                switch (row.source)
                {
                case TableComparisonSource::EarlierOnly:
                    return QStringLiteral("A");
                case TableComparisonSource::LaterOnly:
                    return QStringLiteral("B");
                case TableComparisonSource::Both:
                    return QStringLiteral("AB");
                }
            }

            const int valueIndex = index.column() - 1;
            if (valueIndex < 0 || valueIndex >= m_comparison.columns.size())
            {
                return {};
            }

            const TableSnapshot& snapshot = row.displayLater
                ? m_comparison.laterSnapshot
                : m_comparison.earlierSnapshot;
            const QVector<int>& valueIndexes = row.displayLater
                ? m_comparison.laterValueIndexes
                : m_comparison.earlierValueIndexes;
            const int snapshotRowIndex = row.displayLater
                ? row.laterRowIndex
                : row.earlierRowIndex;
            if (snapshotRowIndex < 0 ||
                snapshotRowIndex >= snapshot.rows.size() ||
                valueIndex >= valueIndexes.size())
            {
                return {};
            }
            const int snapshotValueIndex = valueIndexes.at(valueIndex);
            const TableSnapshotRow& snapshotRow = snapshot.rows.at(snapshotRowIndex);
            return snapshotValueIndex >= 0 && snapshotValueIndex < snapshotRow.values.size()
                ? QVariant(snapshotRow.values.at(snapshotValueIndex))
                : QVariant();
        }

        if (role == Qt::BackgroundRole)
        {
            if (row.source == TableComparisonSource::EarlierOnly)
            {
                return earlierOnlyColor();
            }
            if (row.source == TableComparisonSource::LaterOnly)
            {
                return laterOnlyColor();
            }
            if (index.column() > 0)
            {
                const int sourceIndex = index.column() - 1;
                if (sourceIndex >= 0 && sourceIndex < m_comparison.columns.size() &&
                    row.changedSourceColumns.contains(m_comparison.columns.at(sourceIndex).sourceColumn))
                {
                    return changedFieldColor();
                }
            }
        }
        return {};
    }

    QVariant TableComparisonModel::headerData(
        const int section,
        const Qt::Orientation orientation,
        const int role) const
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0)
        {
            return {};
        }
        if (section == 0)
        {
            return ks::i18n::sourceText(QStringLiteral("来源"));
        }

        const int columnIndex = section - 1;
        return columnIndex >= 0 && columnIndex < m_comparison.columns.size()
            ? QVariant(ks::i18n::displayText(m_comparison.columns.at(columnIndex).headerText))
            : QVariant();
    }

    Qt::ItemFlags TableComparisonModel::flags(const QModelIndex& index) const
    {
        return index.isValid() ? (Qt::ItemIsEnabled | Qt::ItemIsSelectable) : Qt::NoItemFlags;
    }

    void TableComparisonModel::rebuildVisibleRows()
    {
        m_visibleRows.clear();
        m_visibleRows.reserve(m_comparison.rows.size());
        for (int rowIndex = 0; rowIndex < m_comparison.rows.size(); ++rowIndex)
        {
            const TableComparisonRow& row = m_comparison.rows.at(rowIndex);
            if (!m_showDifferencesOnly || row.source != TableComparisonSource::Both ||
                !row.changedSourceColumns.isEmpty())
            {
                m_visibleRows.push_back(rowIndex);
            }
        }
    }
}
