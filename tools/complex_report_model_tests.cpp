// 生产报告模型与规范文档的 Qt Core 回归，不创建 QWidget、不接触驱动或系统剪贴板。
#include "../Ksword5.1/Ksword5.1/UI/ComplexReportModel.h"
#include "../Ksword5.1/Ksword5.1/Internationalization/LanguageManager.h"

#include <QAbstractItemModelTester>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPersistentModelIndex>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <algorithm>
#include <cstdio>
#include <memory>

namespace
{
    bool mockTranslation = false; // 只控制测试翻译映射，不加载或修改真实语言包。
    int checks = 0; // 每条实际断言的计数。
    int failures = 0; // 回归失败数，main 据此返回退出码。

    void expect(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::printf("COMPLEX_REPORT_MODEL_FAILURE=%s\n", message);
        }
    }
}

namespace ks::i18n
{
    // 使用真实头声明和 sourceText 内联入口；只替换翻译依赖，不替换任何生产模型/文档代码。
    struct LanguageManager::State {};
    LanguageManager::LanguageManager() : QObject(nullptr), m_state(std::make_unique<State>()) {}
    LanguageManager::~LanguageManager() = default;
    LanguageManager& LanguageManager::instance()
    {
        static LanguageManager manager; // 与生产入口相同的单例身份。
        return manager;
    }
    QString LanguageManager::sourceText(const QString& source) const
    {
        if (!mockTranslation)
        {
            return source;
        }
        static const QHash<QString, QString> translations{
            {QStringLiteral("Report"), QStringLiteral("报告")},
            {QStringLiteral("Catalog"), QStringLiteral("目录")},
            {QStringLiteral("Size"), QStringLiteral("大小")},
            {QStringLiteral("Offset"), QStringLiteral("偏移")},
            {QStringLiteral("Type"), QStringLiteral("类型")},
            {QStringLiteral("Ready"), QStringLiteral("就绪")}
        };
        return translations.value(source, source);
    }
    bool LanguageManager::eventFilter(QObject* watched, QEvent* event)
    {
        return QObject::eventFilter(watched, event); // 仅满足真实 QObject 虚函数声明，不安装 GUI 过滤器。
    }
}

namespace
{
    using ks::ui::ComplexReportModel;
    using ks::ui::FieldDocument;
    using ks::ui::FieldNode;
    using ks::ui::ReportNodeKind;

    // 检查实际 dataChanged 范围是否覆盖给定父组，避免仅证明子属性发出了通知。
    bool changedIndex(const QSignalSpy& changes, const QModelIndex& index)
    {
        for (const QList<QVariant>& change : changes)
        {
            if (change.size() < 2)
            {
                continue;
            }
            const QModelIndex first = change.at(0).value<QModelIndex>();
            const QModelIndex last = change.at(1).value<QModelIndex>();
            if (first.parent() == index.parent() && index.row() >= first.row() && index.row() <= last.row())
            {
                return true;
            }
        }
        return false;
    }

    // 从规范树构造目录/类型/字段/属性，使用真实 builder；不复制报告模型的内部建树算法。
    FieldDocument catalogDocument()
    {
        FieldDocument document;
        document.title = QStringLiteral("Report");
        document.section(QStringLiteral("Catalog"));
        document.field(QStringLiteral("Status"), QStringLiteral("Ready"), true);
        FieldDocument type;
        type.section(QStringLiteral("_PEB"));
        type.nodes.first().initiallyExpanded = false;
        type.field(QStringLiteral("Size"), QStringLiteral("16"));
        FieldDocument field;
        field.section(QStringLiteral("_PEB.Flag"));
        field.nodes.first().initiallyExpanded = false;
        field.field(QStringLiteral("Offset"), QStringLiteral("0x10"));
        field.field(QStringLiteral("Type"), QStringLiteral("uint32"));
        type.nodes.first().children += field.nodes;
        document.nodes.first().children += type.nodes;
        return document;
    }

