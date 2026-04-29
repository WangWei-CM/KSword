#include "KernelDock.h"

#include "../../../shared/KswordArkLogProtocol.h"
#include "../../../shared/driver/KswordArkCallbackIoctl.h"
#include "../UI/CodeEditorWidget.h"
#include "../theme.h"

#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QMutex>
#include <QMutexLocker>

namespace
{
    QMutex& callbackRemoveHandleMutex()
    {
        static QMutex handleMutex;
        return handleMutex;
    }

    HANDLE& callbackRemoveCachedHandle()
    {
        static HANDLE cachedHandle = INVALID_HANDLE_VALUE;
        return cachedHandle;
    }

    // callbackRemoveParseAddress：
    // - 作用：把输入文本解析为 64 位地址（支持 0x 前缀）。
    bool callbackRemoveParseAddress(const QString& textValue, quint64& addressOut)
    {
        QString normalizedText = textValue.trimmed();
        bool parseOk = false;

        if (normalizedText.isEmpty())
        {
            return false;
        }

        if (normalizedText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        {
            normalizedText = normalizedText.mid(2);
        }

        addressOut = normalizedText.toULongLong(&parseOk, 16);
        return parseOk;
    }

    // callbackRemoveNormalizePath：规范化路径，便于匹配驱动服务映射。
    QString callbackRemoveNormalizePath(const QString& pathText)
    {
        QString normalizedText = pathText.trimmed().toLower();
        normalizedText.replace(QStringLiteral("\""), QString());
        normalizedText.replace(QStringLiteral("\\??\\"), QStringLiteral(""));
        normalizedText.replace(QStringLiteral("\\systemroot"), QStringLiteral("c:\\windows"));
        return normalizedText;
    }

    // callbackRemoveResolveServiceByModule：根据模块路径推断对应的服务名。
    QString callbackRemoveResolveServiceByModule(const QString& modulePath)
    {
        const QString normalizedModulePath = callbackRemoveNormalizePath(modulePath);
        if (normalizedModulePath.isEmpty())
        {
            return QString();
        }

        SC_HANDLE scmHandle = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
        if (scmHandle == nullptr)
        {
            return QString();
        }

        DWORD requiredBytes = 0;
        DWORD serviceCount = 0;
        DWORD resumeHandle = 0;
        (void)::EnumServicesStatusExW(
            scmHandle,
            SC_ENUM_PROCESS_INFO,
            SERVICE_DRIVER,
            SERVICE_STATE_ALL,
            nullptr,
            0,
            &requiredBytes,
            &serviceCount,
            &resumeHandle,
            nullptr);
        if (requiredBytes == 0)
        {
            ::CloseServiceHandle(scmHandle);
            return QString();
        }

        QByteArray serviceBuffer;
        serviceBuffer.resize(static_cast<int>(requiredBytes));
        auto* serviceArray = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(serviceBuffer.data());
        if (!::EnumServicesStatusExW(
            scmHandle,
            SC_ENUM_PROCESS_INFO,
            SERVICE_DRIVER,
            SERVICE_STATE_ALL,
            reinterpret_cast<LPBYTE>(serviceArray),
            requiredBytes,
            &requiredBytes,
            &serviceCount,
            &resumeHandle,
            nullptr))
        {
            ::CloseServiceHandle(scmHandle);
            return QString();
        }

        QString mappedServiceName;
        for (DWORD index = 0; index < serviceCount; ++index)
        {
            SC_HANDLE serviceHandle = ::OpenServiceW(
                scmHandle,
                serviceArray[index].lpServiceName,
                SERVICE_QUERY_CONFIG);
            if (serviceHandle == nullptr)
            {
                continue;
            }

            DWORD configBytes = 0;
            (void)::QueryServiceConfigW(serviceHandle, nullptr, 0, &configBytes);
            if (configBytes == 0)
            {
                ::CloseServiceHandle(serviceHandle);
                continue;
            }

            QByteArray configBuffer;
            configBuffer.resize(static_cast<int>(configBytes));
            auto* configInfo = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBuffer.data());
            if (::QueryServiceConfigW(serviceHandle, configInfo, configBytes, &configBytes) && configInfo->lpBinaryPathName != nullptr)
            {
                const QString serviceBinaryPath = callbackRemoveNormalizePath(QString::fromWCharArray(configInfo->lpBinaryPathName));
                const QString serviceFileName = QFileInfo(serviceBinaryPath).fileName();
                if (!serviceFileName.isEmpty() && normalizedModulePath.endsWith(serviceFileName))
                {
                    mappedServiceName = QString::fromWCharArray(serviceArray[index].lpServiceName);
                    ::CloseServiceHandle(serviceHandle);
                    break;
                }
            }

            ::CloseServiceHandle(serviceHandle);
        }

        ::CloseServiceHandle(scmHandle);
        return mappedServiceName;
    }

