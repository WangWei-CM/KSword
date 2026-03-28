#include "DriverDock.h"
#include "../theme.h"

// ============================================================
// DriverDock.cpp
// 作用说明：
// 1) 提供驱动服务枚举、注册、挂载、卸载、删除；
// 2) 提供已加载内核模块枚举；
// 3) 提供 DBWIN 调试输出读取；
// 4) 提供驱动操作日志与状态可视化输出。
// ============================================================

#include <QAbstractItemView>
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
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm> // std::sort：结果列表排序。
#include <array>     // std::array：固定长度驱动名缓冲。
#include <chrono>    // std::chrono：等待服务状态超时控制。
#include <cstdint>   // std::uint8_t/std::uintptr_t：字节与地址转换。
#include <cstring>   // std::memcpy：DBWIN 缓冲拷贝。
#include <string>    // std::string/std::wstring：Win32 文本桥接。
#include <vector>    // std::vector：枚举缓存容器。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#include <winsvc.h>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Psapi.lib")

namespace
{
    // ServiceHandleGuard：
    // - 作用：SC_HANDLE 的 RAII 关闭器；
    // - 避免错误分支提前 return 时句柄泄露。
    class ServiceHandleGuard
    {
    public:
        explicit ServiceHandleGuard(SC_HANDLE handleValue = nullptr)
            : m_handle(handleValue)
        {
        }

        ~ServiceHandleGuard()
        {
            reset(nullptr);
        }

        ServiceHandleGuard(const ServiceHandleGuard&) = delete;
        ServiceHandleGuard& operator=(const ServiceHandleGuard&) = delete;

        SC_HANDLE get() const
        {
            return m_handle;
        }

        bool valid() const
        {
            return m_handle != nullptr;
        }

        void reset(SC_HANDLE newHandle)
        {
            if (m_handle != nullptr)
            {
                ::CloseServiceHandle(m_handle);
            }
            m_handle = newHandle;
        }

    private:
        SC_HANDLE m_handle = nullptr; // m_handle：当前持有的服务句柄。
    };

    // DbwinBufferPacket：
    // - 作用：DBWIN 共享内存的数据结构定义。
    struct DbwinBufferPacket
    {
        DWORD processId = 0;                            // processId：发出调试输出的进程 PID。
        char messageBuffer[4096 - sizeof(DWORD)] = {}; // messageBuffer：ANSI 调试文本。
    };

    // toWideString：
    // - 作用：QString 转 std::wstring。
    std::wstring toWideString(const QString& textValue)
    {
        return textValue.toStdWString();
    }

    // createReadOnlyItem：
    // - 作用：创建只读单元格，统一表格交互行为。
    QTableWidgetItem* createReadOnlyItem(const QString& textValue)
    {
        QTableWidgetItem* tableItem = new QTableWidgetItem(textValue);
        tableItem->setFlags(tableItem->flags() & ~Qt::ItemIsEditable);
        return tableItem;
    }

    // formatAddress：
    // - 作用：地址转固定宽度十六进制字符串。
    QString formatAddress(const std::uint64_t addressValue)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(addressValue), 16, 16, QChar('0'))
            .toUpper();
    }

    // waitServiceState：
    // - 作用：轮询等待服务达到目标状态；
    // - 参数 serviceHandle：服务句柄；
    // - 参数 targetState：目标状态值；
    // - 参数 timeoutMs：超时时间；
    // - 参数 currentStateOut：输出最终状态。
    bool waitServiceState(
        SC_HANDLE serviceHandle,
        const DWORD targetState,
        const DWORD timeoutMs,
        DWORD* currentStateOut)
    {
        if (serviceHandle == nullptr)
        {
            return false;
        }

        const auto startTick = std::chrono::steady_clock::now();
        while (true)
        {
            SERVICE_STATUS_PROCESS statusInfo{};
            DWORD bytesNeeded = 0;
            if (!::QueryServiceStatusEx(
                serviceHandle,
                SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&statusInfo),
                static_cast<DWORD>(sizeof(statusInfo)),
                &bytesNeeded))
            {
                if (currentStateOut != nullptr)
                {
                    *currentStateOut = 0;
                }
                return false;
            }

            if (currentStateOut != nullptr)
            {
                *currentStateOut = statusInfo.dwCurrentState;
            }

            if (statusInfo.dwCurrentState == targetState)
            {
                return true;
            }

            const auto nowTick = std::chrono::steady_clock::now();
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowTick - startTick);
            if (elapsedMs.count() >= static_cast<long long>(timeoutMs))
            {
                return false;
            }

            ::Sleep(120);
        }
    }
}

DriverDock::DriverDock(QWidget* parent)
    : QWidget(parent)
{
    // initEvent 用途：贯穿整个构造流程，便于按同一 GUID 追踪初始化链路。
    kLogEvent initEvent;
    info << initEvent << "[DriverDock] 开始初始化驱动页。" << eol;

    initializeUi();
    initializeConnections();

    refreshDriverServiceRecords();
    refreshLoadedKernelModuleRecords();
    updateDebugCaptureButtonState();

    info << initEvent << "[DriverDock] 驱动页初始化完成。" << eol;
}

DriverDock::~DriverDock()
{
    stopDebugOutputCapture();

    kLogEvent destroyEvent;
    info << destroyEvent << "[DriverDock] 驱动页已析构。" << eol;
}


// ============================================================
// 说明：为控制单文件体积，具体实现按功能拆分到多个 .inc 文件。
// ============================================================
#include "DriverDock.Ui.inc"
#include "DriverDock.Operation.inc"
#include "DriverDock.DebugAndUtils.inc"