    // 已知小树的结构与原始展开策略；Qt 官方模型测试器额外检查 index/parent 契约。
    void hierarchyAndUpdates()
    {
        FieldDocument document = catalogDocument();
        ComplexReportModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        expect(model.setDocument(document), "initial nonempty snapshot resets model");
        expect(model.rowCount() == 1 && model.columnCount() == 3, "report exposes three independently readable columns");
        const QModelIndex root = model.index(0, 0);
        const QModelIndex type = model.index(1, 0, root);
        const QModelIndex field = model.index(1, 0, type);
        const QModelIndex offset = model.index(0, 0, field);
        expect(root.data(ks::ui::ReportKindRole).toInt() == int(ReportNodeKind::Section), "root retains section identity");
        expect(type.data(ks::ui::ReportKindRole).toInt() == int(ReportNodeKind::Group), "type is nested group");
        expect(field.data(ks::ui::ReportNameRole).toString() == QStringLiteral("Flag"), "only direct type prefix is removed from display name");
        expect(field.data(ks::ui::ReportValueRole).toString() == QStringLiteral("0x10 · uint32"), "collapsed field group previews only actual Offset and Type values");
        expect(type.data(ks::ui::ReportValueRole).toString().isEmpty(), "type header does not receive synthetic field-property summary");
        expect(model.parent(offset) == field && model.parent(field) == type && model.parent(type) == root, "four original levels remain independently navigable");
        expect(model.rowCount(field) == 2 && model.rowCount(offset) == 0, "short metadata has no synthetic expander");
        expect(root.data(ks::ui::ReportCountRole).toInt() == 2, "business count excludes synthetic details");
        expect(model.sectionIndexes() == QVector<QModelIndex>{root}, "summary indexes contain only root sections");
        expect(model.initialExpansion() == QVector<QModelIndex>{root}, "collapsed type and field do not expand by default");
        const QString path = offset.data(ks::ui::ReportPathRole).toString();
        const quintptr identity = offset.internalId(); // 普通更新前的稳定 integer internalId。
        QPersistentModelIndex persistent(offset);
        expect(path.contains(QStringLiteral("_PEB.Flag")), "stable path keeps original qualified field name");
        expect(offset.data(ks::ui::ReportSearchRole).toString().contains(QStringLiteral("_PEB"))
            && offset.data(ks::ui::ReportSearchRole).toString().contains(QStringLiteral("0x10")), "search includes ancestors and complete values");
        expect(!model.index(-1, 0).isValid() && !model.index(0, 3).isValid(), "invalid row and column cannot produce indexes");
        resets.clear();
        changes.clear();
        document.nodes[0].children[1].children[1].children[0].value = QStringLiteral("0x20");
        document.nodes[0].status = FieldNode::Status::Warning;
        document.nodes[0].summaryLabel = QStringLiteral("Runtime");
        expect(!model.setDocument(document), "ordinary value and status update does not reset structure");
        expect(resets.isEmpty() && !changes.isEmpty(), "update emits dataChanged without modelReset");
        expect(persistent.isValid() && persistent.data(ks::ui::ReportValueRole).toString() == QStringLiteral("0x20"), "persistent nested index reads updated value");
        expect(field.data(ks::ui::ReportValueRole).toString() == QStringLiteral("0x20 · uint32")
            && changedIndex(changes, field), "offset mutation updates and notifies collapsed parent group without reset");
        expect(model.textForIndex(field) == QStringLiteral("[_PEB.Flag]\nOffset: 0x20\nType: uint32"), "derived preview is excluded from canonical subtree copy");
        expect(field.data(ks::ui::ReportStatusRole).toInt() == int(FieldNode::Status::Neutral), "field preview never guesses or inherits runtime success status");
        expect(model.indexForPath(path).internalId() == identity, "preorder identity remains stable for same structure");
        expect(root.data(ks::ui::ReportStatusRole).toInt() == int(FieldNode::Status::Warning), "explicit producer status is visible");
        changes.clear();
        document.nodes[0].status = FieldNode::Status::Success;
        expect(!model.setDocument(document) && resets.isEmpty() && !changes.isEmpty(), "status-only update is retained without reset");
        changes.clear();
        expect(!model.setDocument(document) && changes.isEmpty() && resets.isEmpty(), "identical snapshot emits no change events");
        document.nodes[0].children[1].children[1].children[1].value = QStringLiteral("Ready");
        document.nodes[0].children[1].children[1].children[1].translateValue = true;
        expect(!model.setDocument(document) && resets.isEmpty() && changedIndex(changes, field), "type-value mutation also notifies actual parent preview");
        mockTranslation = true;
        model.relocalize();
        expect(resets.isEmpty() && !changes.isEmpty(), "relocalize updates display without rebuilding this structure");
        expect(model.indexForPath(path).data(ks::ui::ReportNameRole).toString() == QStringLiteral("偏移"), "real sourceText entry point supplies changed translation");
        expect(model.index(0, 0).data(ks::ui::ReportNameRole).toString() == QStringLiteral("目录"), "section display is localized");
        expect(field.data(ks::ui::ReportValueRole).toString() == QStringLiteral("0x20 · 就绪"), "parent preview respects original property translation policy");
        expect(persistent.isValid() && model.indexForPath(path).internalId() == identity, "localization preserves path and persistent identity");
        expect(model.document().nodes.first().name == QStringLiteral("Catalog"), "localization never rewrites canonical source names");
        mockTranslation = false;
    }

