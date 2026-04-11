#pragma once

// ============================================================
// ServiceDock.Internal.h
// 作用：
// 1) 统一 ServiceDock 多实现文件共享 include；
// 2) 声明内部工具函数，避免跨文件重复实现；
// 3) 约束内部角色常量，减少魔法值散落。
// ============================================================

#include "ServiceDock.h"
#include "../UI/CodeEditorWidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QSpinBox>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <Windows.h>

#include <algorithm>  // std::sort：过滤后排序。
#include <cstdint>    // std::uint8_t：Win32 缓冲区字节容器。
#include <optional>   // std::optional：可选错误文本返回。
#include <string>     // std::string：日志与 Win32 文本桥接。
#include <utility>    // std::pair：详情页键值构造。

namespace service_dock_detail
{
    // 列表行绑定角色定义：
    // - kServiceNameRole：在首列 item 上保存服务短名，供选择映射。
    inline constexpr int kServiceNameRole = Qt::UserRole;

    // createBlueIcon 作用：
    // - 把 SVG 渲染为统一蓝色主题图标。
    QIcon createBlueIcon(const char* resourcePath, const QSize& iconSize = QSize(16, 16));

    // createReadOnlyItem 作用：
    // - 构造不可编辑、带 tooltip 的表格项。
    QTableWidgetItem* createReadOnlyItem(const QString& textValue);

    // winErrorText 作用：
    // - 把 Win32 错误码转换为可读字符串。
    QString winErrorText(DWORD errorCode);

    // serviceStateToText 作用：
    // - 把服务运行状态值转为中文文本。
    QString serviceStateToText(DWORD stateValue);

    // startTypeToText 作用：
    // - 把启动类型值与延迟标记转为中文文本。
    QString startTypeToText(DWORD startTypeValue, bool delayedAutoStart);

    // serviceTypeToText 作用：
    // - 把服务类型位掩码转为中文文本。
    QString serviceTypeToText(DWORD serviceTypeValue);

    // errorControlToText 作用：
    // - 把错误控制值转为中文文本。
    QString errorControlToText(DWORD errorControlValue);

    // isServiceStatePending 作用：
    // - 判断服务是否处于 Pending 过渡状态。
    bool isServiceStatePending(DWORD stateValue);

    // normalizeServiceImagePath 作用：
    // - 从 BinaryPath 文本中提取可执行路径。
    QString normalizeServiceImagePath(const QString& rawBinaryPathText);
}
