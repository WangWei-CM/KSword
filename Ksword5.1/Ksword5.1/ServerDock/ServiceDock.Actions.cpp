#include "ServiceDock.Internal.h"

#include <chrono>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringConverter>
#include <QTextStream>

#include <Shellapi.h>

using namespace service_dock_detail;

namespace
{
    // buildServiceRegistryPath 作用：生成服务对应注册表路径文本。
    QString buildServiceRegistryPath(const QString& serviceNameText)
    {
        return QStringLiteral("HKLM\\SYSTEM\\CurrentControlSet\\Services\\%1").arg(serviceNameText);
    }

    // openFilePropertiesByPath 作用：打开文件属性对话框。
    bool openFilePropertiesByPath(const QString& filePathText, QString* errorTextOut)
    {
        if (filePathText.trimmed().isEmpty())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("文件路径为空。");
            }
            return false;
        }

        const HINSTANCE shellResult = ::ShellExecuteW(
            nullptr,
            L"properties",
            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(filePathText).utf16()),
            nullptr,
            nullptr,
            SW_SHOW);
        if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("ShellExecute(properties) 失败，返回值=%1")
                    .arg(reinterpret_cast<INT_PTR>(shellResult));
            }
            return false;
        }
        return true;
    }
}

bool ServiceDock::waitForServiceState(
    SC_HANDLE serviceHandle,
    const DWORD expectedState,
    const DWORD timeoutMs,
    DWORD* finalStateOut) const
{
    if (serviceHandle == nullptr)
    {
        return false;
    }

    const auto startTick = std::chrono::steady_clock::now();
    while (true)
    {
        SERVICE_STATUS_PROCESS statusValue{};
        DWORD requiredBytes = 0;
        const BOOL queryOk = ::QueryServiceStatusEx(
            serviceHandle,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&statusValue),
            sizeof(statusValue),
            &requiredBytes);
        if (queryOk == FALSE)
        {
            return false;
        }

        if (finalStateOut != nullptr)
        {
            *finalStateOut = statusValue.dwCurrentState;
        }
        if (statusValue.dwCurrentState == expectedState)
        {
            return true;
        }

        const auto elapsedMs = static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTick).count());
        if (elapsedMs >= timeoutMs)
        {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(180));
    }
}

