"""Compile and run FileDock's actual deleted-file model with QtCore only.

The harness extracts the immutable snapshot, projection builder, cell formatting
and QAbstractTableModel from FileDock.cpp. Only the i18n facade and cancellation
token are substituted. No Widgets/Gui libraries, platform plugin, volume scan,
recovery operation or product process is used. Requires x64 MSVC and Qt SDKs.
"""

import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile


SOURCE_PATH = Path('Ksword5.1/Ksword5.1/FileDock/FileDock.cpp')
HEADER_PATH = Path('Ksword5.1/Ksword5.1/FileDock/ManualFileSystemParser.h')


def between(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def repository_root(start):
    for candidate in (start, *start.parents):
        if (candidate / '.git').exists():
            return candidate
    return start


def first_existing(candidates):
    return next((path for path in candidates if path.exists()), candidates[-1])


def check_editor_gate(source):
    """Check the widget boundary without compiling or instantiating widgets.

    Query trimming and invalid-regex cancellation belong to this boundary; the
    value-only worker intentionally receives an already validated predicate.
    """
    begin = source.index('void FileDock::applyRecoveryFilter()')
    next_function = source.index('\nvoid FileDock::', source.index('{', begin))
    boundary = source[begin:next_function]
    assert 'text().trimmed()' in boundary, 'recovery editor no longer trims queries'
    assert '.isValid()' in boundary, 'invalid regular expression gate is missing'
    invalid_branch = between(boundary, 'if (invalid)', 'else m_recoveryFilterEdit')
    assert 'm_recoveryProjectionOperation->cancel()' in invalid_branch, (
        'invalid regular expression no longer cancels stale projection work')
    assert 'if (!m_recoveryPendingSnapshot) return;' in invalid_branch, (
        'invalid regular expression no longer preserves the committed projection')
    assert boundary.index('.isValid()') < boundary.index('->submit<'), (
        'regular expression must be validated before a projection is submitted')


def check_recovery_contract(source):
    assert 'm_deletedRecoveryItems' not in source, 'legacy row-to-cache coupling returned'
    page = between(source, 'void FileDock::initializeRecoveryPage()',
                   'void FileDock::applyRecoveryFilter()')
    assert 'new ks::ui::TableActionTableView' in page
    assert 'TableActionBarMode::Compact' in page
    for stable_id in ('file-name', 'path-hint', 'size', 'modified',
                      'record-sequence', 'integrity', 'recovery-capability'):
        assert f'QStringLiteral("{stable_id}")' in page, f'missing stable column ID: {stable_id}'
    assert 'QHeaderView::sectionClicked' in page, 'header sorting connection disappeared'
    scan = between(source, 'void FileDock::scanDeletedFilesForRecoveryAsync()',
                   'void FileDock::recoverSelectedDeletedFiles()')
    assert 'new QTableWidgetItem' not in scan and 'setRowCount(' not in scan, (
        'recovery scan recreated per-cell widget items')
    selected = between(source, 'void FileDock::recoverSelectedDeletedFilesAsync()',
                       'void FileDock::recoverDeletedFileEntriesAsync(')
    assert 'snapshot->entries[static_cast<std::size_t>(source)]' in selected
    assert 'recoverDeletedFileEntriesAsync(snapshot->volumeRoot, std::move(selectedItems))' in selected, (
        'recovery selection no longer freezes records and source volume before the modal dialog')


PRELUDE = r'''
#include <QtCore>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

namespace ks::i18n {
QString displayText(const QString& text) { return text; }
}
namespace ks::ui {
struct TestCancellationState {
    std::atomic<bool> canceled{ false };
    std::atomic<int> checks{ 0 };
    int cancelOnCheck = 0;
};
struct AsyncOperationToken {
    std::shared_ptr<TestCancellationState> state = std::make_shared<TestCancellationState>();
    bool isCanceled() const {
        const int check = state->checks.fetch_add(1) + 1;
        if (state->cancelOnCheck > 0 && check >= state->cancelOnCheck)
            state->canceled.store(true);
        return state->canceled.load();
    }
};
}
'''


TESTS = r'''
namespace {
int assertions = 0;
void require(bool condition, const char* message) {
    ++assertions;
    if (!condition) { std::cerr << "FAIL " << message << std::endl; std::exit(1); }
}

std::shared_ptr<FileRecoverySnapshot> snapshot(int count) {
    auto result = std::make_shared<FileRecoverySnapshot>();
    result->volumeRoot = QStringLiteral("E:\\");
    result->revision = 42;
    result->entries.reserve(static_cast<std::size_t>(count));
    for (int row = 0; row < count; ++row) {
        ks::file::NtfsDeletedFileEntry entry;
        entry.fileName = QStringLiteral("item-%1.bin").arg(row);
        entry.pathHint = QStringLiteral("E:\\fixture\\") + entry.fileName;
        entry.sizeBytes = 1024ULL + static_cast<std::uint64_t>(row);
        entry.modifiedTime = QDateTime::fromSecsSinceEpoch(1700000000LL + row, QTimeZone::UTC);
        entry.fileReference = 100000ULL + static_cast<std::uint64_t>(row);
        entry.sequenceNumber = static_cast<std::uint16_t>(row % 65535 + 1);
        entry.estimatedIntegrityPercent = row % 101;
        entry.recoveryCapability = ks::file::NtfsRecoveryCapability::MetadataOnly;
        result->entries.push_back(std::move(entry));
    }
    return result;
}

RecoveryProjectionResult project(const std::shared_ptr<const FileRecoverySnapshot>& value,
    const QString& query = {}, bool regex = false, int column = -1,
    Qt::SortOrder order = Qt::AscendingOrder) {
    return buildRecoveryProjection(RecoveryProjectionRequest{ value, query, regex, column, order },
        ks::ui::AsyncOperationToken{});
}

void checkProjection(const RecoveryProjectionResult& result, const std::vector<int>& expected) {
    require(static_cast<bool>(result.projection), "projection unexpectedly canceled");
    require(result.projection->rows == expected, "projection row identities differ");
    require(result.projection->sourceToView.size() == result.request.snapshot->entries.size(),
        "reverse row mapping does not span complete source snapshot");
    for (std::size_t source = 0; source < result.projection->sourceToView.size(); ++source) {
        const auto found = std::find(expected.begin(), expected.end(), static_cast<int>(source));
        const int expectedView = found == expected.end() ? -1 : static_cast<int>(found - expected.begin());
        require(result.projection->sourceToView[source] == expectedView,
            "source-to-view mapping refers to the wrong record");
    }
}

void checkSizes() {
    for (const int count : { 0, 1000, 80000 }) {
        auto value = snapshot(count);
        const auto result = project(value);
        require(static_cast<bool>(result.projection), "full projection was not produced");
        require(result.projection->rows.size() == static_cast<std::size_t>(count),
            "snapshot projection truncated candidates");
        DeletedRecoveryModel model(nullptr);
        int resets = 0;
        QObject::connect(&model, &QAbstractItemModel::modelReset, [&]() { ++resets; });
        QElapsedTimer timing; timing.start();
        model.publish(value, result.projection);
        const auto publishMicroseconds = timing.nsecsElapsed() / 1000;
        require(resets == 1, "snapshot publish did not reset the real model once");
        require(model.snapshot() == value && model.snapshot()->revision == 42
            && model.snapshot()->volumeRoot == QStringLiteral("E:\\"),
            "snapshot pointer, revision or source volume was lost");
        require(model.rowCount() == count && model.columnCount() == 7,
            "real model row/column count differs");
        require(model.sourceRow(-1) == -1 && model.sourceRow(count) == -1,
            "model accepted an out-of-range visible row");
        require(model.viewRow(-1) == -1 && model.viewRow(count) == -1,
            "model accepted an out-of-range source row");
        require(model.entry(-1) == nullptr && model.entry(count) == nullptr,
            "model exposed an out-of-range candidate");
        require(!model.data(QModelIndex()).isValid(), "invalid index has display data");
        for (int column = 0; column < 7; ++column)
            require(!model.headerData(column, Qt::Horizontal).toString().isEmpty(),
                "one of seven translated headers disappeared");
        if (count > 0) {
            const int last = count - 1;
            require(model.entry(last) == &value->entries.back(), "last row identity differs");
            require(model.entry(last)->fileReference == 100000ULL + static_cast<std::uint64_t>(last),
                "last MFT reference was truncated");
            require(model.sourceRow(last) == last && model.viewRow(last) == last,
                "last row's bidirectional identity differs");
            require(model.rowCount(model.index(0, 0)) == 0
                && model.columnCount(model.index(0, 0)) == 0,
                "flat model exposed child rows or columns");
            require(!(model.flags(model.index(last, 0)) & Qt::ItemIsEditable),
                "recovery model unexpectedly allows editing");
            for (int column = 0; column < 7; ++column)
                require(model.index(last, column).data().isValid(), "last row has a missing cell");
            require(!model.index(last, 0).data(Qt::UserRole + 1).isValid(),
                "unrecognized model role has synthetic data");
        }
        std::cout << "SNAPSHOT rows=" << count << " columns=" << model.columnCount()
            << " publish_us=" << publishMicroseconds << std::endl;
    }
}

void checkFormattingAndCapabilities() {
    auto value = snapshot(1);
    auto& entry = value->entries[0];
    entry.fileReference = 0x100000001ULL;
    entry.sequenceNumber = 65535;
    entry.hasOriginalName = false;
    entry.sizeBytes = 1024;
    entry.modifiedTime = {};
    entry.estimatedIntegrityPercent = -1;
    const auto result = project(value);
    DeletedRecoveryModel model(nullptr); model.publish(value, result.projection);
    require(model.index(0, 4).data().toString() == QStringLiteral("4294967297 / seq 65535"),
        "MFT reference or uint16 sequence is not displayed as a number");
    require(model.index(0, 2).data().toString() == QStringLiteral("1.00 KB"),
        "size formatting changed");
    require(model.index(0, 3).data().toString() == QStringLiteral("-"),
        "unknown modification time changed");
    require(model.index(0, 5).data().toString() == QStringLiteral("未知"),
        "unknown integrity formatting changed");
    require(model.index(0, 6).data().toString().endsWith(QStringLiteral(" / 缺名")),
        "missing-name display suffix disappeared");
    require(!model.index(0, 0).data(Qt::ToolTipRole).toString().isEmpty(),
        "missing-name explanation disappeared");
    using Capability = ks::file::NtfsRecoveryCapability;
    for (const Capability capability : { Capability::MetadataOnly, Capability::Resident,
        Capability::NonResidentIntact, Capability::NonResidentAtRisk, Capability::UnsupportedStream }) {
        entry.recoveryCapability = capability;
        require(isDeletedFileSafelyRecoverable(entry) ==
            (capability == Capability::Resident || capability == Capability::NonResidentIntact),
            "safe recovery capability boundary changed");
        require(!deletedFileRecoveryCapabilityText(entry).isEmpty(), "capability label disappeared");
    }
}

void checkFiltering() {
    auto value = snapshot(8);
    for (auto& entry : value->entries) {
        entry.fileName = QStringLiteral("ordinary.bin");
        entry.pathHint = QStringLiteral("E:\\ordinary");
        entry.sizeBytes = 987654321;
        entry.modifiedTime = QDateTime::fromString(QStringLiteral("2026-10-19 23:45:01"),
            QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        entry.estimatedIntegrityPercent = 99;
    }
    value->entries[0].fileName = QStringLiteral("AlPhA-report.bin");
    value->entries[0].pathHint = QStringLiteral("E:\\CrossFieldTarget");
    value->entries[1].pathHint = QStringLiteral("E:\\BetaFolder\\ordinary.bin");
    value->entries[2].recoveryCapability = ks::file::NtfsRecoveryCapability::Resident;
    value->entries[3].recoveryCapability = ks::file::NtfsRecoveryCapability::NonResidentIntact;
    value->entries[4].recoveryCapability = ks::file::NtfsRecoveryCapability::NonResidentAtRisk;
    value->entries[5].recoveryCapability = ks::file::NtfsRecoveryCapability::UnsupportedStream;
    value->entries[7].fileName = QStringLiteral("deleted_placeholder.bin");
    value->entries[7].hasOriginalName = false;
    checkProjection(project(value), { 0, 1, 2, 3, 4, 5, 6, 7 });
    checkProjection(project(value, QStringLiteral("alpha")), { 0 });
    checkProjection(project(value, QStringLiteral("bEtAfOlDeR")), { 1 });
    checkProjection(project(value, QStringLiteral("resident")), { 2 });
    checkProjection(project(value, QStringLiteral("非驻留完整可恢复")), { 3 });
    checkProjection(project(value, QStringLiteral("^alpha-report\\.bin$"), true), { 0 });
    checkProjection(project(value, QStringLiteral("BetaFolder.*ordinary\\.bin$"), true), { 1 });
    checkProjection(project(value, QStringLiteral("^Resident 可恢复$"), true), { 2 });
    checkProjection(project(value, QStringLiteral("缺名")), {});
    checkProjection(project(value, QStringLiteral("987654321")), {});
    checkProjection(project(value, QStringLiteral("2026-10-19")), {});
    checkProjection(project(value, QStringLiteral("99%")), {});
    checkProjection(project(value, QStringLiteral("seq")), {});
    // Matching is per field: a regular expression must not bridge name/path.
    checkProjection(project(value, QStringLiteral("AlPhA-report.*CrossFieldTarget"), true), {});
    std::cout << "FILTER fields=name,path,base-capability excluded=display-suffix,size,time,integrity,record" << std::endl;
}

void checkSorting() {
    auto value = snapshot(4);
    value->entries[0].fileName = QStringLiteral("bravo");
    value->entries[1].fileName = QStringLiteral("charlie");
    value->entries[2].fileName = QStringLiteral("alpha");
    value->entries[3].fileName = QStringLiteral("alpha");
    const auto ascending = project(value, {}, false, 0, Qt::AscendingOrder);
    const auto descending = project(value, {}, false, 0, Qt::DescendingOrder);
    checkProjection(ascending, { 2, 3, 0, 1 });
    checkProjection(descending, { 1, 0, 2, 3 });
    checkProjection(project(value, QStringLiteral("alpha|charlie"), true, 0, Qt::AscendingOrder),
        { 2, 3, 1 });
    // Preserve the prior item-table contract: these columns sort their display
    // text. In particular "100 B" sorts before "2 B"; this is not a numeric
    // ordering test disguised as a display-text ordering test.
    const std::array<QString, 4> pathKeys{ QStringLiteral("bravo"), QStringLiteral("charlie"),
        QStringLiteral("alpha"), QStringLiteral("alpha") };
    const std::array<std::uint64_t, 4> numbers{ 2, 30, 100, 100 };
    const std::array<int, 4> years{ 2000, 2001, 1990, 1990 };
    const std::array<ks::file::NtfsRecoveryCapability, 4> capabilities{
        ks::file::NtfsRecoveryCapability::MetadataOnly,
        ks::file::NtfsRecoveryCapability::UnsupportedStream,
        ks::file::NtfsRecoveryCapability::Resident,
        ks::file::NtfsRecoveryCapability::Resident };
    for (int column = 1; column < 7; ++column) {
        auto columnValue = snapshot(4);
        for (std::size_t source = 0; source < columnValue->entries.size(); ++source) {
            auto& entry = columnValue->entries[source];
            entry.pathHint = pathKeys[source];
            entry.sizeBytes = numbers[source];
            entry.modifiedTime = QDateTime(QDate(years[source], 1, 1), QTime(0, 0), QTimeZone::UTC);
            entry.fileReference = numbers[source];
            entry.sequenceNumber = 7;
            entry.estimatedIntegrityPercent = static_cast<int>(numbers[source]);
            entry.recoveryCapability = capabilities[source];
        }
        checkProjection(project(columnValue, {}, false, column, Qt::AscendingOrder), { 2, 3, 0, 1 });
        checkProjection(project(columnValue, {}, false, column, Qt::DescendingOrder), { 1, 0, 2, 3 });
    }
    DeletedRecoveryModel model(nullptr); model.publish(value, descending.projection);
    for (int view = 0; view < model.rowCount(); ++view) {
        const int source = descending.projection->rows[static_cast<std::size_t>(view)];
        require(model.sourceRow(view) == source && model.viewRow(source) == view,
            "model sorted mapping drifted from projection");
        require(model.entry(view) == &value->entries[static_cast<std::size_t>(source)],
            "sorted view points at a different MFT identity");
    }
    int requestedColumn = -1;
    Qt::SortOrder requestedOrder = Qt::AscendingOrder;
    model.sortRequested = [&](int column, Qt::SortOrder order) {
        requestedColumn = column; requestedOrder = order;
    };
    model.sort(4, Qt::DescendingOrder);
    require(requestedColumn == 4 && requestedOrder == Qt::DescendingOrder,
        "table header sort did not reach background projection callback");
    require(model.sourceRow(0) == 1, "sort request mutated published model synchronously");
    std::cout << "SORT columns=7 ascending=stable descending=stable mapping=bidirectional callback=deferred" << std::endl;
}

void checkCancellationAndRetirement() {
    auto value = snapshot(1000);
    const RecoveryProjectionRequest request{ value, {}, false, 0, Qt::AscendingOrder };
    ks::ui::AsyncOperationToken before;
    before.state->canceled.store(true);
    const auto early = buildRecoveryProjection(request, before);
    require(!early.projection && early.request.snapshot == value,
        "pre-canceled worker published a projection or lost request identity");
    ks::ui::AsyncOperationToken duringFilter;
    duringFilter.state->cancelOnCheck = 2;
    require(!buildRecoveryProjection(request, duringFilter).projection,
        "filter loop did not stop after cancellation");
    ks::ui::AsyncOperationToken filterCheckpoints;
    require(static_cast<bool>(buildRecoveryProjection(
        RecoveryProjectionRequest{ value, {}, false, -1, Qt::AscendingOrder },
        filterCheckpoints).projection), "baseline filter projection failed");
    ks::ui::AsyncOperationToken duringKeys;
    // Discover filtering's checkpoint count using the unsorted path so a
    // future change in its cancellation polling interval does not break this.
    duringKeys.state->cancelOnCheck = filterCheckpoints.state->checks.load() + 1;
    require(!buildRecoveryProjection(request, duringKeys).projection,
        "sort-key preparation ignored cancellation");
    require(duringKeys.state->checks.load() == duringKeys.state->cancelOnCheck,
        "sort-key cancellation was not reached");
    std::weak_ptr<const FileRecoverySnapshot> retired;
    std::atomic<bool> releasedOffThread{ false };
    QThread* const modelThread = QThread::currentThread();
    {
        DeletedRecoveryModel model(nullptr);
        auto old = std::shared_ptr<FileRecoverySnapshot>(new FileRecoverySnapshot,
            [&releasedOffThread, modelThread](FileRecoverySnapshot* retiredValue) {
                releasedOffThread.store(QThread::currentThread() != modelThread);
                delete retiredValue;
            });
        auto contents = snapshot(80000);
        *old = std::move(*contents);
        retired = old;
        const auto originalProjection = project(old).projection;
        model.publish(old, originalProjection);
        old.reset();
        const auto replacement = project(value);
        model.publish(value, replacement.projection);
        QThreadPool::globalInstance()->waitForDone();
        require(retired.expired(), "retired snapshot remained retained after replacement and worker drain");
        require(releasedOffThread.load(), "large snapshot was destroyed on the model thread");
    }
    QThreadPool::globalInstance()->waitForDone();
    std::cout << "CANCEL before/filter/sort-keys=pass RETIRE rows=80000 background-release=pass" << std::endl;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QLocale::setDefault(QLocale::c());
    checkSizes();
    checkFormattingAndCapabilities();
    checkFiltering();
    checkSorting();
    checkCancellationAndRetirement();
    QThreadPool::globalInstance()->waitForDone();
    std::cout << "PASS assertions=" << assertions
        << " maximum_rows=80000 linked=Qt6Core application=QCoreApplication" << std::endl;
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path,
                        help='Root containing FileDock.cpp (defaults to the script repository)')
    parser.add_argument('--qt-dir', type=Path)
    parser.add_argument('--vcvars', type=Path)
    args = parser.parse_args()
    script_root = Path(__file__).resolve().parents[1]
    source_root = (args.source_root or script_root).resolve()
    root = repository_root(source_root)
    source = (source_root / SOURCE_PATH).read_text(encoding='utf-8')
    header_path = source_root / HEADER_PATH
    if not header_path.exists():
        header_path = root / HEADER_PATH
    header = header_path.read_text(encoding='utf-8')
    check_editor_gate(source)
    check_recovery_contract(source)
    entry = between(header, '    enum class NtfsRecoveryCapability',
                    '    // ManualFileSystemParser')
    implementation = between(source, 'struct FileRecoverySnapshot',
                             '    // localVolumeRootForPath')
    harness = (PRELUDE + '\nnamespace ks::file {\n' + entry + '\n}\n'
               + implementation + '\n}\n' + TESTS)
    assert '#include <QtWidgets>' not in harness and '#include <QtGui>' not in harness
    qt = (args.qt_dir or first_existing([
        root / '.deps/Qt/6.9.3/msvc2022_64',
        Path('D:/Software/Qt/6.9.3/msvc2022_64'),
    ])).resolve()
    vcvars = (args.vcvars or first_existing([
        Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat'),
        Path('D:/Software/VS/VC/Auxiliary/Build/vcvars64.bat'),
    ])).resolve()
    if not (qt / 'lib/Qt6Core.lib').exists() or not vcvars.exists():
        parser.error('x64 MSVC vcvars64.bat and a Qt SDK containing Qt6Core.lib are required')
    with tempfile.TemporaryDirectory(prefix='ksword-recovery-model-test-') as temp:
        out = Path(temp)
        cpp, exe = out / 'test.cpp', out / 'test.exe'
        cpp.write_text(harness, encoding='utf-8')
        build = out / 'build.cmd'
        build.write_text(
            f'@echo off\ncall "{vcvars}" >nul\n'
            'if errorlevel 1 exit /b %errorlevel%\n'
            'cl /nologo /std:c++17 /permissive- /Zc:__cplusplus /utf-8 /EHsc /MD /W4 /WX /O2 /DNOMINMAX '
            f'/external:W0 /external:I"{qt / "include"}" '
            f'/external:I"{qt / "include/QtCore"}" "{cpp}" '
            f'/Fo"{out / "test.obj"}" /Fe"{exe}" /link /LIBPATH:"{qt / "lib"}" '
            'Qt6Core.lib\nexit /b %errorlevel%\n', encoding='utf-8')
        compilation = subprocess.run(['cmd.exe', '/d', '/c', str(build)], cwd=root,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if compilation.returncode:
            print(compilation.stdout)
        compilation.check_returncode()
        environment = dict(os.environ)
        environment['PATH'] = os.pathsep.join([str(qt / 'bin'), environment['PATH']])
        previous = ctypes.windll.kernel32.SetErrorMode(3)
        try:
            subprocess.run([str(exe)], check=True, env=environment, timeout=30)
        finally:
            ctypes.windll.kernel32.SetErrorMode(previous)
    print('SOURCE_STRUCTURE trim/regex-cancel/Compact/column-IDs/header-sort/frozen-selection=pass; no GUI or business IO executed')


if __name__ == '__main__':
    main()
