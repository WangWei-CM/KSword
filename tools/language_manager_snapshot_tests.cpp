#include "LanguageManager.h"

#include <QCoreApplication>
#include <QEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    QJsonArray failures;
    int checks = 0;
#ifdef KSWORD_I18N_TEST_RUNTIME_POLICY
    std::vector<QObject*> runtimeVisited;
#endif

#ifdef KSWORD_I18N_TEST_GENERATION_HOOK
    std::mutex generationHookMutex;
    std::condition_variable generationHookCondition;
    bool generationHookArmed = false;
    bool generationHookReached = false;
    bool generationHookReleased = false;
#endif

    void requireEqual(const QString& id, const QString& actual, const QString& expected)
    {
        ++checks;
        if (actual != expected)
        {
            failures.append(QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("expected"), expected},
                {QStringLiteral("actual"), actual}});
        }
    }

    void requireTrue(const QString& id, const bool actual)
    {
        ++checks;
        if (!actual)
        {
            failures.append(QJsonObject{{QStringLiteral("id"), id}});
        }
    }

    bool writePack(const QString& id, const QString& fallback, const QJsonObject& sources)
    {
        const QJsonObject pack{
            {QStringLiteral("schema"), QStringLiteral("ksword-language-pack")},
            {QStringLiteral("format_version"), 1},
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), id},
            {QStringLiteral("native_name"), id},
            {QStringLiteral("author"), QStringLiteral("logic regression fixture")},
            {QStringLiteral("text_direction"), QStringLiteral("ltr")},
            {QStringLiteral("fallback"), fallback},
            {QStringLiteral("translations"), QJsonObject{
                {QStringLiteral("fixture.sentinel"), QStringLiteral("sentinel")}}},
            {QStringLiteral("source_translations"), sources}};
        QFile file(QCoreApplication::applicationDirPath() + QStringLiteral("/languages/") + id + QStringLiteral(".json"));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            return false;
        }
        const QByteArray bytes = QJsonDocument(pack).toJson();
        return file.write(bytes) == bytes.size();
    }

    void setupPacks()
    {
        requireTrue(QStringLiteral("mkdir fixture languages"),
            QDir().mkpath(QCoreApplication::applicationDirPath() + QStringLiteral("/languages")));
        requireTrue(QStringLiteral("write English fixture"), writePack(QStringLiteral("en-US"), QStringLiteral("zh-CN"), {
            {QStringLiteral("直接命中"), QStringLiteral("exact-hit")},
            {QStringLiteral("空译文"), QString()},
            {QStringLiteral("安全模式"), QStringLiteral("Safe mode")},
            {QStringLiteral("状态：%1"), QStringLiteral("Status: %1")},
            {QStringLiteral("重复：%1；再次：%1"), QStringLiteral("repeat:%1|%1")},
            {QStringLiteral("三值：%L1 / %n / {3}"), QStringLiteral("A=%L1,N=%n,C={3}")},
            {QStringLiteral("泛：%1"), QStringLiteral("general[%1]")},
            {QStringLiteral("泛：固定：%1"), QStringLiteral("specific[%1]")},
            {QStringLiteral("%1中文尾"), QStringLiteral("leading[%1]")},
            {QStringLiteral("前%1中文尾"), QStringLiteral("prefixed[%1]")},
            {QStringLiteral("%1并甲乙丙"), QStringLiteral("leading-specific[%1]")},
            {QStringLiteral("前%1丙"), QStringLiteral("prefix-general[%1]")},
            {QStringLiteral("并%1甲"), QStringLiteral("tie-first[%1]")},
            {QStringLiteral("并%2甲"), QStringLiteral("tie-second[%2]")},
            {QStringLiteral("相邻%1%2结束"), QStringLiteral("first=%1;second=%2")},
            {QStringLiteral("换行%1"), QStringLiteral("line[%1]")},
            {QStringLiteral("末行后%1尾"), QStringLiteral("suffix[%1]")},
            {QStringLiteral("换行实%1\n"), QStringLiteral("literal-newline[%1]")},
            {QStringLiteral("缺%1"), QStringLiteral("bad:%2")},
            {QStringLiteral("AB%1"), QStringLiteral("non-Han[%1]")},
            {QStringLiteral("换代词"), QStringLiteral("generation-English")},
            {QStringLiteral("重名甲"), QStringLiteral("ambiguous-only")},
            {QStringLiteral("重名乙"), QStringLiteral("ambiguous-only")},
            {QStringLiteral("回退重名甲"), QStringLiteral("ambiguous-fallback")},
            {QStringLiteral("回退重名乙"), QStringLiteral("ambiguous-fallback")},
            {QStringLiteral("回退反查"), QStringLiteral("fallback-retranslated")},
            {QStringLiteral("显示模板：%1"), QStringLiteral("Rendered %1")},
            {QStringLiteral("identity"), QStringLiteral("identity")}}));
        requireTrue(QStringLiteral("write French fixture"), writePack(QStringLiteral("fr-FR"), QStringLiteral("en-US"), {
            {QStringLiteral("直接命中"), QStringLiteral("French-exact")},
            {QStringLiteral("法语独有"), QStringLiteral("French-only")},
            {QStringLiteral("换代词"), QStringLiteral("generation-French")},
            {QStringLiteral("显示模板：%1"), QStringLiteral("French rendered %1")}}));
        requireTrue(QStringLiteral("write Chinese fixture"), writePack(QStringLiteral("zh-CN"), QString(), {
            {QStringLiteral("直接命中"), QStringLiteral("包内中文")},
            {QStringLiteral("English canonical"), QStringLiteral("中文强制")},
            {QStringLiteral("仅中文包"), QStringLiteral("Chinese-pack-only")},
            {QStringLiteral("状态：%1"), QStringLiteral("包状态：%1")},
            {QStringLiteral("安全模式"), QStringLiteral("包内安全模式")},
            {QStringLiteral("缺%1"), QStringLiteral("fallback:%1")},
            {QStringLiteral("回退反查"), QStringLiteral("ambiguous-fallback")}}));
        requireTrue(QStringLiteral("write cycle A fixture"), writePack(QStringLiteral("aa-AA"), QStringLiteral("bb-BB"), {
            {QStringLiteral("甲侧命中"), QStringLiteral("cycle-A")}}));
        requireTrue(QStringLiteral("write cycle B fixture"), writePack(QStringLiteral("bb-BB"), QStringLiteral("aa-AA"), {
            {QStringLiteral("环链词"), QStringLiteral("cycle-B")}}));
        requireTrue(QStringLiteral("write failed-chain root fixture"), writePack(QStringLiteral("cc-CC"), QStringLiteral("dd-DD"), {}));
        requireTrue(QStringLiteral("write later-failed fallback metadata fixture"), writePack(QStringLiteral("dd-DD"), QStringLiteral("zh-CN"), {
            {QStringLiteral("失败回退"),QStringLiteral("unused-before-corruption")}}));
        requireTrue(QStringLiteral("write concurrent reverse root fixture"), writePack(QStringLiteral("tt-TT"), QStringLiteral("uu-UU"), {
            {QStringLiteral("并发反查"),QStringLiteral("Thread-root")}}));
        requireTrue(QStringLiteral("write concurrent reverse fallback fixture"), writePack(QStringLiteral("uu-UU"), QString(), {
            {QStringLiteral("并发反查"),QStringLiteral("Thread-rendered")}}));
    }

    template <typename Translator>
    void runSourceCases(const Translator& manager)
    {
        const auto source = [&manager](const QString& text) { return manager.sourceText(text); };
        for (int pass = 0; pass != 3; ++pass)
        {
            const QString suffix = QStringLiteral(" pass %1").arg(pass);
            requireEqual(QStringLiteral("exact") + suffix, source(QStringLiteral("直接命中")), QStringLiteral("exact-hit"));
            requireEqual(QStringLiteral("exact empty value") + suffix, source(QStringLiteral("空译文")), QString());
            requireEqual(QStringLiteral("empty input") + suffix, source(QString()), QString());
            requireEqual(QStringLiteral("unknown Han miss") + suffix, source(QStringLiteral("完全未收录的汉字内容")), QStringLiteral("完全未收录的汉字内容"));
            requireEqual(QStringLiteral("unknown Latin miss") + suffix, source(QStringLiteral("unregistered text")), QStringLiteral("unregistered text"));
            requireEqual(QStringLiteral("nested phrase") + suffix, source(QStringLiteral("状态：安全模式")), QStringLiteral("Status: Safe mode"));
            requireEqual(QStringLiteral("nested template") + suffix, source(QStringLiteral("状态：状态：安全模式")), QStringLiteral("Status: Status: Safe mode"));
            requireEqual(QStringLiteral("first capture for repeated placeholder") + suffix, source(QStringLiteral("重复：甲；再次：乙")), QStringLiteral("repeat:甲|甲"));
            requireEqual(QStringLiteral("placeholder syntaxes") + suffix, source(QStringLiteral("三值：1,234 / 7 / tail")), QStringLiteral("A=1,234,N=7,C=tail"));
            requireEqual(QStringLiteral("specific literal wins") + suffix, source(QStringLiteral("泛：固定：value")), QStringLiteral("specific[value]"));
            requireEqual(QStringLiteral("prefixed candidate beats general bucket") + suffix, source(QStringLiteral("前x中文尾")), QStringLiteral("prefixed[x]"));
            requireEqual(QStringLiteral("general bucket beats prefixed candidate") + suffix, source(QStringLiteral("前x并甲乙丙")), QStringLiteral("leading-specific[前x]"));
            requireEqual(QStringLiteral("prefixless candidate") + suffix, source(QStringLiteral("x中文尾")), QStringLiteral("leading[x]"));
            requireEqual(QStringLiteral("equal specificity follows original sorted order") + suffix, source(QStringLiteral("并x甲")), QStringLiteral("tie-first[x]"));
            requireEqual(QStringLiteral("adjacent placeholder stays lazy") + suffix, source(QStringLiteral("相邻x结束")), QStringLiteral("first=;second=x"));
            requireEqual(QStringLiteral("end-anchor newline with empty suffix") + suffix, source(QStringLiteral("换行abc\n")), QStringLiteral("line[abc]"));
            // Preserve the existing suffix prefilter: a terminal newline does not
            // satisfy a non-newline suffix, even though the matcher supports $.
            requireEqual(QStringLiteral("existing suffix newline prefilter") + suffix, source(QStringLiteral("末行后abc尾\n")), QStringLiteral("末行后abc尾\n"));
            requireEqual(QStringLiteral("literal newline suffix") + suffix, source(QStringLiteral("换行实abc\n")), QStringLiteral("literal-newline[abc]"));
            requireEqual(QStringLiteral("template output unknown placeholder falls back") + suffix, source(QStringLiteral("缺abc")), QStringLiteral("缺abc"));
            requireEqual(QStringLiteral("packed invalid output placeholder fallback") + suffix, manager.packedSourceText(QStringLiteral("缺abc")), QStringLiteral("fallback:abc"));
            requireEqual(QStringLiteral("non-Han templates are not registered") + suffix, source(QStringLiteral("ABvalue")), QStringLiteral("ABvalue"));
            requireEqual(QStringLiteral("normal stops at historical source fallback") + suffix, source(QStringLiteral("仅中文包")), QStringLiteral("仅中文包"));
            requireEqual(QStringLiteral("packed continues through Chinese fallback") + suffix, manager.packedSourceText(QStringLiteral("仅中文包")), QStringLiteral("Chinese-pack-only"));
            requireEqual(QStringLiteral("packed exact fallback") + suffix, manager.packedSourceText(QStringLiteral("English canonical")), QStringLiteral("中文强制"));
        }
    }

    void runEvictionCases(ks::i18n::LanguageManager& manager)
    {
        // More than 1 MiB of distinct short identities forces eviction without
        // depending on internal cache members or exposing test-only APIs.
        for (int index = 0; index != 12000; ++index)
        {
            const QString text = QStringLiteral("缓存淘汰身份_%1").arg(index);
            requireEqual(QStringLiteral("eviction normal %1").arg(index), manager.sourceText(text), text);
            requireEqual(QStringLiteral("eviction packed %1").arg(index), manager.packedSourceText(text), text);
        }
        const QString longText = QStringLiteral("超长身份:") + QString(5000, QChar(u'汉'));
        for (int pass = 0; pass != 8; ++pass)
        {
            requireEqual(QStringLiteral("overlong bypass %1").arg(pass), manager.sourceText(longText), longText);
        }
        requireEqual(QStringLiteral("exact result after eviction"), manager.sourceText(QStringLiteral("直接命中")), QStringLiteral("exact-hit"));
        requireEqual(QStringLiteral("parsed views stable after eviction"), manager.sourceText(QStringLiteral("状态：安全模式")), QStringLiteral("Status: Safe mode"));
    }

    void runConcurrencyCases(ks::i18n::LanguageManager& manager)
    {
        // Language changes remain on the owner thread. This verifies immutable
        // parsed templates and cache synchronization for simultaneous readers.
        std::atomic<int> mismatches{0};
        std::vector<std::thread> threads;
        constexpr int threadCount = 8;
        constexpr int repeats = 4000;
        for (int threadIndex = 0; threadIndex != threadCount; ++threadIndex)
        {
            threads.emplace_back([&manager, &mismatches, threadIndex]() {
                for (int pass = 0; pass != repeats; ++pass)
                {
                    const QString distinct = QStringLiteral("状态：thread-%1-%2").arg(threadIndex).arg(pass % 13);
                    const QString expected = QStringLiteral("Status: thread-%1-%2").arg(threadIndex).arg(pass % 13);
                    if (manager.sourceText(QStringLiteral("直接命中")) != QStringLiteral("exact-hit")
                        || manager.sourceText(QStringLiteral("状态：安全模式")) != QStringLiteral("Status: Safe mode")
                        || manager.sourceText(distinct) != expected
                        || manager.sourceText(QStringLiteral("重复：甲；再次：乙")) != QStringLiteral("repeat:甲|甲")
                        || manager.sourceText(QStringLiteral("并x甲")) != QStringLiteral("tie-first[x]")
                        || manager.sourceText(QStringLiteral("完全未收录的汉字内容")) != QStringLiteral("完全未收录的汉字内容")
                        || manager.packedSourceText(QStringLiteral("English canonical")) != QStringLiteral("中文强制"))
                    {
                        ++mismatches;
                    }
                }
            });
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }
        checks += threadCount * repeats * 7;
        requireTrue(QStringLiteral("concurrent readers return stable results"), mismatches.load() == 0);
    }

    void runBorrowedInputCases(ks::i18n::LanguageManager& manager)
    {
        QString error;
        for (int pass = 1; pass <= 256; ++pass)
        {
            requireTrue(QStringLiteral("reset borrowed-buffer generation %1").arg(pass),
                manager.setLanguage(QStringLiteral("en-US"), &error));
            const QString initial = QStringLiteral("状态：00000");
            const QString changed = QStringLiteral("状态：%1").arg(pass, 5, 10, QChar(u'0'));
            std::vector<QChar> raw(initial.cbegin(), initial.cend());
            const QString borrowed = QString::fromRawData(raw.data(), static_cast<qsizetype>(raw.size()));
            requireEqual(QStringLiteral("borrowed input initial %1").arg(pass),
                manager.sourceText(borrowed), QStringLiteral("Status: 00000"));
            std::copy(changed.cbegin(), changed.cend(), raw.begin());
            // The raw-data QString is allowed to reflect its owner's changes.
            // A result cache must own its key: retaining this borrowed buffer
            // can make the new key retrieve the previous translated value.
            requireEqual(QStringLiteral("borrowed input key remains immutable %1").arg(pass),
                manager.sourceText(QString(raw.data(), static_cast<qsizetype>(raw.size()))),
                QStringLiteral("Status: %1").arg(pass, 5, 10, QChar(u'0')));
        }
    }

