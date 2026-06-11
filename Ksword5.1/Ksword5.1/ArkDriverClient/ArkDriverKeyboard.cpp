#include "ArkDriverClient.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace ksword::ark
{
    namespace
    {
        constexpr std::size_t kKeyboardHotkeyHeaderSize =
            sizeof(KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE) - sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY);

        constexpr std::size_t kKeyboardHookHeaderSize =
            sizeof(KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE) - sizeof(KSWORD_ARK_KEYBOARD_HOOK_ENTRY);

        std::wstring fixedKeyboardWideToString(const wchar_t* const textBuffer, const std::size_t maxChars)
        {
            if (textBuffer == nullptr || maxChars == 0U)
            {
                return {};
            }

            std::size_t length = 0U;
            while (length < maxChars && textBuffer[length] != L'\0')
            {
                ++length;
            }
            return std::wstring(textBuffer, textBuffer + length);
        }
    }

    KeyboardHotkeyEnumResult DriverClient::enumerateKeyboardHotkeys(
        const std::uint32_t processId,
        const unsigned long flags,
        const unsigned long maxEntries) const
    {
        KeyboardHotkeyEnumResult enumResult{};
        KSWORD_ARK_ENUM_KEYBOARD_REQUEST request{};
        request.version = KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION;
        request.flags = flags;
        request.processId = processId;
        request.maxEntries = maxEntries;

        std::vector<std::uint8_t> responseBuffer(2U * 1024U * 1024U, 0U);
        enumResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!enumResult.io.ok)
        {
            enumResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS) failed, error=" +
                std::to_string(enumResult.io.win32Error);
            return enumResult;
        }
        if (enumResult.io.bytesReturned < kKeyboardHotkeyHeaderSize)
        {
            enumResult.io.ok = false;
            enumResult.io.win32Error = ERROR_INSUFFICIENT_BUFFER;
            enumResult.io.message =
                "keyboard-hotkey response too small, bytesReturned=" +
                std::to_string(enumResult.io.bytesReturned);
            return enumResult;
        }

        const auto* responseHeader =
            reinterpret_cast<const KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE*>(responseBuffer.data());
        if (responseHeader->entrySize < sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY))
        {
            enumResult.io.ok = false;
            enumResult.io.win32Error = ERROR_INVALID_DATA;
            enumResult.io.message =
                "keyboard-hotkey entrySize invalid, entrySize=" +
                std::to_string(responseHeader->entrySize);
            return enumResult;
        }

        enumResult.version = static_cast<std::uint32_t>(responseHeader->version);
        enumResult.status = static_cast<std::uint32_t>(responseHeader->status);
        enumResult.totalCount = static_cast<std::uint32_t>(responseHeader->totalCount);
        enumResult.returnedCount = static_cast<std::uint32_t>(responseHeader->returnedCount);
        enumResult.flags = static_cast<std::uint32_t>(responseHeader->flags);
        enumResult.lastStatus = static_cast<long>(responseHeader->lastStatus);
        enumResult.win32kBase = static_cast<std::uint64_t>(responseHeader->win32kBase);
        enumResult.sessionGlobals = static_cast<std::uint64_t>(responseHeader->sessionGlobals);
        enumResult.tableOffset = static_cast<std::uint32_t>(responseHeader->tableOffset);
        enumResult.hotkeyNextOffset = static_cast<std::uint32_t>(responseHeader->hotkeyNextOffset);
        enumResult.hotkeyModifiersOffset = static_cast<std::uint32_t>(responseHeader->hotkeyModifiersOffset);
        enumResult.hotkeyVkOffset = static_cast<std::uint32_t>(responseHeader->hotkeyVkOffset);
        enumResult.hotkeyIdOffset = static_cast<std::uint32_t>(responseHeader->hotkeyIdOffset);
        enumResult.io.ntStatus = enumResult.lastStatus;

        const std::size_t availableCount =
            (enumResult.io.bytesReturned - kKeyboardHotkeyHeaderSize) /
            static_cast<std::size_t>(responseHeader->entrySize);
        const std::size_t parsedCount = std::min<std::size_t>(
            static_cast<std::size_t>(responseHeader->returnedCount),
            availableCount);
        enumResult.entries.reserve(parsedCount);
        for (std::size_t index = 0U; index < parsedCount; ++index)
        {
            const std::size_t entryOffset =
                kKeyboardHotkeyHeaderSize +
                (index * static_cast<std::size_t>(responseHeader->entrySize));
            if (entryOffset + sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY) > responseBuffer.size())
            {
                break;
            }

            const auto* sourceEntry =
                reinterpret_cast<const KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY*>(responseBuffer.data() + entryOffset);
            KeyboardHotkeyEntry row{};
            row.source = static_cast<std::uint32_t>(sourceEntry->source);
            row.status = static_cast<std::uint32_t>(sourceEntry->status);
            row.flags = static_cast<std::uint32_t>(sourceEntry->flags);
            row.bucketIndex = static_cast<std::uint32_t>(sourceEntry->bucketIndex);
            row.depth = static_cast<std::uint32_t>(sourceEntry->depth);
            row.modifiers = static_cast<std::uint32_t>(sourceEntry->modifiers);
            row.modifierFlags2 = static_cast<std::uint32_t>(sourceEntry->modifierFlags2);
            row.virtualKey = static_cast<std::uint32_t>(sourceEntry->virtualKey);
            row.hotkeyId = static_cast<std::uint32_t>(sourceEntry->hotkeyId);
            row.processId = static_cast<std::uint32_t>(sourceEntry->processId);
            row.threadId = static_cast<std::uint32_t>(sourceEntry->threadId);
            row.lastStatus = static_cast<long>(sourceEntry->lastStatus);
            row.hotkeyObject = static_cast<std::uint64_t>(sourceEntry->hotkeyObject);
            row.nextHotkeyObject = static_cast<std::uint64_t>(sourceEntry->nextHotkeyObject);
            row.sessionGlobals = static_cast<std::uint64_t>(sourceEntry->sessionGlobals);
            row.threadInfo = static_cast<std::uint64_t>(sourceEntry->threadInfo);
            row.threadObject = static_cast<std::uint64_t>(sourceEntry->threadObject);
            row.windowObject = static_cast<std::uint64_t>(sourceEntry->windowObject);
            row.detail = fixedKeyboardWideToString(sourceEntry->detail, KSWORD_ARK_KEYBOARD_DETAIL_CHARS);
            enumResult.entries.push_back(std::move(row));
        }

        std::ostringstream stream;
        stream << "version=" << enumResult.version
            << ", status=" << enumResult.status
            << ", total=" << enumResult.totalCount
            << ", returned=" << enumResult.returnedCount
            << ", parsed=" << enumResult.entries.size()
            << ", bytesReturned=" << enumResult.io.bytesReturned;
        enumResult.io.message = stream.str();
        return enumResult;
    }

    KeyboardHookEnumResult DriverClient::enumerateKeyboardHooks(
        const std::uint32_t processId,
        const unsigned long flags,
        const unsigned long maxEntries) const
    {
        KeyboardHookEnumResult enumResult{};
        KSWORD_ARK_ENUM_KEYBOARD_REQUEST request{};
        request.version = KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION;
        request.flags = flags;
        request.processId = processId;
        request.maxEntries = maxEntries;

        std::vector<std::uint8_t> responseBuffer(2U * 1024U * 1024U, 0U);
        enumResult.io = deviceIoControl(
            IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOOKS,
            &request,
            static_cast<unsigned long>(sizeof(request)),
            responseBuffer.data(),
            static_cast<unsigned long>(responseBuffer.size()));
        if (!enumResult.io.ok)
        {
            enumResult.io.message =
                "DeviceIoControl(IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOOKS) failed, error=" +
                std::to_string(enumResult.io.win32Error);
            return enumResult;
        }
        if (enumResult.io.bytesReturned < kKeyboardHookHeaderSize)
        {
            enumResult.io.ok = false;
            enumResult.io.win32Error = ERROR_INSUFFICIENT_BUFFER;
            enumResult.io.message =
                "keyboard-hook response too small, bytesReturned=" +
                std::to_string(enumResult.io.bytesReturned);
            return enumResult;
        }

        const auto* responseHeader =
            reinterpret_cast<const KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE*>(responseBuffer.data());
        if (responseHeader->entrySize < sizeof(KSWORD_ARK_KEYBOARD_HOOK_ENTRY))
        {
            enumResult.io.ok = false;
            enumResult.io.win32Error = ERROR_INVALID_DATA;
            enumResult.io.message =
                "keyboard-hook entrySize invalid, entrySize=" +
                std::to_string(responseHeader->entrySize);
            return enumResult;
        }

        enumResult.version = static_cast<std::uint32_t>(responseHeader->version);
        enumResult.status = static_cast<std::uint32_t>(responseHeader->status);
        enumResult.totalCount = static_cast<std::uint32_t>(responseHeader->totalCount);
        enumResult.returnedCount = static_cast<std::uint32_t>(responseHeader->returnedCount);
        enumResult.flags = static_cast<std::uint32_t>(responseHeader->flags);
        enumResult.lastStatus = static_cast<long>(responseHeader->lastStatus);
        enumResult.win32kBase = static_cast<std::uint64_t>(responseHeader->win32kBase);
        enumResult.threadHookArrayOffset = static_cast<std::uint32_t>(responseHeader->threadHookArrayOffset);
        enumResult.desktopInfoOffset = static_cast<std::uint32_t>(responseHeader->desktopInfoOffset);
        enumResult.desktopHookArrayOffset = static_cast<std::uint32_t>(responseHeader->desktopHookArrayOffset);
        enumResult.hookNextOffset = static_cast<std::uint32_t>(responseHeader->hookNextOffset);
        enumResult.hookTypeOffset = static_cast<std::uint32_t>(responseHeader->hookTypeOffset);
        enumResult.hookProcedureOffset = static_cast<std::uint32_t>(responseHeader->hookProcedureOffset);
        enumResult.hookFlagsOffset = static_cast<std::uint32_t>(responseHeader->hookFlagsOffset);
        enumResult.hookModuleIdOffset = static_cast<std::uint32_t>(responseHeader->hookModuleIdOffset);
        enumResult.hookTargetThreadInfoOffset = static_cast<std::uint32_t>(responseHeader->hookTargetThreadInfoOffset);
        enumResult.io.ntStatus = enumResult.lastStatus;

        const std::size_t availableCount =
            (enumResult.io.bytesReturned - kKeyboardHookHeaderSize) /
            static_cast<std::size_t>(responseHeader->entrySize);
        const std::size_t parsedCount = std::min<std::size_t>(
            static_cast<std::size_t>(responseHeader->returnedCount),
            availableCount);
        enumResult.entries.reserve(parsedCount);
        for (std::size_t index = 0U; index < parsedCount; ++index)
        {
            const std::size_t entryOffset =
                kKeyboardHookHeaderSize +
                (index * static_cast<std::size_t>(responseHeader->entrySize));
            if (entryOffset + sizeof(KSWORD_ARK_KEYBOARD_HOOK_ENTRY) > responseBuffer.size())
            {
                break;
            }

            const auto* sourceEntry =
                reinterpret_cast<const KSWORD_ARK_KEYBOARD_HOOK_ENTRY*>(responseBuffer.data() + entryOffset);
            KeyboardHookEntry row{};
            row.source = static_cast<std::uint32_t>(sourceEntry->source);
            row.status = static_cast<std::uint32_t>(sourceEntry->status);
            row.flags = static_cast<std::uint32_t>(sourceEntry->flags);
            row.hookType = static_cast<std::uint32_t>(sourceEntry->hookType);
            row.hookScope = static_cast<std::uint32_t>(sourceEntry->hookScope);
            row.processId = static_cast<std::uint32_t>(sourceEntry->processId);
            row.threadId = static_cast<std::uint32_t>(sourceEntry->threadId);
            row.moduleId = static_cast<std::uint32_t>(sourceEntry->moduleId);
            row.lastStatus = static_cast<long>(sourceEntry->lastStatus);
            row.hookObject = static_cast<std::uint64_t>(sourceEntry->hookObject);
            row.chainHead = static_cast<std::uint64_t>(sourceEntry->chainHead);
            row.nextHookObject = static_cast<std::uint64_t>(sourceEntry->nextHookObject);
            row.threadInfo = static_cast<std::uint64_t>(sourceEntry->threadInfo);
            row.targetThreadInfo = static_cast<std::uint64_t>(sourceEntry->targetThreadInfo);
            row.desktopInfo = static_cast<std::uint64_t>(sourceEntry->desktopInfo);
            row.procedureAddress = static_cast<std::uint64_t>(sourceEntry->procedureAddress);
            row.procedureOffset = static_cast<std::uint64_t>(sourceEntry->procedureOffset);
            row.moduleBase = static_cast<std::uint64_t>(sourceEntry->moduleBase);
            row.detail = fixedKeyboardWideToString(sourceEntry->detail, KSWORD_ARK_KEYBOARD_DETAIL_CHARS);
            enumResult.entries.push_back(std::move(row));
        }

        std::ostringstream stream;
        stream << "version=" << enumResult.version
            << ", status=" << enumResult.status
            << ", total=" << enumResult.totalCount
            << ", returned=" << enumResult.returnedCount
            << ", parsed=" << enumResult.entries.size()
            << ", bytesReturned=" << enumResult.io.bytesReturned;
        enumResult.io.message = stream.str();
        return enumResult;
    }
}