    // 使用与生产视图完全相同的原生代理选项，区分后代命中与分区本身命中，不创建视图。
    void recursiveProxyScope()
    {
        FieldDocument document = catalogDocument();
        FieldDocument other;
        other.section(QStringLiteral("_PEB.Other"));
        other.field(QStringLiteral("Offset"), QStringLiteral("0x20"));
        other.field(QStringLiteral("Type"), QStringLiteral("uint64"));
        document.nodes[0].children[1].children += other.nodes;
        ComplexReportModel model;
        model.setDocument(document);
        QSortFilterProxyModel proxy;
        proxy.setSourceModel(&model);
        proxy.setFilterRole(ks::ui::ReportSearchRole);
        proxy.setFilterCaseSensitivity(Qt::CaseInsensitive);
        proxy.setRecursiveFilteringEnabled(true);
        proxy.setAutoAcceptChildRows(true);
        const QModelIndex sourceRoot = model.index(0, 0);
        const QModelIndex sourceType = model.index(1, 0, sourceRoot);
        const QModelIndex sourceField = model.index(1, 0, sourceType);
        const QModelIndex sourceSibling = model.index(2, 0, sourceType);
        proxy.setFilterFixedString(QStringLiteral("_peb.flag"));
        const QModelIndex root = proxy.mapFromSource(sourceRoot);
        const QModelIndex type = proxy.mapFromSource(sourceType);
        const QModelIndex field = proxy.mapFromSource(sourceField);
        expect(root.isValid() && type.isValid() && field.isValid(), "field search retains only required ancestor chain");
        expect(proxy.rowCount(root) == 1 && proxy.rowCount(type) == 1, "autoAcceptChildRows does not widen descendant-only match to entire section");
        expect(!proxy.mapFromSource(sourceSibling).isValid()
            && !proxy.mapFromSource(model.index(0, 0, sourceRoot)).isValid(), "unrelated sibling field and root status remain filtered");
        expect(proxy.rowCount(field) == 2, "matching field keeps its complete actual property children");
        proxy.setFilterFixedString(QStringLiteral("0x10"));
        expect(proxy.mapFromSource(sourceField).isValid() && !proxy.mapFromSource(sourceSibling).isValid(), "actual offset preview is searchable without broadening sibling scope");
        proxy.setFilterFixedString(QStringLiteral("_PEB"));
        expect(proxy.rowCount(proxy.mapFromSource(sourceType)) == 3, "direct type-name match intentionally retains entire type contents");
        proxy.setFilterFixedString(QStringLiteral("Catalog"));
        expect(proxy.rowCount(proxy.mapFromSource(sourceRoot)) == 2, "direct section-name match intentionally retains section contents");
    }

