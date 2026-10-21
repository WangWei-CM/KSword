#pragma once

// ============================================================
// HexExport.h
// 作用：
// - HexView 的"导出"：导出二进制、导出十六进制文本（带地址与 ASCII 的转储）、
//   导出选中字节为十六进制文本。取代旧 HexEditorWidget 的三个导出入口。
// - 分三层，彼此独立：
//     格式化层  FormatDump / FormatDumpRow / FormatSelectedHex —— 纯函数，只依赖 Qt Core，夹具里直接测试；
//     写文件层  WriteBinaryFile / WriteTextFile —— 不弹任何对话框，失败返回原因，夹具里可测失败路径；
//     对话框层  PickSavePath / ShowExportError —— QFileDialog 选路径、模态框报错，只有用户手势才会走到。
//
// ------------------------------------------------------------
// 一、转储格式（与旧控件兼容）
// ------------------------------------------------------------
// 每行 16 字节（kDumpBytesPerRow）：
//     0x0000000000001000  41 42 43 44 45 46 47 48 49 4A 4B 4C 4D 4E 4F 50  |ABCDEFGHIJKLMNOP|
// - 地址是 "0x" + 16 位大写十六进制（旧控件的 toUpper 把前缀也变成了 "0X"，这里前缀保持小写 x，其余一致）；
//   地址按"数据起点 + 行偏移"计算，所以起点不对齐时第一行就从起点开始（与旧控件一致，不做画布那样的对齐补位）；
//   地址与十六进制之间、十六进制与 ASCII 之间各两个空格，字节之间一个空格。
// - 最后一行不足 16 字节时，十六进制处用 "--" 补位，ASCII 处用空格补位，保证竖线对齐。
// - ASCII 列：0x20..0x7E 原样，其余一律点号。
// - 行与行之间用 '\n' 连接，末尾没有多余换行；空数据返回空串（导出入口会先拒绝空数据）。
// - 选中字节导出：大写、单空格分隔，如 "41 42 43"。
//
// ------------------------------------------------------------
// 二、写文件
// ------------------------------------------------------------
// - 二进制按原样写入；文本按 UTF-8 写入，换行在 Windows 上落成 \r\n（QIODevice::Text，与旧控件一致）。
// - 失败（打不开、写入不完整、关闭失败）都返回 false 并给出带路径与系统原因的文字，绝不静默。
//
// ------------------------------------------------------------
// 三、对话框
// ------------------------------------------------------------
// - PickSavePath：QFileDialog::getSaveFileName，建议文件名与过滤器按导出种类给出；取消返回空串。
// - ShowExportError：模态错误框，显式设置不透明背景、文字、按钮样式（用静态主题色，每次弹出前新建）。
// ============================================================

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <atomic>
#include <memory>
#include "../CompleteExport.h"

class QWidget;

namespace ks::ui::hexexport
{
    // kDumpBytesPerRow：转储文本每行的字节数。
    inline constexpr int kDumpBytesPerRow = 16;

    // Kind：导出种类。
    enum class Kind : int
    {
        Binary = 0,     // 二进制原样
        HexDump,        // 带地址与 ASCII 的十六进制转储文本
        SelectedHex     // 选中字节的十六进制文本
    };

    // FormatDumpRow：格式化转储的一行。
    // 传入：该行第一个字节的地址；data 整块数据；offset 该行第一个字节在 data 里的下标；
    //       bytesPerRow 每行字节数（默认 16）。越过 data 末尾的列按 "--" 与空格补位。
    // 传出：一行文本，不含换行。
    QString FormatDumpRow(
        std::uint64_t rowAddress,
        const QByteArray& data,
        qsizetype offset,
        int bytesPerRow = kDumpBytesPerRow);

    // FormatDump：格式化整块数据的转储文本。
    // 传入：base 数据起始地址；data 数据；bytesPerRow 每行字节数（默认 16；非正数按 16）。
    // 传出：全部行用 '\n' 连接的文本；data 为空返回空串。
    QString FormatDump(std::uint64_t base, const QByteArray& data, int bytesPerRow = kDumpBytesPerRow);

    // FormatSelectedHex：把字节格式化成大写空格分隔的十六进制文本；空字节返回空串。
    QString FormatSelectedHex(const QByteArray& bytes);

    // WriteBinaryFile：把字节原样写入文件（覆盖）。
    // 传入：路径、字节、原因输出（可为空指针）；传出：是否成功，失败时原因含路径与系统错误。
    bool WriteBinaryFile(const QString& path, const QByteArray& bytes, QString* errorOut);

    // WriteTextFile：把文本按 UTF-8 写入文件（覆盖，文本模式换行）。参数与返回同 WriteBinaryFile。
    bool WriteTextFile(const QString& path, const QString& text, QString* errorOut);

    // One frozen, already acquired range. Capturing bytes is implicit-sharing;
    // no provider, widget, target session or borrowed model enters the worker.
    struct ExportControl final
    {
        enum class Phase { Preparing, Committing, Canceled };
        std::atomic<Phase> phase{Phase::Preparing};
        bool cancelPreparation()
        {
            auto expected = Phase::Preparing;
            return phase.compare_exchange_strong(expected, Phase::Canceled);
        }
    };
    struct ExportSnapshot
    {
        Kind kind = Kind::Binary;
        QByteArray bytes;
        std::uint64_t baseAddress = 0;
        std::uint64_t sourceVersion = 0;
        qsizetype offset = 0;
        qsizetype length = 0;
        std::shared_ptr<ExportControl> control;
    };
    // Bounded serializer used by the shared accepted-export/QSaveFile engine.
    bool SerializeSnapshot(const ExportSnapshot& value, CompleteExportSink& sink);
    // Synchronous compatibility API: still reports actual completion, using the
    // same bounded serializer and atomic replacement, never a full formatted document.
    bool WriteSnapshotFile(const QString& path, const ExportSnapshot& value, QString* errorOut);

    // SuggestedFileName：某种导出的建议文件名。
    QString SuggestedFileName(Kind kind);

    // PickSavePath：弹出保存文件对话框选择路径。
    // 传入：父控件、导出种类；传出：选中的路径，用户取消返回空串。
    QString PickSavePath(QWidget* parent, Kind kind);

    // ShowExportError：弹出模态错误框。传入：父控件、原因文字。样式显式不透明（见文件头第三节）。
    void ShowExportError(QWidget* parent, const QString& reason);
}