    // callbackRemoveAcquireDriverHandle：缓存驱动句柄，避免每次点击都重复打开设备。
    HANDLE callbackRemoveAcquireDriverHandle()
    {
        QMutexLocker lockGuard(&callbackRemoveHandleMutex());
        if (callbackRemoveCachedHandle() != INVALID_HANDLE_VALUE)
        {
            return callbackRemoveCachedHandle();
        }

        callbackRemoveCachedHandle() = ::CreateFileW(
            KSWORD_ARK_LOG_WIN32_PATH,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        return callbackRemoveCachedHandle();
    }

    // callbackRemoveInvalidateDriverHandle：当句柄失效时重置缓存。
    void callbackRemoveInvalidateDriverHandle()
    {
        QMutexLocker lockGuard(&callbackRemoveHandleMutex());
        if (callbackRemoveCachedHandle() != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(callbackRemoveCachedHandle());
            callbackRemoveCachedHandle() = INVALID_HANDLE_VALUE;
        }
    }
}

void KernelDock::initializeCallbackRemoveTab()
{
    if (m_callbackRemovePage == nullptr || m_callbackRemoveLayout != nullptr)
    {
        return;
    }

    m_callbackRemoveLayout = wrapPageInScrollArea(m_callbackRemovePage, &m_callbackRemoveContentWidget);
    if (m_callbackRemoveLayout == nullptr || m_callbackRemoveContentWidget == nullptr)
    {
        return;
    }
    m_callbackRemoveLayout->setContentsMargins(6, 6, 6, 6);
    m_callbackRemoveLayout->setSpacing(6);

    m_callbackRemoveToolLayout = new QHBoxLayout();
    m_callbackRemoveToolLayout->setContentsMargins(0, 0, 0, 0);
    m_callbackRemoveToolLayout->setSpacing(6);

    m_callbackRemoveTypeCombo = new QComboBox(m_callbackRemoveContentWidget);
    m_callbackRemoveTypeCombo->addItem(QStringLiteral("进程创建/退出 Notify"), static_cast<quint32>(KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PROCESS));
    m_callbackRemoveTypeCombo->addItem(QStringLiteral("线程创建/退出 Notify"), static_cast<quint32>(KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_THREAD));
    m_callbackRemoveTypeCombo->addItem(QStringLiteral("镜像加载 Notify"), static_cast<quint32>(KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_IMAGE));
    m_callbackRemoveTypeCombo->addItem(QStringLiteral("Object Callback"), static_cast<quint32>(KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_OBJECT));
    m_callbackRemoveTypeCombo->addItem(QStringLiteral("Registry Callback"), static_cast<quint32>(KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_REGISTRY));
    m_callbackRemoveTypeCombo->addItem(QStringLiteral("Minifilter"), static_cast<quint32>(KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_MINIFILTER));
    m_callbackRemoveTypeCombo->addItem(QStringLiteral("WFP Callout"), static_cast<quint32>(KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_WFP_CALLOUT));
    m_callbackRemoveTypeCombo->addItem(QStringLiteral("ETW Provider/Consumer"), static_cast<quint32>(KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_ETW_PROVIDER));

    m_callbackRemoveAddressEdit = new QLineEdit(m_callbackRemoveContentWidget);
    m_callbackRemoveAddressEdit->setPlaceholderText(QStringLiteral("输入回调地址（例如 0xFFFFF80012345678）"));
    m_callbackRemoveAddressEdit->setClearButtonEnabled(true);

    m_callbackRemoveButton = new QPushButton(QStringLiteral("移除回调"), m_callbackRemoveContentWidget);
    m_callbackRemoveButton->setStyleSheet(QStringLiteral(
        "QPushButton{color:%1;background:%2;border:1px solid %3;border-radius:2px;padding:3px 10px;}"
        "QPushButton:hover{background:%4;color:#FFFFFF;border:1px solid %4;}"
        "QPushButton:pressed{background:%4;color:#FFFFFF;}"
    ).arg(
        KswordTheme::PrimaryBlueHex,
        KswordTheme::SurfaceHex(),
        KswordTheme::PrimaryBlueBorderHex,
        KswordTheme::PrimaryBluePressedHex));

    m_callbackRemoveStatusLabel = new QLabel(QStringLiteral("状态：等待操作（当前仅前三类支持直接移除）"), m_callbackRemoveContentWidget);
    m_callbackRemoveStatusLabel->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(KswordTheme::TextSecondaryHex()));

    m_callbackRemoveToolLayout->addWidget(new QLabel(QStringLiteral("类型："), m_callbackRemoveContentWidget));
    m_callbackRemoveToolLayout->addWidget(m_callbackRemoveTypeCombo, 0);
    m_callbackRemoveToolLayout->addWidget(m_callbackRemoveAddressEdit, 1);
    m_callbackRemoveToolLayout->addWidget(m_callbackRemoveButton, 0);
    m_callbackRemoveLayout->addLayout(m_callbackRemoveToolLayout);
    m_callbackRemoveLayout->addWidget(m_callbackRemoveStatusLabel, 0);

