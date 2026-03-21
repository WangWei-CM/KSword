#include "ProcessDetailWindow.h"

#include "../theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRunnable>
#include <QThreadPool>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <winternl.h>
#include <sddl.h>

#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Advapi32.lib")

namespace
{
    // ThreadRowColumn：线程细节表格列定义。
    enum class ThreadRowColumn : int
    {
        ThreadId = 0,    // 线程 ID。
        State,           // 状态。
        Priority,        // 优先级。
        SwitchCount,     // 上下文切换计数。
        StartAddress,    // 起始地址。
        TebAddress,      // TEB 地址。
        Affinity,        // 亲和性信息。
        RegisterSummary, // 寄存器摘要。
        Count            // 列总数。
    };

    // 线程细节表头：与开发计划字段一一对应。
    const QStringList ThreadInspectHeaders{
        "ThreadID",
        "状态",
        "优先级",
        "上下文切换",
        "起始地址",
        "TEB地址",
        "亲和性",
        "寄存器"
    };

    // 线程列枚举转索引。
    int toThreadColumnIndex(const ThreadRowColumn column)
    {
        return static_cast<int>(column);
    }

    // 模块表列索引枚举：避免硬编码数字索引。
    enum class ModuleColumn : int
    {
        Path = 0,      // 模块路径（含图标）。
        Size,          // 模块大小。
        Signature,     // 数字签名状态。
        EntryOffset,   // 入口偏移量（RVA）。
        State,         // 运行状态。
        ThreadId,      // ThreadID 信息。
        Count          // 列总数。
    };

    // 模块表头文本。
    const QStringList ModuleHeaders{
        "模块路径",
        "大小",
        "数字签名",
        "入口偏移量",
        "运行状态",
        "ThreadID"
    };

    // 模块列 -> 整数索引转换函数。
    int toModuleColumnIndex(const ModuleColumn column)
    {
        return static_cast<int>(column);
    }

    // 统一蓝色按钮风格，和现有项目主题保持一致。
    QString buildBlueButtonStyle()
    {
        return QStringLiteral(
            "QPushButton {"
            "  color: %1;"
            "  background: #FFFFFF;"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  padding: 4px 10px;"
            "}"
            "QPushButton:hover {"
            "  background: %3;"
            "}"
            "QPushButton:pressed {"
            "  background: %4;"
            "  color: #FFFFFF;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueHoverHex)
            .arg(KswordTheme::PrimaryBluePressedHex);
    }

    // 格式化双精度到固定小数位字符串。
    QString formatDoubleText(const double value, const int precision)
    {
        return QString::number(value, 'f', precision);
    }

    // uint64ToHex：统一把 64 位值转十六进制文本。
    QString uint64ToHex(const std::uint64_t value)
    {
        return QString("0x%1").arg(static_cast<qulonglong>(value), 0, 16).toUpper();
    }

    // convertSidToText：SID 转“账户名 + SID 字符串”。
    QString convertSidToText(PSID sid)
    {
        if (sid == nullptr)
        {
            return QStringLiteral("<null sid>");
        }

        WCHAR accountName[256] = {};
        WCHAR domainName[256] = {};
        DWORD accountNameLength = static_cast<DWORD>(std::size(accountName));
        DWORD domainNameLength = static_cast<DWORD>(std::size(domainName));
        SID_NAME_USE sidType = SidTypeUnknown;
        const BOOL accountOk = LookupAccountSidW(
            nullptr,
            sid,
            accountName,
            &accountNameLength,
            domainName,
            &domainNameLength,
            &sidType);

        LPWSTR sidTextRaw = nullptr;
        const BOOL sidTextOk = ConvertSidToStringSidW(sid, &sidTextRaw);
        QString sidText = sidTextOk && sidTextRaw != nullptr
            ? QString::fromWCharArray(sidTextRaw)
            : QStringLiteral("N/A");
        if (sidTextRaw != nullptr)
        {
            LocalFree(sidTextRaw);
            sidTextRaw = nullptr;
        }

        if (accountOk == FALSE)
        {
            return QString("SID=%1").arg(sidText);
        }

        return QString("%1\\%2 (SID=%3)")
            .arg(QString::fromWCharArray(domainName))
            .arg(QString::fromWCharArray(accountName))
            .arg(sidText);
    }

    // readRemoteUnicodeString：读取远程进程 UNICODE_STRING。
    QString readRemoteUnicodeString(HANDLE processHandle, const UNICODE_STRING& remoteUnicode)
    {
        if (processHandle == nullptr || remoteUnicode.Length == 0 || remoteUnicode.Buffer == nullptr)
        {
            return QString();
        }

        std::vector<wchar_t> buffer(static_cast<std::size_t>(remoteUnicode.Length / sizeof(wchar_t)) + 1, L'\0');
        SIZE_T bytesRead = 0;
        const BOOL readOk = ReadProcessMemory(
            processHandle,
            remoteUnicode.Buffer,
            buffer.data(),
            remoteUnicode.Length,
            &bytesRead);
        if (readOk == FALSE || bytesRead == 0)
        {
            return QString();
        }

        return QString::fromWCharArray(buffer.data());
    }

    // closeHandleSafely：安全关闭句柄，避免重复写样板代码。
    void closeHandleSafely(HANDLE& handleValue)
    {
        if (handleValue != nullptr && handleValue != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handleValue);
            handleValue = nullptr;
        }
    }
}


// ============================================================
// 说明：ProcessDetailWindow.cpp 作为聚合入口，
// 将实现拆分到多个 .inc 文件，便于按功能维护与审查。
// ============================================================

#include "ProcessDetailWindow.BaseAndUi.inc"
#include "ProcessDetailWindow.Module.inc"
#include "ProcessDetailWindow.ActionAndUtil.inc"
#include "ProcessDetailWindow.AdvancedInfo.inc"
