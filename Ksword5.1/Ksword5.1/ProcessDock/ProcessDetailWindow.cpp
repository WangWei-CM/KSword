#include "ProcessDetailWindow.h"

#include "../theme.h"
#include "../UI/CodeEditorWidget.h"

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
#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPalette>
#include <QPointer>
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
            "  background: %5;"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  padding: 4px 10px;"
            "}"
            "QPushButton:hover {"
            "  background: %3;"
            "  color: #FFFFFF;"
            "  border: 1px solid %3;"
            "}"
            "QPushButton:pressed {"
            "  background: %4;"
            "  color: #FFFFFF;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(QStringLiteral("#2E8BFF"))
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(KswordTheme::SurfaceHex());
    }

    // buildProcessDetailRootStyle 作用：
    // - 统一进程详情窗口内部控件的深浅色样式；
    // - 明确设置表格/分组框/输入框/Tab/滚动条背景，消除默认白底残留。
    QString buildProcessDetailRootStyle()
    {
        return QStringLiteral(
            "QWidget#ProcessDetailWindowRoot{"
            "  background:%1;"
            "  color:%2;"
            "}"
            "QWidget#ProcessDetailWindowRoot QGroupBox{"
            "  border:1px solid %3;"
            "  border-radius:4px;"
            "  margin-top:8px;"
            "  padding-top:8px;"
            "  background:%4;"
            "  color:%2;"
            "}"
            "QWidget#ProcessDetailWindowRoot QGroupBox::title{"
            "  subcontrol-origin:margin;"
            "  left:8px;"
            "  padding:0 4px;"
            "  color:%2;"
            "}"
            "QWidget#ProcessDetailWindowRoot QLineEdit,"
            "QWidget#ProcessDetailWindowRoot QComboBox,"
            "QWidget#ProcessDetailWindowRoot QPlainTextEdit,"
            "QWidget#ProcessDetailWindowRoot QTextEdit{"
            "  background:%4;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  border-radius:3px;"
            "  padding:3px 6px;"
            "  selection-background-color:%5;"
            "  selection-color:#FFFFFF;"
            "}"
            "QWidget#ProcessDetailWindowRoot QLineEdit[readOnly=\"true\"]{"
            "  background:%6;"
            "}"
            "QWidget#ProcessDetailWindowRoot QComboBox::drop-down{"
            "  border:none;"
            "  width:20px;"
            "}"
            "QWidget#ProcessDetailWindowRoot QTableWidget,"
            "QWidget#ProcessDetailWindowRoot QTreeWidget{"
            "  background:%4;"
            "  alternate-background-color:%6;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  gridline-color:%3;"
            "  selection-background-color:%5;"
            "  selection-color:#FFFFFF;"
            "}"
            "QWidget#ProcessDetailWindowRoot QTableCornerButton::section{"
            "  background:%4;"
            "  border:1px solid %3;"
            "}"
            "QWidget#ProcessDetailWindowRoot QHeaderView::section{"
            "  background:%4;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  padding:4px;"
            "  font-weight:600;"
            "}"
            "QWidget#ProcessDetailWindowRoot QTabWidget::pane{"
            "  border:1px solid %3;"
            "  background:%4;"
            "}"
            "QWidget#ProcessDetailWindowRoot QTabBar::tab{"
            "  background:%4;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  border-bottom:none;"
            "  padding:6px 10px;"
            "  margin-right:1px;"
            "}"
            "QWidget#ProcessDetailWindowRoot QTabBar::tab:selected{"
            "  background:%5;"
            "  color:#FFFFFF;"
            "  border-color:%5;"
            "}"
            "QWidget#ProcessDetailWindowRoot QTabBar::tab:hover:!selected{"
            "  background:%6;"
            "}"
            "QWidget#ProcessDetailWindowRoot QScrollBar:vertical{"
            "  background:%4;"
            "  width:12px;"
            "  margin:0;"
            "}"
            "QWidget#ProcessDetailWindowRoot QScrollBar:horizontal{"
            "  background:%4;"
            "  height:12px;"
            "  margin:0;"
            "}"
            "QWidget#ProcessDetailWindowRoot QScrollBar::handle:vertical,"
            "QWidget#ProcessDetailWindowRoot QScrollBar::handle:horizontal{"
            "  background:%5;"
            "  min-height:20px;"
            "  min-width:20px;"
            "  border-radius:4px;"
            "}"
            "QWidget#ProcessDetailWindowRoot QScrollBar::handle:vertical:hover,"
            "QWidget#ProcessDetailWindowRoot QScrollBar::handle:horizontal:hover{"
            "  background:#2E8BFF;"
            "}"
            "QWidget#ProcessDetailWindowRoot QScrollBar::add-line,"
            "QWidget#ProcessDetailWindowRoot QScrollBar::sub-line{"
            "  background:%4;"
            "  border:none;"
            "}"
            "QWidget#ProcessDetailWindowRoot QScrollBar::add-page,"
            "QWidget#ProcessDetailWindowRoot QScrollBar::sub-page{"
            "  background:%4;"
            "}")
            .arg(QStringLiteral("palette(window)"))
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceAltHex());
    }

    // buildStateLabelStyle 作用：
    // - 统一状态标签（刷新中/完成/警告/失败）样式拼接；
    // - 传入颜色和字重即可生成可复用样式文本。
    QString buildStateLabelStyle(const QColor& textColor, const int fontWeight)
    {
        return QStringLiteral("color:%1; font-weight:%2;")
            .arg(textColor.name(QColor::HexRgb))
            .arg(fontWeight);
    }

    // statusIdleColor 作用：返回空闲状态文本色。
    QColor statusIdleColor()
    {
        return KswordTheme::IsDarkModeEnabled() ? QColor(146, 214, 156) : QColor(47, 125, 50);
    }

    // statusWarningColor 作用：返回警告状态文本色。
    QColor statusWarningColor()
    {
        return KswordTheme::IsDarkModeEnabled() ? QColor(255, 205, 130) : QColor(138, 109, 59);
    }

    // statusErrorColor 作用：返回错误状态文本色。
    QColor statusErrorColor()
    {
        return KswordTheme::IsDarkModeEnabled() ? QColor(255, 145, 145) : QColor(220, 50, 47);
    }

    // statusSecondaryColor 作用：返回中性状态文本色。
    QColor statusSecondaryColor()
    {
        return KswordTheme::IsDarkModeEnabled() ? QColor(178, 178, 178) : QColor(79, 79, 79);
    }

    // signatureTrustedColor 作用：返回签名可信文本色。
    QColor signatureTrustedColor()
    {
        return KswordTheme::IsDarkModeEnabled() ? QColor(130, 210, 140) : QColor(34, 139, 34);
    }

    // signatureUntrustedColor 作用：返回签名不可信文本色。
    QColor signatureUntrustedColor()
    {
        return KswordTheme::IsDarkModeEnabled() ? QColor(255, 155, 155) : QColor(220, 50, 47);
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
