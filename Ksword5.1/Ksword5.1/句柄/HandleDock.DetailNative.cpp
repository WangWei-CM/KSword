#include "HandleDock.h"
#include "../ArkDriverClient/ArkDriverClient.h"

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
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <sddl.h>

namespace
{
    // wideToQString 作用：
    // - 把 ArkDriverClient 返回的 std::wstring 转为 QString；
    // - 空串保留为空，由调用者决定显示“无名称”或“未返回”。
    QString wideToQString(const std::wstring& text)
    {
        if (text.empty())
        {
            return {};
        }
        return QString::fromWCharArray(text.c_str(), static_cast<int>(text.size()));
    }

    // ntStatusHex 作用：把 NTSTATUS/Win32 long 状态统一格式化为 0xXXXXXXXX。
    QString ntStatusHex(const long status)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<quint32>(status), 8, 16, QChar('0'))
            .toUpper();
    }

    // objectQueryStatusText 作用：把 R0 对象查询状态转换成详情页可读文本。
    QString objectQueryStatusText(const std::uint32_t status)
    {
        switch (status)
        {
        case KSWORD_ARK_OBJECT_QUERY_STATUS_OK:
            return QStringLiteral("OK");
        case KSWORD_ARK_OBJECT_QUERY_STATUS_PARTIAL:
            return QStringLiteral("Partial");
        case KSWORD_ARK_OBJECT_QUERY_STATUS_DYNDATA_MISSING:
            return QStringLiteral("DynData Missing");
        case KSWORD_ARK_OBJECT_QUERY_STATUS_PROCESS_LOOKUP_FAILED:
            return QStringLiteral("Process Lookup Failed");
        case KSWORD_ARK_OBJECT_QUERY_STATUS_HANDLE_REFERENCE_FAILED:
            return QStringLiteral("Handle Reference Failed");
        case KSWORD_ARK_OBJECT_QUERY_STATUS_TYPE_QUERY_FAILED:
            return QStringLiteral("Type Query Failed");
        case KSWORD_ARK_OBJECT_QUERY_STATUS_NAME_QUERY_FAILED:
            return QStringLiteral("Name Query Failed");
        case KSWORD_ARK_OBJECT_QUERY_STATUS_NAME_TRUNCATED:
            return QStringLiteral("Name Truncated");
        case KSWORD_ARK_OBJECT_QUERY_STATUS_UNAVAILABLE:
        default:
            return QStringLiteral("Unavailable");
        }
    }

    // proxyStatusText 作用：把受限代理句柄策略状态转换成可读文本。
    QString proxyStatusText(const std::uint32_t status)
    {
        switch (status)
        {
        case KSWORD_ARK_OBJECT_PROXY_STATUS_OPENED:
            return QStringLiteral("Opened then closed (diagnostic only)");
        case KSWORD_ARK_OBJECT_PROXY_STATUS_DENIED_BY_POLICY:
            return QStringLiteral("Denied by policy");
        case KSWORD_ARK_OBJECT_PROXY_STATUS_OPEN_FAILED:
            return QStringLiteral("Open failed");
        case KSWORD_ARK_OBJECT_PROXY_STATUS_REQUESTOR_FAILED:
            return QStringLiteral("Requestor failed");
        case KSWORD_ARK_OBJECT_PROXY_STATUS_NOT_REQUESTED:
        default:
            return QStringLiteral("Not requested");
        }
    }

    // alpcQueryStatusText 作用：把 R0 ALPC 查询状态转换为详情页可读文本。
    QString alpcQueryStatusText(const std::uint32_t status)
    {
        switch (status)
        {
        case KSWORD_ARK_ALPC_QUERY_STATUS_OK:
            return QStringLiteral("OK");
        case KSWORD_ARK_ALPC_QUERY_STATUS_PARTIAL:
            return QStringLiteral("Partial");
        case KSWORD_ARK_ALPC_QUERY_STATUS_DYNDATA_MISSING:
            return QStringLiteral("DynData Missing");
        case KSWORD_ARK_ALPC_QUERY_STATUS_PROCESS_LOOKUP_FAILED:
            return QStringLiteral("Process Lookup Failed");
        case KSWORD_ARK_ALPC_QUERY_STATUS_HANDLE_REFERENCE_FAILED:
            return QStringLiteral("Handle Reference Failed");
        case KSWORD_ARK_ALPC_QUERY_STATUS_TYPE_MISMATCH:
            return QStringLiteral("Type Mismatch");
        case KSWORD_ARK_ALPC_QUERY_STATUS_BASIC_QUERY_FAILED:
            return QStringLiteral("Basic Query Failed");
        case KSWORD_ARK_ALPC_QUERY_STATUS_COMMUNICATION_FAILED:
            return QStringLiteral("Communication Query Failed");
        case KSWORD_ARK_ALPC_QUERY_STATUS_NAME_QUERY_FAILED:
            return QStringLiteral("Name Query Failed");
        case KSWORD_ARK_ALPC_QUERY_STATUS_NAME_TRUNCATED:
            return QStringLiteral("Name Truncated");
        case KSWORD_ARK_ALPC_QUERY_STATUS_UNAVAILABLE:
        default:
            return QStringLiteral("Unavailable");
        }
    }

    // isAlpcPortTypeName 作用：识别句柄详情中应尝试 R0 ALPC 查询的对象类型名。
    bool isAlpcPortTypeName(const QString& typeName)
    {
        const QString normalizedType = typeName.trimmed().toLower();
        return normalizedType == QStringLiteral("alpc port") ||
            normalizedType == QStringLiteral("port") ||
            normalizedType.contains(QStringLiteral("alpc"));
    }

    // alpcPortPresent 作用：按响应字段位判断某个关系端口是否存在。
    bool alpcPortPresent(const std::uint32_t responseFieldFlags, const std::uint32_t relation)
    {
        switch (relation)
        {
        case KSWORD_ARK_ALPC_PORT_RELATION_QUERY:
            return (responseFieldFlags & KSWORD_ARK_ALPC_RESPONSE_FIELD_QUERY_PORT_PRESENT) != 0U;
        case KSWORD_ARK_ALPC_PORT_RELATION_CONNECTION:
            return (responseFieldFlags & KSWORD_ARK_ALPC_RESPONSE_FIELD_CONNECTION_PRESENT) != 0U;
        case KSWORD_ARK_ALPC_PORT_RELATION_SERVER:
            return (responseFieldFlags & KSWORD_ARK_ALPC_RESPONSE_FIELD_SERVER_PRESENT) != 0U;
        case KSWORD_ARK_ALPC_PORT_RELATION_CLIENT:
            return (responseFieldFlags & KSWORD_ARK_ALPC_RESPONSE_FIELD_CLIENT_PRESENT) != 0U;
        default:
            return false;
        }
    }

    // alpcRelationTitle 作用：把 ALPC 关系枚举转换为中文标题前缀。
    QString alpcRelationTitle(const std::uint32_t relation)
    {
        switch (relation)
        {
        case KSWORD_ARK_ALPC_PORT_RELATION_QUERY:
            return QStringLiteral("ALPC当前端口");
        case KSWORD_ARK_ALPC_PORT_RELATION_CONNECTION:
            return QStringLiteral("ALPC连接端口");
        case KSWORD_ARK_ALPC_PORT_RELATION_SERVER:
            return QStringLiteral("ALPC服务端通信端口");
        case KSWORD_ARK_ALPC_PORT_RELATION_CLIENT:
            return QStringLiteral("ALPC客户端通信端口");
        default:
            return QStringLiteral("ALPC未知端口");
        }
    }
}

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
    addField(QStringLiteral("HandleCount"), formatOptionalObjectCount(row.handleCount, row.basicInfoAvailable));
    addField(QStringLiteral("PointerCount"), formatOptionalObjectCount(row.pointerCount, row.basicInfoAvailable));
    addField(QStringLiteral("来源"), formatHandleSourceText(row.sourceMode));
    addField(QStringLiteral("解码状态"), formatHandleDecodeStatusText(row.decodeStatus));
    addField(QStringLiteral("差异状态"), formatHandleDiffStatusText(row.diffStatus));
    addField(QStringLiteral("R0字段位"), formatHex(row.r0FieldFlags, 8));
    addField(QStringLiteral("DynData Capability"), formatHex(row.r0DynDataCapabilityMask, 0));
    addField(QStringLiteral("EPROCESS.ObjectTable偏移"), formatHex(row.epObjectTableOffset, 0));
    addField(QStringLiteral("HandleContentionEvent偏移"), formatHex(row.htHandleContentionEventOffset, 0));
    addField(QStringLiteral("ObDecodeShift"), QString::number(row.obDecodeShift));
    addField(QStringLiteral("ObAttributesShift"), QString::number(row.obAttributesShift));
    addField(QStringLiteral("OBJECT_TYPE.Name偏移"), formatHex(row.otNameOffset, 0));
    addField(QStringLiteral("OBJECT_TYPE.Index偏移"), formatHex(row.otIndexOffset, 0));
    addField(QStringLiteral("对象名"), formatObjectNameDisplayText(row));

    {
        const auto r0ObjectResult = ksword::ark::DriverClient().queryHandleObject(
            row.processId,
            row.handleValue,
            KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_ALL,
            row.grantedAccess);
        addField(QStringLiteral("R0对象查询IO"), QString::fromStdString(r0ObjectResult.io.message));
        if (r0ObjectResult.io.ok)
        {
            const QString r0TypeName = wideToQString(r0ObjectResult.typeName);
            const QString r0ObjectName = wideToQString(r0ObjectResult.objectName);
            addField(QStringLiteral("R0查询状态"), objectQueryStatusText(r0ObjectResult.queryStatus));
            addField(QStringLiteral("R0对象引用状态"), ntStatusHex(r0ObjectResult.objectReferenceStatus));
            addField(QStringLiteral("R0类型状态"), ntStatusHex(r0ObjectResult.typeStatus));
            addField(QStringLiteral("R0名称状态"), ntStatusHex(r0ObjectResult.nameStatus));
            addField(QStringLiteral("R0对象地址"), formatHex(r0ObjectResult.objectAddress, 0));
            addField(QStringLiteral("R0对象类型索引"), QString::number(r0ObjectResult.objectTypeIndex));
            addField(QStringLiteral("R0对象类型名"), r0TypeName.isEmpty() ? QStringLiteral("未返回") : r0TypeName);
            addField(QStringLiteral("R0对象名"), r0ObjectName.isEmpty() ? QStringLiteral("无名称/未返回") : r0ObjectName);
            addField(QStringLiteral("R0实际授权访问"), formatHex(r0ObjectResult.actualGrantedAccess, 8));
            addField(QStringLiteral("R0代理状态"), proxyStatusText(r0ObjectResult.proxyStatus));
            addField(QStringLiteral("R0代理NTSTATUS"), ntStatusHex(r0ObjectResult.proxyNtStatus));
            addField(QStringLiteral("R0代理策略位"), formatHex(r0ObjectResult.proxyPolicyFlags, 8));
            addField(QStringLiteral("R0 OtName偏移"), formatHex(r0ObjectResult.otNameOffset, 0));
            addField(QStringLiteral("R0 OtIndex偏移"), formatHex(r0ObjectResult.otIndexOffset, 0));

            const QString alpcCandidateType = r0TypeName.isEmpty() ? row.typeName : r0TypeName;
            if (isAlpcPortTypeName(alpcCandidateType))
            {
                const auto alpcResult = ksword::ark::DriverClient().queryAlpcPort(
                    row.processId,
                    row.handleValue,
                    KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_ALL);
                addField(QStringLiteral("R0 ALPC查询IO"), QString::fromStdString(alpcResult.io.message));
                if (alpcResult.io.ok)
                {
                    const auto appendAlpcPort = [&addField](const ksword::ark::AlpcPortInfo& portInfo, const std::uint32_t responseFieldFlags)
                        {
                            // 作用：把一个 ALPC 关系端口展开为详情字段。
                            // 处理：字段缺失时明确显示 Unavailable，而不是留下空白。
                            // 返回：无，直接写入详情字段列表。
                            const QString prefix = alpcRelationTitle(portInfo.relation);
                            if (!alpcPortPresent(responseFieldFlags, portInfo.relation))
                            {
                                addField(prefix, QStringLiteral("Not present"));
                                return;
                            }

                            const QString portName = wideToQString(portInfo.portName);
                            addField(prefix + QStringLiteral(".Object"), HandleDock::formatHex(portInfo.objectAddress, 0));
                            addField(prefix + QStringLiteral(".OwnerPid"),
                                (portInfo.fieldFlags & KSWORD_ARK_ALPC_PORT_FIELD_OWNER_PID_PRESENT) != 0U ?
                                QString::number(portInfo.ownerProcessId) :
                                QStringLiteral("Unavailable"));
                            addField(prefix + QStringLiteral(".Flags"),
                                (portInfo.fieldFlags & KSWORD_ARK_ALPC_PORT_FIELD_FLAGS_PRESENT) != 0U ?
                                HandleDock::formatHex(portInfo.flags, 8) :
                                QStringLiteral("Unavailable"));
                            addField(prefix + QStringLiteral(".State"),
                                (portInfo.fieldFlags & KSWORD_ARK_ALPC_PORT_FIELD_STATE_PRESENT) != 0U ?
                                QString::number(portInfo.state) :
                                QStringLiteral("Unavailable"));
                            addField(prefix + QStringLiteral(".SequenceNo"),
                                (portInfo.fieldFlags & KSWORD_ARK_ALPC_PORT_FIELD_SEQUENCE_PRESENT) != 0U ?
                                QString::number(portInfo.sequenceNo) :
                                QStringLiteral("Unavailable"));
                            addField(prefix + QStringLiteral(".Context"),
                                (portInfo.fieldFlags & KSWORD_ARK_ALPC_PORT_FIELD_CONTEXT_PRESENT) != 0U ?
                                HandleDock::formatHex(portInfo.portContext, 0) :
                                QStringLiteral("Unavailable"));
                            addField(prefix + QStringLiteral(".Name"),
                                (portInfo.fieldFlags & KSWORD_ARK_ALPC_PORT_FIELD_NAME_PRESENT) != 0U ?
                                (portName.isEmpty() ? QStringLiteral("(unnamed)") : portName) :
                                QStringLiteral("Unavailable"));
                            addField(prefix + QStringLiteral(".BasicStatus"), ntStatusHex(portInfo.basicStatus));
                            addField(prefix + QStringLiteral(".NameStatus"), ntStatusHex(portInfo.nameStatus));
                        };

                    addField(QStringLiteral("R0 ALPC查询状态"), alpcQueryStatusText(alpcResult.queryStatus));
                    addField(QStringLiteral("R0 ALPC对象引用状态"), ntStatusHex(alpcResult.objectReferenceStatus));
                    addField(QStringLiteral("R0 ALPC类型状态"), ntStatusHex(alpcResult.typeStatus));
                    addField(QStringLiteral("R0 ALPC基础状态"), ntStatusHex(alpcResult.basicStatus));
                    addField(QStringLiteral("R0 ALPC通信状态"), ntStatusHex(alpcResult.communicationStatus));
                    addField(QStringLiteral("R0 ALPC名称状态"), ntStatusHex(alpcResult.nameStatus));
                    addField(QStringLiteral("R0 ALPC类型名"), wideToQString(alpcResult.typeName));
                    addField(QStringLiteral("R0 ALPC字段位"), formatHex(alpcResult.fieldFlags, 8));
                    addField(QStringLiteral("R0 ALPC DynData Capability"), formatHex(alpcResult.dynDataCapabilityMask, 0));
                    addField(QStringLiteral("R0 AlpcCommunicationInfo偏移"), formatHex(alpcResult.alpcCommunicationInfoOffset, 0));
                    addField(QStringLiteral("R0 AlpcOwnerProcess偏移"), formatHex(alpcResult.alpcOwnerProcessOffset, 0));
                    addField(QStringLiteral("R0 AlpcConnectionPort偏移"), formatHex(alpcResult.alpcConnectionPortOffset, 0));
                    addField(QStringLiteral("R0 AlpcServerCommunicationPort偏移"), formatHex(alpcResult.alpcServerCommunicationPortOffset, 0));
                    addField(QStringLiteral("R0 AlpcClientCommunicationPort偏移"), formatHex(alpcResult.alpcClientCommunicationPortOffset, 0));
                    addField(QStringLiteral("R0 AlpcHandleTable偏移"), formatHex(alpcResult.alpcHandleTableOffset, 0));
                    addField(QStringLiteral("R0 AlpcHandleTableLock偏移"), formatHex(alpcResult.alpcHandleTableLockOffset, 0));
                    addField(QStringLiteral("R0 AlpcAttributes偏移"), formatHex(alpcResult.alpcAttributesOffset, 0));
                    addField(QStringLiteral("R0 AlpcAttributesFlags偏移"), formatHex(alpcResult.alpcAttributesFlagsOffset, 0));
                    addField(QStringLiteral("R0 AlpcPortContext偏移"), formatHex(alpcResult.alpcPortContextOffset, 0));
                    addField(QStringLiteral("R0 AlpcPortObjectLock偏移"), formatHex(alpcResult.alpcPortObjectLockOffset, 0));
                    addField(QStringLiteral("R0 AlpcSequenceNo偏移"), formatHex(alpcResult.alpcSequenceNoOffset, 0));
                    addField(QStringLiteral("R0 AlpcState偏移"), formatHex(alpcResult.alpcStateOffset, 0));
                    appendAlpcPort(alpcResult.queryPort, alpcResult.fieldFlags);
                    appendAlpcPort(alpcResult.connectionPort, alpcResult.fieldFlags);
                    appendAlpcPort(alpcResult.serverPort, alpcResult.fieldFlags);
                    appendAlpcPort(alpcResult.clientPort, alpcResult.fieldFlags);
                }
            }
        }
    }

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
        addField(QStringLiteral("注册表路径"), formatObjectNameDisplayText(row));
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
