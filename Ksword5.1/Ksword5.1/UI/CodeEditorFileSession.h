#pragma once

// ============================================================
// CodeEditorFileSession.h
// 作用：
// - 提供文本文件会话元数据结构（编码/BOM/换行风格）；
// - 提供统一的文本解码、换行归一化与状态展示辅助函数；
// - 供 CodeEditorWidget 复用，避免每个页面重复实现文件编解码逻辑。
// ============================================================

#include <QByteArray>
#include <QString>
#include <QStringConverter>

namespace code_editor_file_session
{
    // FileSessionMetadata：
    // - 记录当前文本文件的会话元数据；
    // - 用于“打开后保持原编码/原 BOM/原换行风格”的保存策略。
    struct FileSessionMetadata
    {
        // encoding：目标文本编码（默认 UTF-8）。
        QStringConverter::Encoding encoding = QStringConverter::Utf8;

        // hasBom：保存时是否输出 BOM。
        bool hasBom = false;

        // lineEndingText：换行风格，常见值为 "\r\n"、"\n"、"\r"。
        QString lineEndingText = QStringLiteral("\r\n");

        // validFromFile：是否来自真实文件加载（true=来自文件，false=内存文本）。
        bool validFromFile = false;
    };

    // decodeTextFileBytes：
    // - 作用：按 BOM / UTF-8 / 本地编码规则解码文件字节；
    // - 调用：CodeEditorWidget 加载文件后调用；
    // - 入参 fileBytes：原始文件字节；
    // - 传出 metadataOut：返回解码后应保持的文件会话元数据；
    // - 返回：可直接放入编辑器的 QString 文本。
    QString decodeTextFileBytes(const QByteArray& fileBytes, FileSessionMetadata* metadataOut);

    // detectDominantLineEnding：
    // - 作用：统计文本中主导换行风格；
    // - 调用：加载文件后/保存前可用于推断换行策略；
    // - 入参 textValue：目标文本；
    // - 返回："\r\n"、"\n" 或 "\r"。
    QString detectDominantLineEnding(const QString& textValue);

    // normalizeLineEndingForSaving：
    // - 作用：将文本统一转换为指定换行风格；
    // - 调用：写文件前调用，保证换行可控；
    // - 入参 textValue：原文本；lineEndingText：目标换行；
    // - 返回：转换后的文本。
    QString normalizeLineEndingForSaving(const QString& textValue, const QString& lineEndingText);

    // buildEncodingDisplayText：
    // - 作用：把编码元数据转换为状态栏可读文本；
    // - 调用：CodeEditorWidget 更新状态栏时调用；
    // - 入参 metadata：会话元数据；
    // - 返回：如 "UTF-8"、"UTF-8 BOM"、"UTF-16 LE BOM"、"本地编码"。
    QString buildEncodingDisplayText(const FileSessionMetadata& metadata);

    // buildLineEndingDisplayText：
    // - 作用：把换行风格转换为状态栏可读短语；
    // - 调用：CodeEditorWidget 更新状态栏时调用；
    // - 入参 lineEndingText：换行文本；
    // - 返回：如 "CRLF"、"LF"、"CR"。
    QString buildLineEndingDisplayText(const QString& lineEndingText);
}

