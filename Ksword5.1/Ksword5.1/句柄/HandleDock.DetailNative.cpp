#include "HandleDock.h"

// ============================================================
// HandleDock.DetailNative.cpp
// 作用：
// - 承载 GrantedAccess 语义解码；
// - 承载句柄详情的原生类型分支解析；
// - 与主 Native 文件拆开，避免单文件过长。
// ============================================================

#include <QChar>
#include <QStringList>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <sddl.h>

QString HandleDock::decodeGrantedAccessText(const QString& typeName, const std::uint32_t grantedAccess)
{
    QStringList accessTextList;

    // 通用权限位先统一解码，保证不同对象类型都能看到标准权限。
    if ((grantedAccess & DELETE) != 0) accessTextList.push_back(QStringLiteral("DELETE"));
    if ((grantedAccess & READ_CONTROL) != 0) accessTextList.push_back(QStringLiteral("READ_CONTROL"));
    if ((grantedAccess & WRITE_DAC) != 0) accessTextList.push_back(QStringLiteral("WRITE_DAC"));
    if ((grantedAccess & WRITE_OWNER) != 0) accessTextList.push_back(QStringLiteral("WRITE_OWNER"));
    if ((grantedAccess & SYNCHRONIZE) != 0) accessTextList.push_back(QStringLiteral("SYNCHRONIZE"));
    if ((grantedAccess & ACCESS_SYSTEM_SECURITY) != 0) accessTextList.push_back(QStringLiteral("ACCESS_SYSTEM_SECURITY"));
    if ((grantedAccess & GENERIC_READ) != 0) accessTextList.push_back(QStringLiteral("GENERIC_READ"));
    if ((grantedAccess & GENERIC_WRITE) != 0) accessTextList.push_back(QStringLiteral("GENERIC_WRITE"));
    if ((grantedAccess & GENERIC_EXECUTE) != 0) accessTextList.push_back(QStringLiteral("GENERIC_EXECUTE"));
    if ((grantedAccess & GENERIC_ALL) != 0) accessTextList.push_back(QStringLiteral("GENERIC_ALL"));

    const QString normalizedType = typeName.trimmed().toLower();
    const auto appendIf = [&accessTextList, grantedAccess](const std::uint32_t maskValue, const QString& nameText)
        {
            if ((grantedAccess & maskValue) != 0)
            {
                accessTextList.push_back(nameText);
            }
        };

    if (normalizedType == QStringLiteral("file") || normalizedType == QStringLiteral("directory"))
    {
        appendIf(FILE_READ_DATA, QStringLiteral("FILE_READ_DATA/LIST_DIRECTORY"));
        appendIf(FILE_WRITE_DATA, QStringLiteral("FILE_WRITE_DATA/ADD_FILE"));
        appendIf(FILE_APPEND_DATA, QStringLiteral("FILE_APPEND_DATA/ADD_SUBDIR"));
        appendIf(FILE_READ_EA, QStringLiteral("FILE_READ_EA"));
        appendIf(FILE_WRITE_EA, QStringLiteral("FILE_WRITE_EA"));
        appendIf(FILE_EXECUTE, QStringLiteral("FILE_EXECUTE/TRAVERSE"));
        appendIf(FILE_DELETE_CHILD, QStringLiteral("FILE_DELETE_CHILD"));
        appendIf(FILE_READ_ATTRIBUTES, QStringLiteral("FILE_READ_ATTRIBUTES"));
        appendIf(FILE_WRITE_ATTRIBUTES, QStringLiteral("FILE_WRITE_ATTRIBUTES"));
    }
    else if (normalizedType == QStringLiteral("key"))
    {
        appendIf(KEY_QUERY_VALUE, QStringLiteral("KEY_QUERY_VALUE"));
        appendIf(KEY_SET_VALUE, QStringLiteral("KEY_SET_VALUE"));
        appendIf(KEY_CREATE_SUB_KEY, QStringLiteral("KEY_CREATE_SUB_KEY"));
        appendIf(KEY_ENUMERATE_SUB_KEYS, QStringLiteral("KEY_ENUMERATE_SUB_KEYS"));
        appendIf(KEY_NOTIFY, QStringLiteral("KEY_NOTIFY"));
        appendIf(KEY_CREATE_LINK, QStringLiteral("KEY_CREATE_LINK"));
        appendIf(KEY_WOW64_32KEY, QStringLiteral("KEY_WOW64_32KEY"));
        appendIf(KEY_WOW64_64KEY, QStringLiteral("KEY_WOW64_64KEY"));
    }
    else if (normalizedType == QStringLiteral("process"))
    {
        appendIf(PROCESS_TERMINATE, QStringLiteral("PROCESS_TERMINATE"));
        appendIf(PROCESS_CREATE_THREAD, QStringLiteral("PROCESS_CREATE_THREAD"));
        appendIf(PROCESS_SET_SESSIONID, QStringLiteral("PROCESS_SET_SESSIONID"));
        appendIf(PROCESS_VM_OPERATION, QStringLiteral("PROCESS_VM_OPERATION"));
        appendIf(PROCESS_VM_READ, QStringLiteral("PROCESS_VM_READ"));
        appendIf(PROCESS_VM_WRITE, QStringLiteral("PROCESS_VM_WRITE"));
        appendIf(PROCESS_DUP_HANDLE, QStringLiteral("PROCESS_DUP_HANDLE"));
        appendIf(PROCESS_CREATE_PROCESS, QStringLiteral("PROCESS_CREATE_PROCESS"));
        appendIf(PROCESS_SET_QUOTA, QStringLiteral("PROCESS_SET_QUOTA"));
        appendIf(PROCESS_SET_INFORMATION, QStringLiteral("PROCESS_SET_INFORMATION"));
        appendIf(PROCESS_QUERY_INFORMATION, QStringLiteral("PROCESS_QUERY_INFORMATION"));
        appendIf(PROCESS_SUSPEND_RESUME, QStringLiteral("PROCESS_SUSPEND_RESUME"));
        appendIf(PROCESS_QUERY_LIMITED_INFORMATION, QStringLiteral("PROCESS_QUERY_LIMITED_INFORMATION"));
    }
    else if (normalizedType == QStringLiteral("thread"))
    {
        appendIf(THREAD_TERMINATE, QStringLiteral("THREAD_TERMINATE"));
        appendIf(THREAD_SUSPEND_RESUME, QStringLiteral("THREAD_SUSPEND_RESUME"));
        appendIf(THREAD_GET_CONTEXT, QStringLiteral("THREAD_GET_CONTEXT"));
        appendIf(THREAD_SET_CONTEXT, QStringLiteral("THREAD_SET_CONTEXT"));
        appendIf(THREAD_SET_INFORMATION, QStringLiteral("THREAD_SET_INFORMATION"));
        appendIf(THREAD_QUERY_INFORMATION, QStringLiteral("THREAD_QUERY_INFORMATION"));
        appendIf(THREAD_SET_THREAD_TOKEN, QStringLiteral("THREAD_SET_THREAD_TOKEN"));
        appendIf(THREAD_IMPERSONATE, QStringLiteral("THREAD_IMPERSONATE"));
        appendIf(THREAD_DIRECT_IMPERSONATION, QStringLiteral("THREAD_DIRECT_IMPERSONATION"));
        appendIf(THREAD_SET_LIMITED_INFORMATION, QStringLiteral("THREAD_SET_LIMITED_INFORMATION"));
        appendIf(THREAD_QUERY_LIMITED_INFORMATION, QStringLiteral("THREAD_QUERY_LIMITED_INFORMATION"));
    }
    else if (normalizedType == QStringLiteral("token"))
    {
        appendIf(TOKEN_ASSIGN_PRIMARY, QStringLiteral("TOKEN_ASSIGN_PRIMARY"));
        appendIf(TOKEN_DUPLICATE, QStringLiteral("TOKEN_DUPLICATE"));
        appendIf(TOKEN_IMPERSONATE, QStringLiteral("TOKEN_IMPERSONATE"));
        appendIf(TOKEN_QUERY, QStringLiteral("TOKEN_QUERY"));
        appendIf(TOKEN_QUERY_SOURCE, QStringLiteral("TOKEN_QUERY_SOURCE"));
        appendIf(TOKEN_ADJUST_PRIVILEGES, QStringLiteral("TOKEN_ADJUST_PRIVILEGES"));
        appendIf(TOKEN_ADJUST_GROUPS, QStringLiteral("TOKEN_ADJUST_GROUPS"));
        appendIf(TOKEN_ADJUST_DEFAULT, QStringLiteral("TOKEN_ADJUST_DEFAULT"));
        appendIf(TOKEN_ADJUST_SESSIONID, QStringLiteral("TOKEN_ADJUST_SESSIONID"));
    }
    else if (normalizedType == QStringLiteral("section"))
    {
        appendIf(SECTION_QUERY, QStringLiteral("SECTION_QUERY"));
        appendIf(SECTION_MAP_WRITE, QStringLiteral("SECTION_MAP_WRITE"));
        appendIf(SECTION_MAP_READ, QStringLiteral("SECTION_MAP_READ"));
        appendIf(SECTION_MAP_EXECUTE, QStringLiteral("SECTION_MAP_EXECUTE"));
        appendIf(SECTION_EXTEND_SIZE, QStringLiteral("SECTION_EXTEND_SIZE"));
    }
    else if (normalizedType == QStringLiteral("event"))
    {
        appendIf(0x0001U, QStringLiteral("EVENT_QUERY_STATE"));
        appendIf(EVENT_MODIFY_STATE, QStringLiteral("EVENT_MODIFY_STATE"));
    }
    else if (normalizedType == QStringLiteral("mutant"))
    {
        appendIf(MUTANT_QUERY_STATE, QStringLiteral("MUTANT_QUERY_STATE"));
    }
    else if (normalizedType == QStringLiteral("semaphore"))
    {
        appendIf(0x0001U, QStringLiteral("SEMAPHORE_QUERY_STATE"));
        appendIf(SEMAPHORE_MODIFY_STATE, QStringLiteral("SEMAPHORE_MODIFY_STATE"));
    }
    else if (normalizedType == QStringLiteral("timer"))
    {
        appendIf(TIMER_QUERY_STATE, QStringLiteral("TIMER_QUERY_STATE"));
        appendIf(TIMER_MODIFY_STATE, QStringLiteral("TIMER_MODIFY_STATE"));
    }

    accessTextList.removeDuplicates();
    if (accessTextList.isEmpty())
    {
        return QStringLiteral("-");
    }
    return accessTextList.join(QStringLiteral(" | "));
}

