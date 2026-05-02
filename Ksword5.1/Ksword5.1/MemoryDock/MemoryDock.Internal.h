#pragma once

// ============================================================
// MemoryDock.Internal.h
// 作用：
// - 汇总 MemoryDock 多个 .cpp 共享的 Qt/Win32 include 与内部工具声明；
// - 替代旧的聚合式源码结构，让 UI、进程区域、搜索和查看器逻辑独立编译；
// - 只服务 MemoryDock 内部实现，不扩大 public API。
// ============================================================

#include "MemoryDock.h"
#include "../theme.h"
#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/HexEditorWidget.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QChar>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
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
#include <QIODevice>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QModelIndex>
#include <QPlainTextEdit>
#include <QPoint>
#include <QProgressBar>
#include <QPushButton>
#include <QPointer>
#include <QSignalBlocker>
#include <QSize>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>

#pragma comment(lib, "Psapi.lib")

namespace ksword::memory_dock_internal
{
    // UI 样式函数：输入为空，返回主题样式表文本。
    QString buildBlueButtonStyle();
    QString buildBlueComboStyle();
    QString buildBlueInputStyle();
    QString buildBlueTableHeaderStyle();

    // 十六进制查看器分页常量：每页固定 16 * 32 = 512 字节。
    extern const int kHexBytesPerRow;
    extern const int kHexRowCount;
    extern const std::uint64_t kHexPageBytes;

    // ModuleTreeColumn：进程模块树列定义，与 ProcessDetailWindow 模块页对齐。
    enum class ModuleTreeColumn : int
    {
        Path = 0,
        Size,
        Signature,
        EntryOffset,
        State,
        ThreadId,
        Count
    };

    // ModuleTreeHeaders：模块树表头文本，供 UI 构建代码使用。
    extern const QStringList ModuleTreeHeaders;

    // 内部转换和解析工具：输入业务值，返回 Qt/Win32 需要的辅助结果。
    int toModuleTreeColumnIndex(ModuleTreeColumn column);
    DWORD toDwordPid(std::uint32_t pid);
    bool isReadableProtect(std::uint32_t protectValue);
    bool parseHexByte(const QString& text, std::uint8_t& valueOut);
    QIcon resolveIconByPath(const QString& absolutePath, QHash<QString, QIcon>& cache);
}
