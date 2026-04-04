#include "OtherDock.h"

// ============================================================
// OtherDock.DesktopSwitch.cpp
// 作用说明：
// 1) 单独承载桌面管理页中的 SwitchDesktop 与右键菜单逻辑；
// 2) 与枚举逻辑拆分，控制单文件规模并保持“按 Tab 分文件”的组织方式；
// 3) 对非当前窗口站的桌面也尝试 OpenDesktopW（直接名/带窗口站名）以提升可达性。
// ============================================================

#include <QApplication>
#include <QClipboard>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndex>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace
{
    // 桌面管理表格列索引：
    // - 与 OtherDock.Desktop.cpp 保持一致；
    // - 右键菜单和切换逻辑依赖窗口站、桌面、SID、SID详情四类列。
    constexpr int kDesktopColumnWindowStation = 0;
    constexpr int kDesktopColumnDesktopName = 1;
    constexpr int kDesktopColumnSid = 9;
    constexpr int kDesktopColumnSidDetail = 10;

    // data role：
    // - 复用表格项上挂载的窗口站名、桌面名和 SID 元数据；
    // - refreshDesktopList 已负责把这些元数据写入每个单元格。
    constexpr int kDesktopRoleWindowStationName = Qt::UserRole;
    constexpr int kDesktopRoleDesktopName = Qt::UserRole + 1;
    constexpr int kDesktopRoleOwnerSidText = Qt::UserRole + 3;
    constexpr int kDesktopRoleOwnerSidDetailText = Qt::UserRole + 4;

    // DesktopOpenResult：
    // - 作用：描述本次 OpenDesktopW 尝试结果；
    // - 调用：switchToSelectedDesktop 复用，避免重复手写两轮尝试代码。
    struct DesktopOpenResult
    {
        HDESK desktopHandle = nullptr; // desktopHandle：成功打开时返回的桌面句柄。
        QString methodText;            // methodText：成功打开的方法描述。
        QString detailText;            // detailText：失败链或补充说明。
    };

    // tryOpenDesktopForSwitch：
    // - 作用：尝试用“桌面名”与“窗口站\桌面名”两种方式打开桌面；
    // - 调用：桌面切换前执行；
    // - 传入 switchAccessMask：本次切换所需权限；
    // - 传出：成功返回桌面句柄，失败返回完整错误链。
    DesktopOpenResult tryOpenDesktopForSwitch(
        const QString& windowStationName,
        const QString& desktopName,
        const ACCESS_MASK switchAccessMask)
    {
        DesktopOpenResult openResult;
        QStringList detailTextList;

        auto tryOpenOnce = [&openResult, &detailTextList, switchAccessMask](const QString& candidateName, const QString& tag) -> bool {
            if (candidateName.trimmed().isEmpty())
            {
                return false;
            }
            HDESK desktopHandle = ::OpenDesktopW(
                reinterpret_cast<LPCWSTR>(candidateName.utf16()),
                0,
                FALSE,
                switchAccessMask);
            if (desktopHandle != nullptr)
            {
                openResult.desktopHandle = desktopHandle;
                openResult.methodText = tag;
                openResult.detailText = QStringLiteral("OpenDesktopW 成功");
                return true;
            }
            detailTextList << QStringLiteral("%1=错误码%2").arg(tag).arg(::GetLastError());
            return false;
        };

        if (tryOpenOnce(desktopName, QStringLiteral("直接名")))
        {
            return openResult;
        }

        const QString qualifiedDesktopName = windowStationName.trimmed().isEmpty()
            ? QString()
            : QStringLiteral("%1\\%2").arg(windowStationName, desktopName);
        if (qualifiedDesktopName.compare(desktopName, Qt::CaseInsensitive) != 0
            && tryOpenOnce(qualifiedDesktopName, QStringLiteral("带窗口站名")))
        {
            return openResult;
        }

        openResult.detailText = detailTextList.join(QStringLiteral("；"));
        return openResult;
    }
}