void ServiceDock::showServiceContextMenu(const QPoint& localPos)
{
    if (m_serviceTable == nullptr)
    {
        return;
    }

    QTableWidgetItem* clickedItem = m_serviceTable->itemAt(localPos);
    if (clickedItem == nullptr)
    {
        return;
    }
    if (clickedItem->row() >= 0)
    {
        m_serviceTable->selectRow(clickedItem->row());
    }

    QMenu contextMenu(this);
    QAction* refreshAction = contextMenu.addAction(createBlueIcon(":/Icon/process_refresh.svg"), QStringLiteral("刷新当前服务"));
    QAction* startAction = contextMenu.addAction(createBlueIcon(":/Icon/process_start.svg"), QStringLiteral("启动服务"));
    QAction* stopAction = contextMenu.addAction(createBlueIcon(":/Icon/process_terminate.svg"), QStringLiteral("停止服务"));
    QAction* pauseAction = contextMenu.addAction(createBlueIcon(":/Icon/process_pause.svg"), QStringLiteral("暂停服务"));
    QAction* continueAction = contextMenu.addAction(createBlueIcon(":/Icon/process_resume.svg"), QStringLiteral("继续服务"));
    contextMenu.addSeparator();
    QAction* jumpProcessAction = contextMenu.addAction(createBlueIcon(":/Icon/process_details.svg"), QStringLiteral("跳转进程详情"));
    QAction* jumpHandleAction = contextMenu.addAction(createBlueIcon(":/Icon/process_list.svg"), QStringLiteral("跳转句柄筛选"));
    contextMenu.addSeparator();
    QAction* copyNameAction = contextMenu.addAction(createBlueIcon(":/Icon/log_copy.svg"), QStringLiteral("复制服务名"));
    QAction* openRegistryAction = contextMenu.addAction(createBlueIcon(":/Icon/file_find.svg"), QStringLiteral("打开服务注册表位置"));
    QAction* openBinaryLocationAction = contextMenu.addAction(createBlueIcon(":/Icon/process_open_folder.svg"), QStringLiteral("打开 BinaryPath 文件位置"));
    QAction* openServiceDllLocationAction = contextMenu.addAction(createBlueIcon(":/Icon/process_open_folder.svg"), QStringLiteral("打开 ServiceDll 文件位置"));
    QAction* openBinaryPropertiesAction = contextMenu.addAction(createBlueIcon(":/Icon/process_details.svg"), QStringLiteral("查看 BinaryPath 文件属性"));
    QAction* jumpFileDockBinaryAction = contextMenu.addAction(createBlueIcon(":/Icon/file_find.svg"), QStringLiteral("转到 FileDock 分析 BinaryPath"));
    QAction* jumpFileDockServiceDllAction = contextMenu.addAction(createBlueIcon(":/Icon/file_find.svg"), QStringLiteral("转到 FileDock 分析 ServiceDll"));
    contextMenu.addSeparator();
    QAction* exportListAction = contextMenu.addAction(createBlueIcon(":/Icon/log_export.svg"), QStringLiteral("导出当前列表 TSV"));
    QAction* exportServiceJsonAction = contextMenu.addAction(createBlueIcon(":/Icon/log_export.svg"), QStringLiteral("导出当前服务 JSON"));

    startAction->setEnabled(m_startButton != nullptr && m_startButton->isEnabled());
    stopAction->setEnabled(m_stopButton != nullptr && m_stopButton->isEnabled());
    pauseAction->setEnabled(m_pauseButton != nullptr && m_pauseButton->isEnabled());
    continueAction->setEnabled(m_continueButton != nullptr && m_continueButton->isEnabled());
    refreshAction->setEnabled(m_refreshCurrentButton != nullptr && m_refreshCurrentButton->isEnabled());
    copyNameAction->setEnabled(!selectedServiceName().isEmpty());
    openRegistryAction->setEnabled(!selectedServiceName().isEmpty());
    jumpProcessAction->setEnabled(!selectedServiceName().isEmpty());
    jumpHandleAction->setEnabled(!selectedServiceName().isEmpty());
    openBinaryLocationAction->setEnabled(!selectedServiceName().isEmpty());
    openServiceDllLocationAction->setEnabled(!selectedServiceName().isEmpty());
    openBinaryPropertiesAction->setEnabled(!selectedServiceName().isEmpty());
    jumpFileDockBinaryAction->setEnabled(!selectedServiceName().isEmpty());
    jumpFileDockServiceDllAction->setEnabled(!selectedServiceName().isEmpty());
    exportListAction->setEnabled(m_serviceTable != nullptr && m_serviceTable->rowCount() > 0);
    exportServiceJsonAction->setEnabled(!selectedServiceName().isEmpty());

    QAction* selectedAction = contextMenu.exec(m_serviceTable->viewport()->mapToGlobal(localPos));
    if (selectedAction == refreshAction)
    {
        refreshSelectedService();
    }
    else if (selectedAction == startAction)
    {
        startSelectedService();
    }
    else if (selectedAction == stopAction)
    {
        stopSelectedService();
    }
    else if (selectedAction == pauseAction)
    {
        pauseSelectedService();
    }
    else if (selectedAction == continueAction)
    {
        continueSelectedService();
    }
    else if (selectedAction == jumpProcessAction)
    {
        jumpToSelectedProcessDetail();
    }
    else if (selectedAction == jumpHandleAction)
    {
        jumpToSelectedHandleFilter();
    }
    else if (selectedAction == copyNameAction)
    {
        copySelectedServiceName();
    }
    else if (selectedAction == openRegistryAction)
    {
        openSelectedServiceRegistryPath();
    }
    else if (selectedAction == openBinaryLocationAction)
    {
        openSelectedBinaryLocation();
    }
    else if (selectedAction == openServiceDllLocationAction)
    {
        openSelectedServiceDllLocation();
    }
    else if (selectedAction == openBinaryPropertiesAction)
    {
        openSelectedBinaryProperties();
    }
    else if (selectedAction == jumpFileDockBinaryAction)
    {
        jumpToFileDockBinaryDetail();
    }
    else if (selectedAction == jumpFileDockServiceDllAction)
    {
        jumpToFileDockServiceDllDetail();
    }
    else if (selectedAction == exportListAction)
    {
        exportCurrentListAsTsv();
    }
    else if (selectedAction == exportServiceJsonAction)
    {
        exportSelectedServiceAsJson();
    }
}

