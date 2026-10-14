// 完整文件的编码回归：只处理内存字节，不创建窗口、不读写用户文件。
#include "../../Ksword5.1/Ksword5.1/UI/CodeEditorFileSession.h"
#include <QStringDecoder>
#include <iostream>

namespace
{
    int checks = 0; // 实际执行的断言数。
    int failures = 0; // 失败断言数，最终决定进程退出码。

    // Check：接收实际条件与检查名称，累计结果并输出可复核的诊断。
    void Check(const bool condition, const char* name)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::cerr << "FAIL " << name << '\n';
        }
    }
}

// main：直接调用生产解码器，覆盖完整 UTF-8、合法替换字符及残缺多字节尾巴。
int main()
{
    const QByteArray valid = QStringLiteral("ASCII 中文 emoji 😀\r\n").toUtf8(); // 完整多字节样本。
    code_editor_file_session::FileSessionMetadata metadata; // 生产函数返回的真实编码与换行状态。
    Check(code_editor_file_session::decodeTextFileBytes(valid, &metadata) == QString::fromUtf8(valid),
        "complete UTF-8 keeps every character");
    Check(metadata.encoding == QStringConverter::Utf8 && !metadata.hasBom && metadata.validFromFile
        && metadata.lineEndingText == QStringLiteral("\r\n"), "UTF-8 session metadata stays accurate");

    const QByteArray replacement = QByteArray::fromHex("61efbfbd"); // 合法 U+FFFD 不是解码错误。
    Check(code_editor_file_session::decodeTextFileBytes(replacement, &metadata).toUtf8() == replacement
        && metadata.encoding == QStringConverter::Utf8, "literal replacement scalar remains valid UTF-8");
    Check(code_editor_file_session::decodeTextFileBytes(QByteArray(), &metadata).isEmpty()
        && metadata.encoding == QStringConverter::Utf8, "empty complete file is valid UTF-8");

    const QByteArray bom = QByteArray::fromHex("efbbbf") + valid; // BOM 应被识别，而不进入正文。
    Check(code_editor_file_session::decodeTextFileBytes(bom, &metadata) == QString::fromUtf8(valid)
        && metadata.hasBom && metadata.encoding == QStringConverter::Utf8, "UTF-8 BOM retains content and metadata");

    // 两、三、四字节字符在每个残缺位置截断，完整文件必须按既有策略回退系统编码。
    const QByteArray tails[] = {QByteArray::fromHex("c2"), QByteArray::fromHex("e4"),
        QByteArray::fromHex("e4b8"), QByteArray::fromHex("f0"), QByteArray::fromHex("f09f"),
        QByteArray::fromHex("f09f98")}; // 每个样本都缺少至少一个 UTF-8 延续字节。
    for (const auto& tail : tails)
    {
        const QByteArray bytes = QByteArray("abc") + tail; // 可检测静默丢弃的 ASCII 前缀与残缺尾巴。
        const QString decoded = code_editor_file_session::decodeTextFileBytes(bytes, &metadata); // 生产输出。
        Check(metadata.encoding == QStringConverter::System && !metadata.hasBom && metadata.validFromFile,
            "incomplete UTF-8 tail is rejected as a complete UTF-8 file");
        Check(decoded == QString::fromLocal8Bit(bytes) && decoded != QStringLiteral("abc"),
            "existing system-codepage fallback receives all original bytes");
        Check(code_editor_file_session::decodeTextFileBytes(bytes, nullptr) == decoded,
            "null metadata output follows the same complete-file decoding path");
    }

    const QByteArray completeTail = QByteArray::fromHex("616263e4b8ad"); // 上述三字节样本补齐后的正常情况。
    Check(code_editor_file_session::decodeTextFileBytes(completeTail, &metadata) == QStringLiteral("abc中")
        && metadata.encoding == QStringConverter::Utf8, "complete multibyte tail never uses fallback");
    std::cout << "FILE_DECODE_CHECKS=" << checks << " FAILURES=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
