#include "RegistryDock.h"
#include "RegistryWorkbenchAccess.h"
#include "../UI/TableInteractionSupport.h"
#include "../UI/AsyncOperation.h"
#include "../UI/AsyncCleanupRegistry.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>

void RegistryDock::startSearchAsync()
{
    if (m_searchRunning.load()) return;
    QString keyword = m_searchEdit->text();
    if (keyword.isEmpty()) return;
    SearchOptions options;
    options.searchKeyName = m_matchKeysCheck->isChecked();
    options.searchValueName = m_matchNamesCheck->isChecked();
    options.searchValueData = m_matchDataCheck->isChecked();
    options.caseSensitive = m_matchCaseCheck->isChecked();
    options.exactMatch = m_matchExactCheck->isChecked();
    options.recursive = m_searchScopeCombo->currentData().toInt() != 0;
    options.viewBits = m_viewBits;
    options.valueType = m_searchTypeCombo->currentData().toInt();
    options.generation = ++m_searchGeneration;
    if (!options.searchKeyName && !options.searchValueName && !options.searchValueData)
    { QMessageBox::information(this, QStringLiteral("搜索"), QStringLiteral("至少选择一种搜索范围。")); return; }
    if (keyword.startsWith(QStringLiteral("hex:"), Qt::CaseInsensitive))
    {
        QString pattern = keyword.mid(4);
        pattern.remove(QRegularExpression(QStringLiteral("\\s+")));
        if (pattern.isEmpty() || (pattern.size() % 2) != 0
            || !QRegularExpression(QStringLiteral("^[0-9a-fA-F]+$")).match(pattern).hasMatch())
        { QMessageBox::warning(this, QStringLiteral("搜索"), QStringLiteral("hex: 后请输入成对的十六进制字节。")); return; }
        options.binaryPattern = QByteArray::fromHex(pattern.toLatin1());
        options.searchKeyName = false;
        options.searchValueName = false;
        options.searchValueData = true;
    }
    QStringList roots;
    if (m_searchScopeCombo->currentData().toInt() == 2)
        roots = {QStringLiteral("HKEY_CLASSES_ROOT"), QStringLiteral("HKEY_CURRENT_USER"),
            QStringLiteral("HKEY_LOCAL_MACHINE"), QStringLiteral("HKEY_USERS"), QStringLiteral("HKEY_CURRENT_CONFIG")};
    else roots.append(m_currentPath);
    const bool r0 = shouldUseRegistryR0();
    auto session = std::make_shared<SearchSession>();
    session->generation = options.generation;
    auto lease = ks::ui::AsyncCleanupRegistry::global().acquire([session]() { session->stopping.store(true); });
    if (!lease) { updateStatusBar(QStringLiteral("宿主正在关闭，注册表搜索未提交。")); return; }
    auto retired = std::move(m_searchSession);
    if (retired) QThreadPool::globalInstance()->start([retired]() {});
    m_searchSession = session;
    m_searchRunning.store(true);
    m_searchSkipped.store(0);
    m_searchDropped.store(0);
    m_searchScannedKeys = 0;
    m_searchHitCount = 0;
    m_lastSearchStopped = false;
    m_lastSearchFailed = false;
    m_searchResultTable->setRowCount(0);
    m_rightTabWidget->setCurrentWidget(m_searchResultTable);
    m_searchButton->setEnabled(false);
    m_stopSearchButton->setEnabled(true);
    m_searchFlushTimer->start();
    m_progressPid = kPro.add(this, "注册表", "搜索");
    session->progressPid = m_progressPid;
    kPro.set(m_progressPid, "搜索中", 0, 0.0f);
    struct Request {
        QStringList roots; QString keyword; SearchOptions options; bool r0;
        std::shared_ptr<SearchSession> session;
        std::shared_ptr<ks::ui::AsyncCleanupRegistry::Lease> lease;
    } request{roots, keyword, options, r0, session, std::move(lease)};
    struct Result {};
    const auto ticket = m_searchOperation->submit<Request, Result>(std::move(request),
        [](const Request& input, const ks::ui::AsyncOperationToken& token) {
            const auto shouldStop = [&]() { return token.isCanceled() || input.session->stopping.load(); };
            std::size_t scanned = 0, hits = 0;
            auto terminal = SearchSession::State::Complete;
            try
            {
                for (const auto& root : input.roots)
                {
                    if (shouldStop()) break;
                    const bool useR0 = input.r0 && root != QStringLiteral("HKEY_CLASSES_ROOT")
                        && !root.startsWith(QStringLiteral("HKEY_CLASSES_ROOT\\"), Qt::CaseInsensitive);
                    searchRegistryPath(root, useR0, input.keyword, input.options, input.session, shouldStop, &scanned, &hits);
                }
                terminal = shouldStop() ? SearchSession::State::Stopped : SearchSession::State::Complete;
            }
            catch (...) { terminal = SearchSession::State::Failed; }
            input.session->scanned.store(scanned);
            input.session->hits.store(hits);
            input.session->state.store(terminal);
            return Result{};
        },
        [this](const Result&, std::uint64_t) { flushPendingSearchRows(); return true; },
        [this, session](std::exception_ptr) { session->state.store(SearchSession::State::Failed); flushPendingSearchRows(); },
        [pid = session->progressPid](const ks::ui::AsyncOperationOutcome outcome) {
            if (outcome == ks::ui::AsyncOperationOutcome::Canceled || outcome == ks::ui::AsyncOperationOutcome::OwnerClosed)
                kPro.set(pid, "搜索停止", 0, 100.0f);
        });
    if (!ticket) { session->state.store(SearchSession::State::Failed); flushPendingSearchRows(); }
}