void OtherDock::switchToSelectedDesktop()
{
    // actionEvent：整条“读取选中行 -> OpenDesktopW -> SwitchDesktop”流程共用一条日志事件。
    kLogEvent actionEvent;

    if (m_desktopTable == nullptr || m_desktopStatusLabel == nullptr || m_desktopTable->currentRow() < 0)
    {
        warn << actionEvent
            << "[OtherDock] 切换桌面失败：未选中目标桌面。"
            << eol;
        if (m_desktopStatusLabel != nullptr)
        {
            m_desktopStatusLabel->setText(QStringLiteral("请先选择要切换的桌面。"));
        }
        return;
    }

    // row/windowStationItem/desktopItem：从当前行中取出切换所需的窗口站名与桌面名。
    const int row = m_desktopTable->currentRow();
    QTableWidgetItem* windowStationItem = m_desktopTable->item(row, kDesktopColumnWindowStation);
    QTableWidgetItem* desktopItem = m_desktopTable->item(row, kDesktopColumnDesktopName);
    if (windowStationItem == nullptr || desktopItem == nullptr)
    {
        err << actionEvent
            << "[OtherDock] 切换桌面失败：目标行缺少窗口站或桌面列。"
            << eol;
        m_desktopStatusLabel->setText(QStringLiteral("切换失败：选中行缺少窗口站或桌面信息。"));
        return;
    }

    // windowStationName/desktopName：来自刷新阶段写入的行元数据。
    const QString windowStationName = windowStationItem->data(kDesktopRoleWindowStationName).toString().trimmed();
    const QString desktopName = desktopItem->data(kDesktopRoleDesktopName).toString().trimmed();
    if (windowStationName.isEmpty() || desktopName.isEmpty())
    {
        err << actionEvent
            << "[OtherDock] 切换桌面失败：窗口站名或桌面名为空。"
            << eol;
        m_desktopStatusLabel->setText(QStringLiteral("切换失败：窗口站名或桌面名为空。"));
        return;
    }

    if (desktopName.startsWith('<') && desktopName.endsWith('>'))
    {
        warn << actionEvent
            << "[OtherDock] 切换桌面失败：选中行不是可切换的真实桌面, station="
            << windowStationName.toStdString()
            << ", desktop="
            << desktopName.toStdString()
            << eol;
        m_desktopStatusLabel->setText(QStringLiteral("切换失败：当前行是状态占位项，不是真实桌面。"));
        return;
    }

    info << actionEvent
        << "[OtherDock] 开始切换桌面, station="
        << windowStationName.toStdString()
        << ", desktop="
        << desktopName.toStdString()
        << eol;

    // openResult：无论是否当前窗口站，都执行两轮 OpenDesktopW 尝试。
    const DesktopOpenResult openResult = tryOpenDesktopForSwitch(
        windowStationName,
        desktopName,
        DESKTOP_SWITCHDESKTOP | DESKTOP_READOBJECTS);
    if (openResult.desktopHandle == nullptr)
    {
        err << actionEvent
            << "[OtherDock] 切换桌面失败：OpenDesktopW失败, station="
            << windowStationName.toStdString()
            << ", desktop="
            << desktopName.toStdString()
            << ", detail="
            << openResult.detailText.toStdString()
            << eol;
        m_desktopStatusLabel->setText(
            QStringLiteral("切换失败：OpenDesktopW 详情=%1").arg(openResult.detailText));
        return;
    }

    const BOOL switchOk = ::SwitchDesktop(openResult.desktopHandle);
    const DWORD switchErrorCode = switchOk ? ERROR_SUCCESS : ::GetLastError();
    ::CloseDesktop(openResult.desktopHandle);

    if (switchOk == FALSE)
    {
        err << actionEvent
            << "[OtherDock] 切换桌面失败：SwitchDesktop失败, station="
            << windowStationName.toStdString()
            << ", desktop="
            << desktopName.toStdString()
            << ", openMethod="
            << openResult.methodText.toStdString()
            << ", code="
            << switchErrorCode
            << eol;
        m_desktopStatusLabel->setText(QStringLiteral("切换失败：SwitchDesktop 错误码=%1").arg(switchErrorCode));
        return;
    }

    info << actionEvent
        << "[OtherDock] 切换桌面成功, station="
        << windowStationName.toStdString()
        << ", desktop="
        << desktopName.toStdString()
        << ", openMethod="
        << openResult.methodText.toStdString()
        << eol;
    m_desktopStatusLabel->setText(
        QStringLiteral("切换成功：%1\\%2（方式=%3）")
        .arg(windowStationName, desktopName, openResult.methodText));
    refreshDesktopList();
}

