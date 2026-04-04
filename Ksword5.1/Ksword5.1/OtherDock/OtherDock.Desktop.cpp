#include "OtherDock.h"

// ============================================================
// OtherDock.Desktop.cpp
// 作用说明：
// 1) 拆分 OtherDock 中“桌面管理”页的窗口站/桌面枚举逻辑；
// 2) 补充窗口站、桌面、SessionId、所有者 SID、桌面堆与切换能力展示；
// 3) 保持 SwitchDesktop 逻辑仅作用于当前进程窗口站，避免影响 Qt 主界面上下文。
// ============================================================

#include <QColor>
#include <QFont>
#include <QLabel>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <AclAPI.h>
#include <sddl.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{
    // 桌面管理表列索引：
    // - 统一维护列顺序，避免刷新与切换逻辑散落魔法数字；
    // - 当前列定义覆盖窗口站、桌面、SessionId、SID 与备注信息。
    constexpr int kDesktopColumnWindowStation = 0;
    constexpr int kDesktopColumnDesktopName = 1;
    constexpr int kDesktopColumnCurrentWindowStation = 2;
    constexpr int kDesktopColumnCurrentDesktop = 3;
    constexpr int kDesktopColumnInteractive = 4;
    constexpr int kDesktopColumnReadable = 5;
    constexpr int kDesktopColumnSwitchable = 6;
    constexpr int kDesktopColumnSessionId = 7;
    constexpr int kDesktopColumnOwner = 8;
    constexpr int kDesktopColumnSid = 9;
    constexpr int kDesktopColumnSidDetail = 10;
    constexpr int kDesktopColumnHeapKb = 11;
    constexpr int kDesktopColumnRemark = 12;

    // item data role：
    // - 给表格行挂载窗口站名、桌面名与“是否当前窗口站”元数据；
    // - switchToSelectedDesktop 直接复用这些数据完成目标校验。
    constexpr int kDesktopRoleWindowStationName = Qt::UserRole;
    constexpr int kDesktopRoleDesktopName = Qt::UserRole + 1;
    constexpr int kDesktopRoleIsCurrentWindowStation = Qt::UserRole + 2;
    constexpr int kDesktopRoleOwnerSidText = Qt::UserRole + 3;
    constexpr int kDesktopRoleOwnerSidDetailText = Qt::UserRole + 4;

    // DesktopCapabilityState：
    // - 表示“可读/可切换”能力的测试结果；
    // - Unknown 表示未测试或当前上下文不安全而主动跳过。
    enum class DesktopCapabilityState
    {
        Unknown = 0,
        Yes,
        No
    };

    // SidDescription：
    // - 作用：统一承载账户名、SID 字符串和 SID 结构详情；
    // - 调用：窗口站/桌面对象所有者解析后复用到表格列与右键菜单。
    struct SidDescription
    {
        QString accountText; // accountText：域\账户格式的可读名称。
        QString sidText;     // sidText：S-1-5-... 形式的 SID 字符串。
        QString detailText;  // detailText：SID 类型/机构/子权限等详细信息。
    };

    // DesktopOpenResult：
    // - 作用：统一描述 OpenDesktopW 的尝试结果；
    // - 调用：读取能力与切换能力探测都会复用。
    struct DesktopOpenResult
    {
        HDESK desktopHandle = nullptr; // desktopHandle：成功打开时返回的桌面句柄。
        QString methodText;            // methodText：成功时使用的方法描述。
        QString detailText;            // detailText：失败或过程详情。
    };

    // DesktopRowData：
    // - 作用：缓存单行窗口站/桌面记录，随后统一写入 QTableWidget；
    // - ownerAccountText / ownerSidText 优先展示桌面对象所有者，失败时回退到窗口站所有者。
    struct DesktopRowData
    {
        QString windowStationName;                          // windowStationName：窗口站名称。
        QString desktopName;                                // desktopName：桌面名称。
        bool isCurrentWindowStation = false;                // isCurrentWindowStation：是否当前进程窗口站。
        bool isCurrentDesktop = false;                      // isCurrentDesktop：是否当前线程所在桌面。
        bool isInteractiveWindowStation = false;            // isInteractiveWindowStation：窗口站是否可见/交互。
        bool inputDesktopKnown = false;                     // inputDesktopKnown：是否成功读取桌面输入能力。
        bool inputDesktopEnabled = false;                   // inputDesktopEnabled：桌面是否接收输入。
        DesktopCapabilityState readCapability = DesktopCapabilityState::Unknown;     // readCapability：桌面可读测试结果。
        DesktopCapabilityState switchCapability = DesktopCapabilityState::Unknown;   // switchCapability：桌面可切换测试结果。
        std::uint32_t sessionId = std::numeric_limits<std::uint32_t>::max();         // sessionId：当前进程会话编号。
        QString ownerAccountText;                           // ownerAccountText：所有者账户名。
        QString ownerSidText;                               // ownerSidText：所有者 SID 文本。
        QString ownerSidDetailText;                         // ownerSidDetailText：SID 类型/机构/子权限等详情。
        QString heapSizeText;                               // heapSizeText：桌面堆大小（KB）。
        QString remarkText;                                 // remarkText：综合备注（标志/错误/限制说明）。
    };

    // capabilityStateText：把 DesktopCapabilityState 转成中文文本。
    QString capabilityStateText(const DesktopCapabilityState capabilityState)
    {
        switch (capabilityState)
        {
        case DesktopCapabilityState::Yes:
            return QStringLiteral("是");
        case DesktopCapabilityState::No:
            return QStringLiteral("否");
        default:
            return QStringLiteral("未测");
        }
    }

    // queryUserObjectName：读取窗口站或桌面对象名称。
    QString queryUserObjectName(HANDLE userObjectHandle)
    {
        if (userObjectHandle == nullptr)
        {
            return QString();
        }

        // requiredBytes：API 预估缓冲区字节数，用于后续分配 wchar_t 缓冲。
        DWORD requiredBytes = 0;
        ::GetUserObjectInformationW(userObjectHandle, UOI_NAME, nullptr, 0, &requiredBytes);
        if (requiredBytes < sizeof(wchar_t))
        {
            return QString();
        }

        // nameBuffer：承载 UOI_NAME 查询结果，额外预留一个终止符位置。
        std::vector<wchar_t> nameBuffer(requiredBytes / sizeof(wchar_t) + 1, L'\0');
        const BOOL queryOk = ::GetUserObjectInformationW(
            userObjectHandle,
            UOI_NAME,
            nameBuffer.data(),
            static_cast<DWORD>(nameBuffer.size() * sizeof(wchar_t)),
            &requiredBytes);
        if (queryOk == FALSE)
        {
            return QString();
        }

        return QString::fromWCharArray(nameBuffer.data()).trimmed();
    }

    // queryUserObjectFlags：读取 USEROBJECTFLAGS。
    bool queryUserObjectFlags(HANDLE userObjectHandle, USEROBJECTFLAGS& userObjectFlagsOut)
    {
        DWORD returnedBytes = 0;
        const BOOL queryOk = ::GetUserObjectInformationW(
            userObjectHandle,
            UOI_FLAGS,
            &userObjectFlagsOut,
            sizeof(userObjectFlagsOut),
            &returnedBytes);
        return queryOk != FALSE && returnedBytes >= sizeof(userObjectFlagsOut);
    }

    // queryDesktopInputState：读取桌面是否接收输入。
    bool queryDesktopInputState(HDESK desktopHandle, bool& inputEnabledOut)
    {
        BOOL inputEnabled = FALSE;
        DWORD returnedBytes = 0;
        const BOOL queryOk = ::GetUserObjectInformationW(
            desktopHandle,
            UOI_IO,
            &inputEnabled,
            sizeof(inputEnabled),
            &returnedBytes);
        if (queryOk == FALSE || returnedBytes < sizeof(inputEnabled))
        {
            return false;
        }

        inputEnabledOut = inputEnabled != FALSE;
        return true;
    }

    // formatUserObjectFlags：格式化 USEROBJECTFLAGS 文本。
    QString formatUserObjectFlags(
        const USEROBJECTFLAGS& userObjectFlagsValue,
        const bool treatAsWindowStation)
    {
        QStringList flagTextList;
        if (userObjectFlagsValue.fInherit != FALSE)
        {
            flagTextList << QStringLiteral("可继承");
        }
        if (treatAsWindowStation && (userObjectFlagsValue.dwFlags & WSF_VISIBLE) != 0)
        {
            flagTextList << QStringLiteral("可见/交互式");
        }
        if (flagTextList.isEmpty())
        {
            flagTextList << QStringLiteral("无特殊标志");
        }
        return flagTextList.join(QStringLiteral(" | "));
    }

    // sidToStringText：把二进制 SID 转成 S-1-5-... 字符串。
    QString sidToStringText(PSID sidValue)
    {
        if (sidValue == nullptr)
        {
            return QStringLiteral("-");
        }

        LPWSTR sidStringBuffer = nullptr;
        if (::ConvertSidToStringSidW(sidValue, &sidStringBuffer) == FALSE || sidStringBuffer == nullptr)
        {
            return QStringLiteral("<SID转换失败:%1>").arg(::GetLastError());
        }

        // sidText：托管 SID 文本，随后立即释放 Win32 分配的缓冲区。
        const QString sidText = QString::fromWCharArray(sidStringBuffer);
        ::LocalFree(sidStringBuffer);
        return sidText;
    }

    // sidUseToText：把 SID_NAME_USE 枚举值转换成中文文本。
    QString sidUseToText(const SID_NAME_USE sidUse)
    {
        switch (sidUse)
        {
        case SidTypeUser: return QStringLiteral("用户");
        case SidTypeGroup: return QStringLiteral("组");
        case SidTypeDomain: return QStringLiteral("域");
        case SidTypeAlias: return QStringLiteral("别名");
        case SidTypeWellKnownGroup: return QStringLiteral("众所周知组");
        case SidTypeDeletedAccount: return QStringLiteral("已删除账户");
        case SidTypeInvalid: return QStringLiteral("无效");
        case SidTypeUnknown: return QStringLiteral("未知");
        case SidTypeComputer: return QStringLiteral("计算机");
        case SidTypeLabel: return QStringLiteral("完整性标签");
        default:
            return QStringLiteral("未识别类型");
        }
    }

    // sidIdentifierAuthorityText：提取 SID 的 Identifier Authority。
    QString sidIdentifierAuthorityText(PSID sidValue)
    {
        if (sidValue == nullptr || ::IsValidSid(sidValue) == FALSE)
        {
            return QStringLiteral("<无效机构>");
        }

        // authorityValue：把 6 字节 Identifier Authority 转换成十进制文本。
        const SID_IDENTIFIER_AUTHORITY* authorityValue = ::GetSidIdentifierAuthority(sidValue);
        unsigned long long authorityNumber = 0;
        for (int i = 0; i < 6; ++i)
        {
            authorityNumber = (authorityNumber << 8) | authorityValue->Value[i];
        }
        return QString::number(authorityNumber);
    }

    // sidSubAuthorityText：提取 SID 的子权限链。
    QString sidSubAuthorityText(PSID sidValue)
    {
        if (sidValue == nullptr || ::IsValidSid(sidValue) == FALSE)
        {
            return QStringLiteral("<无效子权限>");
        }

        QStringList subAuthorityTextList;
        const UCHAR subAuthorityCount = *::GetSidSubAuthorityCount(sidValue);
        for (UCHAR i = 0; i < subAuthorityCount; ++i)
        {
            subAuthorityTextList << QString::number(*::GetSidSubAuthority(sidValue, i));
        }
        return subAuthorityTextList.isEmpty()
            ? QStringLiteral("<空>")
            : subAuthorityTextList.join(',');
    }

    // describeSid：把原始 SID 解析成“账户名 + SID + 结构详情”。
    SidDescription describeSid(PSID sidValue)
    {
        SidDescription sidDescription;
        if (sidValue == nullptr || ::IsValidSid(sidValue) == FALSE)
        {
            sidDescription.accountText = QStringLiteral("-");
            sidDescription.sidText = QStringLiteral("-");
            sidDescription.detailText = QStringLiteral("<无效SID>");
            return sidDescription;
        }

        sidDescription.sidText = sidToStringText(sidValue);

        // 先走一轮空缓冲查询，拿到名字与域名所需字符数。
        DWORD accountNameLength = 0;
        DWORD domainNameLength = 0;
        SID_NAME_USE sidUse = SidTypeUnknown;
        ::LookupAccountSidW(
            nullptr,
            sidValue,
            nullptr,
            &accountNameLength,
            nullptr,
            &domainNameLength,
            &sidUse);
        const DWORD firstLookupError = ::GetLastError();

        if (firstLookupError == ERROR_INSUFFICIENT_BUFFER || firstLookupError == ERROR_SUCCESS)
        {
            std::vector<wchar_t> accountNameBuffer(accountNameLength + 1, L'\0');
            std::vector<wchar_t> domainNameBuffer(domainNameLength + 1, L'\0');
            const BOOL lookupOk = ::LookupAccountSidW(
                nullptr,
                sidValue,
                accountNameBuffer.data(),
                &accountNameLength,
                domainNameBuffer.data(),
                &domainNameLength,
                &sidUse);
            if (lookupOk != FALSE)
            {
                const QString domainName = QString::fromWCharArray(domainNameBuffer.data()).trimmed();
                const QString accountName = QString::fromWCharArray(accountNameBuffer.data()).trimmed();
                sidDescription.accountText = domainName.isEmpty()
                    ? (accountName.isEmpty() ? QStringLiteral("<空账户名>") : accountName)
                    : QStringLiteral("%1\\%2").arg(domainName, accountName);
            }
        }

        if (sidDescription.accountText.isEmpty())
        {
            sidDescription.accountText = QStringLiteral("<账户解析失败:%1>").arg(firstLookupError);
        }

        sidDescription.detailText = QStringLiteral("类型=%1；修订=%2；机构=%3；子权限=%4")
            .arg(sidUseToText(sidUse))
            .arg(static_cast<int>(reinterpret_cast<const SID*>(sidValue)->Revision))
            .arg(sidIdentifierAuthorityText(sidValue))
            .arg(sidSubAuthorityText(sidValue));
        if (!sidDescription.accountText.isEmpty())
        {
            sidDescription.detailText.prepend(QStringLiteral("账户=%1；").arg(sidDescription.accountText));
        }
        return sidDescription;
    }

    // queryWindowObjectOwnerInfo：读取窗口站或桌面对象的所有者完整 SID 描述。
    SidDescription queryWindowObjectOwnerInfo(HANDLE userObjectHandle)
    {
        SidDescription sidDescription;
        if (userObjectHandle == nullptr)
        {
            sidDescription.accountText = QStringLiteral("-");
            sidDescription.sidText = QStringLiteral("-");
            sidDescription.detailText = QStringLiteral("<空对象句柄>");
            return sidDescription;
        }

        // ownerSid/securityDescriptor：由 GetSecurityInfo 返回，securityDescriptor 需 LocalFree。
        PSID ownerSid = nullptr;
        PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
        const DWORD securityStatus = ::GetSecurityInfo(
            userObjectHandle,
            SE_WINDOW_OBJECT,
            OWNER_SECURITY_INFORMATION,
            &ownerSid,
            nullptr,
            nullptr,
            nullptr,
            &securityDescriptor);
        if (securityStatus != ERROR_SUCCESS)
        {
            if (securityDescriptor != nullptr)
            {
                ::LocalFree(securityDescriptor);
            }
            sidDescription.accountText = QStringLiteral("<查询失败:%1>").arg(securityStatus);
            sidDescription.sidText = QStringLiteral("<查询失败:%1>").arg(securityStatus);
            sidDescription.detailText = QStringLiteral("<对象所有者查询失败:%1>").arg(securityStatus);
            return sidDescription;
        }

        sidDescription = describeSid(ownerSid);
        if (securityDescriptor != nullptr)
        {
            ::LocalFree(securityDescriptor);
        }
        return sidDescription;
    }

    // tryOpenDesktopHandle：无论是否当前窗口站，都尝试对桌面执行 OpenDesktopW。
    DesktopOpenResult tryOpenDesktopHandle(
        const QString& windowStationName,
        const QString& desktopName,
        const ACCESS_MASK accessMask)
    {
        DesktopOpenResult openResult;
        QStringList detailTextList;

        auto tryOpenOnce = [&openResult, &detailTextList, accessMask](const QString& candidateName, const QString& tag) -> bool {
            if (candidateName.trimmed().isEmpty())
            {
                return false;
            }
            HDESK desktopHandle = ::OpenDesktopW(
                reinterpret_cast<LPCWSTR>(candidateName.utf16()),
                0,
                FALSE,
                accessMask);
            if (desktopHandle != nullptr)
            {
                openResult.desktopHandle = desktopHandle;
                openResult.methodText = tag;
                return true;
            }
            detailTextList << QStringLiteral("%1=错误码%2").arg(tag).arg(::GetLastError());
            return false;
        };

        if (tryOpenOnce(desktopName, QStringLiteral("直接名")))
        {
            openResult.detailText = QStringLiteral("OpenDesktopW 成功");
            return openResult;
        }

        const QString qualifiedDesktopName = windowStationName.trimmed().isEmpty()
            ? QString()
            : QStringLiteral("%1\\%2").arg(windowStationName, desktopName);
        if (qualifiedDesktopName.compare(desktopName, Qt::CaseInsensitive) != 0
            && tryOpenOnce(qualifiedDesktopName, QStringLiteral("带窗口站名")))
        {
            openResult.detailText = QStringLiteral("OpenDesktopW 成功");
            return openResult;
        }

        openResult.detailText = detailTextList.join(QStringLiteral("；"));
        return openResult;
    }

    // queryCurrentSessionId：查询当前进程会话编号。
    std::uint32_t queryCurrentSessionId()
    {
        DWORD sessionId = 0;
        if (::ProcessIdToSessionId(::GetCurrentProcessId(), &sessionId) == FALSE)
        {
            return std::numeric_limits<std::uint32_t>::max();
        }
        return static_cast<std::uint32_t>(sessionId);
    }

    // formatSessionIdText：把会话编号转换成 UI 文本。
    QString formatSessionIdText(const std::uint32_t sessionId)
    {
        if (sessionId == std::numeric_limits<std::uint32_t>::max())
        {
            return QStringLiteral("<未知>");
        }
        return QString::number(sessionId);
    }

    // queryDesktopHeapSizeText：读取桌面对象的堆大小。
    QString queryDesktopHeapSizeText(HDESK desktopHandle)
    {
        DWORD heapSizeKb = 0;
        DWORD returnedBytes = 0;
        const BOOL queryOk = ::GetUserObjectInformationW(
            desktopHandle,
            UOI_HEAPSIZE,
            &heapSizeKb,
            sizeof(heapSizeKb),
            &returnedBytes);
        if (queryOk == FALSE || returnedBytes < sizeof(heapSizeKb))
        {
            return QStringLiteral("<失败:%1>").arg(::GetLastError());
        }
        return QString::number(heapSizeKb);
    }

    // enumerateWindowStationNameList：枚举当前会话可见的窗口站名称。
    std::vector<QString> enumerateWindowStationNameList(DWORD& errorCodeOut)
    {
        errorCodeOut = ERROR_SUCCESS;

        // nameList：回调里累加窗口站名称，函数末尾统一排序去重。
        std::vector<QString> nameList;
        nameList.reserve(8);

        struct WindowStationEnumContext
        {
            std::vector<QString>* outputNameList = nullptr; // outputNameList：窗口站名输出容器。
        };

        WindowStationEnumContext enumContext;
        enumContext.outputNameList = &nameList;
        auto enumWindowStationProc = [](LPWSTR windowStationName, LPARAM lParam) -> BOOL {
            WindowStationEnumContext* context = reinterpret_cast<WindowStationEnumContext*>(lParam);
            if (context == nullptr || context->outputNameList == nullptr)
            {
                return FALSE;
            }
            if (windowStationName != nullptr)
            {
                context->outputNameList->push_back(QString::fromWCharArray(windowStationName));
            }
            return TRUE;
        };

        const BOOL enumOk = ::EnumWindowStationsW(
            enumWindowStationProc,
            reinterpret_cast<LPARAM>(&enumContext));
        if (enumOk == FALSE)
        {
            errorCodeOut = ::GetLastError();
        }

        std::sort(nameList.begin(), nameList.end(), [](const QString& left, const QString& right) {
            return left.localeAwareCompare(right) < 0;
        });
        nameList.erase(
            std::unique(nameList.begin(), nameList.end()),
            nameList.end());
        return nameList;
    }

    // enumerateDesktopNameList：在指定窗口站中枚举桌面名称。
    std::vector<QString> enumerateDesktopNameList(HWINSTA windowStationHandle, DWORD& errorCodeOut)
    {
        errorCodeOut = ERROR_SUCCESS;

        // nameList：回调里追加桌面名称，最后统一排序去重。
        std::vector<QString> nameList;
        nameList.reserve(16);

        struct DesktopEnumContext
        {
            std::vector<QString>* outputNameList = nullptr; // outputNameList：桌面名输出容器。
        };

        DesktopEnumContext enumContext;
        enumContext.outputNameList = &nameList;
        auto enumDesktopProc = [](LPWSTR desktopName, LPARAM lParam) -> BOOL {
            DesktopEnumContext* context = reinterpret_cast<DesktopEnumContext*>(lParam);
            if (context == nullptr || context->outputNameList == nullptr)
            {
                return FALSE;
            }
            if (desktopName != nullptr)
            {
                context->outputNameList->push_back(QString::fromWCharArray(desktopName));
            }
            return TRUE;
        };

        const BOOL enumOk = ::EnumDesktopsW(
            windowStationHandle,
            enumDesktopProc,
            reinterpret_cast<LPARAM>(&enumContext));
        if (enumOk == FALSE)
        {
            errorCodeOut = ::GetLastError();
        }

        std::sort(nameList.begin(), nameList.end(), [](const QString& left, const QString& right) {
            return left.localeAwareCompare(right) < 0;
        });
        nameList.erase(
            std::unique(nameList.begin(), nameList.end()),
            nameList.end());
        return nameList;
    }

    // applyDesktopRowStyle：给当前桌面、当前窗口站和不可读行设置视觉区分。
    void applyDesktopRowStyle(QTableWidgetItem* item, const DesktopRowData& rowData)
    {
        if (item == nullptr)
        {
            return;
        }

        if (rowData.isCurrentDesktop)
        {
            QFont currentFont = item->font();
            currentFont.setBold(true);
            item->setFont(currentFont);
            item->setBackground(QColor(223, 246, 232));
            item->setForeground(QColor(22, 92, 54));
            return;
        }

        if (rowData.isCurrentWindowStation)
        {
            item->setBackground(QColor(231, 241, 255));
        }

        if (rowData.readCapability == DesktopCapabilityState::No)
        {
            item->setForeground(QColor(168, 52, 52));
        }
    }

    // setDesktopTableItem：统一创建表格项，并写入右键菜单与切换所需元数据。
    void setDesktopTableItem(
        QTableWidget* table,
        const int row,
        const int column,
        const QString& text,
        const DesktopRowData& rowData)
    {
        if (table == nullptr)
        {
            return;
        }

        QTableWidgetItem* item = new QTableWidgetItem(text);
        item->setToolTip(text);
        item->setData(kDesktopRoleWindowStationName, rowData.windowStationName);
        item->setData(kDesktopRoleDesktopName, rowData.desktopName);
        item->setData(kDesktopRoleIsCurrentWindowStation, rowData.isCurrentWindowStation);
        item->setData(kDesktopRoleOwnerSidText, rowData.ownerSidText);
        item->setData(kDesktopRoleOwnerSidDetailText, rowData.ownerSidDetailText);

        if (column >= kDesktopColumnCurrentWindowStation
            && column <= kDesktopColumnHeapKb
            && column != kDesktopColumnSidDetail)
        {
            item->setTextAlignment(Qt::AlignCenter);
        }

        applyDesktopRowStyle(item, rowData);
        table->setItem(row, column, item);
    }
}