    m_callbackRemoveDetailEditor = new CodeEditorWidget(m_callbackRemoveContentWidget);
    m_callbackRemoveDetailEditor->setReadOnly(true);
    m_callbackRemoveDetailEditor->setText(QStringLiteral("提示：该页面通过 KswordARK 驱动调用内核接口移除指定地址的回调，并尝试映射对应模块/服务。"));
    m_callbackRemoveLayout->addWidget(m_callbackRemoveDetailEditor, 1);

    connect(m_callbackRemoveButton, &QPushButton::clicked, this, [this]() {
        HANDLE deviceHandle = callbackRemoveAcquireDriverHandle();
        if (deviceHandle == INVALID_HANDLE_VALUE)
        {
            const DWORD errorCode = ::GetLastError();
            m_callbackRemoveStatusLabel->setText(QStringLiteral("状态：连接驱动失败，error=%1").arg(errorCode));
            QMessageBox::warning(this, QStringLiteral("回调移除"), m_callbackRemoveStatusLabel->text());
            return;
        }

        quint64 callbackAddress = 0;
        if (!callbackRemoveParseAddress(m_callbackRemoveAddressEdit->text(), callbackAddress) || callbackAddress == 0)
        {
            QMessageBox::warning(this, QStringLiteral("回调移除"), QStringLiteral("请输入合法的十六进制回调地址。"));
            return;
        }

        KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST requestPacket{};
        requestPacket.size = sizeof(requestPacket);
        requestPacket.version = KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_PROTOCOL_VERSION;
        requestPacket.callbackClass = static_cast<quint32>(m_callbackRemoveTypeCombo->currentData().toUInt());
        requestPacket.flags = KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_FLAG_NONE;
        requestPacket.callbackAddress = callbackAddress;

        KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE responsePacket{};
        DWORD bytesReturned = 0;
        const BOOL ioctlOk = ::DeviceIoControl(
            deviceHandle,
            IOCTL_KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK,
            &requestPacket,
            static_cast<DWORD>(sizeof(requestPacket)),
            &responsePacket,
            static_cast<DWORD>(sizeof(responsePacket)),
            &bytesReturned,
            nullptr);
        const DWORD lastError = ::GetLastError();

        if (!ioctlOk)
        {
            callbackRemoveInvalidateDriverHandle();
            m_callbackRemoveStatusLabel->setText(QStringLiteral("状态：移除失败，error=%1").arg(lastError));
            m_callbackRemoveDetailEditor->setText(QStringLiteral("DeviceIoControl 失败，Win32 错误码=%1。\n地址=0x%2")
                .arg(lastError)
                .arg(QString::number(callbackAddress, 16).toUpper()));
            QMessageBox::warning(this, QStringLiteral("回调移除"), m_callbackRemoveStatusLabel->text());
            return;
        }

        const QString modulePath = QString::fromWCharArray(responsePacket.modulePath);
        const QString serviceName = callbackRemoveResolveServiceByModule(modulePath);
        const QString detailText = QStringLiteral(
            "移除请求已执行。\n"
            "- 类型：%1\n"
            "- 地址：0x%2\n"
            "- 返回字节：%3\n"
            "- NTSTATUS：0x%4\n"
            "- 模块路径：%5\n"
            "- 模块基址：0x%6\n"
            "- 模块大小：0x%7\n"
            "- 服务映射：%8")
            .arg(m_callbackRemoveTypeCombo->currentText())
            .arg(QString::number(callbackAddress, 16).toUpper())
            .arg(bytesReturned)
            .arg(QString::number(static_cast<quint32>(responsePacket.ntstatus), 16).rightJustified(8, QLatin1Char('0')).toUpper())
            .arg(modulePath.isEmpty() ? QStringLiteral("未解析") : modulePath)
            .arg(QString::number(responsePacket.moduleBase, 16).toUpper())
            .arg(QString::number(responsePacket.moduleSize, 16).toUpper())
            .arg(serviceName.isEmpty() ? QStringLiteral("未匹配") : serviceName);
        m_callbackRemoveDetailEditor->setText(detailText);

        if (responsePacket.ntstatus >= 0)
        {
            m_callbackRemoveStatusLabel->setText(QStringLiteral("状态：移除完成。"));
        }
        else
        {
            m_callbackRemoveStatusLabel->setText(QStringLiteral("状态：驱动返回失败，NTSTATUS=0x%1")
                .arg(QString::number(static_cast<quint32>(responsePacket.ntstatus), 16).toUpper()));
            QMessageBox::warning(this, QStringLiteral("回调移除"), m_callbackRemoveStatusLabel->text());
        }
    });
}