void ServiceDock::refreshSelectedService()
{
    const QString serviceNameText = selectedServiceName();
    if (serviceNameText.isEmpty())
    {
        return;
    }

    const kLogEvent refreshEvent;
    info << refreshEvent
        << "[ServiceDock] 刷新单服务详情, service="
        << serviceNameText.toStdString()
        << eol;

    const int progressPid = kPro.add("服务管理", "刷新单服务详情");
    kPro.set(progressPid, "读取服务配置", 0, 45.0f);

    ServiceEntry updatedEntry;
    QString errorText;
    if (!querySingleServiceByName(serviceNameText, &updatedEntry, &errorText))
    {
        kPro.set(progressPid, "刷新失败", 0, 100.0f);
        err << refreshEvent
            << "[ServiceDock] 刷新单服务详情失败, service="
            << serviceNameText.toStdString()
            << ", error="
            << errorText.toStdString()
            << eol;
        QMessageBox::warning(
            this,
            QStringLiteral("服务管理"),
            QStringLiteral("刷新服务详情失败：\n%1").arg(errorText));
        return;
    }

    applyServiceUpdateToCache(updatedEntry);
    rebuildServiceTable();
    kPro.set(progressPid, "刷新完成", 0, 100.0f);
}

void ServiceDock::startSelectedService()
{
    controlSelectedService(
        SERVICE_START,
        QStringLiteral("启动服务"),
        0,
        true,
        SERVICE_RUNNING,
        false);
}

void ServiceDock::stopSelectedService()
{
    controlSelectedService(
        SERVICE_STOP,
        QStringLiteral("停止服务"),
        SERVICE_CONTROL_STOP,
        false,
        SERVICE_STOPPED,
        true);
}

void ServiceDock::pauseSelectedService()
{
    controlSelectedService(
        SERVICE_PAUSE_CONTINUE,
        QStringLiteral("暂停服务"),
        SERVICE_CONTROL_PAUSE,
        false,
        SERVICE_PAUSED,
        false);
}

void ServiceDock::continueSelectedService()
{
    controlSelectedService(
        SERVICE_PAUSE_CONTINUE,
        QStringLiteral("继续服务"),
        SERVICE_CONTROL_CONTINUE,
        false,
        SERVICE_RUNNING,
        false);
}

