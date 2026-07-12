#include "KernelDockAtomWorker.h"

// ============================================================
// KernelDockAtomWorker.cpp
// 作用说明：
// 1) 遍历全局原子范围 [0xC000, 0xFFFF]；
// 2) 输出 GlobalGetAtomNameW / GetClipboardFormatNameW 可见条目；
// 3) 提供 GlobalFindAtomW 校验工具。
// ============================================================

#include "../Framework.h"

#include <algorithm> // std::sort：按 Atom 值排序。
#include <array>     // std::array：固定栈缓冲。
#include <vector>    // std::vector：结果容器。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace
{
    // 原子范围常量：
    // - Windows 字符串原子通常位于 0xC000~0xFFFF。
    constexpr unsigned int kAtomStartValue = 0xC000U;
    constexpr unsigned int kAtomEndValue = 0xFFFFU;

    // formatAtomHexText：
    // - 作用：把原子值格式化为统一 0xXXXX 文本。
    QString formatAtomHexText(const std::uint16_t atomValue)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<unsigned int>(atomValue), 4, 16, QChar('0'))
            .toUpper();
    }
}

bool runAtomTableSnapshotTask(std::vector<KernelAtomEntry>& rowsOut, QString& errorTextOut)
{
    rowsOut.clear();
    errorTextOut.clear();

    kLogEvent taskEvent;
    info << taskEvent << "[KernelDockAtomWorker] 开始遍历原子表。" << eol;

    std::vector<KernelAtomEntry> resultRows;
    resultRows.reserve(2048);

    for (unsigned int atomValueRaw = kAtomStartValue; atomValueRaw <= kAtomEndValue; ++atomValueRaw)
    {
        const ATOM atomValue = static_cast<ATOM>(atomValueRaw);

        std::array<wchar_t, 512> globalNameBuffer{};
        const UINT globalNameLength = ::GlobalGetAtomNameW(
            atomValue,
            globalNameBuffer.data(),
            static_cast<int>(globalNameBuffer.size()));

        std::array<wchar_t, 512> clipboardNameBuffer{};
        const int clipboardNameLength = ::GetClipboardFormatNameW(
            static_cast<UINT>(atomValue),
            clipboardNameBuffer.data(),
            static_cast<int>(clipboardNameBuffer.size()));

        if (globalNameLength == 0 && clipboardNameLength <= 0)
        {
            continue;
        }

        const QString globalNameText = globalNameLength > 0
            ? QString::fromWCharArray(globalNameBuffer.data(), static_cast<int>(globalNameLength)).trimmed()
            : QString();

        const QString clipboardNameText = clipboardNameLength > 0
            ? QString::fromWCharArray(clipboardNameBuffer.data(), clipboardNameLength).trimmed()
            : QString();

        KernelAtomEntry entry;
        entry.atomValue = static_cast<std::uint16_t>(atomValue);
        entry.atomNameText = !globalNameText.isEmpty() ? globalNameText : clipboardNameText;
        entry.statusText = QStringLiteral("SUCCESS");
        entry.querySucceeded = true;

        if (!globalNameText.isEmpty() && !clipboardNameText.isEmpty())
        {
            entry.sourceText = QStringLiteral("GlobalGetAtomNameW + GetClipboardFormatNameW");
            if (QString::compare(globalNameText, clipboardNameText, Qt::CaseInsensitive) == 0)
            {
                entry.detailText = QStringLiteral(
                    "Atom值: %1 (%2)\n"
                    "名称: %3\n"
                    "来源: Global + ClipboardFormat（同名）")
                    .arg(entry.atomValue)
                    .arg(formatAtomHexText(entry.atomValue))
                    .arg(entry.atomNameText);
            }
            else
            {
                entry.detailText = QStringLiteral(
                    "Atom值: %1 (%2)\n"
                    "Global名称: %3\n"
                    "ClipboardFormat名称: %4\n"
                    "来源: Global + ClipboardFormat（名称不同）")
                    .arg(entry.atomValue)
                    .arg(formatAtomHexText(entry.atomValue))
                    .arg(globalNameText)
                    .arg(clipboardNameText);
            }
        }
        else if (!globalNameText.isEmpty())
        {
            entry.sourceText = QStringLiteral("GlobalGetAtomNameW");
            entry.detailText = QStringLiteral(
                "Atom值: %1 (%2)\n"
                "名称: %3\n"
                "来源: GlobalGetAtomNameW")
                .arg(entry.atomValue)
                .arg(formatAtomHexText(entry.atomValue))
                .arg(entry.atomNameText);
        }
        else
        {
            entry.sourceText = QStringLiteral("GetClipboardFormatNameW");
            entry.detailText = QStringLiteral(
                "Atom值: %1 (%2)\n"
                "名称: %3\n"
                "来源: GetClipboardFormatNameW")
                .arg(entry.atomValue)
                .arg(formatAtomHexText(entry.atomValue))
                .arg(entry.atomNameText);
        }

        resultRows.push_back(std::move(entry));
    }

    std::sort(resultRows.begin(), resultRows.end(), [](const KernelAtomEntry& left, const KernelAtomEntry& right) {
        if (left.atomValue == right.atomValue)
        {
            return QString::compare(left.atomNameText, right.atomNameText, Qt::CaseInsensitive) < 0;
        }
        return left.atomValue < right.atomValue;
    });

    rowsOut = std::move(resultRows);

    info << taskEvent
        << "[KernelDockAtomWorker] 原子表遍历完成, count="
        << rowsOut.size()
        << eol;
    return true;
}

bool verifyGlobalAtomByName(
    const QString& atomNameText,
    std::uint16_t& atomValueOut,
    QString& detailTextOut)
{
    atomValueOut = 0;
    detailTextOut.clear();

    const QString trimmedAtomNameText = atomNameText.trimmed();
    if (trimmedAtomNameText.isEmpty())
    {
        detailTextOut = QStringLiteral("校验失败：原子名称为空。");
        return false;
    }

    const ATOM foundAtomValue = ::GlobalFindAtomW(reinterpret_cast<LPCWSTR>(trimmedAtomNameText.utf16()));
    if (foundAtomValue == 0)
    {
        detailTextOut = QStringLiteral(
            "GlobalFindAtomW 未命中。\n"
            "名称: %1")
            .arg(trimmedAtomNameText);
        return false;
    }

    atomValueOut = static_cast<std::uint16_t>(foundAtomValue);
    detailTextOut = QStringLiteral(
        "GlobalFindAtomW 命中。\n"
        "名称: %1\n"
        "Atom值: %2 (%3)")
        .arg(trimmedAtomNameText)
        .arg(atomValueOut)
        .arg(formatAtomHexText(atomValueOut));
    return true;
}
