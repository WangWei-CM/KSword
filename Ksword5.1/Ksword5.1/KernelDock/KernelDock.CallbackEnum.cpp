#include "KernelDock.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../FileDock/FilePropertyPeAnalyzer.h"
#include "../UI/CodeEditorWidget.h"
#include "../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMetaObject>
#include <QPoint>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QStringList>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    QString callbackEnumButtonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    QString callbackEnumInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %2;border-radius:2px;background:%3;color:%4;padding:2px 6px;}"
            "QLineEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    QString callbackEnumHeaderStyle()
    {
        return QStringLiteral(
            "QHeaderView::section{color:%1;background:%2;border:1px solid %3;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    QString callbackEnumSelectionStyle()
    {
        return QStringLiteral("QTableWidget::item:selected{background:%1;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex);
    }

    QString callbackEnumStatusLabelStyle(const QString& colorHex)
    {
        return QStringLiteral("color:%1;font-weight:600;").arg(colorHex);
    }

    QString callbackEnumSafeText(const QString& valueText, const QString& fallbackText = QStringLiteral("<空>"))
    {
        return valueText.trimmed().isEmpty() ? fallbackText : valueText;
    }

    QString callbackEnumFormatAddress(const std::uint64_t value)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 16, 16, QChar('0'))
            .toUpper();
    }

    QString callbackEnumWindowsDirectoryPath()
    {
        // 作用：解析 Windows 目录，用于把 \SystemRoot\xxx 转成可打开的 Win32 路径。
        // 返回：Windows 目录绝对路径；失败时回退环境变量。
        wchar_t windowsPathBuffer[MAX_PATH] = {};
        const UINT copiedChars = ::GetWindowsDirectoryW(windowsPathBuffer, MAX_PATH);
        if (copiedChars > 0U && copiedChars < MAX_PATH)
        {
            return QDir::toNativeSeparators(QString::fromWCharArray(windowsPathBuffer));
        }

        const QString envPath = qEnvironmentVariable("SystemRoot");
        return envPath.isEmpty()
            ? QStringLiteral("C:\\Windows")
            : QDir::toNativeSeparators(envPath);
    }

    QString callbackEnumSystemDrivePrefix()
    {
        // 作用：从 Windows 目录中提取系统盘符，处理 \Windows\xxx 这类内核路径。
        // 返回：形如 C: 的盘符；无法判断时返回 C:。
        const QString windowsPath = callbackEnumWindowsDirectoryPath();
        if (windowsPath.size() >= 2 && windowsPath.at(1) == QLatin1Char(':'))
        {
            return windowsPath.left(2);
        }
        return QStringLiteral("C:");
    }

    QString callbackEnumMapNtDevicePathToDosPath(const QString& ntPathText)
    {
        // 作用：尝试把 \Device\HarddiskVolumeX\... 映射为 C:\...。
        // 返回：映射成功返回 Win32 路径；失败返回空字符串。
        const QString normalizedNtPath = QDir::toNativeSeparators(ntPathText.trimmed());
        if (!normalizedNtPath.startsWith(QStringLiteral("\\Device\\"), Qt::CaseInsensitive))
        {
            return QString();
        }

        for (wchar_t driveLetter = L'A'; driveLetter <= L'Z'; ++driveLetter)
        {
            const QString driveName = QStringLiteral("%1:").arg(QChar(driveLetter));
            wchar_t deviceNameBuffer[1024] = {};
            const DWORD copiedChars = ::QueryDosDeviceW(
                reinterpret_cast<LPCWSTR>(driveName.utf16()),
                deviceNameBuffer,
                static_cast<DWORD>(sizeof(deviceNameBuffer) / sizeof(deviceNameBuffer[0])));
            if (copiedChars == 0U)
            {
                continue;
            }

            const QString deviceName = QDir::toNativeSeparators(QString::fromWCharArray(deviceNameBuffer));
            if (deviceName.isEmpty() || !normalizedNtPath.startsWith(deviceName, Qt::CaseInsensitive))
            {
                continue;
            }

            const QString suffixText = normalizedNtPath.mid(deviceName.size());
            return QDir::toNativeSeparators(driveName + suffixText);
        }

        return QString();
    }

    QString callbackEnumNormalizeModulePath(const QString& modulePathText)
    {
        // 作用：把 R0 返回的模块路径规范化为 R3 可访问的 Win32 文件路径。
        // 返回：可访问 Win32 路径；无法转换时返回空字符串。
        QString pathText = modulePathText.trimmed();
        if (pathText.isEmpty() || pathText == QStringLiteral("<未解析>"))
        {
            return QString();
        }

        pathText = QDir::toNativeSeparators(pathText);
        if (pathText.startsWith(QStringLiteral("\\??\\"), Qt::CaseInsensitive))
        {
            pathText = pathText.mid(4);
        }
        if (pathText.startsWith(QStringLiteral("\\SystemRoot\\"), Qt::CaseInsensitive))
        {
            pathText = callbackEnumWindowsDirectoryPath() + pathText.mid(QStringLiteral("\\SystemRoot").size());
        }
        else if (pathText.startsWith(QStringLiteral("SystemRoot\\"), Qt::CaseInsensitive))
        {
            pathText = callbackEnumWindowsDirectoryPath() + QStringLiteral("\\") + pathText.mid(QStringLiteral("SystemRoot\\").size());
        }
        else if (pathText.startsWith(QStringLiteral("\\Windows\\"), Qt::CaseInsensitive))
        {
            pathText = callbackEnumSystemDrivePrefix() + pathText;
        }
        else if (pathText.startsWith(QStringLiteral("\\Device\\"), Qt::CaseInsensitive))
        {
            pathText = callbackEnumMapNtDevicePathToDosPath(pathText);
        }

        if (pathText.size() >= 2 && pathText.at(1) == QLatin1Char(':'))
        {
            const QFileInfo fileInfo(pathText);
            return fileInfo.exists() ? fileInfo.absoluteFilePath() : QDir::toNativeSeparators(pathText);
        }
        return QString();
    }

    QString callbackEnumBuildModuleFileGeneralText(const QString& filePath)
    {
        // 作用：生成模块文件详情窗口的常规信息页。
        // 返回：包含路径、大小和时间戳的纯文本。
        const QFileInfo fileInfo(filePath);
        QString detailText;
        detailText += QStringLiteral("文件路径：%1\n").arg(QDir::toNativeSeparators(fileInfo.absoluteFilePath()));
        detailText += QStringLiteral("文件名：%1\n").arg(fileInfo.fileName());
        detailText += QStringLiteral("所在目录：%1\n").arg(QDir::toNativeSeparators(fileInfo.absolutePath()));
        detailText += QStringLiteral("是否存在：%1\n").arg(fileInfo.exists() ? QStringLiteral("是") : QStringLiteral("否"));
        detailText += QStringLiteral("大小：%1 字节\n").arg(fileInfo.exists() ? QString::number(fileInfo.size()) : QStringLiteral("<不可用>"));
        detailText += QStringLiteral("创建时间：%1\n").arg(fileInfo.birthTime().isValid() ? fileInfo.birthTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) : QStringLiteral("<不可用>"));
        detailText += QStringLiteral("修改时间：%1\n").arg(fileInfo.lastModified().isValid() ? fileInfo.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) : QStringLiteral("<不可用>"));
        detailText += QStringLiteral("访问时间：%1\n").arg(fileInfo.lastRead().isValid() ? fileInfo.lastRead().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) : QStringLiteral("<不可用>"));
        detailText += QStringLiteral("可读：%1\n").arg(fileInfo.isReadable() ? QStringLiteral("是") : QStringLiteral("否"));
        detailText += QStringLiteral("可写：%1\n").arg(fileInfo.isWritable() ? QStringLiteral("是") : QStringLiteral("否"));
        detailText += QStringLiteral("可执行：%1\n").arg(fileInfo.isExecutable() ? QStringLiteral("是") : QStringLiteral("否"));
        return detailText;
    }

    void callbackEnumShowModuleFileDetailDialog(QWidget* parentWidget, const QString& filePath)
    {
        // 作用：弹出模块文件详细信息窗口，复用 FileDock 的 PE 解析报告。
        // 返回：无；窗口为模态，关闭后自动释放局部对象。
        QDialog detailDialog(parentWidget);
        detailDialog.setObjectName(QStringLiteral("CallbackEnumModuleFileDetailDialog"));
        detailDialog.setWindowTitle(QStringLiteral("模块文件详细信息 - %1").arg(QFileInfo(filePath).fileName()));
        detailDialog.resize(980, 680);
        detailDialog.setStyleSheet(KswordTheme::OpaqueDialogStyle(detailDialog.objectName()));

        QVBoxLayout* rootLayout = new QVBoxLayout(&detailDialog);
        QTabWidget* tabWidget = new QTabWidget(&detailDialog);
        rootLayout->addWidget(tabWidget, 1);

        CodeEditorWidget* generalEditor = new CodeEditorWidget(&detailDialog);
        generalEditor->setReadOnly(true);
        generalEditor->setText(callbackEnumBuildModuleFileGeneralText(filePath));
        tabWidget->addTab(generalEditor, QStringLiteral("常规信息"));

        CodeEditorWidget* peEditor = new CodeEditorWidget(&detailDialog);
        peEditor->setReadOnly(true);
        peEditor->setText(file_dock_detail::buildPeAnalysisText(filePath));
        tabWidget->addTab(peEditor, QStringLiteral("PE信息"));

        QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &detailDialog);
        QObject::connect(buttonBox, &QDialogButtonBox::rejected, &detailDialog, &QDialog::reject);
        QObject::connect(buttonBox, &QDialogButtonBox::accepted, &detailDialog, &QDialog::accept);
        rootLayout->addWidget(buttonBox, 0);
        detailDialog.exec();
    }

    bool callbackEnumOpenModuleInExplorer(const QString& filePath)
    {
        // 作用：用 Explorer 定位模块文件，失败时返回 false 让调用方更新状态栏。
        // 返回：成功启动 Explorer 返回 true。
        if (filePath.trimmed().isEmpty())
        {
            return false;
        }
        const QString nativePath = QDir::toNativeSeparators(filePath);
        const QString selectArgument = QStringLiteral("/select,\"%1\"").arg(nativePath);
        return QProcess::startDetached(QStringLiteral("explorer.exe"), QStringList{ selectArgument });
    }

    QString callbackEnumClassText(const std::uint32_t callbackClass)
    {
        switch (callbackClass)
        {
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY:
            return QStringLiteral("注册表 CmCallback");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS:
            return QStringLiteral("进程 Notify");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_THREAD:
            return QStringLiteral("线程 Notify");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE:
            return QStringLiteral("镜像加载 Notify");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT:
            return QStringLiteral("Object Callback");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER:
            return QStringLiteral("Minifilter");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT:
            return QStringLiteral("WFP Callout");
        case KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER:
            return QStringLiteral("ETW Provider/Consumer");
        default:
            return QStringLiteral("未知(%1)").arg(callbackClass);
        }
    }

    QString callbackEnumSourceText(const std::uint32_t source)
    {
        switch (source)
        {
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_KSWORD_SELF:
            return QStringLiteral("Ksword 自身注册");
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_FLTMGR_ENUMERATION:
            return QStringLiteral("FltMgr 公开枚举");
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_UNSUPPORTED:
            return QStringLiteral("私有结构诊断");
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_PATTERN_SCAN:
            return QStringLiteral("私有特征定位");
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_NOTIFY_ARRAY:
            return QStringLiteral("Psp Notify 数组");
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_REGISTRY_LIST:
            return QStringLiteral("Cm 回调链表");
        case KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_OBJECT_TYPE_LIST:
            return QStringLiteral("Ob 对象类型链表");
        default:
            return QStringLiteral("未知(%1)").arg(source);
        }
    }

    QString callbackEnumPrimaryAddressText(const KernelCallbackEnumEntry& entry)
    {
        // 作用：根据 fieldFlags 选择表格主地址，避免把定位/诊断行误显示为 0 地址。
        // 返回：真实回调地址、全局/节点地址、诊断地址或“无回调地址”占位文本。
        if ((entry.fieldFlags & KSWORD_ARK_CALLBACK_ENUM_FIELD_CALLBACK_ADDRESS) != 0U
            && entry.callbackAddress != 0U)
        {
            return callbackEnumFormatAddress(entry.callbackAddress);
        }
        if ((entry.fieldFlags & KSWORD_ARK_CALLBACK_ENUM_FIELD_REGISTRATION_ADDRESS) != 0U
            && entry.registrationAddress != 0U)
        {
            const bool locateRow = entry.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_PATTERN_SCAN;
            return QStringLiteral("%1 %2")
                .arg(locateRow ? QStringLiteral("全局") : QStringLiteral("节点"))
                .arg(callbackEnumFormatAddress(entry.registrationAddress));
        }
        if ((entry.fieldFlags & KSWORD_ARK_CALLBACK_ENUM_FIELD_CONTEXT_ADDRESS) != 0U
            && entry.contextAddress != 0U)
        {
            return QStringLiteral("诊断 %1").arg(callbackEnumFormatAddress(entry.contextAddress));
        }
        return QStringLiteral("<无回调地址>");
    }

    QString callbackEnumRowStatusText(const std::uint32_t status, const long lastStatus)
    {
        switch (status)
        {
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_OK:
            return QStringLiteral("可见/成功");
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_NOT_REGISTERED:
            return QStringLiteral("未注册");
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_UNSUPPORTED:
            return QStringLiteral("当前不支持");
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_QUERY_FAILED:
            return QStringLiteral("查询失败(0x%1)")
                .arg(QString::number(static_cast<quint32>(lastStatus), 16).rightJustified(8, QLatin1Char('0')).toUpper());
        case KSWORD_ARK_CALLBACK_ENUM_STATUS_BUFFER_TRUNCATED:
            return QStringLiteral("缓冲截断");
        default:
            return QStringLiteral("未知(%1)").arg(status);
        }
    }

    KernelCallbackEnumEntry callbackEnumConvertEntry(const ksword::ark::CallbackEnumEntry& source)
    {
        KernelCallbackEnumEntry row;
        row.callbackClass = source.callbackClass;
        row.source = source.source;
        row.status = source.status;
        row.fieldFlags = source.fieldFlags;
        row.operationMask = source.operationMask;
        row.objectTypeMask = source.objectTypeMask;
        row.lastStatus = source.lastStatus;
        row.callbackAddress = source.callbackAddress;
        row.contextAddress = source.contextAddress;
        row.registrationAddress = source.registrationAddress;
        row.moduleBase = source.moduleBase;
        row.moduleSize = source.moduleSize;
        row.classText = callbackEnumClassText(source.callbackClass);
        row.sourceText = callbackEnumSourceText(source.source);
        row.statusText = callbackEnumRowStatusText(source.status, source.lastStatus);
        row.nameText = QString::fromStdWString(source.name);
        row.altitudeText = QString::fromStdWString(source.altitude);
        row.modulePathText = QString::fromStdWString(source.modulePath);
        row.detailText = QString::fromStdWString(source.detail);
        return row;
    }

    enum class CallbackEnumColumn : int
    {
        Class = 0,
        Source,
        Status,
        Name,
        CallbackAddress,
        Module,
        Altitude,
        Count
    };

    QString callbackEnumColumnHeaderText(const CallbackEnumColumn column)
    {
        // 作用：把回调遍历表格列枚举映射为右键菜单和剪贴板表头文本。
        // 返回：该列对应的中文表头；未知列返回“未知列”。
        switch (column)
        {
        case CallbackEnumColumn::Class:
            return QStringLiteral("类别");
        case CallbackEnumColumn::Source:
            return QStringLiteral("来源");
        case CallbackEnumColumn::Status:
            return QStringLiteral("状态");
        case CallbackEnumColumn::Name:
            return QStringLiteral("名称");
        case CallbackEnumColumn::CallbackAddress:
            return QStringLiteral("回调/对象地址");
        case CallbackEnumColumn::Module:
            return QStringLiteral("模块");
        case CallbackEnumColumn::Altitude:
            return QStringLiteral("Altitude");
        default:
            return QStringLiteral("未知列");
        }
    }

    QString callbackEnumEntryColumnText(
        const KernelCallbackEnumEntry& entry,
        const CallbackEnumColumn column)
    {
        // 作用：从缓存行中提取指定表格列文本，保证复制菜单不依赖当前单元格对象。
        // 返回：可直接写入剪贴板的单列文本。
        switch (column)
        {
        case CallbackEnumColumn::Class:
            return entry.classText;
        case CallbackEnumColumn::Source:
            return entry.sourceText;
        case CallbackEnumColumn::Status:
            return entry.statusText;
        case CallbackEnumColumn::Name:
            return callbackEnumSafeText(entry.nameText);
        case CallbackEnumColumn::CallbackAddress:
            return callbackEnumPrimaryAddressText(entry);
        case CallbackEnumColumn::Module:
            return entry.modulePathText.isEmpty() ? QStringLiteral("<未解析>") : entry.modulePathText;
        case CallbackEnumColumn::Altitude:
            return callbackEnumSafeText(entry.altitudeText);
        default:
            return QString();
        }
    }

    QString callbackEnumEntryAsTsv(const KernelCallbackEnumEntry& entry)
    {
        // 作用：把一条回调遍历记录按表格列顺序序列化为 TSV。
        // 返回：单行 TSV，不包含换行符。
        QStringList fieldList;
        fieldList.reserve(static_cast<int>(CallbackEnumColumn::Count));
        for (int columnIndex = 0; columnIndex < static_cast<int>(CallbackEnumColumn::Count); ++columnIndex)
        {
            fieldList.push_back(callbackEnumEntryColumnText(
                entry,
                static_cast<CallbackEnumColumn>(columnIndex)));
        }
        return fieldList.join('\t');
    }

    QString callbackEnumHeaderAsTsv()
    {
        // 作用：生成回调遍历表头 TSV，配合“复制表头+选中行”使用。
        // 返回：表头单行 TSV。
        QStringList headerList;
        headerList.reserve(static_cast<int>(CallbackEnumColumn::Count));
        for (int columnIndex = 0; columnIndex < static_cast<int>(CallbackEnumColumn::Count); ++columnIndex)
        {
            headerList.push_back(callbackEnumColumnHeaderText(static_cast<CallbackEnumColumn>(columnIndex)));
        }
        return headerList.join('\t');
    }

    std::vector<int> callbackEnumSelectedVisualRows(
        const QTableWidget* tableWidget,
        const int fallbackRow)
    {
        // 作用：收集当前可视表格中所有选中行，按可视行号排序去重。
        // 返回：可视行号数组；没有显式选择时使用 fallbackRow 兜底。
        std::vector<int> selectedRows;
        if (tableWidget == nullptr)
        {
            return selectedRows;
        }

        const QList<QTableWidgetItem*> selectedItems = tableWidget->selectedItems();
        selectedRows.reserve(static_cast<std::size_t>(selectedItems.size()));
        for (QTableWidgetItem* item : selectedItems)
        {
            if (item != nullptr)
            {
                selectedRows.push_back(item->row());
            }
        }

        if (selectedRows.empty() && fallbackRow >= 0)
        {
            selectedRows.push_back(fallbackRow);
        }

        std::sort(selectedRows.begin(), selectedRows.end());
        selectedRows.erase(std::unique(selectedRows.begin(), selectedRows.end()), selectedRows.end());
        return selectedRows;
    }

    std::vector<std::size_t> callbackEnumSelectedSourceIndices(
        const QTableWidget* tableWidget,
        const std::vector<KernelCallbackEnumEntry>& sourceRows,
        const int fallbackRow)
    {
        // 作用：把表格可视选中行转换成 m_callbackEnumRows 的源索引。
        // 返回：有效源索引数组，顺序与当前排序/筛选后的可视顺序一致。
        std::vector<std::size_t> sourceIndices;
        if (tableWidget == nullptr)
        {
            return sourceIndices;
        }

        const std::vector<int> selectedRows = callbackEnumSelectedVisualRows(tableWidget, fallbackRow);
        sourceIndices.reserve(selectedRows.size());
        for (const int visualRow : selectedRows)
        {
            QTableWidgetItem* classItem = tableWidget->item(
                visualRow,
                static_cast<int>(CallbackEnumColumn::Class));
            if (classItem == nullptr)
            {
                continue;
            }

            const std::size_t sourceIndex =
                static_cast<std::size_t>(classItem->data(Qt::UserRole).toULongLong());
            if (sourceIndex < sourceRows.size())
            {
                sourceIndices.push_back(sourceIndex);
            }
        }
        return sourceIndices;
    }

    void callbackEnumCopyTextToClipboard(const QString& contentText)
    {
        // 作用：统一写入系统剪贴板；QApplication 未就绪时静默跳过。
        // 返回：无。
        QClipboard* clipboard = QApplication::clipboard();
        if (clipboard != nullptr)
        {
            clipboard->setText(contentText);
        }
    }
}