bool ServiceDock::controlSelectedService(
    const DWORD desiredAccess,
    const QString& actionText,
    const DWORD controlCode,
    const bool useStartService,
    const DWORD expectedState,
    const bool highRiskAction)
{
    const QString serviceNameText = selectedServiceName();
    if (serviceNameText.isEmpty())
    {
        return false;
    }

    const int selectedIndex = findServiceIndexByName(serviceNameText);
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_serviceList.size()))
    {
        return false;
    }

    const ServiceEntry& selectedEntry = m_serviceList[static_cast<std::size_t>(selectedIndex)];
    if (isServiceStatePending(selectedEntry.currentState))
    {
        QMessageBox::information(
            this,
            QStringLiteral("服务管理"),
            QStringLiteral("当前服务处于状态切换中，请稍后再试。"));
        return false;
    }

    if (highRiskAction)
    {
        const QMessageBox::StandardButton confirmButton = QMessageBox::warning(
            this,
            QStringLiteral("高风险动作确认"),
            QStringLiteral("确认执行“%1”？\n\n服务：%2").arg(actionText).arg(serviceNameText),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (confirmButton != QMessageBox::Yes)
        {
            return false;
        }
    }

    const kLogEvent actionEvent;
    info << actionEvent
        << "[ServiceDock] 开始执行服务动作, action="
        << actionText.toStdString()
        << ", service="
        << serviceNameText.toStdString()
        << eol;

    const int progressPid = kPro.add("服务管理", actionText.toStdString() + std::string(" - ") + serviceNameText.toStdString());
    kPro.set(progressPid, "连接服务控制管理器", 0, 15.0f);

    SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scmHandle == nullptr)
    {
        const QString errorText = winErrorText(::GetLastError());
        err << actionEvent
            << "[ServiceDock] OpenSCManagerW 失败, error="
            << errorText.toStdString()
            << eol;
        kPro.set(progressPid, "执行失败", 0, 100.0f);
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("连接 SCM 失败：\n%1").arg(errorText));
        return false;
    }

    SC_HANDLE serviceHandle = ::OpenServiceW(
        scmHandle,
        reinterpret_cast<LPCWSTR>(serviceNameText.utf16()),
        desiredAccess | SERVICE_QUERY_STATUS);
    if (serviceHandle == nullptr)
    {
        const QString errorText = winErrorText(::GetLastError());
        ::CloseServiceHandle(scmHandle);
        err << actionEvent
            << "[ServiceDock] OpenServiceW 失败, service="
            << serviceNameText.toStdString()
            << ", error="
            << errorText.toStdString()
            << eol;
        kPro.set(progressPid, "执行失败", 0, 100.0f);
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("打开服务失败：\n%1").arg(errorText));
        return false;
    }

    kPro.set(progressPid, "下发服务控制指令", 0, 55.0f);
    bool actionOk = false;
    if (useStartService)
    {
        actionOk = ::StartServiceW(serviceHandle, 0, nullptr) != FALSE;
        if (!actionOk && ::GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
        {
            actionOk = true;
        }
    }
    else
    {
        SERVICE_STATUS statusValue{};
        actionOk = ::ControlService(serviceHandle, controlCode, &statusValue) != FALSE;
        if (!actionOk && controlCode == SERVICE_CONTROL_STOP && ::GetLastError() == ERROR_SERVICE_NOT_ACTIVE)
        {
            actionOk = true;
        }
    }

    if (!actionOk)
    {
        const QString errorText = winErrorText(::GetLastError());
        ::CloseServiceHandle(serviceHandle);
        ::CloseServiceHandle(scmHandle);
        err << actionEvent
            << "[ServiceDock] 服务动作执行失败, action="
            << actionText.toStdString()
            << ", service="
            << serviceNameText.toStdString()
            << ", error="
            << errorText.toStdString()
            << eol;
        kPro.set(progressPid, "执行失败", 0, 100.0f);
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("操作失败：\n%1").arg(errorText));
        return false;
    }

    kPro.set(progressPid, "等待状态稳定", 0, 80.0f);
    DWORD finalStateValue = 0;
    const bool waitOk = (expectedState == 0)
        ? true
        : waitForServiceState(serviceHandle, expectedState, 6000, &finalStateValue);

    ::CloseServiceHandle(serviceHandle);
    ::CloseServiceHandle(scmHandle);

    if (!waitOk)
    {
        warn << actionEvent
            << "[ServiceDock] 服务动作已下发但状态未在超时内到达期望, action="
            << actionText.toStdString()
            << ", service="
            << serviceNameText.toStdString()
            << ", finalState="
            << finalStateValue
            << eol;
    }
    else
    {
        info << actionEvent
            << "[ServiceDock] 服务动作执行成功, action="
            << actionText.toStdString()
            << ", service="
            << serviceNameText.toStdString()
            << eol;
    }

    kPro.set(progressPid, "刷新列表", 0, 92.0f);
    requestAsyncRefresh(true);
    kPro.set(progressPid, "执行完成", 0, 100.0f);
    return true;
}