#ifdef KSWORD_I18N_TEST_GENERATION_HOOK
    void runGenerationRaceCase(ks::i18n::LanguageManager& manager)
    {
        QString error;
        requireTrue(QStringLiteral("reset language before in-flight generation case"),
            manager.setLanguage(QStringLiteral("en-US"), &error));
        {
            std::lock_guard<std::mutex> lock(generationHookMutex);
            generationHookArmed = true;
            generationHookReached = false;
            generationHookReleased = false;
        }
        QString oldResult;
        std::thread worker([&manager, &oldResult]() {
            oldResult = manager.sourceText(QStringLiteral("换代词"));
        });
        bool reached = false;
        {
            std::unique_lock<std::mutex> lock(generationHookMutex);
            reached = generationHookCondition.wait_for(lock, std::chrono::seconds(5), []() {
                return generationHookReached;
            });
        }
        requireTrue(QStringLiteral("old query paused after resolving its snapshot"), reached);
        requireTrue(QStringLiteral("switch language while old query is in flight"),
            manager.setLanguage(QStringLiteral("fr-FR"), &error));
        {
            std::lock_guard<std::mutex> lock(generationHookMutex);
            generationHookReleased = true;
        }
        generationHookCondition.notify_all();
        worker.join();
        requireEqual(QStringLiteral("in-flight caller may retain its original language snapshot"),
            oldResult, QStringLiteral("generation-English"));
        requireEqual(QStringLiteral("old generation cannot populate new language cache"),
            manager.sourceText(QStringLiteral("换代词")), QStringLiteral("generation-French"));
        requireTrue(QStringLiteral("return English after generation race"),
            manager.setLanguage(QStringLiteral("en-US"), &error));
    }
