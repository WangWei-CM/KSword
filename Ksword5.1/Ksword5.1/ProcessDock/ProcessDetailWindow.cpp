#include "ProcessDetailWindow.InternalCommon.h"

// ============================================================
// ProcessDetailWindow.cpp
// 作用：
// - 提供 ProcessDetailWindow 多个实现文件共享的内部常量与工具函数；
// - 保持其它 cpp 专注于成员函数，不再通过 .inc 做文本拼接。
// ============================================================

namespace process_detail_window_internal
{
    // 线程细节表头：与开发计划字段一一对应。
    const QStringList ThreadInspectHeaders{
        "ThreadID",
        "状态",
        "优先级",
        "上下文切换",
        "起始地址",
        "TEB地址",
        "亲和性",
        "寄存器",
        "R0栈边界"
    };

    int toThreadColumnIndex(const ThreadRowColumn column)
    {
        return static_cast<int>(column);
    }

    // 模块表头文本。
    const QStringList ModuleHeaders{
        "模块路径",
        "大小",
        "数字签名",
        "入口偏移量",
        "运行状态",
        "ThreadID"
    };

    int toModuleColumnIndex(const ModuleColumn column)
    {
        return static_cast<int>(column);
    }

    QString buildBlueButtonStyle()
    {
        return QStringLiteral(
            "QPushButton {"
            "  color: %1;"
            "  background: %5;"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  padding: 4px 10px;"
            "}"
            "QPushButton:hover {"
            "  background: %3;"
            "  color: #FFFFFF;"
            "  border: 1px solid %3;"
            "}"
            "QPushButton:pressed {"
            "  background: %4;"
            "  color: #FFFFFF;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueSolidHoverHex())
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(KswordTheme::SurfaceHex());
    }

    QString buildProcessDetailRootStyle()
    {
        return QStringLiteral(
            "QWidget#ProcessDetailWindowRoot{"
            "  background:%1;"
            "  color:%2;"
            "}"
            "QWidget#ProcessDetailWindowRoot QGroupBox{"
            "  border:1px solid %3;"
            "  border-radius:4px;"
            "  margin-top:8px;"
            "  padding-top:8px;"
            "  background:%4;"
            "  color:%2;"
            "}"
            "QWidget#ProcessDetailWindowRoot QGroupBox::title{"
            "  subcontrol-origin:margin;"
            "  left:8px;"
            "  padding:0 4px;"
            "  color:%2;"
            "}"
            "QWidget#ProcessDetailWindowRoot QLineEdit,"
            "QWidget#ProcessDetailWindowRoot QComboBox,"
            "QWidget#ProcessDetailWindowRoot QPlainTextEdit,"
            "QWidget#ProcessDetailWindowRoot QTextEdit{"
            "  background:%4;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  border-radius:3px;"
            "  padding:3px 6px;"
            "  selection-background-color:%5;"
            "  selection-color:#FFFFFF;"
            "}"
            "QWidget#ProcessDetailWindowRoot QLineEdit[readOnly=\"true\"]{"
            "  background:%6;"
            "}"
            "QWidget#ProcessDetailWindowRoot QComboBox::drop-down{"
            "  border:none;"
            "  width:20px;"
            "}"
            "QWidget#ProcessDetailWindowRoot QTableWidget,"
            "QWidget#ProcessDetailWindowRoot QTreeWidget{"
            "  background:%4;"
            "  alternate-background-color:%6;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  gridline-color:%3;"
            "  selection-background-color:%5;"
            "  selection-color:#FFFFFF;"
            "}"
            "QWidget#ProcessDetailWindowRoot QTableCornerButton::section{"
            "  background:%4;"
            "  border:1px solid %3;"
            "}"
            "QWidget#ProcessDetailWindowRoot QHeaderView::section{"
            "  background:%4;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  padding:4px;"
            "  font-weight:600;"
            "}"
            "QWidget#ProcessDetailWindowRoot QTabWidget::pane{"
            "  border:1px solid %3;"
            "  background:%4;"
            "}"
            "QWidget#ProcessDetailWindowRoot QTabBar::tab{"
            "  background:%4;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  border-bottom:none;"
            "  padding:6px 10px;"
            "  margin-right:1px;"
            "}"
            "QWidget#ProcessDetailWindowRoot QTabBar::tab:selected{"
            "  background:%5;"
            "  color:#FFFFFF;"
            "  border-color:%5;"
            "}"
            "QWidget#ProcessDetailWindowRoot QTabBar::tab:hover:!selected{"
            "  background:%6;"
            "}"
            "QWidget#ProcessDetailWindowRoot QMenu{"
            "  background:%4;"
            "  color:%2;"
            "  border:1px solid %3;"
            "}"
            "QWidget#ProcessDetailWindowRoot QMenu::item{"
            "  padding:5px 24px 5px 24px;"
            "  background:transparent;"
            "}"
            "QWidget#ProcessDetailWindowRoot QMenu::item:selected{"
            "  background:%5;"
            "  color:#FFFFFF;"
            "}"
            "QWidget#ProcessDetailWindowRoot QMenu::item:disabled{"
            "  color:%6;"
            "  background:%4;"
            "}"
            "QWidget#ProcessDetailWindowRoot QMenu::separator{"
            "  height:1px;"
            "  background:%3;"
            "  margin:2px 6px;"
            "}"
            "QWidget#ProcessDetailWindowRoot QScrollBar:vertical{"
            "  background:%4;"
            "  width:12px;"
            "  margin:0;"
            "}"
            "QWidget#ProcessDetailWindowRoot QScrollBar:horizontal{"
            "  background:%4;"
            "  height:12px;"
            "  margin:0;"
            "}"
            "QWidget#ProcessDetailWindowRoot QScrollBar::handle:vertical,"
            "QWidget#ProcessDetailWindowRoot QScrollBar::handle:horizontal{"
            "  background:%5;"
            "  min-height:20px;"
            "  min-width:20px;"
            "  border-radius:4px;"
            "}"
            "QWidget#ProcessDetailWindowRoot QScrollBar::handle:vertical:hover,"
            "QWidget#ProcessDetailWindowRoot QScrollBar::handle:horizontal:hover{"
            "  background:%1;"
            "}"
            "QWidget#ProcessDetailWindowRoot QScrollBar::add-line,"
            "QWidget#ProcessDetailWindowRoot QScrollBar::sub-line{"
            "  background:%4;"
            "  border:none;"
            "}"
            "QWidget#ProcessDetailWindowRoot QScrollBar::add-page,"
            "QWidget#ProcessDetailWindowRoot QScrollBar::sub-page{"
            "  background:%4;"
            "}")
            .arg(QStringLiteral("palette(window)"))
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueSubtleHex());
    }