void ServiceDock::applySelectedStartType()
{
    const QString serviceNameText = selectedServiceName();
    if (serviceNameText.isEmpty())
    {
        return;
    }

    const int selectedIndex = findServiceIndexByName(serviceNameText);
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_serviceList.size()))
    {
        return;
    }

    const ServiceEntry& selectedEntry = m_serviceList[static_cast<std::size_t>(selectedIndex)];
    if (isServiceStatePending(selectedEntry.currentState))
    {
        QMessageBox::information(this, QStringLiteral("服务管理"), QStringLiteral("当前服务处于过渡态，请稍后再改启动类型。"));
        return;
    }

    const DWORD targetStartType = static_cast<DWORD>(m_startTypeCombo->currentData(Qt::UserRole).toULongLong());
    const bool targetDelayedAutoStart = m_startTypeCombo->currentData(Qt::UserRole + 1).toBool();
    const QString targetStartTypeText = m_startTypeCombo->currentText();

    if (targetStartType == SERVICE_DISABLED)
    {
        const QMessageBox::StandardButton confirmButton = QMessageBox::warning(
            this,
            QStringLiteral("高风险动作确认"),
            QStringLiteral("确认将服务“%1”设置为禁用吗？").arg(serviceNameText),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (confirmButton != QMessageBox::Yes)
        {
            return;
        }
    }

    const kLogEvent changeEvent;
    info << changeEvent
        << "[ServiceDock] 开始修改启动类型, service="
        << serviceNameText.toStdString()
        << ", target="
        << targetStartTypeText.toStdString()
        << eol;

    const int progressPid = kPro.add("服务管理", "修改启动类型");
    kPro.set(progressPid, "连接服务控制管理器", 0, 20.0f);

    SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scmHandle == nullptr)
    {
        const QString errorText = winErrorText(::GetLastError());
        err << changeEvent << "[ServiceDock] OpenSCManagerW 失败, error=" << errorText.toStdString() << eol;
        kPro.set(progressPid, "执行失败", 0, 100.0f);
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("连接 SCM 失败：\n%1").arg(errorText));
        return;
    }

    SC_HANDLE serviceHandle = ::OpenServiceW(
        scmHandle,
        reinterpret_cast<LPCWSTR>(serviceNameText.utf16()),
        SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS);
    if (serviceHandle == nullptr)
    {
        const QString errorText = winErrorText(::GetLastError());
        ::CloseServiceHandle(scmHandle);
        err << changeEvent << "[ServiceDock] OpenServiceW 失败, error=" << errorText.toStdString() << eol;
        kPro.set(progressPid, "执行失败", 0, 100.0f);
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("打开服务失败：\n%1").arg(errorText));
        return;
    }

    kPro.set(progressPid, "写入启动类型", 0, 60.0f);
    const BOOL changeStartTypeOk = ::ChangeServiceConfigW(
        serviceHandle,
        SERVICE_NO_CHANGE,
        targetStartType,
        SERVICE_NO_CHANGE,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);
    if (changeStartTypeOk == FALSE)
    {
        const QString errorText = winErrorText(::GetLastError());
        ::CloseServiceHandle(serviceHandle);
        ::CloseServiceHandle(scmHandle);
        err << changeEvent << "[ServiceDock] ChangeServiceConfigW 失败, error=" << errorText.toStdString() << eol;
        kPro.set(progressPid, "执行失败", 0, 100.0f);
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("修改启动类型失败：\n%1").arg(errorText));
        return;
    }

    // 延迟自动启动配置：
    // - 仅自动启动类型可设置 delayed=true；
    // - 非自动启动时强制写回 false，避免脏配置残留。
    SERVICE_DELAYED_AUTO_START_INFO delayedInfo{};
    delayedInfo.fDelayedAutostart = (targetStartType == SERVICE_AUTO_START && targetDelayedAutoStart) ? TRUE : FALSE;
    const BOOL delayedConfigOk = ::ChangeServiceConfig2W(
        serviceHandle,
        SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
        reinterpret_cast<LPBYTE>(&delayedInfo));
    if (delayedConfigOk == FALSE)
    {
        warn << changeEvent
            << "[ServiceDock] ChangeServiceConfig2W(DelayedAutoStart) 失败，继续后续流程, error="
            << winErrorText(::GetLastError()).toStdString()
            << eol;
    }

    ::CloseServiceHandle(serviceHandle);
    ::CloseServiceHandle(scmHandle);

    kPro.set(progressPid, "刷新列表", 0, 90.0f);
    requestAsyncRefresh(true);
    kPro.set(progressPid, "修改完成", 0, 100.0f);
}