    // 只验证规范组织约束，不模拟驱动或 identity 校验；生产分支另做源码核对。
    void identityAndSampleSections()
    {
        const QString skipped = QStringLiteral("deep catalog identity 未与当前 ntoskrnl 匹配，已跳过 R0 字段采样，避免错误偏移。");
        const QString noFields = QStringLiteral("PDB deep offset JSON 未提供可安全采样的小字段，或 profiles\\pdb_deep_offsets 未找到。");
        for (int scenario = 0; scenario < 3; ++scenario)
        {
            FieldDocument document;
            document.section(QStringLiteral("PDB Deep Runtime Identity Guard"));
            document.nodes.last().summaryLabel = QStringLiteral("Identity");
            document.nodes.last().status = scenario == 0 ? FieldNode::Status::Warning : FieldNode::Status::Success;
            document.field(QStringLiteral("结论"), scenario == 0 ? QStringLiteral("不匹配") : QStringLiteral("匹配"));
            document.field(QStringLiteral("原因"), QStringLiteral("identity evidence"));
            document.section(QStringLiteral("PDB Deep Runtime Sample - process_detail"));
            document.nodes.last().summaryLabel = QStringLiteral("Sample");
            document.nodes.last().status = scenario == 2 ? FieldNode::Status::Success : FieldNode::Status::Warning;
            if (scenario == 2)
            {
                document.field(QStringLiteral("Status"), QStringLiteral("OK"));
            }
            else
            {
                document.note(scenario == 0 ? skipped : noFields);
            }
            ComplexReportModel model;
            model.setDocument(document);
            const auto sections = model.sectionIndexes();
            expect(sections.size() == 2, "guard and sample have independent canonical sections");
            const QModelIndex guard = sections.at(0);
            const QModelIndex sample = sections.at(1);
            expect(model.rowCount(guard) == 2 && model.rowCount(sample) == 1, "every report section has its own actual content and no empty sample heading");
            expect(guard.data(ks::ui::ReportSummaryRole).toString() == QStringLiteral("Identity")
                && sample.data(ks::ui::ReportSummaryRole).toString() == QStringLiteral("Sample"), "summary labels keep identity and sampling distinct");
            expect(!model.textForIndex(guard).contains(skipped) && !model.textForIndex(guard).contains(noFields), "sampling skip note cannot leak into identity subtree copy");
            if (scenario != 2)
            {
                expect(model.textForIndex(sample).contains(scenario == 0 ? skipped : noFields)
                    && sample.data(ks::ui::ReportStatusRole).toInt() == int(FieldNode::Status::Warning), "skipped or empty sampling preserves its own warning and complete reason");
            }
            else
            {
                expect(sample.data(ks::ui::ReportStatusRole).toInt() == int(FieldNode::Status::Success), "explicit actual-sample state is preserved without inference");
            }
        }
    }

    // 重复字段必须仍可分别恢复选择与展开，不能以显示名作唯一键。
    void duplicateNames()
    {
        FieldDocument document;
        document.section(QStringLiteral("Duplicate"));
        document.field(QStringLiteral("Value"), QStringLiteral("first"));
        document.field(QStringLiteral("Value"), QStringLiteral("second"));
        ComplexReportModel model;
        model.setDocument(document);
        const QModelIndex root = model.index(0, 0);
        const QModelIndex first = model.index(0, 0, root);
        const QModelIndex second = model.index(1, 0, root);
        const QString firstPath = first.data(ks::ui::ReportPathRole).toString();
        const QString secondPath = second.data(ks::ui::ReportPathRole).toString();
        expect(firstPath != secondPath, "same-name siblings have separate stable paths");
        expect(model.indexForPath(firstPath) == first && model.indexForPath(secondPath) == second, "path lookup finds each duplicate independently");
        expect(!model.indexForPath(QStringLiteral("/missing")).isValid(), "unknown path returns invalid index");
    }