void OtherDock::showDesktopContextMenu(const QPoint& localPos)
{
    // 桌面表右键菜单：提供“转到桌面 / 查看SID详情 / 复制SID / 复制完整桌面名 / 刷新”。
    if (m_desktopTable == nullptr)
    {
        return;
    }

    const QModelIndex indexAtPos = m_desktopTable->indexAt(localPos);
    if (!indexAtPos.isValid())
    {
        return;
    }

    const int row = indexAtPos.row();
    m_desktopTable->selectRow(row);

    // stationItem/desktopItem/sidItem/sidDetailItem：收集右键菜单后续动作依赖的数据。
    QTableWidgetItem* stationItem = m_desktopTable->item(row, kDesktopColumnWindowStation);
    QTableWidgetItem* desktopItem = m_desktopTable->item(row, kDesktopColumnDesktopName);
    QTableWidgetItem* sidItem = m_desktopTable->item(row, kDesktopColumnSid);
    QTableWidgetItem* sidDetailItem = m_desktopTable->item(row, kDesktopColumnSidDetail);
    if (stationItem == nullptr || desktopItem == nullptr)
    {
        return;
    }

    const QString windowStationName = stationItem->data(kDesktopRoleWindowStationName).toString().trimmed();
    const QString desktopName = desktopItem->data(kDesktopRoleDesktopName).toString().trimmed();
    const QString sidText = sidItem != nullptr
        ? sidItem->data(kDesktopRoleOwnerSidText).toString().trimmed()
        : QString();
    const QString sidDetailText = sidDetailItem != nullptr
        ? sidDetailItem->data(kDesktopRoleOwnerSidDetailText).toString().trimmed()
        : QString();
    const QString qualifiedDesktopName = QStringLiteral("%1\\%2").arg(windowStationName, desktopName);

    QMenu menu(this);
    QAction* goDesktopAction = menu.addAction(QIcon(":/Icon/process_start.svg"), QStringLiteral("转到桌面"));
    QAction* sidDetailAction = menu.addAction(QIcon(":/Icon/process_details.svg"), QStringLiteral("查看SID详情"));
    QAction* copySidAction = menu.addAction(QIcon(":/Icon/log_track.svg"), QStringLiteral("复制SID"));
    QAction* copyDesktopAction = menu.addAction(QIcon(":/Icon/process_tree.svg"), QStringLiteral("复制完整桌面名"));
    menu.addSeparator();
    QAction* refreshAction = menu.addAction(QIcon(":/Icon/process_refresh.svg"), QStringLiteral("刷新桌面列表"));

    const bool isPlaceholderDesktop = desktopName.startsWith('<') && desktopName.endsWith('>');
    goDesktopAction->setEnabled(!isPlaceholderDesktop);
    sidDetailAction->setEnabled(!sidDetailText.trimmed().isEmpty());
    copySidAction->setEnabled(!sidText.trimmed().isEmpty());

    QAction* selectedAction = menu.exec(m_desktopTable->viewport()->mapToGlobal(localPos));
    if (selectedAction == nullptr)
    {
        return;
    }

    if (selectedAction == goDesktopAction)
    {
        switchToSelectedDesktop();
        return;
    }

    if (selectedAction == sidDetailAction)
    {
        QMessageBox::information(
            this,
            QStringLiteral("SID详情"),
            QStringLiteral("对象：%1\nSID：%2\n\n%3")
                .arg(qualifiedDesktopName, sidText.isEmpty() ? QStringLiteral("<空>") : sidText, sidDetailText));
        return;
    }

    if (selectedAction == copySidAction)
    {
        QApplication::clipboard()->setText(sidText);
        if (m_desktopStatusLabel != nullptr)
        {
            m_desktopStatusLabel->setText(QStringLiteral("已复制 SID：%1").arg(sidText));
        }
        return;
    }

    if (selectedAction == copyDesktopAction)
    {
        QApplication::clipboard()->setText(qualifiedDesktopName);
        if (m_desktopStatusLabel != nullptr)
        {
            m_desktopStatusLabel->setText(QStringLiteral("已复制桌面名：%1").arg(qualifiedDesktopName));
        }
        return;
    }

    if (selectedAction == refreshAction)
    {
        refreshDesktopList();
    }
}