void ServiceDock::copySelectedServiceName()
{
    const QString serviceNameText = selectedServiceName();
    if (serviceNameText.isEmpty())
    {
        return;
    }

    QApplication::clipboard()->setText(serviceNameText);
}

void ServiceDock::openSelectedServiceRegistryPath()
{
    const QString serviceNameText = selectedServiceName();
    if (serviceNameText.isEmpty())
    {
        return;
    }

    const QString registryPathText = buildServiceRegistryPath(serviceNameText);
    QApplication::clipboard()->setText(registryPathText);
    QProcess::startDetached(QStringLiteral("regedit.exe"), {});
    QMessageBox::information(
        this,
        QStringLiteral("服务管理"),
        QStringLiteral("已复制注册表路径到剪贴板：\n%1\n\n并尝试打开 regedit。").arg(registryPathText));
}

void ServiceDock::openSelectedBinaryLocation()
{
    const int selectedIndex = findServiceIndexByName(selectedServiceName());
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_serviceList.size()))
    {
        return;
    }

    const QString filePathText = m_serviceList[static_cast<std::size_t>(selectedIndex)].imagePathText.trimmed();
    if (filePathText.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("服务管理"), QStringLiteral("当前服务没有可定位的 BinaryPath 文件。"));
        return;
    }

    QProcess::startDetached(
        QStringLiteral("explorer.exe"),
        { QStringLiteral("/select,%1").arg(QDir::toNativeSeparators(filePathText)) });
}

void ServiceDock::openSelectedServiceDllLocation()
{
    const int selectedIndex = findServiceIndexByName(selectedServiceName());
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_serviceList.size()))
    {
        return;
    }

    const QString filePathText = m_serviceList[static_cast<std::size_t>(selectedIndex)].serviceDllPathText.trimmed();
    if (filePathText.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("服务管理"), QStringLiteral("当前服务未配置 ServiceDll。"));
        return;
    }

    QProcess::startDetached(
        QStringLiteral("explorer.exe"),
        { QStringLiteral("/select,%1").arg(QDir::toNativeSeparators(filePathText)) });
}

void ServiceDock::openSelectedBinaryProperties()
{
    const int selectedIndex = findServiceIndexByName(selectedServiceName());
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_serviceList.size()))
    {
        return;
    }

    const QString filePathText = m_serviceList[static_cast<std::size_t>(selectedIndex)].imagePathText.trimmed();
    if (filePathText.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("服务管理"), QStringLiteral("当前服务没有可查看属性的 BinaryPath 文件。"));
        return;
    }

    QString errorText;
    if (!openFilePropertiesByPath(filePathText, &errorText))
    {
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("打开文件属性失败：\n%1").arg(errorText));
    }
}