    // 长值子行来自原完整值；阈值跨越是实际行内结构变化，普通长值变化仍保持索引。
    void longValueTransitions()
    {
        FieldDocument document;
        document.section(QStringLiteral("Long"));
        const QString longValue(81, QLatin1Char('x'));
        document.field(QStringLiteral("Payload"), longValue);
        ComplexReportModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        FieldDocument compact = document;
        compact.nodes[0].children[0].value = QString(80, QLatin1Char('x'));
        model.setDocument(compact);
        expect(model.rowCount(model.index(0, 0, model.index(0, 0))) == 0, "compact threshold has no synthetic expansion");
        model.setDocument(document);
        QModelIndex root = model.index(0, 0);
        QModelIndex payload = model.index(0, 0, root);
        const QString path = payload.data(ks::ui::ReportPathRole).toString();
        const QModelIndex detail = model.index(0, 0, payload);
        expect(model.rowCount(payload) == 1 && detail.data(ks::ui::ReportKindRole).toInt() == int(ReportNodeKind::ValueDetail), "long field exposes one actual inline detail row");
        expect(!payload.data(ks::ui::ReportInitiallyExpandedRole).toBool(), "long leaf is collapsed initially");
        expect(detail.data(ks::ui::ReportValueRole).toString() == longValue && model.textForIndex(detail) == longValue, "inline detail and copy contain complete original value");
        expect(model.textForIndex(root) == QStringLiteral("[Long]\nPayload: ") + longValue, "subtree copy contains long value exactly once");
        expect(model.textForIndex(payload) == QStringLiteral("Payload: ") + longValue, "field copy excludes synthetic duplicate");
        expect(payload.data(ks::ui::ReportCountRole).toInt() == 0, "synthetic detail is excluded from business count");
        resets.clear();
        QPersistentModelIndex persistent(payload);
        document.nodes[0].children[0].value = longValue + QLatin1Char('y');
        expect(!model.setDocument(document) && resets.isEmpty() && persistent.isValid(), "long-to-long value update preserves persistent identity");
        document.nodes[0].children[0].value = QStringLiteral("short");
        expect(model.setDocument(document) && resets.size() == 1, "long-to-short change emits actual structure reset");
        payload = model.indexForPath(path);
        expect(payload.isValid() && model.rowCount(payload) == 0, "canonical field path survives removal of synthetic row");
        resets.clear();
        document.nodes[0].children[0].value = QStringLiteral("first\nsecond");
        expect(model.setDocument(document) && resets.size() == 1, "multiline short text also has inline detail structure");
        payload = model.indexForPath(path);
        expect(model.rowCount(payload) == 1, "multiline value has one detail row");
        document.nodes[0].children[0].value = QStringLiteral("first\nsecond updated");
        resets.clear();
        expect(!model.setDocument(document) && resets.isEmpty(), "ordinary multiline update does not reset");
        FieldDocument note;
        note.section(QStringLiteral("Note"));
        note.note(longValue);
        model.setDocument(note);
        const QModelIndex noteIndex = model.index(0, 0, model.index(0, 0));
        expect(noteIndex.data(ks::ui::ReportKindRole).toInt() == int(ReportNodeKind::Note)
            && model.rowCount(noteIndex) == 1, "long explanatory note supports inline expansion too");
    }

    // 期待值独立写明既有公开格式，覆盖根字段、分区、嵌套字段与说明，不从模型反推。
    void canonicalCopy()
    {
        FieldDocument document;
        document.title = QStringLiteral("Audit");
        document.field(QStringLiteral("Loose"), QStringLiteral("raw"));
        document.note(QStringLiteral("note-before-section"));
        document.section(QStringLiteral("Runtime"));
        document.field(QStringLiteral("Status"), QStringLiteral("OK"));
        FieldNode branch;
        branch.name = QStringLiteral("Branch");
        branch.value = QStringLiteral("parent");
        FieldNode leaf;
        leaf.name = QStringLiteral("Leaf");
        leaf.value = QStringLiteral("child");
        branch.children.append(leaf);
        FieldDocument nested;
        nested.section(QStringLiteral("Nested"));
        nested.field(QStringLiteral("NestedValue"), QStringLiteral("answer"));
        nested.note(QStringLiteral("nested-note"));
        branch.children += nested.nodes;
        document.nodes.last().children.append(branch);
        const QString expected = QStringLiteral("Audit\nLoose: raw\nnote-before-section\n[Runtime]\nStatus: OK\nBranch: parent\n  Leaf: child\n  [Nested]\n  NestedValue: answer\n  nested-note");
        expect(document.toPlainText(false) == expected, "canonical serialization retains existing public plain-text format");
        ComplexReportModel model;
        model.setDocument(document);
        expect(model.textForIndex(QModelIndex()) == expected, "copy all uses full canonical document with root fields");
        expect(model.document().toPlainText(false) == expected, "model snapshot does not alter canonical serialization");
    }

