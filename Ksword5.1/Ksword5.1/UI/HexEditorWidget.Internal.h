#pragma once

// ============================================================
// HexEditorWidget.Internal.h
// 作用：
// - 汇总 HexEditorWidget 多个编译单元共享的 Qt include 与内部工具声明；
// - 替代旧的文本拼接式实现，让 UI 和工具逻辑以真实 .cpp 参与构建；
// - 仅供 HexEditorWidget 内部实现使用，外部模块继续包含 HexEditorWidget.h。
// ============================================================

#include "HexEditorWidget.h"
#include "CodeEditorWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QChar>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIODevice>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QModelIndex>
#include <QModelIndexList>
#include <QPoint>
#include <QPointer>
#include <QPushButton>
#include <QRect>
#include <QRegularExpression>
#include <QShortcut>
#include <QStringList>
#include <QStringConverter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace ksword::ui::hex_editor_internal
{
    // 样式函数：输入为空，返回当前主题下的样式表文本。
    QString buildToolbarButtonStyle();
    QString buildInputStyle();
    QString buildHeaderStyle();
    QString buildMenuStyle();

    // 颜色函数：输入为空，返回查找命中或选区高亮颜色。
    QColor buildMatchColor();
    QColor buildCurrentMatchColor();
    QColor buildSelectionColor();

    // byteToHexText：输入单字节值，返回两位大写 HEX 文本。
    QString byteToHexText(std::uint8_t byteValue);

    // normalizeBytesPerRow：输入请求行宽，返回安全夹取后的行宽。
    int normalizeBytesPerRow(int requestedBytesPerRow);
}
