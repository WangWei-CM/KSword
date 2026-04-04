#pragma once

// ============================================================
// StartupDock.Internal.h
// 作用：
// 1) 统一 StartupDock 多个实现文件的公共 include；
// 2) 声明内部工具函数，避免跨 .cpp 重复实现；
// 3) 保持 StartupDock 目录内实现解耦。
// ============================================================

#include "StartupDock.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <Windows.h>

#include <algorithm>  // std::sort/std::clamp：结果排序与范围控制。
#include <array>      // std::array：固定大小注册表键清单。
#include <cstdint>    // std::uint*_t：系统状态字段。
#include <memory>     // std::unique_ptr：局部对象托管。
#include <optional>   // std::optional：可选字段判断。
#include <string>     // std::string：Win32/Qt 文本桥接。
#include <vector>     // std::vector：启动项缓存。

namespace startup_dock_detail
{
    // StartupTreeNodeKind：
    // - 作用：区分注册表树中的“位置节点 / 条目节点 / 占位节点”。
    enum class StartupTreeNodeKind : int
    {
        Group = 0,      // Group：一级注册表位置节点。
        Entry,          // Entry：实际启动项叶子节点。
        Placeholder     // Placeholder：无条目/无匹配项占位节点。
    };

    // 树节点数据角色：
    // - 统一保存条目索引、节点类型与注册表路径；
    // - UI 和交互逻辑共用，避免散落魔法数字。
    inline constexpr int kStartupEntryIndexRole = Qt::UserRole;
    inline constexpr int kStartupTreeNodeKindRole = Qt::UserRole + 1;
    inline constexpr int kStartupTreeLocationRole = Qt::UserRole + 2;

    // createBlueIcon 作用：
    // - 生成和现有 Dock 一致的蓝色 SVG 图标。
    QIcon createBlueIcon(const char* resourcePath, const QSize& iconSize = QSize(16, 16));

    // createReadOnlyItem 作用：
    // - 统一创建不可编辑表格单元格。
    QTableWidgetItem* createReadOnlyItem(const QString& textValue);

    // winErrorText 作用：
    // - 把 Win32 错误码转换为可读文本。
    QString winErrorText(DWORD errorCode);

    // normalizeFilePathText 作用：
    // - 从命令行文本中尽量提取可执行文件路径。
    QString normalizeFilePathText(const QString& commandText);

    // queryPublisherTextByPath 作用：
    // - 读取文件签名/公司名，返回发布者显示文本；
    // - 失败时返回空串。
    QString queryPublisherTextByPath(const QString& filePathText);

    // buildStatusText 作用：
    // - 把启用状态转成统一文本。
    QString buildStatusText(bool enabled);

    // parseCsvLine 作用：
    // - 解析一行 CSV 文本，支持引号转义；
    // - 供 `schtasks` 输出解析复用。
    QStringList parseCsvLine(const QString& csvLineText);

    // buildKnownStartupRegistryLocationList 作用：
    // - 返回注册表树应展示的已知 Autoruns 风格位置列表；
    // - 注册表树会按该顺序创建默认展开的一级节点。
    QStringList buildKnownStartupRegistryLocationList();
}