void OtherDock::refreshDesktopList()
{
    // refreshEvent：整条“窗口站枚举 -> 桌面枚举 -> UI 刷新”链路共用一个事件 GUID。
    kLogEvent refreshEvent;
    info << refreshEvent
        << "[OtherDock] 开始刷新桌面列表（窗口站/桌面扩展模式）。"
        << eol;

    if (m_desktopTable == nullptr || m_desktopStatusLabel == nullptr)
    {
        err << refreshEvent
            << "[OtherDock] 刷新桌面列表失败：桌面管理控件未初始化。"
            << eol;
        return;
    }

    // currentWindowStationHandle/currentWindowStationName/currentDesktopName：记录当前上下文，用于高亮和切换限制。
    HWINSTA currentWindowStationHandle = ::GetProcessWindowStation();
    const QString currentWindowStationName = queryUserObjectName(currentWindowStationHandle);
    const QString currentDesktopName = queryUserObjectName(::GetThreadDesktop(::GetCurrentThreadId()));
    const std::uint32_t currentSessionId = queryCurrentSessionId();

    if (currentWindowStationHandle == nullptr)
    {
        const DWORD errorCode = ::GetLastError();
        err << refreshEvent
            << "[OtherDock] 刷新桌面列表失败：GetProcessWindowStation失败, code="
            << errorCode
            << eol;
        m_desktopTable->clearContents();
        m_desktopTable->setRowCount(0);
        m_desktopStatusLabel->setText(QStringLiteral("刷新失败：无法获取当前窗口站，错误码=%1").arg(errorCode));
        return;
    }

    // windowStationEnumError/windowStationNameList：先枚举窗口站；若失败则至少保留当前窗口站。
    DWORD windowStationEnumError = ERROR_SUCCESS;
    std::vector<QString> windowStationNameList = enumerateWindowStationNameList(windowStationEnumError);
    if (!currentWindowStationName.isEmpty())
    {
        windowStationNameList.push_back(currentWindowStationName);
    }
    std::sort(windowStationNameList.begin(), windowStationNameList.end(), [](const QString& left, const QString& right) {
        return left.localeAwareCompare(right) < 0;
    });
    windowStationNameList.erase(
        std::unique(windowStationNameList.begin(), windowStationNameList.end()),
        windowStationNameList.end());

    if (windowStationNameList.empty())
    {
        err << refreshEvent
            << "[OtherDock] 刷新桌面列表失败：未能获取任何窗口站名称, enumCode="
            << windowStationEnumError
            << eol;
        m_desktopTable->clearContents();
        m_desktopTable->setRowCount(0);
        m_desktopStatusLabel->setText(QStringLiteral("刷新失败：无法枚举窗口站，错误码=%1").arg(windowStationEnumError));
        return;
    }

    // rowDataList：缓存最终要写进表格的所有行；selectRowIndex：优先选中当前桌面所在行。
    std::vector<DesktopRowData> rowDataList;
    rowDataList.reserve(32);
    int selectRowIndex = -1;
    int desktopCountForStatus = 0;
    int accessibleWindowStationCount = 0;

    for (const QString& windowStationName : windowStationNameList)
    {
        // isCurrentWindowStation：标识当前循环处理的窗口站是否就是当前进程窗口站。
        const bool isCurrentWindowStation = !currentWindowStationName.isEmpty()
            && QString::compare(windowStationName, currentWindowStationName, Qt::CaseInsensitive) == 0;

        // windowStationHandle/needCloseWindowStation：非当前窗口站需显式 Open/Close。
        HWINSTA windowStationHandle = currentWindowStationHandle;
        bool needCloseWindowStation = false;
        if (!isCurrentWindowStation)
        {
            windowStationHandle = ::OpenWindowStationW(
                reinterpret_cast<LPCWSTR>(windowStationName.utf16()),
                FALSE,
                WINSTA_ENUMDESKTOPS | WINSTA_ENUMERATE | WINSTA_READATTRIBUTES | READ_CONTROL);
            needCloseWindowStation = windowStationHandle != nullptr;
        }

        // stationFlags/stationFlagsText/stationOwnerInfo：窗口站级别的上下文。
        USEROBJECTFLAGS stationFlags{};
        const bool stationFlagsOk = windowStationHandle != nullptr
            && queryUserObjectFlags(windowStationHandle, stationFlags);
        const QString stationFlagsText = stationFlagsOk
            ? formatUserObjectFlags(stationFlags, true)
            : QStringLiteral("未知");
        const bool isInteractiveWindowStation = stationFlagsOk
            && ((stationFlags.dwFlags & WSF_VISIBLE) != 0);

        const SidDescription stationOwnerInfo = windowStationHandle != nullptr
            ? queryWindowObjectOwnerInfo(windowStationHandle)
            : SidDescription{
                QStringLiteral("-"),
                QStringLiteral("-"),
                QStringLiteral("<窗口站句柄为空>")
            };

        if (windowStationHandle == nullptr)
        {
            const DWORD openErrorCode = ::GetLastError();
            DesktopRowData rowData;
            rowData.windowStationName = windowStationName;
            rowData.desktopName = QStringLiteral("<窗口站无法打开>");
            rowData.isCurrentWindowStation = isCurrentWindowStation;
            rowData.isInteractiveWindowStation = false;
            rowData.sessionId = currentSessionId;
            rowData.ownerAccountText = QStringLiteral("-");
            rowData.ownerSidText = QStringLiteral("-");
            rowData.ownerSidDetailText = QStringLiteral("<窗口站无法打开>");
            rowData.heapSizeText = QStringLiteral("-");
            rowData.remarkText = QStringLiteral("OpenWindowStationW 失败，错误码=%1").arg(openErrorCode);
            rowDataList.push_back(rowData);

            warn << refreshEvent
                << "[OtherDock] 窗口站打开失败, station="
                << windowStationName.toStdString()
                << ", code="
                << openErrorCode
                << eol;
            continue;
        }

        ++accessibleWindowStationCount;

        // desktopEnumError/desktopNameList：枚举该窗口站下的桌面名称。
        DWORD desktopEnumError = ERROR_SUCCESS;
        const std::vector<QString> desktopNameList = enumerateDesktopNameList(windowStationHandle, desktopEnumError);
        if (desktopNameList.empty())
        {
            DesktopRowData rowData;
            rowData.windowStationName = windowStationName;
            rowData.desktopName = desktopEnumError == ERROR_SUCCESS
                ? QStringLiteral("<无桌面>")
                : QStringLiteral("<枚举失败>");
            rowData.isCurrentWindowStation = isCurrentWindowStation;
            rowData.isInteractiveWindowStation = isInteractiveWindowStation;
            rowData.sessionId = currentSessionId;
            rowData.ownerAccountText = stationOwnerInfo.accountText;
            rowData.ownerSidText = stationOwnerInfo.sidText;
            rowData.ownerSidDetailText = stationOwnerInfo.detailText;
            rowData.heapSizeText = QStringLiteral("-");
            rowData.remarkText = desktopEnumError == ERROR_SUCCESS
                ? QStringLiteral("该窗口站未枚举到桌面；工作站标志=%1").arg(stationFlagsText)
                : QStringLiteral("EnumDesktopsW 失败，错误码=%1；工作站标志=%2")
                    .arg(desktopEnumError)
                    .arg(stationFlagsText);
            rowDataList.push_back(rowData);

            if (desktopEnumError != ERROR_SUCCESS)
            {
                warn << refreshEvent
                    << "[OtherDock] 枚举桌面失败, station="
                    << windowStationName.toStdString()
                    << ", code="
                    << desktopEnumError
                    << eol;
            }

            if (needCloseWindowStation)
            {
                ::CloseWindowStation(windowStationHandle);
            }
            continue;
        }

        desktopCountForStatus += static_cast<int>(desktopNameList.size());

        for (const QString& desktopName : desktopNameList)
        {
            DesktopRowData rowData;
            rowData.windowStationName = windowStationName;
            rowData.desktopName = desktopName;
            rowData.isCurrentWindowStation = isCurrentWindowStation;
            rowData.isCurrentDesktop = isCurrentWindowStation
                && !currentDesktopName.isEmpty()
                && QString::compare(desktopName, currentDesktopName, Qt::CaseInsensitive) == 0;
            rowData.isInteractiveWindowStation = isInteractiveWindowStation;
            rowData.sessionId = currentSessionId;
            rowData.ownerAccountText = stationOwnerInfo.accountText;
            rowData.ownerSidText = stationOwnerInfo.sidText;
            rowData.ownerSidDetailText = stationOwnerInfo.detailText;
            rowData.heapSizeText = QStringLiteral("-");

            // remarkList：聚合本行的工作站标志、输入能力、错误码与限制说明。
            QStringList remarkList;
            remarkList << QStringLiteral("工作站标志=%1").arg(stationFlagsText);
            const DesktopOpenResult readOpenResult = tryOpenDesktopHandle(
                windowStationName,
                desktopName,
                DESKTOP_READOBJECTS | READ_CONTROL);
            if (readOpenResult.desktopHandle != nullptr)
            {
                rowData.readCapability = DesktopCapabilityState::Yes;
                rowData.heapSizeText = queryDesktopHeapSizeText(readOpenResult.desktopHandle);
                remarkList << QStringLiteral("打开(读)=%1").arg(readOpenResult.methodText);

                bool inputDesktopEnabled = false;
                if (queryDesktopInputState(readOpenResult.desktopHandle, inputDesktopEnabled))
                {
                    rowData.inputDesktopKnown = true;
                    rowData.inputDesktopEnabled = inputDesktopEnabled;
                    remarkList << QStringLiteral("接收输入=%1")
                        .arg(inputDesktopEnabled ? QStringLiteral("是") : QStringLiteral("否"));
                }
                else
                {
                    remarkList << QStringLiteral("接收输入=<查询失败:%1>").arg(::GetLastError());
                }

                const SidDescription desktopOwnerInfo = queryWindowObjectOwnerInfo(readOpenResult.desktopHandle);
                if (!desktopOwnerInfo.sidText.startsWith(QStringLiteral("<查询失败")))
                {
                    rowData.ownerAccountText = desktopOwnerInfo.accountText;
                    rowData.ownerSidText = desktopOwnerInfo.sidText;
                    rowData.ownerSidDetailText = desktopOwnerInfo.detailText;
                    remarkList << QStringLiteral("所有者来源=桌面对象");
                }
                else
                {
                    remarkList << QStringLiteral("所有者来源=窗口站（桌面查询失败）");
                }

                ::CloseDesktop(readOpenResult.desktopHandle);
            }
            else
            {
                rowData.readCapability = DesktopCapabilityState::No;
                rowData.heapSizeText = QStringLiteral("-");
                remarkList << QStringLiteral("打开(读)失败=%1").arg(readOpenResult.detailText);
                remarkList << QStringLiteral("所有者来源=窗口站");
            }

            const DesktopOpenResult switchOpenResult = tryOpenDesktopHandle(
                windowStationName,
                desktopName,
                DESKTOP_SWITCHDESKTOP | DESKTOP_READOBJECTS);
            if (switchOpenResult.desktopHandle != nullptr)
            {
                rowData.switchCapability = DesktopCapabilityState::Yes;
                remarkList << QStringLiteral("打开(切换)=%1").arg(switchOpenResult.methodText);
                ::CloseDesktop(switchOpenResult.desktopHandle);
            }
            else
            {
                rowData.switchCapability = DesktopCapabilityState::No;
                remarkList << QStringLiteral("打开(切换)失败=%1").arg(switchOpenResult.detailText);
            }

            if (!isCurrentWindowStation)
            {
                remarkList << QStringLiteral("非当前窗口站也已尝试 OpenDesktopW");
            }

            rowData.remarkText = remarkList.join(QStringLiteral("；"));
            rowDataList.push_back(rowData);

            if (rowData.isCurrentDesktop)
            {
                selectRowIndex = static_cast<int>(rowDataList.size()) - 1;
            }
        }

        if (needCloseWindowStation)
        {
            ::CloseWindowStation(windowStationHandle);
        }
    }

    // 清空旧表格内容后统一写入，避免残留上一轮的宽表数据。
    m_desktopTable->clearContents();
    m_desktopTable->setRowCount(static_cast<int>(rowDataList.size()));

    for (int row = 0; row < static_cast<int>(rowDataList.size()); ++row)
    {
        const DesktopRowData& rowData = rowDataList[static_cast<std::size_t>(row)];
        setDesktopTableItem(m_desktopTable, row, kDesktopColumnWindowStation, rowData.windowStationName, rowData);
        setDesktopTableItem(m_desktopTable, row, kDesktopColumnDesktopName, rowData.desktopName, rowData);
        setDesktopTableItem(
            m_desktopTable,
            row,
            kDesktopColumnCurrentWindowStation,
            rowData.isCurrentWindowStation ? QStringLiteral("是") : QStringLiteral("否"),
            rowData);
        setDesktopTableItem(
            m_desktopTable,
            row,
            kDesktopColumnCurrentDesktop,
            rowData.isCurrentDesktop ? QStringLiteral("是") : QStringLiteral("否"),
            rowData);
        setDesktopTableItem(
            m_desktopTable,
            row,
            kDesktopColumnInteractive,
            rowData.isInteractiveWindowStation ? QStringLiteral("是") : QStringLiteral("否"),
            rowData);
        setDesktopTableItem(
            m_desktopTable,
            row,
            kDesktopColumnReadable,
            capabilityStateText(rowData.readCapability),
            rowData);
        setDesktopTableItem(
            m_desktopTable,
            row,
            kDesktopColumnSwitchable,
            capabilityStateText(rowData.switchCapability),
            rowData);
        setDesktopTableItem(
            m_desktopTable,
            row,
            kDesktopColumnSessionId,
            formatSessionIdText(rowData.sessionId),
            rowData);
        setDesktopTableItem(m_desktopTable, row, kDesktopColumnOwner, rowData.ownerAccountText, rowData);
        setDesktopTableItem(m_desktopTable, row, kDesktopColumnSid, rowData.ownerSidText, rowData);
        setDesktopTableItem(m_desktopTable, row, kDesktopColumnSidDetail, rowData.ownerSidDetailText, rowData);
        setDesktopTableItem(m_desktopTable, row, kDesktopColumnHeapKb, rowData.heapSizeText, rowData);
        setDesktopTableItem(m_desktopTable, row, kDesktopColumnRemark, rowData.remarkText, rowData);
    }

    if (selectRowIndex >= 0 && selectRowIndex < m_desktopTable->rowCount())
    {
        m_desktopTable->selectRow(selectRowIndex);
    }
    else if (m_desktopTable->rowCount() > 0)
    {
        m_desktopTable->selectRow(0);
    }

    const QString statusText = QStringLiteral(
        "已枚举 %1 个窗口站（可访问 %2 个）、%3 个桌面；当前站：%4；当前桌面：%5；SessionId：%6")
        .arg(windowStationNameList.size())
        .arg(accessibleWindowStationCount)
        .arg(desktopCountForStatus)
        .arg(currentWindowStationName.isEmpty() ? QStringLiteral("<未知>") : currentWindowStationName)
        .arg(currentDesktopName.isEmpty() ? QStringLiteral("<未知>") : currentDesktopName)
        .arg(formatSessionIdText(currentSessionId));
    m_desktopStatusLabel->setText(statusText);

    info << refreshEvent
        << "[OtherDock] 桌面列表刷新完成, windowStationCount="
        << windowStationNameList.size()
        << ", accessibleWindowStationCount="
        << accessibleWindowStationCount
        << ", desktopCount="
        << desktopCountForStatus
        << ", currentWindowStation="
        << currentWindowStationName.toStdString()
        << ", currentDesktop="
        << currentDesktopName.toStdString()
        << eol;
}
