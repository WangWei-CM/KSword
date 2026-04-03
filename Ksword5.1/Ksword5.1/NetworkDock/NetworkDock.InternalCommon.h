#pragma once

// ============================================================
// NetworkDock.InternalCommon.h
// 作用：
// 1) 统一 NetworkDock 多个 .cpp 的私有 include；
// 2) 避免每个实现文件重复维护同一批系统/Qt 头；
// 3) 作为拆分后实现文件的“编译上下文基座”。
// ============================================================

#include "NetworkDock.h"
#include "NetworkDock.InternalHelpers.h"

#include "../ProcessDock/ProcessDetailWindow.h"
#include "../UI/HexEditorWidget.h"

#include <QAction>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QModelIndex>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QThreadPool>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm> // std::min/std::max：预览长度与范围标准化。
#include <atomic>    // std::atomic_bool：跨线程状态门控。
#include <limits>    // std::numeric_limits：包长上限范围表达。
#include <string>    // std::string：日志桥接文本类型。
#include <thread>    // std::thread：长耗时请求放到后台执行。
#include <unordered_set> // std::unordered_set：ARP接口索引去重。
#include <vector>    // std::vector：批量刷新队列临时容器。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <IcmpAPI.h>
#include <Iphlpapi.h>
#include <Windns.h>
#include <Ws2tcpip.h>

#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Dnsapi.lib")
#pragma comment(lib, "Ws2_32.lib")