void KernelDock::initializeCallbackEnumTab()
{
    if (m_callbackEnumPage == nullptr || m_callbackEnumLayout != nullptr)
    {
        return;
    }

    m_callbackEnumLayout = new QVBoxLayout(m_callbackEnumPage);
    m_callbackEnumLayout->setContentsMargins(4, 4, 4, 4);
    m_callbackEnumLayout->setSpacing(6);

    m_callbackEnumToolLayout = new QHBoxLayout();
    m_callbackEnumToolLayout->setContentsMargins(0, 0, 0, 0);
    m_callbackEnumToolLayout->setSpacing(6);

    m_refreshCallbackEnumButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_callbackEnumPage);
    m_refreshCallbackEnumButton->setToolTip(QStringLiteral("刷新回调遍历结果"));
    m_refreshCallbackEnumButton->setStyleSheet(callbackEnumButtonStyle());
    m_refreshCallbackEnumButton->setFixedWidth(34);

    m_callbackEnumFilterEdit = new QLineEdit(m_callbackEnumPage);
    m_callbackEnumFilterEdit->setPlaceholderText(QStringLiteral("按类别/来源/状态/名称/地址/模块/Altitude筛选"));
    m_callbackEnumFilterEdit->setToolTip(QStringLiteral("输入关键字后实时过滤回调遍历结果"));
    m_callbackEnumFilterEdit->setClearButtonEnabled(true);
    m_callbackEnumFilterEdit->setStyleSheet(callbackEnumInputStyle());

    m_callbackEnumStatusLabel = new QLabel(QStringLiteral("状态：等待刷新"), m_callbackEnumPage);
    m_callbackEnumStatusLabel->setStyleSheet(callbackEnumStatusLabelStyle(KswordTheme::TextSecondaryHex()));

    m_callbackEnumToolLayout->addWidget(m_refreshCallbackEnumButton, 0);
    m_callbackEnumToolLayout->addWidget(m_callbackEnumFilterEdit, 1);
    m_callbackEnumToolLayout->addWidget(m_callbackEnumStatusLabel, 0);
    m_callbackEnumLayout->addLayout(m_callbackEnumToolLayout);

    QSplitter* splitter = new QSplitter(Qt::Vertical, m_callbackEnumPage);
    m_callbackEnumLayout->addWidget(splitter, 1);

    m_callbackEnumTable = new QTableWidget(splitter);
    m_callbackEnumTable->setColumnCount(static_cast<int>(CallbackEnumColumn::Count));
    m_callbackEnumTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("类别"),
        QStringLiteral("来源"),
        QStringLiteral("状态"),
        QStringLiteral("名称"),
        QStringLiteral("回调/对象地址"),
        QStringLiteral("模块"),
        QStringLiteral("Altitude")
        });
    m_callbackEnumTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_callbackEnumTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_callbackEnumTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_callbackEnumTable->setAlternatingRowColors(true);
    m_callbackEnumTable->setStyleSheet(callbackEnumSelectionStyle());
    m_callbackEnumTable->setCornerButtonEnabled(false);
    m_callbackEnumTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_callbackEnumTable->verticalHeader()->setVisible(false);
    m_callbackEnumTable->horizontalHeader()->setStyleSheet(callbackEnumHeaderStyle());
    m_callbackEnumTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_callbackEnumTable->horizontalHeader()->setSectionResizeMode(static_cast<int>(CallbackEnumColumn::Name), QHeaderView::Stretch);
    m_callbackEnumTable->setColumnWidth(static_cast<int>(CallbackEnumColumn::CallbackAddress), 180);
    m_callbackEnumTable->setColumnWidth(static_cast<int>(CallbackEnumColumn::Module), 220);

    m_callbackEnumDetailEditor = new CodeEditorWidget(splitter);
    m_callbackEnumDetailEditor->setReadOnly(true);
    m_callbackEnumDetailEditor->setText(QStringLiteral("请选择一条回调记录查看详情。"));

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    connect(m_refreshCallbackEnumButton, &QPushButton::clicked, this, [this]() {
        refreshCallbackEnumAsync();
    });
    connect(m_callbackEnumFilterEdit, &QLineEdit::textChanged, this, [this](const QString& filterText) {
        rebuildCallbackEnumTable(filterText.trimmed());
    });
    connect(m_callbackEnumTable, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) {
        showCallbackEnumDetailByCurrentRow();
    });
    connect(m_callbackEnumTable, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& localPosition) {
        showCallbackEnumContextMenu(localPosition);
    });
}

