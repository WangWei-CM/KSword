#pragma once

// ============================================================
// ProcessDetailWindow.InternalCommon.h
// 作用：
// - 为 ProcessDetailWindow 多个实现 cpp 提供统一包含入口；
// - 集中声明跨文件复用的内部辅助类型、列定义与样式工具；
// - 避免再次退回到 .inc 聚合式实现。
// ============================================================

#include "ProcessDetailWindow.h"

#include "../theme.h"
#include "../UI/CodeEditorWidget.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QEvent>
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
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QThreadPool>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

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

namespace process_detail_window_internal
{
    // ThreadRowColumn 作用：
    // - 统一线程细节表格列顺序；
    // - 供 UI 初始化和结果回填两端共享。
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

    // ThreadInspectHeaders 作用：线程细节表头文本常量。
    extern const QStringList ThreadInspectHeaders;

    // toThreadColumnIndex 作用：把线程列枚举转换成表格索引。
    int toThreadColumnIndex(ThreadRowColumn column);

    // ModuleColumn 作用：
    // - 统一模块表格列顺序；
    // - 避免模块页读写 UserRole 数据时出现魔法数字。
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

    // ModuleHeaders 作用：模块表头文本常量。
    extern const QStringList ModuleHeaders;

    // toModuleColumnIndex 作用：把模块列枚举转换成表格索引。
    int toModuleColumnIndex(ModuleColumn column);

    // buildBlueButtonStyle 作用：生成统一蓝色按钮样式。
    QString buildBlueButtonStyle();

    // buildProcessDetailRootStyle 作用：生成详情窗口根样式。
    QString buildProcessDetailRootStyle();

    // buildStateLabelStyle 作用：生成状态标签样式文本。
    QString buildStateLabelStyle(const QColor& textColor, int fontWeight);

    // statusIdleColor 作用：返回空闲态文本色。
    QColor statusIdleColor();

    // statusWarningColor 作用：返回警告态文本色。
    QColor statusWarningColor();

    // statusErrorColor 作用：返回错误态文本色。
    QColor statusErrorColor();

    // statusSecondaryColor 作用：返回中性文本色。
    QColor statusSecondaryColor();

    // signatureTrustedColor 作用：返回可信签名文本色。
    QColor signatureTrustedColor();

    // signatureUntrustedColor 作用：返回不可信签名文本色。
    QColor signatureUntrustedColor();

    // formatDoubleText 作用：把浮点数格式化成固定小数位文本。
    QString formatDoubleText(double value, int precision);

    // uint64ToHex 作用：把 64 位数值格式化为十六进制文本。
    QString uint64ToHex(std::uint64_t value);

    // convertSidToText 作用：把 SID 转为“账户名 + SID”可读文本。
    QString convertSidToText(PSID sid);

    // readRemoteUnicodeString 作用：读取远程进程中的 UNICODE_STRING 内容。
    QString readRemoteUnicodeString(HANDLE processHandle, const UNICODE_STRING& remoteUnicode);
}