    // 构造指定数量的真实树节点，用来记录模型提交/定位成本，不把耗时解释为 GUI 或硬件验收。
    FieldDocument largeDocument(int nodeCount)
    {
        FieldDocument document;
        document.section(QStringLiteral("Large Report"));
        int remaining = nodeCount - 1; // 根节点已经计入总量。
        for (int groupIndex = 0; remaining > 0; ++groupIndex)
        {
            FieldDocument group;
            const QString type = QStringLiteral("Type%1").arg(groupIndex);
            group.section(type);
            group.nodes.first().initiallyExpanded = false;
            --remaining;
            const int fields = std::min(remaining, 63); // 保持多层结构，而非一个平面数组。
            for (int fieldIndex = 0; fieldIndex < fields; ++fieldIndex)
            {
                group.field(type + QStringLiteral(".Field%1").arg(fieldIndex), QStringLiteral("0x10"));
            }
            remaining -= fields;
            document.nodes.first().children += group.nodes;
        }
        return document;
    }

    // 搜集模型公开路径作为定位输入；不访问 State、不复刻 preorder 编号算法。
    void collectPaths(const ComplexReportModel& model, const QModelIndex& parent, QStringList& paths)
    {
        for (int row = 0; row < model.rowCount(parent); ++row)
        {
            const QModelIndex index = model.index(row, 0, parent);
            paths.append(index.data(ks::ui::ReportPathRole).toString());
            collectPaths(model, index, paths);
        }
    }

    void recordScale(int nodeCount)
    {
        const FieldDocument document = largeDocument(nodeCount);
        ComplexReportModel model;
        QElapsedTimer timer;
        timer.start();
        const bool reset = model.setDocument(document);
        const qint64 submitNs = timer.nsecsElapsed(); // 仅包含生产 setDocument，不包含测试树生成。
        QStringList paths;
        collectPaths(model, {}, paths);
        expect(reset && paths.size() == nodeCount, "large snapshot preserves every canonical node");
        timer.restart();
        int found = 0;
        for (const QString& path : paths)
        {
            const QModelIndex index = model.indexForPath(path);
            if (index.isValid() && index.data(ks::ui::ReportPathRole).toString() == path)
            {
                ++found;
            }
        }
        const qint64 lookupNs = timer.nsecsElapsed(); // 对全部节点做一次公开路径定位。
        expect(found == nodeCount, "all large-snapshot paths resolve to the correct node");
        std::printf("COMPLEX_REPORT_MODEL_SCALE nodes=%d submit_ms=%.3f lookup_ms=%.3f found=%d\n",
            nodeCount, double(submitNs) / 1000000.0, double(lookupNs) / 1000000.0, found);
    }
}