void KernelDock::refreshCallbackEnumAsync()
{
    if (m_callbackEnumRefreshRunning.exchange(true))
    {
        return;
    }

    if (m_refreshCallbackEnumButton != nullptr)
    {
        m_refreshCallbackEnumButton->setEnabled(false);
    }
    if (m_callbackEnumStatusLabel != nullptr)
    {
        m_callbackEnumStatusLabel->setText(QStringLiteral("状态：刷新中..."));
        m_callbackEnumStatusLabel->setStyleSheet(callbackEnumStatusLabelStyle(KswordTheme::PrimaryBlueHex));
    }

    QPointer<KernelDock> guardThis(this);
    std::thread([guardThis]() {
        std::vector<KernelCallbackEnumEntry> resultRows;
        QString errorText;
        std::uint32_t responseFlags = 0;
        const ksword::ark::DriverClient driverClient;
        const ksword::ark::CallbackEnumResult enumResult = driverClient.enumerateCallbacks();
        const bool success = enumResult.io.ok;

        if (success)
        {
            responseFlags = enumResult.flags;
            resultRows.reserve(enumResult.entries.size());
            for (const ksword::ark::CallbackEnumEntry& entry : enumResult.entries)
            {
                resultRows.push_back(callbackEnumConvertEntry(entry));
            }
        }
        else
        {
            errorText = QStringLiteral("回调遍历 IOCTL 调用失败。\nWin32=%1\n详情=%2")
                .arg(enumResult.io.win32Error)
                .arg(QString::fromStdString(enumResult.io.message));
        }

        QMetaObject::invokeMethod(guardThis, [guardThis, success, errorText, responseFlags, resultRows = std::move(resultRows)]() mutable {
            if (guardThis == nullptr)
            {
                return;
            }

            guardThis->m_callbackEnumRefreshRunning.store(false);
            if (guardThis->m_refreshCallbackEnumButton != nullptr)
            {
                guardThis->m_refreshCallbackEnumButton->setEnabled(true);
            }

            if (!success)
            {
                guardThis->m_callbackEnumStatusLabel->setText(QStringLiteral("状态：刷新失败"));
                guardThis->m_callbackEnumStatusLabel->setStyleSheet(callbackEnumStatusLabelStyle(QStringLiteral("#B23A3A")));
                guardThis->m_callbackEnumDetailEditor->setText(errorText);
                return;
            }

            guardThis->m_callbackEnumRows = std::move(resultRows);
            guardThis->rebuildCallbackEnumTable(guardThis->m_callbackEnumFilterEdit->text().trimmed());

            std::size_t unsupportedCount = 0U;
            for (const KernelCallbackEnumEntry& entry : guardThis->m_callbackEnumRows)
            {
                if (entry.status == KSWORD_ARK_CALLBACK_ENUM_STATUS_UNSUPPORTED)
                {
                    ++unsupportedCount;
                }
            }

            const bool truncated = (responseFlags & KSWORD_ARK_ENUM_CALLBACK_RESPONSE_FLAG_TRUNCATED) != 0U;
            guardThis->m_callbackEnumStatusLabel->setText(
                QStringLiteral("状态：已刷新 %1 项，私有未支持 %2 项%3")
                .arg(guardThis->m_callbackEnumRows.size())
                .arg(unsupportedCount)
                .arg(truncated ? QStringLiteral("，响应截断") : QString()));
            guardThis->m_callbackEnumStatusLabel->setStyleSheet(callbackEnumStatusLabelStyle(
                truncated ? QStringLiteral("#D77A00") : QStringLiteral("#3A8F3A")));

            if (guardThis->m_callbackEnumTable->rowCount() > 0)
            {
                guardThis->m_callbackEnumTable->setCurrentCell(0, 0);
            }
            else
            {
                guardThis->m_callbackEnumDetailEditor->setText(QStringLiteral("当前环境未返回可见回调记录。"));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void KernelDock::rebuildCallbackEnumTable(const QString& filterKeyword)
{
    if (m_callbackEnumTable == nullptr)
    {
        return;
    }

    m_callbackEnumTable->setSortingEnabled(false);
    m_callbackEnumTable->setRowCount(0);

    for (std::size_t sourceIndex = 0; sourceIndex < m_callbackEnumRows.size(); ++sourceIndex)
    {
        const KernelCallbackEnumEntry& entry = m_callbackEnumRows[sourceIndex];
        const QString addressText = callbackEnumFormatAddress(entry.callbackAddress);
        const QString moduleText = entry.modulePathText.isEmpty() ? QStringLiteral("<未解析>") : entry.modulePathText;
        const bool matched = filterKeyword.isEmpty()
            || entry.classText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.sourceText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.statusText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.nameText.contains(filterKeyword, Qt::CaseInsensitive)
            || addressText.contains(filterKeyword, Qt::CaseInsensitive)
            || moduleText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.altitudeText.contains(filterKeyword, Qt::CaseInsensitive)
            || entry.detailText.contains(filterKeyword, Qt::CaseInsensitive);
        if (!matched)
        {
            continue;
        }

        const int rowIndex = m_callbackEnumTable->rowCount();
        m_callbackEnumTable->insertRow(rowIndex);

        auto* classItem = new QTableWidgetItem(entry.classText);
        classItem->setData(Qt::UserRole, static_cast<qulonglong>(sourceIndex));
        auto* sourceItem = new QTableWidgetItem(entry.sourceText);
        auto* statusItem = new QTableWidgetItem(entry.statusText);
        auto* nameItem = new QTableWidgetItem(callbackEnumSafeText(entry.nameText));
        auto* addressItem = new QTableWidgetItem(addressText);
        auto* moduleItem = new QTableWidgetItem(moduleText);
        auto* altitudeItem = new QTableWidgetItem(callbackEnumSafeText(entry.altitudeText));

        if (entry.status == KSWORD_ARK_CALLBACK_ENUM_STATUS_UNSUPPORTED)
        {
            statusItem->setForeground(QBrush(QColor(QStringLiteral("#D77A00"))));
        }
        else if (entry.status == KSWORD_ARK_CALLBACK_ENUM_STATUS_QUERY_FAILED)
        {
            statusItem->setForeground(QBrush(QColor(QStringLiteral("#B23A3A"))));
        }

        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::Class), classItem);
        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::Source), sourceItem);
        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::Status), statusItem);
        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::Name), nameItem);
        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::CallbackAddress), addressItem);
        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::Module), moduleItem);
        m_callbackEnumTable->setItem(rowIndex, static_cast<int>(CallbackEnumColumn::Altitude), altitudeItem);
    }

    m_callbackEnumTable->setSortingEnabled(true);
}