#endif


    void runSnapshotCases(ks::i18n::LanguageManager& manager)
    {
        const ks::i18n::SourceTextSnapshot empty;
        requireEqual(QStringLiteral("default snapshot passes source through"), empty.sourceText(QStringLiteral("安全模式")), QStringLiteral("安全模式"));
        requireEqual(QStringLiteral("default snapshot passes packed source through"), empty.packedSourceText(QStringLiteral("安全模式")), QStringLiteral("安全模式"));
        requireTrue(QStringLiteral("default snapshot has no selected language"), empty.languageId().isEmpty());

        QString error;
        requireTrue(QStringLiteral("snapshot starts in English"), manager.setLanguage(QStringLiteral("en-US"), &error));
        runSourceCases(manager.sourceTextSnapshot());
        const QStringList samples{
            QString(), QStringLiteral("直接命中"), QStringLiteral("空译文"), QStringLiteral("安全模式"),
            QStringLiteral("状态：安全模式"), QStringLiteral("状态：状态：安全模式"),
            QStringLiteral("重复：甲；再次：乙"), QStringLiteral("三值：1,234 / 7 / tail"),
            QStringLiteral("泛：固定：value"), QStringLiteral("前x中文尾"), QStringLiteral("前x并甲乙丙"),
            QStringLiteral("x中文尾"), QStringLiteral("并x甲"), QStringLiteral("相邻x结束"),
            QStringLiteral("换行abc\n"), QStringLiteral("末行后abc尾\n"), QStringLiteral("换行实abc\n"),
            QStringLiteral("缺abc"), QStringLiteral("ABvalue"), QStringLiteral("仅中文包"),
            QStringLiteral("English canonical"), QStringLiteral("法语独有"), QStringLiteral("换代词"),
            QStringLiteral("环链词"), QStringLiteral("循环中缺项"), QStringLiteral("完全未收录的汉字内容"),
            QStringLiteral("长输入") + QString(5000, QChar(u'汉'))};
        const QStringList languages{QStringLiteral("en-US"), QStringLiteral("fr-FR"),
            QStringLiteral("zh-CN"), QStringLiteral("aa-AA")};
        struct ExpectedSnapshot
        {
            ks::i18n::SourceTextSnapshot translator;
            QStringList normal;
            QStringList packed;
        };
        std::vector<ExpectedSnapshot> snapshots;
        for (const QString& language : languages)
        {
            requireTrue(QStringLiteral("activate snapshot language ") + language, manager.setLanguage(language, &error));
            ExpectedSnapshot captured{manager.sourceTextSnapshot(), {}, {}};
            requireEqual(QStringLiteral("snapshot identity ") + language, captured.translator.languageId(), manager.currentLanguageId());
            for (const QString& text : samples)
            {
                captured.normal.append(manager.sourceText(text));
                captured.packed.append(manager.packedSourceText(text));
                requireEqual(QStringLiteral("snapshot normal equivalence ") + language + text,
                    captured.translator.sourceText(text), captured.normal.back());
                requireEqual(QStringLiteral("snapshot packed equivalence ") + language + text,
                    captured.translator.packedSourceText(text), captured.packed.back());
            }
            snapshots.push_back(std::move(captured));
        }

        // Workers retain only frozen values. The owner repeatedly switches the
        // live manager while each worker resolves its original language chain.
        std::atomic<int> mismatches{0};
        std::vector<std::thread> threads;
        constexpr int repeats = 100;
        for (const ExpectedSnapshot& captured : snapshots)
        {
            threads.emplace_back([captured, samples, &mismatches]() {
                for (int pass = 0; pass != repeats; ++pass)
                {
                    for (qsizetype index = 0; index != samples.size(); ++index)
                    {
                        if (captured.translator.sourceText(samples[index]) != captured.normal[index]
                            || captured.translator.packedSourceText(samples[index]) != captured.packed[index])
                        {
                            ++mismatches;
                        }
                    }
                }
            });
        }
        for (int pass = 0; pass != 32; ++pass)
        {
            requireTrue(QStringLiteral("live language activation during frozen worker %1").arg(pass),
                manager.setLanguage(languages[pass % languages.size()], &error));
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }
        checks += static_cast<int>(snapshots.size()) * repeats * static_cast<int>(samples.size()) * 2;
        requireTrue(QStringLiteral("frozen workers ignore subsequent language activations"), mismatches.load() == 0);

        // A raw-data query/result must not be retained by the immutable value.
        const auto english = snapshots.front().translator;
        const QString initial = QStringLiteral("状态：00000");
        std::vector<QChar> raw(initial.cbegin(), initial.cend());
        const QString borrowed = QString::fromRawData(raw.data(), static_cast<qsizetype>(raw.size()));
        const QString held = english.sourceText(borrowed);
        const QString changed = QStringLiteral("状态：00001");
        std::copy(changed.cbegin(), changed.cend(), raw.begin());
        requireEqual(QStringLiteral("snapshot previous translated result owns captures"), held, QStringLiteral("Status: 00000"));
        requireEqual(QStringLiteral("snapshot observes a new borrowed value"), english.sourceText(borrowed), QStringLiteral("Status: 00001"));
        requireTrue(QStringLiteral("return English after snapshot cases"), manager.setLanguage(QStringLiteral("en-US"), &error));
    }