    QString buildProcessDetailMenuStyle()
    {
        // 菜单样式必须显式声明背景/文字/选中/禁用态，避免透明父控件导致黑底黑字。
        return QStringLiteral(
            "QMenu{"
            "  background:%1;"
            "  color:%2;"
            "  border:1px solid %3;"
            "  padding:4px;"
            "}"
            "QMenu::item{"
            "  padding:5px 24px 5px 24px;"
            "  background:transparent;"
            "}"
            "QMenu::item:selected{"
            "  background:%4;"
            "  color:#FFFFFF;"
            "}"
            "QMenu::item:disabled{"
            "  color:%5;"
            "  background:%1;"
            "}"
            "QMenu::separator{"
            "  height:1px;"
            "  background:%3;"
            "  margin:3px 6px;"
            "}")
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::TextSecondaryHex());
    }

    QIcon buildProcessDetailR0ActionIcon(const QString& iconPath)
    {
        // R0 图标构造：
        // - 先加载普通业务图标；
        // - 再叠加 qrc 中的 Kernel.png，满足 R0 入口统一视觉标识要求。
        constexpr QSize detailR0IconSize(18, 18);
        QPixmap iconPixmap(iconPath);
        if (!iconPixmap.isNull())
        {
            iconPixmap = iconPixmap.scaled(
                detailR0IconSize,
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation);
        }
        if (iconPixmap.isNull())
        {
            iconPixmap = QPixmap(detailR0IconSize);
            iconPixmap.fill(Qt::transparent);
        }

        const QPixmap kernelPixmap(QStringLiteral(":/Image/kernel_badge.png"));
        if (!kernelPixmap.isNull())
        {
            const int badgeSide = std::max(8, std::min(iconPixmap.width(), iconPixmap.height()) / 2);
            const QPixmap scaledBadge = kernelPixmap.scaled(
                badgeSide,
                badgeSide,
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation);

            QPainter painter(&iconPixmap);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.drawPixmap(
                iconPixmap.width() - scaledBadge.width(),
                iconPixmap.height() - scaledBadge.height(),
                scaledBadge);
            painter.end();
        }

        return QIcon(iconPixmap);
    }

    QString buildStateLabelStyle(const QColor& textColor, const int fontWeight)
    {
        return QStringLiteral("color:%1; font-weight:%2;")
            .arg(textColor.name(QColor::HexRgb))
            .arg(fontWeight);
    }

    QColor statusIdleColor()
    {
        return KswordTheme::IsDarkModeEnabled() ? QColor(146, 214, 156) : QColor(47, 125, 50);
    }

    QColor statusWarningColor()
    {
        return KswordTheme::IsDarkModeEnabled() ? QColor(255, 205, 130) : QColor(138, 109, 59);
    }

    QColor statusErrorColor()
    {
        return KswordTheme::IsDarkModeEnabled() ? QColor(255, 145, 145) : QColor(220, 50, 47);
    }

    QColor statusSecondaryColor()
    {
        return KswordTheme::IsDarkModeEnabled() ? QColor(178, 178, 178) : QColor(79, 79, 79);
    }

    QColor signatureTrustedColor()
    {
        return KswordTheme::IsDarkModeEnabled() ? QColor(130, 210, 140) : QColor(34, 139, 34);
    }

    QColor signatureUntrustedColor()
    {
        return KswordTheme::IsDarkModeEnabled() ? QColor(255, 155, 155) : QColor(220, 50, 47);
    }

    QString formatDoubleText(const double value, const int precision)
    {
        return QString::number(value, 'f', precision);
    }

    QString uint64ToHex(const std::uint64_t value)
    {
        return QString("0x%1").arg(static_cast<qulonglong>(value), 0, 16).toUpper();
    }

    QString convertSidToText(PSID sid)
    {
        if (sid == nullptr)
        {
            return QStringLiteral("<null sid>");
        }

        WCHAR accountName[256] = {};
        WCHAR domainName[256] = {};
        DWORD accountNameLength = static_cast<DWORD>(std::size(accountName));
        DWORD domainNameLength = static_cast<DWORD>(std::size(domainName));
        SID_NAME_USE sidType = SidTypeUnknown;
        const BOOL accountOk = LookupAccountSidW(
            nullptr,
            sid,
            accountName,
            &accountNameLength,
            domainName,
            &domainNameLength,
            &sidType);

        LPWSTR sidTextRaw = nullptr;
        const BOOL sidTextOk = ConvertSidToStringSidW(sid, &sidTextRaw);
        QString sidText = sidTextOk && sidTextRaw != nullptr
            ? QString::fromWCharArray(sidTextRaw)
            : QStringLiteral("N/A");
        if (sidTextRaw != nullptr)
        {
            LocalFree(sidTextRaw);
            sidTextRaw = nullptr;
        }

        if (accountOk == FALSE)
        {
            return QString("SID=%1").arg(sidText);
        }

        return QString("%1\\%2 (SID=%3)")
            .arg(QString::fromWCharArray(domainName))
            .arg(QString::fromWCharArray(accountName))
            .arg(sidText);
    }

    QString readRemoteUnicodeString(HANDLE processHandle, const UNICODE_STRING& remoteUnicode)
    {
        if (processHandle == nullptr || remoteUnicode.Length == 0 || remoteUnicode.Buffer == nullptr)
        {
            return QString();
        }

        std::vector<wchar_t> buffer(
            static_cast<std::size_t>(remoteUnicode.Length / sizeof(wchar_t)) + 1,
            L'\0');
        SIZE_T bytesRead = 0;
        const BOOL readOk = ReadProcessMemory(
            processHandle,
            remoteUnicode.Buffer,
            buffer.data(),
            remoteUnicode.Length,
            &bytesRead);
        if (readOk == FALSE || bytesRead == 0)
        {
            return QString();
        }

        return QString::fromWCharArray(buffer.data());
    }
}