HandleDock::HandleDetailRefreshResult HandleDock::buildHandleDetailRefreshResult(const HandleRow& row)
{
    HandleDetailRefreshResult result{};
    const auto beginTime = std::chrono::steady_clock::now();

    auto addField = [&result](const QString& keyText, const QString& valueText)
        {
            HandleDetailField field{};
            field.keyText = keyText;
            field.valueText = valueText;
            result.fields.push_back(std::move(field));
        };

    addField(QStringLiteral("PID"), QString::number(row.processId));
    addField(QStringLiteral("进程名"), row.processName);
    addField(QStringLiteral("句柄值"), formatHex(row.handleValue, 0));
    addField(QStringLiteral("TypeIndex"), QString::number(row.typeIndex));
    addField(QStringLiteral("类型"), row.typeName);
    addField(QStringLiteral("对象地址"), formatHex(row.objectAddress, 0));
    addField(QStringLiteral("访问掩码"), formatHex(row.grantedAccess, 8));
    addField(QStringLiteral("访问掩码语义"), decodeGrantedAccessText(row.typeName, row.grantedAccess));
    addField(QStringLiteral("属性"), formatHandleAttributes(row.attributes));
    addField(QStringLiteral("HandleCount"), row.handleCount == 0 ? QStringLiteral("-") : QString::number(row.handleCount));
    addField(QStringLiteral("PointerCount"), row.pointerCount == 0 ? QStringLiteral("-") : QString::number(row.pointerCount));
    addField(QStringLiteral("对象名"), row.objectName.trimmed().isEmpty() ? QStringLiteral("-") : row.objectName);

    HANDLE processHandle = ::OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, row.processId);
    if (processHandle == nullptr)
    {
        processHandle = ::OpenProcess(PROCESS_DUP_HANDLE, FALSE, row.processId);
    }
    if (processHandle == nullptr)
    {
        result.diagnosticText = QStringLiteral("OpenProcess 失败，error=%1").arg(::GetLastError());
        result.elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - beginTime).count());
        return result;
    }

    HANDLE duplicatedHandle = nullptr;
    const BOOL duplicateOk = ::DuplicateHandle(
        processHandle,
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(row.handleValue)),
        ::GetCurrentProcess(),
        &duplicatedHandle,
        0,
        FALSE,
        DUPLICATE_SAME_ACCESS);
    ::CloseHandle(processHandle);
    if (duplicateOk == FALSE || duplicatedHandle == nullptr)
    {
        result.diagnosticText = QStringLiteral("DuplicateHandle 失败，error=%1").arg(::GetLastError());
        result.elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - beginTime).count());
        return result;
    }

    const QString normalizedType = row.typeName.trimmed().toLower();

    if (normalizedType == QStringLiteral("file") || normalizedType == QStringLiteral("directory"))
    {
        wchar_t pathBuffer[4096] = {};
        const DWORD pathLength = ::GetFinalPathNameByHandleW(
            duplicatedHandle,
            pathBuffer,
            static_cast<DWORD>(std::size(pathBuffer)),
            FILE_NAME_NORMALIZED);
        if (pathLength > 0 && pathLength < std::size(pathBuffer))
        {
            addField(QStringLiteral("最终路径"), QString::fromWCharArray(pathBuffer));
        }
        addField(QStringLiteral("文件类型"), QString::number(::GetFileType(duplicatedHandle)));
    }
    else if (normalizedType == QStringLiteral("process"))
    {
        addField(QStringLiteral("目标PID"), QString::number(::GetProcessId(duplicatedHandle)));
        wchar_t imagePathBuffer[2048] = {};
        DWORD bufferChars = static_cast<DWORD>(std::size(imagePathBuffer));
        if (::QueryFullProcessImageNameW(duplicatedHandle, 0, imagePathBuffer, &bufferChars) != FALSE)
        {
            addField(QStringLiteral("目标进程路径"), QString::fromWCharArray(imagePathBuffer));
        }
        addField(QStringLiteral("优先级类"), QString::number(::GetPriorityClass(duplicatedHandle)));
    }
    else if (normalizedType == QStringLiteral("thread"))
    {
        addField(QStringLiteral("目标TID"), QString::number(::GetThreadId(duplicatedHandle)));
        addField(QStringLiteral("所属PID"), QString::number(::GetProcessIdOfThread(duplicatedHandle)));
        addField(QStringLiteral("线程优先级"), QString::number(::GetThreadPriority(duplicatedHandle)));
    }
    else if (normalizedType == QStringLiteral("token"))
    {
        DWORD requiredLength = 0;
        ::GetTokenInformation(duplicatedHandle, TokenUser, nullptr, 0, &requiredLength);
        if (requiredLength > 0)
        {
            std::vector<std::uint8_t> tokenUserBuffer(requiredLength, 0);
            if (::GetTokenInformation(duplicatedHandle, TokenUser, tokenUserBuffer.data(), requiredLength, &requiredLength) != FALSE)
            {
                const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenUserBuffer.data());
                if (tokenUser != nullptr && tokenUser->User.Sid != nullptr)
                {
                    wchar_t accountName[256] = {};
                    wchar_t domainName[256] = {};
                    DWORD accountNameLength = static_cast<DWORD>(std::size(accountName));
                    DWORD domainNameLength = static_cast<DWORD>(std::size(domainName));
                    SID_NAME_USE sidType = SidTypeUnknown;
                    if (::LookupAccountSidW(
                        nullptr,
                        tokenUser->User.Sid,
                        accountName,
                        &accountNameLength,
                        domainName,
                        &domainNameLength,
                        &sidType) != FALSE)
                    {
                        addField(
                            QStringLiteral("用户"),
                            QStringLiteral("%1\\%2")
                            .arg(QString::fromWCharArray(domainName), QString::fromWCharArray(accountName)));
                    }

                    LPWSTR sidTextRaw = nullptr;
                    if (::ConvertSidToStringSidW(tokenUser->User.Sid, &sidTextRaw) != FALSE && sidTextRaw != nullptr)
                    {
                        addField(QStringLiteral("SID"), QString::fromWCharArray(sidTextRaw));
                        ::LocalFree(sidTextRaw);
                    }
                }
            }
        }

        requiredLength = 0;
        ::GetTokenInformation(duplicatedHandle, TokenIntegrityLevel, nullptr, 0, &requiredLength);
        if (requiredLength > 0)
        {
            std::vector<std::uint8_t> integrityBuffer(requiredLength, 0);
            if (::GetTokenInformation(
                duplicatedHandle,
                TokenIntegrityLevel,
                integrityBuffer.data(),
                requiredLength,
                &requiredLength) != FALSE)
            {
                const auto* tokenLabel = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrityBuffer.data());
                if (tokenLabel != nullptr && tokenLabel->Label.Sid != nullptr)
                {
                    const DWORD integrityRid =
                        *::GetSidSubAuthority(
                            tokenLabel->Label.Sid,
                            static_cast<DWORD>(*::GetSidSubAuthorityCount(tokenLabel->Label.Sid) - 1));
                    addField(QStringLiteral("完整性RID"), QString::number(integrityRid));
                }
            }
        }
    }
    else if (normalizedType == QStringLiteral("key"))
    {
        addField(QStringLiteral("注册表路径"), row.objectName.trimmed().isEmpty() ? QStringLiteral("-") : row.objectName);
    }
    else
    {
        addField(QStringLiteral("类型专用解析"), QStringLiteral("该类型当前使用通用详情展示。"));
    }

    ::CloseHandle(duplicatedHandle);
    result.elapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - beginTime).count());
    return result;
}