#ifdef KSWORD_I18N_TEST_RUNTIME_POLICY
    void runRuntimePolicyCases(ks::i18n::LanguageManager& manager)
    {
        QObject root;
        QObject unknown(&root);
        QObject managed(&root);
        QObject leaf(&managed);
        QObject nested(&managed);
        QObject nestedLeaf(&nested);
        manager.setRuntimeTranslationManaged(&managed);
        manager.setRuntimeTranslationManaged(&nested);
        const auto drain = []() { QCoreApplication::processEvents(); };
        runtimeVisited.clear();
        manager.markRuntimeTranslationDirty(&root);
        drain();
        requireTrue(QStringLiteral("unknown compatibility subtree is still visited"),
            std::find(runtimeVisited.cbegin(), runtimeVisited.cend(), &unknown) != runtimeVisited.cend());
        requireTrue(QStringLiteral("clean explicit subtree stops ancestor traversal"),
            std::find(runtimeVisited.cbegin(), runtimeVisited.cend(), &managed) == runtimeVisited.cend());
        runtimeVisited.clear();
        manager.markRuntimeTranslationDirty(&root);
        manager.markRuntimeTranslationDirty(&leaf);
        drain();
        requireTrue(QStringLiteral("ancestor queue does not swallow dirty leaf behind clean boundary"),
            std::count(runtimeVisited.cbegin(), runtimeVisited.cend(), &leaf) == 1);
        requireTrue(QStringLiteral("dirty leaf does not open its clean managed siblings"),
            std::find(runtimeVisited.cbegin(), runtimeVisited.cend(), &nestedLeaf) == runtimeVisited.cend());
        runtimeVisited.clear();
        manager.markRuntimeTranslationDirty(&managed);
        manager.markRuntimeTranslationDirty(&leaf);
        drain();
        requireTrue(QStringLiteral("dirty boundary consumes its own source subtree once"),
            std::count(runtimeVisited.cbegin(), runtimeVisited.cend(), &managed) == 1
            && std::count(runtimeVisited.cbegin(), runtimeVisited.cend(), &leaf) == 1);
        requireTrue(QStringLiteral("nested clean boundaries remain independent"),
            std::find(runtimeVisited.cbegin(), runtimeVisited.cend(), &nested) == runtimeVisited.cend());
        runtimeVisited.clear();
        manager.markRuntimeTranslationDirty(&root);
        drain();
        requireTrue(QStringLiteral("dirty boundary returns to clean after consumption"),
            std::find(runtimeVisited.cbegin(), runtimeVisited.cend(), &managed) == runtimeVisited.cend());
        manager.setRuntimeTranslationManaged(&managed, false);
        runtimeVisited.clear();
        manager.markRuntimeTranslationDirty(&root);
        drain();
        requireTrue(QStringLiteral("removing managed declaration restores compatibility"),
            std::find(runtimeVisited.cbegin(), runtimeVisited.cend(), &managed) != runtimeVisited.cend()
            && std::find(runtimeVisited.cbegin(), runtimeVisited.cend(), &leaf) != runtimeVisited.cend());
        auto* destroyed = new QObject;
        manager.markRuntimeTranslationDirty(destroyed);
        delete destroyed;
        drain();
        requireTrue(QStringLiteral("destroyed pending runtime root is safe"), true);

        // Reproduce the hidden-leaf dirty state without constructing QWidget:
        // root is queued, managed stays clean, and leaf retains a dirty bit.
        // Deliver the actual Show event through the real manager eventFilter
        // installed on this QObject. No GUI or offscreen widget is involved.
        manager.setRuntimeTranslationManaged(&managed);
        runtimeVisited.clear();
        manager.markRuntimeTranslationDirty(&root);
        leaf.setProperty("ks_i18n_runtime_dirty_subtree", true);
        leaf.installEventFilter(&manager);
        QEvent show(QEvent::Show);
        QCoreApplication::sendEvent(&leaf, &show);
        requireTrue(QStringLiteral("Show prepares dirty leaf below clean boundary before queued ancestor"),
            std::count(runtimeVisited.cbegin(), runtimeVisited.cend(), &leaf) == 1);
        requireTrue(QStringLiteral("Show preparation does not visit the blocked outer dirty root"),
            std::find(runtimeVisited.cbegin(), runtimeVisited.cend(), &root) == runtimeVisited.cend());
        requireTrue(QStringLiteral("Show consumes the reachable leaf dirty bit"),
            !leaf.property("ks_i18n_runtime_dirty_subtree").toBool());
        requireTrue(QStringLiteral("Show retains an unrelated outer dirty request"),
            root.property("ks_i18n_runtime_dirty_subtree").toBool());
        drain();
        requireTrue(QStringLiteral("later outer drain still stops at the clean boundary"),
            std::count(runtimeVisited.cbegin(), runtimeVisited.cend(), &leaf) == 1
            && std::find(runtimeVisited.cbegin(), runtimeVisited.cend(), &managed) == runtimeVisited.cend());
        leaf.removeEventFilter(&manager);
    }
