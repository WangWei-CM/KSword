#include "MemoryDock.Internal.h"

namespace ksword::memory_dock_internal
{
    // ========================================================
    // 主题样式函数：统一按钮/输入框/下拉框风格。
    // ========================================================

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

    QString buildBlueComboStyle()
    {
        return QStringLiteral(
            "QComboBox {"
            "  border: 1px solid %1;"
            "  border-radius: 3px;"
            "  padding: 2px 6px;"
            "  background: %3;"
            "  color: %4;"
            "}"
            "QComboBox:hover {"
            "  border-color: %2;"
            "}"
            "QComboBox::drop-down {"
            "  border: none;"
            "}")
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    QString buildBlueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit, QTextEdit, QPlainTextEdit {"
            "  border: 1px solid %2;"
            "  border-radius: 3px;"
            "  background: %3;"
            "  color: %4;"
            "  padding: 3px 5px;"
            "}"
            "QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus {"
            "  border: 1px solid %1;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    // 表格表头统一主题样式，确保“内存页”整体视觉与主主题贴合。
    QString buildBlueTableHeaderStyle()
    {
        return QStringLiteral(
            "QHeaderView::section {"
            "  color:%1;"
            "  background:%2;"
            "  border:1px solid %3;"
            "  padding:4px;"
            "  font-weight:600;"
            "}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    // 十六进制查看器常量：每行 16 字节，共 32 行，每页 512 字节。
    const int kHexBytesPerRow = 16;
    const int kHexRowCount = 32;
    const std::uint64_t kHexPageBytes = static_cast<std::uint64_t>(kHexBytesPerRow * kHexRowCount);

    // 模块表头文本：直接对齐进程详细信息模块页体验。
    const QStringList ModuleTreeHeaders{
        "模块路径",
        "大小",
        "数字签名",
        "入口偏移量",
        "运行状态",
        "ThreadID"
    };

    // 枚举列 -> 整数索引转换，避免代码里散落硬编码数字。
    int toModuleTreeColumnIndex(const ModuleTreeColumn column)
    {
        return static_cast<int>(column);
    }

    // PID 转 DWORD 的显式封装，避免隐式转换警告。
    DWORD toDwordPid(const std::uint32_t pid)
    {
        return static_cast<DWORD>(pid);
    }

    // 判断内存保护属性是否可读。
    bool isReadableProtect(const std::uint32_t protectValue)
    {
        if ((protectValue & PAGE_GUARD) != 0 || (protectValue & PAGE_NOACCESS) != 0)
        {
            return false;
        }
        const std::uint32_t baseProtect = protectValue & 0xFF;
        switch (baseProtect)
        {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    // 解析两位十六进制字节文本，例如 "7F"、"ff"。
    bool parseHexByte(const QString& text, std::uint8_t& valueOut)
    {
        bool parseOk = false;
        const int value = text.trimmed().toInt(&parseOk, 16);
        if (!parseOk || value < 0 || value > 0xFF)
        {
            return false;
        }
        valueOut = static_cast<std::uint8_t>(value);
        return true;
    }

    // 统一按路径加载图标并做缓存，减少重复读取系统图标带来的卡顿。
    QIcon resolveIconByPath(const QString& absolutePath, QHash<QString, QIcon>& cache)
    {
        if (absolutePath.trimmed().isEmpty())
        {
            return QIcon(":/Icon/process_main.svg");
        }

        auto foundIt = cache.find(absolutePath);
        if (foundIt != cache.end())
        {
            return foundIt.value();
        }

        QIcon resolvedIcon(absolutePath);
        if (resolvedIcon.isNull())
        {
            QFileIconProvider iconProvider;
            resolvedIcon = iconProvider.icon(QFileInfo(absolutePath));
        }
        if (resolvedIcon.isNull())
        {
            resolvedIcon = QIcon(":/Icon/process_main.svg");
        }

        cache.insert(absolutePath, resolvedIcon);
        return resolvedIcon;
    }
}