bool KernelDock::currentCallbackEnumSourceIndex(std::size_t& sourceIndexOut) const
{
    sourceIndexOut = 0U;
    if (m_callbackEnumTable == nullptr)
    {
        return false;
    }

    const int currentRow = m_callbackEnumTable->currentRow();
    if (currentRow < 0)
    {
        return false;
    }

    QTableWidgetItem* classItem = m_callbackEnumTable->item(currentRow, static_cast<int>(CallbackEnumColumn::Class));
    if (classItem == nullptr)
    {
        return false;
    }

    sourceIndexOut = static_cast<std::size_t>(classItem->data(Qt::UserRole).toULongLong());
    return sourceIndexOut < m_callbackEnumRows.size();
}

const KernelCallbackEnumEntry* KernelDock::currentCallbackEnumEntry() const
{
    std::size_t sourceIndex = 0U;
    if (!currentCallbackEnumSourceIndex(sourceIndex))
    {
        return nullptr;
    }
    return &m_callbackEnumRows[sourceIndex];
}

void KernelDock::showCallbackEnumDetailByCurrentRow()
{
    if (m_callbackEnumDetailEditor == nullptr)
    {
        return;
    }

    const KernelCallbackEnumEntry* entry = currentCallbackEnumEntry();
    if (entry == nullptr)
    {
        m_callbackEnumDetailEditor->setText(QStringLiteral("请选择一条回调记录查看详情。"));
        return;
    }

    const QString win32ModulePath = callbackEnumNormalizeModulePath(entry->modulePathText);
    const QString detailText = QStringLiteral(
        "类别: %1\n"
        "来源: %2\n"
        "状态: %3\n"
        "名称: %4\n"
        "Altitude: %5\n"
        "主地址显示: %6\n"
        "真实回调地址: %7\n"
        "上下文/诊断值: %8\n"
        "注册句柄/Cookie/全局节点: %9\n"
        "模块路径: %10\n"
        "Win32模块路径: %11\n"
        "模块基址: %12\n"
        "模块大小: 0x%13\n"
        "操作掩码: 0x%14\n"
        "对象类型掩码: 0x%15\n"
        "字段标志: 0x%16\n"
        "LastStatus: 0x%17\n\n"
        "说明: 主地址显示会优先显示真实回调函数；定位/诊断行没有真实回调函数时显示全局数组、链表节点或诊断值，所以不再把这些行误判为 0 地址。\n\n"
        "详情:\n%18")
        .arg(entry->classText)
        .arg(entry->sourceText)
        .arg(entry->statusText)
        .arg(callbackEnumSafeText(entry->nameText))
        .arg(callbackEnumSafeText(entry->altitudeText))
        .arg(callbackEnumPrimaryAddressText(*entry))
        .arg(callbackEnumFormatAddress(entry->callbackAddress))
        .arg(callbackEnumFormatAddress(entry->contextAddress))
        .arg(callbackEnumFormatAddress(entry->registrationAddress))
        .arg(entry->modulePathText.isEmpty() ? QStringLiteral("<未解析>") : entry->modulePathText)
        .arg(win32ModulePath.isEmpty() ? QStringLiteral("<不可映射或不存在>") : win32ModulePath)
        .arg(callbackEnumFormatAddress(entry->moduleBase))
        .arg(QString::number(static_cast<qulonglong>(entry->moduleSize), 16).toUpper())
        .arg(static_cast<qulonglong>(entry->operationMask), 8, 16, QChar('0'))
        .arg(static_cast<qulonglong>(entry->objectTypeMask), 8, 16, QChar('0'))
        .arg(static_cast<qulonglong>(entry->fieldFlags), 8, 16, QChar('0'))
        .arg(static_cast<qulonglong>(static_cast<std::uint32_t>(entry->lastStatus)), 8, 16, QChar('0'))
        .arg(callbackEnumSafeText(entry->detailText, QStringLiteral("<无详情>")));

    m_callbackEnumDetailEditor->setText(detailText);
}