#endif


    void runFrozenDisplayCases(ks::i18n::LanguageManager& manager)
    {
        QString error;
        const ks::i18n::SourceTextSnapshot empty;
        requireEqual(QStringLiteral("default display snapshot passes through"), empty.displayText(QStringLiteral("exact-hit")), QStringLiteral("exact-hit"));
        const QStringList samples{QString(), QStringLiteral("直接命中"), QStringLiteral("exact-hit"),
            QStringLiteral("French-exact"), QStringLiteral("Safe mode"), QStringLiteral("安全模式"),
            QStringLiteral("状态：安全模式"), QStringLiteral("Status: Safe mode"),
            QStringLiteral("Rendered %1"), QStringLiteral("显示模板：安全模式"),
            QStringLiteral("French rendered %1"), QStringLiteral("ambiguous-only"),
            QStringLiteral("ambiguous-fallback"), QStringLiteral("fallback-retranslated"),
            QStringLiteral("Chinese-pack-only"), QStringLiteral("cycle-B"), QStringLiteral("identity"),
            QStringLiteral("用户原始路径_安全模式"), QStringLiteral("unknown-English")};
        struct Expected { ks::i18n::SourceTextSnapshot value; QStringList rendered; };
        std::vector<Expected> frozen;
        const QStringList languages{QStringLiteral("en-US"),QStringLiteral("fr-FR"),QStringLiteral("zh-CN"),QStringLiteral("aa-AA")};
        for (const QString& language : languages)
        {
            requireTrue(QStringLiteral("activate display language ")+language, manager.setLanguage(language,&error));
            Expected captured{manager.sourceTextSnapshot(),{}};
            for (const QString& text : samples)
            {
                captured.rendered.append(manager.displayText(text));
                requireEqual(QStringLiteral("frozen display equivalence ")+language+text,
                    captured.value.displayText(text),captured.rendered.back());
            }
            frozen.push_back(std::move(captured));
        }
        requireEqual(QStringLiteral("French reverses fallback-rendered exact value"), frozen[1].value.displayText(QStringLiteral("exact-hit")),QStringLiteral("French-exact"));
        requireEqual(QStringLiteral("ambiguous rendering without later unique source stays raw"),frozen[1].value.displayText(QStringLiteral("ambiguous-only")),QStringLiteral("ambiguous-only"));
        requireEqual(QStringLiteral("ambiguity in one pack still permits unique fallback source"),frozen[1].value.displayText(QStringLiteral("ambiguous-fallback")),QStringLiteral("fallback-retranslated"));
        requireEqual(QStringLiteral("rendered template identity is retranslated before arguments"),frozen[1].value.displayText(QStringLiteral("Rendered %1")),QStringLiteral("French rendered %1"));
        requireEqual(QStringLiteral("source template retains nested capture rules"),frozen[1].value.displayText(QStringLiteral("显示模板：安全模式")),QStringLiteral("French rendered Safe mode"));
        requireEqual(QStringLiteral("historical Chinese unique reverse returns canonical source"),frozen[2].value.displayText(QStringLiteral("Chinese-pack-only")),QStringLiteral("仅中文包"));

        std::atomic<int> mismatches{0};
        std::vector<std::thread> readers;
        constexpr int repeats=100;
        for (const Expected& value : frozen)
            readers.emplace_back([value,samples,&mismatches]() {
                for (int pass=0;pass!=repeats;++pass)
                    for (qsizetype index=0;index!=samples.size();++index)
                        if (value.value.displayText(samples[index])!=value.rendered[index]) ++mismatches;
            });
        for (int pass=0;pass!=32;++pass)
            requireTrue(QStringLiteral("live activation during frozen display %1").arg(pass), manager.setLanguage(languages[pass%languages.size()],&error));
        for (auto& reader:readers) reader.join();
        checks+=static_cast<int>(frozen.size())*repeats*static_cast<int>(samples.size());
        requireTrue(QStringLiteral("display readers ignore later language state"),mismatches.load()==0);

        // No reverse index in either of these packs has been queried before.
        // All workers race the same std::call_once index initialization while
        // retaining only the immutable frozen translator value.
        requireTrue(QStringLiteral("activate cold concurrent reverse pack"),manager.setLanguage(QStringLiteral("tt-TT"),&error));
        const auto cold=manager.sourceTextSnapshot();
        readers.clear(); mismatches=0;
        for (int thread=0;thread!=8;++thread)
            readers.emplace_back([cold,&mismatches]() {
                for (int pass=0;pass!=1000;++pass)
                    if (cold.displayText(QStringLiteral("Thread-rendered"))!=QStringLiteral("Thread-root")) ++mismatches;
            });
        requireTrue(QStringLiteral("switch live state after cold reverse capture"),manager.setLanguage(QStringLiteral("en-US"),&error));
        for (auto& reader:readers) reader.join();
        checks+=8000;
        requireTrue(QStringLiteral("concurrent cold reverse index is stable"),mismatches.load()==0);

        // The metadata was discovered at startup, but this fallback body has
        // never loaded. Reverse lookup must continue through its failure while
        // normal source lookup keeps its original stop-at-failure semantics.
        QFile broken(QCoreApplication::applicationDirPath()+QStringLiteral("/languages/dd-DD.json"));
        requireTrue(QStringLiteral("open isolated fallback corruption fixture"),broken.open(QIODevice::WriteOnly|QIODevice::Truncate));
        requireTrue(QStringLiteral("write isolated fallback corruption fixture"),broken.write("invalid fixture pack")>0); broken.close();
        requireTrue(QStringLiteral("activate failed-middle chain root"),manager.setLanguage(QStringLiteral("cc-CC"),&error));
        const auto failedMiddle=manager.sourceTextSnapshot();
        requireEqual(QStringLiteral("live reverse continues through failed pack"),manager.displayText(QStringLiteral("Chinese-pack-only")),QStringLiteral("仅中文包"));
        requireEqual(QStringLiteral("frozen reverse retains metadata beyond failed pack"),failedMiddle.displayText(QStringLiteral("Chinese-pack-only")),QStringLiteral("仅中文包"));
        requireEqual(QStringLiteral("frozen source still stops at failed pack"),failedMiddle.sourceText(QStringLiteral("仅中文包")),QStringLiteral("仅中文包"));
        requireTrue(QStringLiteral("return English after frozen display cases"),manager.setLanguage(QStringLiteral("en-US"),&error));
    }

    int writeReport(const QString& output)
    {
        const QJsonObject result{
            {QStringLiteral("application_kind"), QStringLiteral("QCoreApplication; no QWidget or QApplication constructed")},
            {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
            {QStringLiteral("checks"), checks},
            {QStringLiteral("failures"), failures},
            {QStringLiteral("passed"), failures.isEmpty()}};
        QFile report(output);
        if (!report.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || report.write(QJsonDocument(result).toJson()) < 0)
        {
            std::cerr << "Could not write result.\n";
            return 2;
        }
        std::cout << "checks=" << checks << " failures=" << failures.size() << '\n';
        return failures.isEmpty() ? 0 : 1;
    }
}

