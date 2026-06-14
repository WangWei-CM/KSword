#pragma once

// ============================================================
// DriverDock.Internal.h
// 作用：
// - 汇总 DriverDock 多个 .cpp 共享的 Qt/Win32 include 与内部工具声明；
// - 替代旧的文本拼接式实现，保持概览、操作和 DBWIN 逻辑独立编译；
// - 仅供 DriverDock 内部实现使用，不改变 DriverDock 对外接口。
// ============================================================

#include "DriverDock.h"
#include "../theme.h"
#include "../UI/CodeEditorWidget.h"

#include <QAbstractItemView>
#include <QAction>
#include <QBrush>
#include <QChar>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QMessageBox>
#include <QModelIndex>
#include <QModelIndexList>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QRunnable>
#include <QShowEvent>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThreadPool>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#include <winsvc.h>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Psapi.lib")

namespace ksword::driver_dock_internal
{
    // ModuleRecordIndexRole：
    // - 输入：写入已加载模块表格的第一列单元格；
    // - 处理：保存该行对应 m_loadedModuleCache/m_loadedModuleEvidenceCache 的源索引；
    // - 返回：常量本身无返回值，读取时用于从 UI 行反查缓存。
    constexpr int ModuleRecordIndexRole = Qt::UserRole + 41;

    // DbwinBufferPacket：DBWIN 共享内存中的固定布局。
    struct DbwinBufferPacket
    {
        DWORD processId = 0;
        char messageBuffer[4096 - sizeof(DWORD)] = {};
    };

    // DriverDock 内部工具：输入 UI/Win32 数据，返回格式化文本或操作状态。
    std::wstring toWideString(const QString& textValue);
    QTableWidgetItem* createReadOnlyItem(const QString& textValue);
    QString formatAddress(std::uint64_t addressValue);
    QString formatCompactAddress(std::uint64_t addressValue);
    QString formatHex32(std::uint32_t value);
    QString formatNtStatusText(long statusValue);
    bool isDriverSignatureLoadError(DWORD errorCode);
    QString formatWin32ErrorTextForAdvice(DWORD errorCode);
    QString buildDriverSignatureLoadAdvice(DWORD errorCode, const QString& serviceNameText, const QString& binaryPathText);
    QString driverObjectQueryStatusText(std::uint32_t statusValue);
    QString driverForceUnloadStatusText(std::uint32_t statusValue);
    QString driverMajorFunctionName(std::uint32_t majorFunction);
    QString driverDeviceTypeText(std::uint32_t deviceType);
    QString driverDispatchLocationText(std::uint32_t flags);
}
