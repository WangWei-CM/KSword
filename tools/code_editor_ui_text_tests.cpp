#include "LanguageManager.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <iostream>

// The runner extracts only the real status formatter into this pure value
// harness. No QWidget, editor, document, native dialog or business action runs.
struct TextSink { QString value; void setText(const QString& text) { value = text; } };
struct StatusHarness
{
    QString m_statusSourceTemplate;
    QStringList m_statusArguments;
    quint32 m_statusTranslatedArgumentMask = 0;
    QString m_statusSemanticKey;
    TextSink sink;
    TextSink* m_statusLabel = &sink;
    void refreshOperationStatus();
};
#include "editor_status_body.inc"

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    int checks = 0;
    int failures = 0;
    const auto equal = [&](const QString& actual, const QString& expected) {
        ++checks;
        if (actual != expected) { ++failures; std::cerr << actual.toStdString() << " != " << expected.toStdString() << '\n'; }
    };
    const auto writePack = [&](const QString& language, const QJsonObject& sources, const QJsonObject& translations) {
        const QJsonObject pack{{QStringLiteral("schema"), QStringLiteral("ksword-language-pack")},
            {QStringLiteral("format_version"), 1}, {QStringLiteral("id"), language},
            {QStringLiteral("name"), language}, {QStringLiteral("native_name"), language},
            {QStringLiteral("translations"), translations}, {QStringLiteral("source_translations"), sources}};
        QFile file(QCoreApplication::applicationDirPath()+QStringLiteral("/languages/")+language+QStringLiteral(".json"));
        return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(pack).toJson()) > 0;
    };
    if (!QDir().mkpath(QCoreApplication::applicationDirPath()+QStringLiteral("/languages"))
        || !writePack(QStringLiteral("en-US"), {
            {QStringLiteral("查找成功：%1"),QStringLiteral("Found: %1")},
            {QStringLiteral("打开成功：%1（%2%3）"),QStringLiteral("Opened %1 (%2%3)")},
            {QStringLiteral("保存成功：%1（%2）"),QStringLiteral("Saved %1 (%2)")},
            {QStringLiteral("本地编码"),QStringLiteral("Local encoding")},
            {QStringLiteral("安全模式"),QStringLiteral("Safe mode")},
            {QStringLiteral("已替换当前命中。"),QStringLiteral("Replaced.")}},
            {{QStringLiteral("ui.code_editor.export.failed"),QStringLiteral("Export failed: %1")}})
        || !writePack(QStringLiteral("zh-CN"), {}, {{QStringLiteral("sentinel"),QStringLiteral("sentinel")}})) return 2;
    auto& manager = ks::i18n::LanguageManager::instance();
    QString error;
    if (!manager.initialize(QStringLiteral("en-US"), &error)) return 2;
    StatusHarness status;
    status.m_statusSourceTemplate = QStringLiteral("查找成功：%1");
    status.m_statusArguments = {QStringLiteral("安全模式")};
    status.refreshOperationStatus();
    equal(status.sink.value, QStringLiteral("Found: 安全模式"));
    status.m_statusSourceTemplate = QStringLiteral("打开成功：%1（%2%3）");
    status.m_statusArguments = {QStringLiteral("C:\\安全模式\\%2.txt"), QStringLiteral("本地编码"), QString()};
    status.m_statusTranslatedArgumentMask = 1U << 1;
    status.refreshOperationStatus();
    equal(status.sink.value, QStringLiteral("Opened C:\\安全模式\\%2.txt (Local encoding)"));
    status.m_statusTranslatedArgumentMask = 0;
    status.refreshOperationStatus();
    equal(status.sink.value, QStringLiteral("Opened C:\\安全模式\\%2.txt (本地编码)"));
    status.m_statusTranslatedArgumentMask = 1U << 1;
    if (!manager.setLanguage(QStringLiteral("zh-CN"), &error)) return 2;
    status.refreshOperationStatus();
    equal(status.sink.value, QStringLiteral("打开成功：C:\\安全模式\\%2.txt（本地编码）"));
    if (!manager.setLanguage(QStringLiteral("en-US"), &error)) return 2;
    status.refreshOperationStatus();
    equal(status.sink.value, QStringLiteral("Opened C:\\安全模式\\%2.txt (Local encoding)"));
    status.m_statusSourceTemplate = QStringLiteral("保存成功：%1（%2）");
    status.m_statusArguments = {QStringLiteral("%2_安全模式"), QStringLiteral("本地编码")};
    status.refreshOperationStatus();
    equal(status.sink.value, QStringLiteral("Saved %2_安全模式 (Local encoding)"));
    status.m_statusSourceTemplate = QStringLiteral("已替换当前命中。");
    status.m_statusArguments.clear();
    status.refreshOperationStatus();
    equal(status.sink.value, QStringLiteral("Replaced."));
    status.m_statusSourceTemplate = QStringLiteral("导出失败：%1");
    status.m_statusArguments = {QStringLiteral("系统原文 %1 安全模式")};
    status.m_statusSemanticKey = QStringLiteral("ui.code_editor.export.failed");
    status.refreshOperationStatus();
    equal(status.sink.value, QStringLiteral("Export failed: 系统原文 %1 安全模式"));
    if (!manager.setLanguage(QStringLiteral("zh-CN"), &error)) return 2;
    status.refreshOperationStatus();
    equal(status.sink.value, QStringLiteral("导出失败：系统原文 %1 安全模式"));
    std::cout << "QCoreApplication only; status checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