#ifdef KSWORD_I18N_TEST_GENERATION_HOOK
void ksword_i18n_test_after_source_resolved()
{
    std::unique_lock<std::mutex> lock(generationHookMutex);
    if (!generationHookArmed)
    {
        return;
    }
    generationHookArmed = false;
    generationHookReached = true;
    generationHookCondition.notify_all();
    generationHookCondition.wait_for(lock, std::chrono::seconds(5), []() {
        return generationHookReleased;
    });
}
#endif

#ifdef KSWORD_I18N_TEST_RUNTIME_POLICY
void ksword_i18n_test_runtime_visited(QObject* object)
{
    runtimeVisited.push_back(object);
}
#endif

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const bool noPacks = app.arguments().size() == 3
        && app.arguments().at(1) == QStringLiteral("--no-packs");
    if (!noPacks && app.arguments().size() != 2)
    {
        std::cerr << "Pass a JSON result filename under the fixture executable directory.\n";
        return 2;
    }
    const QString output = app.arguments().at(noPacks ? 2 : 1);
    if (QDir::cleanPath(QFileInfo(output).absolutePath()) != QDir::cleanPath(QCoreApplication::applicationDirPath()))
    {
        std::cerr << "Result must remain under the dedicated fixture executable directory.\n";
        return 2;
    }
    if (noPacks)
    {
        ks::i18n::LanguageManager& manager = ks::i18n::LanguageManager::instance();
        QString error;
        requireTrue(QStringLiteral("no language pack was discovered"), manager.availableLanguages().isEmpty());
        requireTrue(QStringLiteral("initialize without packs returns false"), !manager.initialize(QStringLiteral("en-US"), &error));
        requireTrue(QStringLiteral("missing-pack initialization reports its status"), !error.isEmpty());
        requireEqual(QStringLiteral("missing packs retain historical Chinese language"), manager.currentLanguageId(), QStringLiteral("zh-CN"));
        requireEqual(QStringLiteral("missing packs normal fallback"), manager.sourceText(QStringLiteral("没有语言包")), QStringLiteral("没有语言包"));
        requireEqual(QStringLiteral("missing packs packed fallback"), manager.packedSourceText(QStringLiteral("没有语言包")), QStringLiteral("没有语言包"));
        requireEqual(QStringLiteral("missing packs empty fallback"), manager.sourceText(QString()), QString());
        requireTrue(QStringLiteral("initialize retry without packs returns false"), !manager.initialize(QStringLiteral("fr-FR"), &error));
        requireEqual(QStringLiteral("retry retains normal fallback"), manager.sourceText(QStringLiteral("retry text")), QStringLiteral("retry text"));
        requireEqual(QStringLiteral("retry retains packed fallback"), manager.packedSourceText(QStringLiteral("retry text")), QStringLiteral("retry text"));
        const auto noPackSnapshot = manager.sourceTextSnapshot();
        requireEqual(QStringLiteral("no packs frozen normal fallback"), noPackSnapshot.sourceText(QStringLiteral("没有语言包")), QStringLiteral("没有语言包"));
        requireEqual(QStringLiteral("no packs frozen packed fallback"), noPackSnapshot.packedSourceText(QStringLiteral("没有语言包")), QStringLiteral("没有语言包"));
        requireEqual(QStringLiteral("no packs frozen display fallback"),noPackSnapshot.displayText(QStringLiteral("没有语言包")),QStringLiteral("没有语言包"));
        return writeReport(output);
    }
    setupPacks();
    if (!failures.isEmpty())
    {
        std::cerr << "Fixture pack creation failed.\n";
        return 2;
    }

    ks::i18n::LanguageManager& manager = ks::i18n::LanguageManager::instance();
    QString error;
    requireTrue(QStringLiteral("initialize English"), manager.initialize(QStringLiteral("en-US"), &error));
    runSourceCases(manager);
    runSnapshotCases(manager);
    runFrozenDisplayCases(manager);