void ServiceDock::jumpToSelectedProcessDetail()
{
    const int selectedIndex = findServiceIndexByName(selectedServiceName());
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_serviceList.size()))
    {
        return;
    }

    const std::uint32_t processIdValue = m_serviceList[static_cast<std::size_t>(selectedIndex)].processId;
    if (processIdValue == 0)
    {
        QMessageBox::information(this, QStringLiteral("服务管理"), QStringLiteral("当前服务没有关联运行中 PID。"));
        return;
    }

    QWidget* mainWindowWidget = window();
    if (mainWindowWidget == nullptr)
    {
        return;
    }

    QMetaObject::invokeMethod(
        mainWindowWidget,
        "openProcessDetailByPid",
        Qt::QueuedConnection,
        Q_ARG(quint32, static_cast<quint32>(processIdValue)));
}

void ServiceDock::jumpToSelectedHandleFilter()
{
    const int selectedIndex = findServiceIndexByName(selectedServiceName());
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_serviceList.size()))
    {
        return;
    }

    const std::uint32_t processIdValue = m_serviceList[static_cast<std::size_t>(selectedIndex)].processId;
    if (processIdValue == 0)
    {
        QMessageBox::information(this, QStringLiteral("服务管理"), QStringLiteral("当前服务没有关联运行中 PID。"));
        return;
    }

    QWidget* mainWindowWidget = window();
    if (mainWindowWidget == nullptr)
    {
        return;
    }

    QMetaObject::invokeMethod(
        mainWindowWidget,
        "focusHandleDockByPid",
        Qt::QueuedConnection,
        Q_ARG(quint32, static_cast<quint32>(processIdValue)));
}

void ServiceDock::exportCurrentListAsTsv()
{
    const QString outputPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出服务列表"),
        QStringLiteral("ServiceList.tsv"),
        QStringLiteral("TSV Files (*.tsv);;All Files (*.*)"));
    if (outputPath.trimmed().isEmpty())
    {
        return;
    }

    QSaveFile outputFile(outputPath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("打开导出文件失败：\n%1").arg(outputFile.errorString()));
        return;
    }

    QTextStream outputStream(&outputFile);
    outputStream.setEncoding(QStringConverter::Utf8);
    outputStream << "服务名\t显示名\t状态\t启动类型\tPID\t账户\tBinaryPath\tServiceDll\t风险\n";

    for (int rowIndex = 0; rowIndex < m_serviceTable->rowCount(); ++rowIndex)
    {
        QTableWidgetItem* nameItem = m_serviceTable->item(rowIndex, toServiceColumn(ServiceColumn::Name));
        if (nameItem == nullptr)
        {
            continue;
        }

        const QString serviceNameText = nameItem->data(kServiceNameRole).toString();
        const int serviceIndex = findServiceIndexByName(serviceNameText);
        if (serviceIndex < 0 || serviceIndex >= static_cast<int>(m_serviceList.size()))
        {
            continue;
        }

        const ServiceEntry& entry = m_serviceList[static_cast<std::size_t>(serviceIndex)];
        outputStream
            << entry.serviceNameText << '\t'
            << entry.displayNameText << '\t'
            << entry.stateText << '\t'
            << entry.startTypeText << '\t'
            << entry.processId << '\t'
            << entry.accountText << '\t'
            << entry.commandLineText << '\t'
            << entry.serviceDllPathText << '\t'
            << entry.riskSummaryText << '\n';
    }

    if (!outputFile.commit())
    {
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("写入导出文件失败。"));
        return;
    }
}

