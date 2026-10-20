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
#include <Objbase.h>

#include <algorithm>  // std::sort：过滤后排序。
#include <cstdint>    // std::uint8_t：Win32 缓冲区字节容器。
#include <optional>   // std::optional：可选错误文本返回。
#include <string>     // std::string：日志与 Win32 文本桥接。
#include <utility>    // std::pair：详情页键值构造。

namespace service_dock_detail
{
    // RegistryServiceSnapshot 作用：
    // - 承载 HKLM\SYSTEM\CurrentControlSet\Services 下单个键的独立快照；
    // - 与 SCM ServiceRecord 分离，确保幽灵服务检测保留独立数据来源。
    struct RegistryServiceSnapshot
    {
        QString serviceNameText;       // serviceNameText：Services 根键下的子键名。
        QString displayNameText;       // displayNameText：注册表 DisplayName 原始文本。
        QString descriptionText;       // descriptionText：注册表 Description 原始文本。
        QString binaryPathText;        // binaryPathText：注册表 ImagePath 原始文本。
        QString serviceDllPathText;    // serviceDllPathText：Parameters\\ServiceDll 原始文本。
        QString accountText;           // accountText：注册表 ObjectName 原始文本。
        DWORD serviceTypeValue = 0;    // serviceTypeValue：Type DWORD 值。
        DWORD startTypeValue = 0;      // startTypeValue：Start DWORD 值。
        DWORD errorControlValue = 0;   // errorControlValue：ErrorControl DWORD 值。
        bool delayedAutoStart = false; // delayedAutoStart：DelayedAutostart DWORD 状态。
        bool keyReadable = false;      // keyReadable：子键是否成功以 KEY_READ 打开。
        bool hasServiceType = false;   // hasServiceType：Type 值是否存在且类型正确。
        bool hasStartType = false;     // hasStartType：Start 值是否存在且类型正确。
        bool hasErrorControl = false;  // hasErrorControl：ErrorControl 值是否存在且类型正确。
    };

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

    // enumerateRegistryServiceSnapshots 作用：独立枚举 Services 注册表根键。
    // 入参：snapshotListOut 接收全部子键；errorTextOut/errorCodeOut 接收根扫描错误。
    // 返回：完整枚举成功返回 true；单个受保护子键仍会以 keyReadable=false 保留名字。
    bool enumerateRegistryServiceSnapshots(
        std::vector<RegistryServiceSnapshot>* snapshotListOut,
        QString* errorTextOut = nullptr,
        DWORD* errorCodeOut = nullptr,
        const std::function<bool()>& shouldStop = {});

    // queryRegistryServiceSnapshot 作用：读取单个服务注册表配置快照。
    // 入参：serviceNameText 为不含路径分隔符的服务短名；snapshotOut 接收快照。
    // 返回：键存在且可读时返回 true。
    bool queryRegistryServiceSnapshot(
        const QString& serviceNameText,
        RegistryServiceSnapshot* snapshotOut,
        QString* errorTextOut = nullptr,
        DWORD* errorCodeOut = nullptr);

    // deleteRegistryServiceKey 作用：删除 SCM 不可见服务的完整注册表键树。
    // 入参：serviceNameText 为经过校验的服务短名；错误输出用于 UI 提权恢复提示。
    // 返回：键已删除、已不存在时返回 true。
    bool deleteRegistryServiceKey(
        const QString& serviceNameText,
        QString* errorTextOut = nullptr,
        DWORD* errorCodeOut = nullptr);

    // querySingleServiceSnapshot 作用：后台刷新单条 SCM/注册表服务快照。
    // 入参中的 sourceScmRecordPresent/sourceRegistryScanCompleted 保留全量交叉扫描语义。
    // 返回：任一可信来源仍能提供目标快照时返回 true。
    bool querySingleServiceSnapshot(
        const QString& serviceNameText,
        bool sourceScmRecordPresent,
        bool sourceRegistryScanCompleted,
        ServiceDock::ServiceEntry* entryOut,
        QString* errorTextOut);
}