void KernelDock::showCallbackEnumContextMenu(const QPoint& localPosition)
{
    if (m_callbackEnumTable == nullptr)
    {
        return;
    }

    // 右键选区规则：
    // - 点在未选中行上时切换为该单行；
    // - 点在已选中行上时保留 Ctrl 多选集合；
    // - 点在空白处时保留现有选择，复制动作继续对当前选择生效。
    QTableWidgetItem* clickedItem = m_callbackEnumTable->itemAt(localPosition);
    const int clickedRow = clickedItem != nullptr ? clickedItem->row() : -1;
    const int clickedColumn = m_callbackEnumTable->columnAt(localPosition.x());
    if (clickedItem != nullptr)
    {
        if (!clickedItem->isSelected())
        {
            m_callbackEnumTable->clearSelection();
            m_callbackEnumTable->setCurrentItem(clickedItem);
            m_callbackEnumTable->selectRow(clickedRow);
        }
        else
        {
            if (QItemSelectionModel* selectionModel = m_callbackEnumTable->selectionModel())
            {
                // 右键点在已选中行时只移动当前单元格，不清空 Ctrl 多选集合。
                selectionModel->setCurrentIndex(
                    m_callbackEnumTable->indexFromItem(clickedItem),
                    QItemSelectionModel::NoUpdate);
            }
        }
    }

    const int fallbackRow = clickedRow >= 0 ? clickedRow : m_callbackEnumTable->currentRow();
    const std::vector<std::size_t> selectedSourceIndices =
        callbackEnumSelectedSourceIndices(m_callbackEnumTable, m_callbackEnumRows, fallbackRow);
    const bool hasSelection = !selectedSourceIndices.empty();
    QString clickedModulePath;
    if (clickedRow >= 0)
    {
        std::vector<std::size_t> clickedSourceIndices =
            callbackEnumSelectedSourceIndices(m_callbackEnumTable, m_callbackEnumRows, clickedRow);
        if (!clickedSourceIndices.empty() && clickedSourceIndices.front() < m_callbackEnumRows.size())
        {
            clickedModulePath = callbackEnumNormalizeModulePath(
                m_callbackEnumRows[clickedSourceIndices.front()].modulePathText);
        }
    }
    if (clickedModulePath.isEmpty() && !selectedSourceIndices.empty() && selectedSourceIndices.front() < m_callbackEnumRows.size())
    {
        clickedModulePath = callbackEnumNormalizeModulePath(
            m_callbackEnumRows[selectedSourceIndices.front()].modulePathText);
    }
    const bool hasModuleFile = !clickedModulePath.isEmpty() && QFileInfo(clickedModulePath).exists();

    QMenu contextMenu(this);
    contextMenu.setStyleSheet(KswordTheme::ContextMenuStyle());

    QAction* refreshAction = contextMenu.addAction(
        QIcon(":/Icon/process_refresh.svg"),
        QStringLiteral("刷新回调遍历"));
    QAction* openModuleFolderAction = contextMenu.addAction(
        QIcon(":/Icon/process_open_folder.svg"),
        QStringLiteral("打开模块所在目录"));
    QAction* moduleFileDetailAction = contextMenu.addAction(
        QIcon(":/Icon/process_details.svg"),
        QStringLiteral("模块文件详细信息"));
    openModuleFolderAction->setEnabled(hasModuleFile);
    moduleFileDetailAction->setEnabled(hasModuleFile);
    contextMenu.addSeparator();

    QMenu* copyMenu = contextMenu.addMenu(
        QIcon(":/Icon/process_copy_row.svg"),
        QStringLiteral("复制"));
    QAction* copyCurrentColumnAction = copyMenu->addAction(
        QIcon(":/Icon/process_copy_cell.svg"),
        QStringLiteral("复制当前列（选中行）"));
    QAction* copySelectedRowsAction = copyMenu->addAction(
        QIcon(":/Icon/process_copy_row.svg"),
        QStringLiteral("复制选中行（TSV）"));
    QAction* copySelectedRowsWithHeaderAction = copyMenu->addAction(
        QStringLiteral("复制表头+选中行（TSV）"));
    QAction* copyDetailAction = copyMenu->addAction(
        QStringLiteral("复制详情（选中行）"));
    copyMenu->addSeparator();

    QMenu* copyColumnMenu = copyMenu->addMenu(QStringLiteral("复制指定栏目（选中行）"));
    for (int columnIndex = 0; columnIndex < static_cast<int>(CallbackEnumColumn::Count); ++columnIndex)
    {
        const CallbackEnumColumn column = static_cast<CallbackEnumColumn>(columnIndex);
        QAction* columnAction = copyColumnMenu->addAction(callbackEnumColumnHeaderText(column));
        columnAction->setData(columnIndex);
    }

    copyCurrentColumnAction->setEnabled(hasSelection);
    copySelectedRowsAction->setEnabled(hasSelection);
    copySelectedRowsWithHeaderAction->setEnabled(hasSelection);
    copyDetailAction->setEnabled(hasSelection);
    copyColumnMenu->setEnabled(hasSelection);

    QAction* selectedAction = contextMenu.exec(m_callbackEnumTable->viewport()->mapToGlobal(localPosition));
    if (selectedAction == nullptr)
    {
        return;
    }

    if (selectedAction == refreshAction)
    {
        refreshCallbackEnumAsync();
        return;
    }

    if (selectedAction == openModuleFolderAction)
    {
        const bool opened = callbackEnumOpenModuleInExplorer(clickedModulePath);
        if (m_callbackEnumStatusLabel != nullptr)
        {
            m_callbackEnumStatusLabel->setText(opened
                ? QStringLiteral("状态：已打开模块所在目录")
                : QStringLiteral("状态：打开模块所在目录失败"));
        }
        return;
    }

    if (selectedAction == moduleFileDetailAction)
    {
        callbackEnumShowModuleFileDetailDialog(this, clickedModulePath);
        if (m_callbackEnumStatusLabel != nullptr)
        {
            m_callbackEnumStatusLabel->setText(QStringLiteral("状态：已打开模块文件详细信息"));
        }
        return;
    }

    if (!hasSelection)
    {
        return;
    }

    const auto buildColumnText = [this, &selectedSourceIndices](const CallbackEnumColumn column) -> QString
    {
        // 作用：把指定栏目在所有选中行中的值拼成多行文本。
        // 返回：以换行分隔的栏目值。
        QStringList valueList;
        valueList.reserve(static_cast<int>(selectedSourceIndices.size()));
        for (const std::size_t sourceIndex : selectedSourceIndices)
        {
            if (sourceIndex < m_callbackEnumRows.size())
            {
                valueList.push_back(callbackEnumEntryColumnText(m_callbackEnumRows[sourceIndex], column));
            }
        }
        return valueList.join('\n');
    };

    if (selectedAction == copyCurrentColumnAction)
    {
        int activeColumn = clickedColumn >= 0 ? clickedColumn : m_callbackEnumTable->currentColumn();
        if (activeColumn < 0 || activeColumn >= static_cast<int>(CallbackEnumColumn::Count))
        {
            activeColumn = static_cast<int>(CallbackEnumColumn::Class);
        }
        callbackEnumCopyTextToClipboard(buildColumnText(static_cast<CallbackEnumColumn>(activeColumn)));
        if (m_callbackEnumStatusLabel != nullptr)
        {
            m_callbackEnumStatusLabel->setText(QStringLiteral("状态：已复制 %1 行的“%2”栏目")
                .arg(static_cast<qulonglong>(selectedSourceIndices.size()))
                .arg(callbackEnumColumnHeaderText(static_cast<CallbackEnumColumn>(activeColumn))));
        }
        return;
    }

    if (selectedAction == copySelectedRowsAction || selectedAction == copySelectedRowsWithHeaderAction)
    {
        QStringList rowList;
        rowList.reserve(static_cast<int>(selectedSourceIndices.size()) + 1);
        if (selectedAction == copySelectedRowsWithHeaderAction)
        {
            rowList.push_back(callbackEnumHeaderAsTsv());
        }
        for (const std::size_t sourceIndex : selectedSourceIndices)
        {
            if (sourceIndex < m_callbackEnumRows.size())
            {
                rowList.push_back(callbackEnumEntryAsTsv(m_callbackEnumRows[sourceIndex]));
            }
        }
        callbackEnumCopyTextToClipboard(rowList.join('\n'));
        if (m_callbackEnumStatusLabel != nullptr)
        {
            m_callbackEnumStatusLabel->setText(QStringLiteral("状态：已复制 %1 行回调记录")
                .arg(static_cast<qulonglong>(selectedSourceIndices.size())));
        }
        return;
    }

    if (selectedAction == copyDetailAction)
    {
        QStringList detailList;
        detailList.reserve(static_cast<int>(selectedSourceIndices.size()));
        for (const std::size_t sourceIndex : selectedSourceIndices)
        {
            if (sourceIndex >= m_callbackEnumRows.size())
            {
                continue;
            }

            const KernelCallbackEnumEntry& entry = m_callbackEnumRows[sourceIndex];
            detailList.push_back(QStringLiteral("[%1] %2\n%3")
                .arg(entry.classText)
                .arg(callbackEnumSafeText(entry.nameText))
                .arg(callbackEnumSafeText(entry.detailText, QStringLiteral("<无详情>"))));
        }
        callbackEnumCopyTextToClipboard(detailList.join(QStringLiteral("\n\n---\n\n")));
        if (m_callbackEnumStatusLabel != nullptr)
        {
            m_callbackEnumStatusLabel->setText(QStringLiteral("状态：已复制 %1 行详情")
                .arg(static_cast<qulonglong>(selectedSourceIndices.size())));
        }
        return;
    }

    const QList<QAction*> columnActionList = copyColumnMenu->actions();
    if (columnActionList.contains(selectedAction))
    {
        const int columnIndex = selectedAction->data().toInt();
        if (columnIndex >= 0 && columnIndex < static_cast<int>(CallbackEnumColumn::Count))
        {
            const CallbackEnumColumn column = static_cast<CallbackEnumColumn>(columnIndex);
            callbackEnumCopyTextToClipboard(buildColumnText(column));
            if (m_callbackEnumStatusLabel != nullptr)
            {
                m_callbackEnumStatusLabel->setText(QStringLiteral("状态：已复制 %1 行的“%2”栏目")
                    .arg(static_cast<qulonglong>(selectedSourceIndices.size()))
                    .arg(callbackEnumColumnHeaderText(column)));
            }
        }
    }
}