namespace
{
    // 新的直接浏览必须能独立复制列值，隐藏重复属性，并在空匹配时仍严格保持选择的范围。
    void compactBrowsingContract()
    {
        FieldDocument document = catalogDocument();
        auto& catalog = document.nodes[0];
        catalog.reportRole = FieldNode::ReportRole::Catalog;
        catalog.summaryLabel = QStringLiteral("进程结构");
        catalog.preferredScope = true;
        catalog.children[0].reportRole = FieldNode::ReportRole::Metadata;
        auto& type = catalog.children[1];
        type.reportRole = FieldNode::ReportRole::Collection;
        type.reportedCount = 1;
        type.children[0].reportRole = FieldNode::ReportRole::Metadata;
        auto& field = type.children[1];
        field.reportRole = FieldNode::ReportRole::Record;
        for (auto& property : field.children) property.reportRole = FieldNode::ReportRole::Metadata;
        FieldNode alias;
        alias.name = QStringLiteral("DynData");
        alias.value = QStringLiteral("PebFlags");
        field.children.append(alias);
        FieldNode other = catalog;
        other.name = QStringLiteral("OtherCatalog");
        other.summaryLabel = QStringLiteral("线程结构");
        other.preferredScope = false;
        other.children[1].children[1].name = QStringLiteral("_PEB.SecondOnly");
        document.nodes.append(other);
        document.section(QStringLiteral("Runtime"));
        document.nodes.last().status = FieldNode::Status::Warning;
        document.note(QStringLiteral("Driver unavailable"));

        ComplexReportModel model;
        QAbstractItemModelTester modelTester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
        model.setDocument(document);
        const QModelIndex root = model.index(0, 0);
        const QModelIndex typeIndex = model.index(1, 0, root);
        const QModelIndex fieldIndex = model.index(1, 0, typeIndex);
        expect(fieldIndex.data().toString() == QStringLiteral("Flag"), "compact name omits the repeated type qualifier");
        expect(fieldIndex.siblingAtColumn(1).data().toString() == QStringLiteral("0x10"), "offset is an independently copyable real column");
        expect(fieldIndex.siblingAtColumn(2).data().toString() == QStringLiteral("uint32"), "type is an independently copyable real column");
        expect(fieldIndex.siblingAtColumn(1).data(Qt::AccessibleTextRole).toString() == QStringLiteral("0x10"),
            "accessibility reads the selected column instead of repeating the full row");
        expect(typeIndex.data(ks::ui::ReportReportedCountRole).toInt() == 1, "type count is supplied field count, not property count");
        expect(root.data(ks::ui::ReportPreferredScopeRole).toBool(), "producer specifies the default scope");
        expect(model.rowCount(fieldIndex.siblingAtColumn(1)) == 0, "only the name column owns child properties");

        ks::ui::ComplexReportFilter proxy;
        proxy.setSourceModel(&model);
        QAbstractItemModelTester proxyTester(&proxy, QAbstractItemModelTester::FailureReportingMode::Fatal);
        const QString scope = root.data(ks::ui::ReportPathRole).toString();
        proxy.setScopePath(scope);
        expect(proxy.rowCount() == 1, "scope selection removes unrelated catalogs and diagnostics");
        const QModelIndex visibleRoot = proxy.mapFromSource(root);
        const QModelIndex visibleType = proxy.mapFromSource(typeIndex);
        const QModelIndex visibleField = proxy.mapFromSource(fieldIndex);
        expect(proxy.rowCount(visibleRoot) == 1 && proxy.rowCount(visibleType) == 1, "metadata does not consume browsing rows");
        expect(proxy.rowCount(visibleField) == 1 && proxy.index(0, 0, visibleField).data().toString() == QStringLiteral("DynData"),
            "expanded record only shows its additional property");
        expect(model.textForIndex(fieldIndex).contains(QStringLiteral("Offset: 0x10"))
            && model.textForIndex(fieldIndex).contains(QStringLiteral("Type: uint32")), "canonical original retains the projected metadata");
        proxy.setQuery(QStringLiteral("pEb 0x10"));
        expect(proxy.mapFromSource(fieldIndex).isValid(), "multiple search terms match name context and offset without a qualified path");
        proxy.setQuery(QStringLiteral("进程结构 0x10"));
        expect(proxy.mapFromSource(fieldIndex).isValid(), "displayed scope label participates in the same canonical search context");
        proxy.setQuery(QStringLiteral("SecondOnly"));
        expect(proxy.rowCount() == 0, "empty scoped search never falls back to another matching catalog");
        proxy.setScopePath(QString());
        expect(proxy.rowCount() == 1 && proxy.index(0, 0).data().toString() == QStringLiteral("线程结构"),
            "all-report search deliberately reaches the other catalog");
        proxy.setQuery(QString());
        proxy.setDiagnosticsOnly(true);
        expect(proxy.rowCount() == 1 && proxy.index(0, 0).data().toString() == QStringLiteral("Runtime"),
            "diagnostic navigation excludes entire catalog subtrees");
        QPersistentModelIndex offsetCell(fieldIndex.siblingAtColumn(1));
        document.nodes[0].children[1].children[1].children[0].value = QStringLiteral("0x18");
        expect(!model.setDocument(document) && offsetCell.isValid() && offsetCell.data().toString() == QStringLiteral("0x18"),
            "value refresh preserves the independent offset cell identity");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv); // 无 QApplication、窗口、离屏平台或 QWidget 对象。
    hierarchyAndUpdates();
    recursiveProxyScope();
    identityAndSampleSections();
    duplicateNames();
    longValueTransitions();
    canonicalCopy();
    compactBrowsingContract();
    recordScale(15000);
    recordScale(30000);
    std::printf("COMPLEX_REPORT_MODEL_RESULT checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