void ServiceDock::exportSelectedServiceAsJson()
{
    const int selectedIndex = findServiceIndexByName(selectedServiceName());
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_serviceList.size()))
    {
        return;
    }

    const ServiceEntry& entry = m_serviceList[static_cast<std::size_t>(selectedIndex)];
    const QString outputPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出当前服务 JSON"),
        QStringLiteral("%1.json").arg(entry.serviceNameText),
        QStringLiteral("JSON Files (*.json);;All Files (*.*)"));
    if (outputPath.trimmed().isEmpty())
    {
        return;
    }

    QJsonObject rootObject;
    rootObject.insert(QStringLiteral("service_name"), entry.serviceNameText);
    rootObject.insert(QStringLiteral("display_name"), entry.displayNameText);
    rootObject.insert(QStringLiteral("description"), entry.descriptionText);
    rootObject.insert(QStringLiteral("state_text"), entry.stateText);
    rootObject.insert(QStringLiteral("start_type_text"), entry.startTypeText);
    rootObject.insert(QStringLiteral("service_type_text"), entry.serviceTypeText);
    rootObject.insert(QStringLiteral("error_control_text"), entry.errorControlText);
    rootObject.insert(QStringLiteral("binary_path"), entry.commandLineText);
    rootObject.insert(QStringLiteral("image_path"), entry.imagePathText);
    rootObject.insert(QStringLiteral("service_dll_path"), entry.serviceDllPathText);
    rootObject.insert(QStringLiteral("account"), entry.accountText);
    rootObject.insert(QStringLiteral("pid"), static_cast<int>(entry.processId));
    rootObject.insert(QStringLiteral("current_state"), static_cast<int>(entry.currentState));
    rootObject.insert(QStringLiteral("start_type"), static_cast<int>(entry.startTypeValue));
    rootObject.insert(QStringLiteral("service_type"), static_cast<int>(entry.serviceTypeValue));
    rootObject.insert(QStringLiteral("error_control"), static_cast<int>(entry.errorControlValue));
    rootObject.insert(QStringLiteral("delayed_auto_start"), entry.delayedAutoStart);
    rootObject.insert(QStringLiteral("risk_summary"), entry.riskSummaryText);

    QJsonArray riskArray;
    for (const QString& riskTagText : entry.riskTagList)
    {
        riskArray.push_back(riskTagText);
    }
    rootObject.insert(QStringLiteral("risk_tags"), riskArray);

    QSaveFile outputFile(outputPath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("打开导出文件失败：\n%1").arg(outputFile.errorString()));
        return;
    }

    const QJsonDocument jsonDocument(rootObject);
    outputFile.write(jsonDocument.toJson(QJsonDocument::Indented));
    if (!outputFile.commit())
    {
        QMessageBox::warning(this, QStringLiteral("服务管理"), QStringLiteral("写入导出文件失败。"));
    }
}

void ServiceDock::jumpToFileDockBinaryDetail()
{
    const int selectedIndex = findServiceIndexByName(selectedServiceName());
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_serviceList.size()))
    {
        return;
    }

    const QString filePathText = m_serviceList[static_cast<std::size_t>(selectedIndex)].imagePathText.trimmed();
    if (filePathText.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("服务管理"), QStringLiteral("当前服务没有可分析的 BinaryPath 文件。"));
        return;
    }

    QWidget* mainWindowWidget = window();
    if (mainWindowWidget == nullptr)
    {
        return;
    }

    QMetaObject::invokeMethod(
        mainWindowWidget,
        "openFileDetailDockByPath",
        Qt::QueuedConnection,
        Q_ARG(QString, filePathText));
}

void ServiceDock::jumpToFileDockServiceDllDetail()
{
    const int selectedIndex = findServiceIndexByName(selectedServiceName());
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_serviceList.size()))
    {
        return;
    }

    const QString filePathText = m_serviceList[static_cast<std::size_t>(selectedIndex)].serviceDllPathText.trimmed();
    if (filePathText.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("服务管理"), QStringLiteral("当前服务未配置 ServiceDll。"));
        return;
    }

    QWidget* mainWindowWidget = window();
    if (mainWindowWidget == nullptr)
    {
        return;
    }

    QMetaObject::invokeMethod(
        mainWindowWidget,
        "openFileDetailDockByPath",
        Qt::QueuedConnection,
        Q_ARG(QString, filePathText));
}
