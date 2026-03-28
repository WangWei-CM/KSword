#include "MemoryDock.h"

#include "../theme.h"
#include "../UI/HexEditorWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QPointer>
#include <QSignalBlocker>
#include <QSize>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>

// Win32 API 头文件：进程枚举、模块枚举、内存遍历、读写内存全部来自这些头。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>

// 读取进程内存和映射文件路径需要链接 Psapi。
#pragma comment(lib, "Psapi.lib")

namespace
{
    // ========================================================
    // 主题样式函数：统一按钮/输入框/下拉框风格。
    // ========================================================

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

    QString buildBlueComboStyle()
    {
        return QStringLiteral(
            "QComboBox {"
            "  border: 1px solid %1;"
            "  border-radius: 3px;"
            "  padding: 2px 6px;"
            "  background: %3;"
            "  color: %4;"
            "}"
            "QComboBox:hover {"
            "  border-color: %2;"
            "}"
            "QComboBox::drop-down {"
            "  border: none;"
            "}")
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    QString buildBlueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit, QTextEdit, QPlainTextEdit {"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  background: %3;"
            "  color: %4;"
            "  padding: 3px 5px;"
            "}"
            "QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus {"
            "  border: 1px solid %1;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    // 表格表头统一主题样式，确保“内存页”整体视觉与主主题贴合。
    QString buildBlueTableHeaderStyle()
    {
        return QStringLiteral(
            "QHeaderView::section {"
            "  color:%1;"
            "  background:%2;"
            "  border:1px solid %3;"
            "  padding:4px;"
            "  font-weight:600;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    // 十六进制查看器常量：每行 16 字节，共 32 行，每页 512 字节。
    constexpr int kHexBytesPerRow = 16;
    constexpr int kHexRowCount = 32;
    constexpr std::uint64_t kHexPageBytes = static_cast<std::uint64_t>(kHexBytesPerRow * kHexRowCount);

    // 模块表列定义：与 ProcessDetailWindow 模块页保持一致。
    enum class ModuleTreeColumn : int
    {
        Path = 0,      // 模块路径（含图标）。
        Size,          // 模块大小。
        Signature,     // 数字签名状态。
        EntryOffset,   // 入口偏移（RVA）。
        State,         // 运行状态。
        ThreadId,      // ThreadID 信息。
        Count          // 列总数。
    };

    // 模块表头文本：直接对齐进程详细信息模块页体验。
    const QStringList ModuleTreeHeaders{
        "模块路径",
        "大小",
        "数字签名",
        "入口偏移量",
        "运行状态",
        "ThreadID"
    };

    // 枚举列 -> 整数索引转换，避免代码里散落硬编码数字。
    int toModuleTreeColumnIndex(const ModuleTreeColumn column)
    {
        return static_cast<int>(column);
    }

    // PID 转 DWORD 的显式封装，避免隐式转换警告。
    DWORD toDwordPid(const std::uint32_t pid)
    {
        return static_cast<DWORD>(pid);
    }

    // 判断内存保护属性是否可读。
    bool isReadableProtect(const std::uint32_t protectValue)
    {
        if ((protectValue & PAGE_GUARD) != 0 || (protectValue & PAGE_NOACCESS) != 0)
        {
            return false;
        }
        const std::uint32_t baseProtect = protectValue & 0xFF;
        switch (baseProtect)
        {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    // 解析两位十六进制字节文本，例如 "7F"、"ff"。
    bool parseHexByte(const QString& text, std::uint8_t& valueOut)
    {
        bool parseOk = false;
        const int value = text.trimmed().toInt(&parseOk, 16);
        if (!parseOk || value < 0 || value > 0xFF)
        {
            return false;
        }
        valueOut = static_cast<std::uint8_t>(value);
        return true;
    }

    // 统一按路径加载图标并做缓存，减少重复读取系统图标带来的卡顿。
    QIcon resolveIconByPath(const QString& absolutePath, QHash<QString, QIcon>& cache)
    {
        if (absolutePath.trimmed().isEmpty())
        {
            return QIcon(":/Icon/process_main.svg");
        }

        auto foundIt = cache.find(absolutePath);
        if (foundIt != cache.end())
        {
            return foundIt.value();
        }

        QIcon resolvedIcon(absolutePath);
        if (resolvedIcon.isNull())
        {
            QFileIconProvider iconProvider;
            resolvedIcon = iconProvider.icon(QFileInfo(absolutePath));
        }
        if (resolvedIcon.isNull())
        {
            resolvedIcon = QIcon(":/Icon/process_main.svg");
        }

        cache.insert(absolutePath, resolvedIcon);
        return resolvedIcon;
    }
}


// ============================================================
// 说明：MemoryDock.cpp 作为聚合入口，仅保留公共 include/工具函数。
// 具体业务实现按功能拆分到多个 .inc 文件，降低单文件体积并提升可维护性。
// ============================================================

#include "MemoryDock.UiBuild.inc"
#include "MemoryDock.UiWireAndStatus.inc"
#include "MemoryDock.ProcessRegion.inc"
#include "MemoryDock.SearchParseAndFilter.inc"
#include "MemoryDock.SearchFlow.inc"
#include "MemoryDock.ViewBreakpointUtil.inc"