void RegistryDock::searchRegistryPath(const QString& start, bool useR0, const QString& keyword,
    const SearchOptions& options, const std::shared_ptr<SearchSession>& session,
    const std::function<bool()>& shouldStop, std::size_t* scanned, std::size_t* hits)
{
    struct Location { QString path; int depth; };
    QVector<Location> stack{{start, 0}};
    const auto sensitivity = options.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const auto matches = [&](const QString& text) {
        return options.exactMatch ? text.compare(keyword, sensitivity) == 0 : text.contains(keyword, sensitivity);
    };
    const RegistryAccessContext context{options.viewBits, useR0};
    while (!stack.isEmpty() && !shouldStop())
    {
        const Location location = stack.takeLast();
        if (location.depth > options.maximumDepth) { ++session->skipped; continue; }
        RegistryKeyListing listing;
        QString error;
        if (!RegistryWorkbenchAccess::enumerate(location.path, context, &listing, &error, options.recursive))
        { ++session->skipped; continue; }
        if (shouldStop()) break;
        ++*scanned;
        session->scanned.store(*scanned);
        if (!listing.complete) ++session->skipped;
        const QString keyName = location.path.mid(location.path.lastIndexOf(QLatin1Char('\\')) + 1);
        if (options.searchKeyName && options.valueType < 0 && matches(keyName))
        {
            PendingSearchRow row;
            row.viewBits = context.viewBits;
            row.useR0 = context.useR0;
            row.keyPathText = location.path; row.valueNameText = QStringLiteral("<Key>");
            row.valueTypeText = QStringLiteral("<Key>"); row.hitSourceText = QStringLiteral("KeyName");
            row.isKeyResult = true; enqueuePendingSearchRow(session, std::move(row)); ++*hits; session->hits.store(*hits);
        }
        for (auto value : listing.values)
        {
            if (shouldStop()) break;
            if (options.valueType >= 0 && value.type != static_cast<quint32>(options.valueType)) continue;
            bool matched = options.searchValueName && matches(value.name);
            QString source = matched ? QStringLiteral("ValueName") : QString();
            if (!matched && options.searchValueData)
            {
                if (!value.complete)
                {
                    RegistryValueState full;
                    if (!RegistryWorkbenchAccess::read(location.path, value.name, context, &full, &error) || !full.exists || !full.complete)
                    { ++session->skipped; continue; }
                    if (shouldStop()) break;
                    value = std::move(full);
                }
                if (!options.binaryPattern.isEmpty())
                    matched = options.exactMatch ? value.data == options.binaryPattern : value.data.contains(options.binaryPattern);
                else if (value.type == REG_SZ || value.type == REG_EXPAND_SZ || value.type == REG_MULTI_SZ)
                {
                    if ((value.data.size() % 2) != 0) { ++session->skipped; continue; }
                    const QString dataText = QString::fromUtf16(reinterpret_cast<const char16_t*>(value.data.constData()), value.data.size() / 2);
                    if (options.exactMatch)
                    {
                        for (const auto& part : dataText.split(QChar::Null)) if (matches(part)) { matched = true; break; }
                    }
                    else matched = matches(dataText);
                }
                else if (value.type == REG_DWORD || value.type == REG_QWORD) matched = matches(formatValueData(value.type, value.data));
                else matched = options.exactMatch ? value.data == keyword.toUtf8() : value.data.contains(keyword.toUtf8());
                if (matched) source = QStringLiteral("ValueData");
            }
            if (!matched) continue;
            PendingSearchRow row;
            row.viewBits = context.viewBits;
            row.useR0 = context.useR0;
            row.keyPathText = location.path; row.rawValueName = value.name;
            row.valueNameText = value.name.isEmpty() ? QStringLiteral("(默认)") : value.name;
            row.valueTypeText = valueTypeToText(value.type);
            row.valueDataPreviewText = formatValueData(value.type, value.data);
            if (!value.complete) row.valueDataPreviewText += QStringLiteral(" <数据摘要不完整>");
            row.hitSourceText = source + (useR0 ? QStringLiteral(" / R0") : QStringLiteral(" / Win32"));
            enqueuePendingSearchRow(session, std::move(row)); ++*hits; session->hits.store(*hits);
        }
        if (options.recursive)
            for (auto it = listing.subKeys.crbegin(); it != listing.subKeys.crend(); ++it)
            {
                if (stack.size() >= 100000) { ++session->skipped; break; }
                stack.push_back({location.path + QLatin1Char('\\') + *it, location.depth + 1});
            }
    }
}
