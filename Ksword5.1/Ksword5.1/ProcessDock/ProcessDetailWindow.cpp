#include "ProcessDetailWindow.h"

#include "../theme.h"

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
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QThreadPool>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

namespace
{
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
            "  background: #FFFFFF;"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  padding: 4px 10px;"
            "}"
            "QPushButton:hover {"
            "  background: %3;"
            "}"
            "QPushButton:pressed {"
            "  background: %4;"
            "  color: #FFFFFF;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueHoverHex)
            .arg(KswordTheme::PrimaryBluePressedHex);
    }

    // 格式化双精度到固定小数位字符串。
    QString formatDoubleText(const double value, const int precision)
    {
        return QString::number(value, 'f', precision);
    }
}


// ============================================================
// 说明：ProcessDetailWindow.cpp 作为聚合入口，
// 将实现拆分到多个 .inc 文件，便于按功能维护与审查。
// ============================================================

#include "ProcessDetailWindow.BaseAndUi.inc"
#include "ProcessDetailWindow.Module.inc"
#include "ProcessDetailWindow.ActionAndUtil.inc"