#ifdef KSWORD_I18N_TEST_RUNTIME_POLICY
    runRuntimePolicyCases(manager);
#endif
    runEvictionCases(manager);
    runConcurrencyCases(manager);
    runBorrowedInputCases(manager);
#ifdef KSWORD_I18N_TEST_GENERATION_HOOK
    runGenerationRaceCase(manager);
#endif

    requireEqual(QStringLiteral("cache negative before generation switch"), manager.sourceText(QStringLiteral("法语独有")), QStringLiteral("法语独有"));
    requireEqual(QStringLiteral("cache positive before generation switch"), manager.sourceText(QStringLiteral("换代词")), QStringLiteral("generation-English"));
    requireTrue(QStringLiteral("switch French"), manager.setLanguage(QStringLiteral("fr-FR"), &error));
    requireEqual(QStringLiteral("negative cache invalidated"), manager.sourceText(QStringLiteral("法语独有")), QStringLiteral("French-only"));
    requireEqual(QStringLiteral("positive cache invalidated"), manager.sourceText(QStringLiteral("换代词")), QStringLiteral("generation-French"));
    requireEqual(QStringLiteral("third-language exact fallback"), manager.sourceText(QStringLiteral("安全模式")), QStringLiteral("Safe mode"));
    requireEqual(QStringLiteral("third-language nested fallback"), manager.sourceText(QStringLiteral("状态：安全模式")), QStringLiteral("Status: Safe mode"));
    requireEqual(QStringLiteral("third-language normal historical fallback"), manager.sourceText(QStringLiteral("仅中文包")), QStringLiteral("仅中文包"));
    requireEqual(QStringLiteral("third-language packed historical fallback"), manager.packedSourceText(QStringLiteral("仅中文包")), QStringLiteral("Chinese-pack-only"));

    requireTrue(QStringLiteral("switch historical Chinese"), manager.setLanguage(QStringLiteral("zh-CN"), &error));
    for (int pass = 0; pass != 3; ++pass)
    {
        requireEqual(QStringLiteral("Chinese normal source wins %1").arg(pass), manager.sourceText(QStringLiteral("直接命中")), QStringLiteral("直接命中"));
        requireEqual(QStringLiteral("Chinese packed source lookup %1").arg(pass), manager.packedSourceText(QStringLiteral("直接命中")), QStringLiteral("包内中文"));
        requireEqual(QStringLiteral("Chinese normal English source %1").arg(pass), manager.sourceText(QStringLiteral("English canonical")), QStringLiteral("English canonical"));
        requireEqual(QStringLiteral("Chinese packed English source %1").arg(pass), manager.packedSourceText(QStringLiteral("English canonical")), QStringLiteral("中文强制"));
        requireEqual(QStringLiteral("packed captures retain normal nested semantics %1").arg(pass), manager.packedSourceText(QStringLiteral("状态：安全模式")), QStringLiteral("包状态：安全模式"));
    }

    requireTrue(QStringLiteral("switch fallback cycle A"), manager.setLanguage(QStringLiteral("aa-AA"), &error));
    for (int pass = 0; pass != 3; ++pass)
    {
        requireEqual(QStringLiteral("cycle reaches second pack %1").arg(pass), manager.sourceText(QStringLiteral("环链词")), QStringLiteral("cycle-B"));
        requireEqual(QStringLiteral("cycle miss terminates %1").arg(pass), manager.sourceText(QStringLiteral("循环中缺项")), QStringLiteral("循环中缺项"));
        requireEqual(QStringLiteral("packed cycle miss terminates %1").arg(pass), manager.packedSourceText(QStringLiteral("循环中缺项")), QStringLiteral("循环中缺项"));
    }
    requireTrue(QStringLiteral("return English"), manager.setLanguage(QStringLiteral("en-US"), &error));
    requireEqual(QStringLiteral("return generation restores exact result"), manager.sourceText(QStringLiteral("直接命中")), QStringLiteral("exact-hit"));
    requireEqual(QStringLiteral("return generation restores negative result"), manager.sourceText(QStringLiteral("法语独有")), QStringLiteral("法语独有"));

    return writeReport(output);
}
