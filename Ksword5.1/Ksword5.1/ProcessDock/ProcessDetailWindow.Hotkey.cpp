#include "ProcessDetailWindow.InternalCommon.h"

using namespace process_detail_window_internal;

// ============================================================
// ProcessDetailWindow.Hotkey.cpp
// 作用：
// - 负责“进程热键”页 UI、异步扫描和结果回填；
// - 当前实现只使用稳定 R3 API/资源解析，不在 Dock 层直接访问 KswordARK 设备。
// ============================================================

namespace
{
    struct HotkeyCandidate
    {
        QString objectText;
        std::uint32_t hotkeyId = 0;
        std::uint32_t modifiers = 0;
        std::uint32_t virtualKey = 0;
        QString hotkeyText;
        std::uint32_t processId = 0;
        std::uint32_t threadId = 0;
        QString processName;
        QString sourceText;
        QString detailText;
    };

    struct KeyboardHookCandidate
    {
        QString objectText;
        QString typeText;
        QString scopeText;
        std::uint32_t processId = 0;
        std::uint32_t threadId = 0;
        QString procedureText;
        QString moduleText;
        QString sourceText;
        QString flagsText;
        QString detailText;
    };

    void appendDiagnostic(QString& diagnosticText, const QString& message)
    {
        const QString trimmedMessage = message.trimmed();
        if (trimmedMessage.isEmpty())
        {
            return;
        }

        if (!diagnosticText.trimmed().isEmpty())
        {
            diagnosticText += QStringLiteral(" | ");
        }
        diagnosticText += trimmedMessage;
    }

    QString hex64Text(const std::uint64_t value)
    {
        if (value == 0U)
        {
            return QStringLiteral("0x0");
        }
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(value), 0, 16)
            .toUpper();
    }

    QString pointerText(const void* value)
    {
        return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(reinterpret_cast<std::uintptr_t>(value)), 0, 16)
            .toUpper();
    }

    QString normalizedFilePath(const QString& pathText)
    {
        const QString trimmedPath = pathText.trimmed();
        if (trimmedPath.isEmpty())
        {
            return QString();
        }

        const QFileInfo fileInfo(trimmedPath);
        const QString normalizedPath = fileInfo.exists()
            ? fileInfo.canonicalFilePath()
            : fileInfo.absoluteFilePath();
        return QDir::fromNativeSeparators(normalizedPath).toLower();
    }

    QString windowTitleText(HWND windowHandle)
    {
        const int titleLength = ::GetWindowTextLengthW(windowHandle);
        if (titleLength <= 0)
        {
            return QString();
        }

        std::vector<wchar_t> titleBuffer(static_cast<std::size_t>(titleLength) + 1U, L'\0');
        const int copiedLength = ::GetWindowTextW(
            windowHandle,
            titleBuffer.data(),
            static_cast<int>(titleBuffer.size()));
        if (copiedLength <= 0)
        {
            return QString();
        }
        return QString::fromWCharArray(titleBuffer.data(), copiedLength);
    }

    std::uint32_t hotkeyModifiersFromHotkeyf(const std::uint32_t hotkeyf)
    {
        std::uint32_t modifiers = 0;
        if ((hotkeyf & HOTKEYF_ALT) != 0U) modifiers |= MOD_ALT;
        if ((hotkeyf & HOTKEYF_CONTROL) != 0U) modifiers |= MOD_CONTROL;
        if ((hotkeyf & HOTKEYF_SHIFT) != 0U) modifiers |= MOD_SHIFT;
        return modifiers;
    }

    QString virtualKeyName(const std::uint32_t virtualKey)
    {
        if (virtualKey >= 'A' && virtualKey <= 'Z')
        {
            return QString(QChar(static_cast<ushort>(virtualKey)));
        }
        if (virtualKey >= '0' && virtualKey <= '9')
        {
            return QString(QChar(static_cast<ushort>(virtualKey)));
        }
        if (virtualKey >= VK_F1 && virtualKey <= VK_F24)
        {
            return QStringLiteral("F%1").arg(virtualKey - VK_F1 + 1U);
        }

        switch (virtualKey)
        {
        case VK_BACK: return QStringLiteral("Backspace");
        case VK_TAB: return QStringLiteral("Tab");
        case VK_RETURN: return QStringLiteral("Enter");
        case VK_ESCAPE: return QStringLiteral("Esc");
        case VK_SPACE: return QStringLiteral("Space");
        case VK_PRIOR: return QStringLiteral("PageUp");
        case VK_NEXT: return QStringLiteral("PageDown");
        case VK_END: return QStringLiteral("End");
        case VK_HOME: return QStringLiteral("Home");
        case VK_LEFT: return QStringLiteral("Left");
        case VK_UP: return QStringLiteral("Up");
        case VK_RIGHT: return QStringLiteral("Right");
        case VK_DOWN: return QStringLiteral("Down");
        case VK_INSERT: return QStringLiteral("Insert");
        case VK_DELETE: return QStringLiteral("Delete");
        case VK_SNAPSHOT: return QStringLiteral("PrintScreen");
        case VK_PAUSE: return QStringLiteral("Pause");
        case VK_APPS: return QStringLiteral("Apps");
        case VK_OEM_PLUS: return QStringLiteral("+");
        case VK_OEM_MINUS: return QStringLiteral("-");
        case VK_OEM_COMMA: return QStringLiteral(",");
        case VK_OEM_PERIOD: return QStringLiteral(".");
        default:
            break;
        }

        const UINT scanCode = ::MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
        if (scanCode != 0U)
        {
            LONG keyNameParam = static_cast<LONG>(scanCode << 16);
            switch (virtualKey)
            {
            case VK_INSERT:
            case VK_DELETE:
            case VK_HOME:
            case VK_END:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_LEFT:
            case VK_RIGHT:
            case VK_UP:
            case VK_DOWN:
                keyNameParam |= (1L << 24);
                break;
            default:
                break;
            }

            wchar_t keyNameBuffer[64] = {};
            if (::GetKeyNameTextW(keyNameParam, keyNameBuffer, static_cast<int>(std::size(keyNameBuffer))) > 0)
            {
                return QString::fromWCharArray(keyNameBuffer);
            }
        }

        return QStringLiteral("VK_0x%1")
            .arg(static_cast<unsigned int>(virtualKey), 2, 16, QChar('0'))
            .toUpper();
    }

    QString hotkeyTextFromParts(
        const std::uint32_t modifiers,
        const std::uint32_t virtualKey,
        const QString& keyOverride = QString())
    {
        QStringList parts;
        if ((modifiers & MOD_CONTROL) != 0U) parts << QStringLiteral("Ctrl");
        if ((modifiers & MOD_SHIFT) != 0U) parts << QStringLiteral("Shift");
        if ((modifiers & MOD_ALT) != 0U) parts << QStringLiteral("Alt");
        if ((modifiers & MOD_WIN) != 0U) parts << QStringLiteral("Win");
        parts << (keyOverride.trimmed().isEmpty() ? virtualKeyName(virtualKey) : keyOverride.trimmed());
        return parts.join(QStringLiteral("+"));
    }

    QString keyboardEnumStatusText(const std::uint32_t statusValue)
    {
        switch (statusValue)
        {
        case KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK:
            return QStringLiteral("OK");
        case KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL:
            return QStringLiteral("Partial");
        case KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNSUPPORTED:
            return QStringLiteral("Unsupported");
        case KSWORD_ARK_KEYBOARD_ENUM_STATUS_WIN32K_NOT_FOUND:
            return QStringLiteral("win32k not found");
        case KSWORD_ARK_KEYBOARD_ENUM_STATUS_PATTERN_NOT_FOUND:
            return QStringLiteral("pattern not found");
        case KSWORD_ARK_KEYBOARD_ENUM_STATUS_SESSION_UNAVAILABLE:
            return QStringLiteral("session unavailable");
        case KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED:
            return QStringLiteral("buffer truncated");
        case KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED:
            return QStringLiteral("read failed");
        default:
            return QStringLiteral("Unknown");
        }
    }

    QString keyboardHotkeySourceText(const std::uint32_t sourceValue)
    {
        switch (sourceValue)
        {
        case KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_HOTKEY_TABLE:
            return QStringLiteral("R0 RegisterHotKey");
        default:
            return QStringLiteral("R0 Unknown");
        }
    }

    QString keyboardHookSourceText(const std::uint32_t sourceValue)
    {
        switch (sourceValue)
        {
        case KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_THREAD_HOOK_CHAIN:
            return QStringLiteral("R0 Thread Hook Chain");
        case KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_GLOBAL_HOOK_CHAIN:
            return QStringLiteral("R0 Global Hook Chain");
        default:
            return QStringLiteral("R0 Unknown Hook Chain");
        }
    }

    QString keyboardHookScopeText(const std::uint32_t scopeValue)
    {
        switch (scopeValue)
        {
        case KSWORD_ARK_KEYBOARD_HOOK_SCOPE_THREAD:
            return QStringLiteral("线程");
        case KSWORD_ARK_KEYBOARD_HOOK_SCOPE_GLOBAL:
            return QStringLiteral("全局/桌面");
        default:
            return QStringLiteral("未知");
        }
    }

    QString keyboardHookTypeText(const std::uint32_t typeValue)
    {
        switch (typeValue)
        {
        case KSWORD_ARK_KEYBOARD_HOOK_TYPE_KEYBOARD:
            return QStringLiteral("WH_KEYBOARD");
        case KSWORD_ARK_KEYBOARD_HOOK_TYPE_KEYBOARD_LL:
            return QStringLiteral("WH_KEYBOARD_LL");
        default:
            return QStringLiteral("WH_%1").arg(typeValue);
        }
    }

    bool parseHotkeyToken(const QString& tokenText, std::uint32_t& virtualKeyOut, QString& keyTextOut)
    {
        QString token = tokenText.trimmed();
        token.remove(QLatin1Char('&'));
        token = token.trimmed();
        if (token.isEmpty())
        {
            return false;
        }

        const QString upperToken = token.toUpper();
        if (upperToken.size() == 1)
        {
            const QChar ch = upperToken.at(0);
            if ((ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) ||
                (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')))
            {
                virtualKeyOut = static_cast<std::uint32_t>(ch.unicode());
                keyTextOut = QString(ch);
                return true;
            }
        }

        if (upperToken.size() >= 2 && upperToken.at(0) == QLatin1Char('F'))
        {
            bool ok = false;
            const int functionIndex = upperToken.mid(1).toInt(&ok);
            if (ok && functionIndex >= 1 && functionIndex <= 24)
            {
                virtualKeyOut = static_cast<std::uint32_t>(VK_F1 + functionIndex - 1);
                keyTextOut = QStringLiteral("F%1").arg(functionIndex);
                return true;
            }
        }

        static const std::unordered_map<std::string, std::uint32_t> keyNameMap{
            {"ESC", VK_ESCAPE},
            {"ESCAPE", VK_ESCAPE},
            {"TAB", VK_TAB},
            {"ENTER", VK_RETURN},
            {"RETURN", VK_RETURN},
            {"SPACE", VK_SPACE},
            {"BACKSPACE", VK_BACK},
            {"BKSP", VK_BACK},
            {"DEL", VK_DELETE},
            {"DELETE", VK_DELETE},
            {"INS", VK_INSERT},
            {"INSERT", VK_INSERT},
            {"HOME", VK_HOME},
            {"END", VK_END},
            {"PGUP", VK_PRIOR},
            {"PAGEUP", VK_PRIOR},
            {"PGDN", VK_NEXT},
            {"PAGEDOWN", VK_NEXT},
            {"LEFT", VK_LEFT},
            {"RIGHT", VK_RIGHT},
            {"UP", VK_UP},
            {"DOWN", VK_DOWN},
            {"PRINTSCREEN", VK_SNAPSHOT},
            {"PRTSC", VK_SNAPSHOT},
            {"PAUSE", VK_PAUSE},
            {"APPS", VK_APPS}
        };

        const auto it = keyNameMap.find(upperToken.toStdString());
        if (it == keyNameMap.end())
        {
            return false;
        }

        virtualKeyOut = it->second;
        keyTextOut = virtualKeyName(virtualKeyOut);
        return true;
    }

    bool parseHotkeyText(const QString& sourceText, std::uint32_t& modifiersOut, std::uint32_t& virtualKeyOut)
    {
        QString text = sourceText.trimmed();
        if (text.isEmpty() || !text.contains(QLatin1Char('+')))
        {
            return false;
        }

        text.replace(QStringLiteral("Control"), QStringLiteral("Ctrl"), Qt::CaseInsensitive);
        text.replace(QStringLiteral("Windows"), QStringLiteral("Win"), Qt::CaseInsensitive);
        const QStringList parts = text.split(QLatin1Char('+'), Qt::SkipEmptyParts);
        if (parts.isEmpty())
        {
            return false;
        }

        std::uint32_t modifiers = 0;
        for (int index = 0; index + 1 < parts.size(); ++index)
        {
            const QString modifierText = parts.at(index).trimmed().toUpper();
            if (modifierText == QStringLiteral("CTRL"))
            {
                modifiers |= MOD_CONTROL;
            }
            else if (modifierText == QStringLiteral("SHIFT"))
            {
                modifiers |= MOD_SHIFT;
            }
            else if (modifierText == QStringLiteral("ALT"))
            {
                modifiers |= MOD_ALT;
            }
            else if (modifierText == QStringLiteral("WIN"))
            {
                modifiers |= MOD_WIN;
            }
        }

        QString keyText;
        std::uint32_t virtualKey = 0;
        if (!parseHotkeyToken(parts.last(), virtualKey, keyText))
        {
            return false;
        }

        modifiersOut = modifiers;
        virtualKeyOut = virtualKey;
        return true;
    }

    void appendCandidate(std::vector<HotkeyCandidate>& rows, QSet<QString>& dedupeSet, HotkeyCandidate row)
    {
        if (row.hotkeyText.trimmed().isEmpty() && row.virtualKey != 0U)
        {
            row.hotkeyText = hotkeyTextFromParts(row.modifiers, row.virtualKey);
        }
        if (row.hotkeyText.trimmed().isEmpty())
        {
            return;
        }

        const QString dedupeKey = QStringLiteral("%1|%2|%3|%4|%5|%6|%7")
            .arg(row.sourceText)
            .arg(row.objectText)
            .arg(row.hotkeyId)
            .arg(row.modifiers)
            .arg(row.virtualKey)
            .arg(row.threadId)
            .arg(row.detailText);
        if (dedupeSet.contains(dedupeKey))
        {
            return;
        }
        dedupeSet.insert(dedupeKey);
        rows.push_back(std::move(row));
    }

    struct WindowCollectorContext
    {
        std::uint32_t targetPid = 0;
        QSet<qulonglong>* seenWindows = nullptr;
        std::vector<HWND>* windows = nullptr;
    };

    BOOL CALLBACK collectTopLevelWindowProc(HWND windowHandle, LPARAM parameter)
    {
        auto* context = reinterpret_cast<WindowCollectorContext*>(parameter);
        if (context == nullptr || context->seenWindows == nullptr || context->windows == nullptr)
        {
            return TRUE;
        }

        DWORD processId = 0;
        ::GetWindowThreadProcessId(windowHandle, &processId);
        if (processId != context->targetPid)
        {
            return TRUE;
        }

        const qulonglong key = static_cast<qulonglong>(reinterpret_cast<std::uintptr_t>(windowHandle));
        if (!context->seenWindows->contains(key))
        {
            context->seenWindows->insert(key);
            context->windows->push_back(windowHandle);
        }
        return TRUE;
    }

    std::vector<DWORD> collectThreadIdsForProcess(const std::uint32_t processId)
    {
        std::vector<DWORD> threadIds;
        HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            return threadIds;
        }

        THREADENTRY32 threadEntry{};
        threadEntry.dwSize = sizeof(threadEntry);
        BOOL hasThread = ::Thread32First(snapshotHandle, &threadEntry);
        while (hasThread != FALSE)
        {
            if (threadEntry.th32OwnerProcessID == processId)
            {
                threadIds.push_back(threadEntry.th32ThreadID);
            }
            hasThread = ::Thread32Next(snapshotHandle, &threadEntry);
        }

        ::CloseHandle(snapshotHandle);
        return threadIds;
    }

    std::vector<HWND> collectWindowsForProcess(const std::uint32_t processId)
    {
        std::vector<HWND> windows;
        QSet<qulonglong> seenWindows;
        WindowCollectorContext context{ processId, &seenWindows, &windows };
        ::EnumWindows(collectTopLevelWindowProc, reinterpret_cast<LPARAM>(&context));

        const std::vector<DWORD> threadIds = collectThreadIdsForProcess(processId);
        for (const DWORD threadId : threadIds)
        {
            ::EnumThreadWindows(threadId, collectTopLevelWindowProc, reinterpret_cast<LPARAM>(&context));
        }

        return windows;
    }

    void collectWindowActivationHotkeys(
        const std::uint32_t processId,
        const QString& processName,
        std::vector<HotkeyCandidate>& rows,
        QSet<QString>& dedupeSet)
    {
        const std::vector<HWND> windows = collectWindowsForProcess(processId);
        for (HWND windowHandle : windows)
        {
            DWORD ownerProcessId = 0;
            const DWORD threadId = ::GetWindowThreadProcessId(windowHandle, &ownerProcessId);
            DWORD_PTR resultValue = 0;
            const LRESULT sendOk = ::SendMessageTimeoutW(
                windowHandle,
                WM_GETHOTKEY,
                0,
                0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                100,
                &resultValue);
            if (sendOk == 0 || resultValue == 0)
            {
                continue;
            }

            const std::uint32_t virtualKey = static_cast<std::uint32_t>(LOBYTE(LOWORD(resultValue)));
            const std::uint32_t hotkeyf = static_cast<std::uint32_t>(HIBYTE(LOWORD(resultValue)));
            if (virtualKey == 0U)
            {
                continue;
            }

            HotkeyCandidate row{};
            row.objectText = QStringLiteral("HWND=%1").arg(pointerText(windowHandle));
            row.hotkeyId = 0;
            row.modifiers = hotkeyModifiersFromHotkeyf(hotkeyf);
            row.virtualKey = virtualKey;
            row.hotkeyText = hotkeyTextFromParts(row.modifiers, row.virtualKey);
            row.processId = processId;
            row.threadId = threadId;
            row.processName = processName;
            row.sourceText = QStringLiteral("窗口热键");
            row.detailText = windowTitleText(windowHandle);
            appendCandidate(rows, dedupeSet, std::move(row));
        }
    }

    void collectMenuHotkeysRecursive(
        HMENU menuHandle,
        HWND windowHandle,
        const QString& menuPath,
        const std::uint32_t processId,
        const std::uint32_t threadId,
        const QString& processName,
        std::vector<HotkeyCandidate>& rows,
        QSet<QString>& dedupeSet,
        const int depth)
    {
        if (menuHandle == nullptr || depth > 12)
        {
            return;
        }

        const int itemCount = ::GetMenuItemCount(menuHandle);
        if (itemCount <= 0)
        {
            return;
        }

        for (int index = 0; index < itemCount; ++index)
        {
            wchar_t textBuffer[512] = {};
            MENUITEMINFOW itemInfo{};
            itemInfo.cbSize = sizeof(itemInfo);
            itemInfo.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_ID;
            itemInfo.dwTypeData = textBuffer;
            itemInfo.cch = static_cast<UINT>(std::size(textBuffer));
            if (::GetMenuItemInfoW(menuHandle, static_cast<UINT>(index), TRUE, &itemInfo) == FALSE)
            {
                continue;
            }

            const QString rawText = QString::fromWCharArray(textBuffer).trimmed();
            const QString itemPath = menuPath.isEmpty()
                ? rawText
                : QStringLiteral("%1 > %2").arg(menuPath, rawText);

            const int tabIndex = rawText.indexOf(QLatin1Char('\t'));
            if (tabIndex >= 0 && tabIndex + 1 < rawText.size())
            {
                const QString shortcutText = rawText.mid(tabIndex + 1).trimmed();
                std::uint32_t modifiers = 0;
                std::uint32_t virtualKey = 0;
                if (parseHotkeyText(shortcutText, modifiers, virtualKey))
                {
                    HotkeyCandidate row{};
                    row.objectText = QStringLiteral("HWND=%1 HMENU=%2")
                        .arg(pointerText(windowHandle), pointerText(menuHandle));
                    row.hotkeyId = itemInfo.wID;
                    row.modifiers = modifiers;
                    row.virtualKey = virtualKey;
                    row.hotkeyText = hotkeyTextFromParts(row.modifiers, row.virtualKey);
                    row.processId = processId;
                    row.threadId = threadId;
                    row.processName = processName;
                    row.sourceText = QStringLiteral("菜单快捷键");
                    row.detailText = itemPath;
                    appendCandidate(rows, dedupeSet, std::move(row));
                }
            }

            if (itemInfo.hSubMenu != nullptr)
            {
                collectMenuHotkeysRecursive(
                    itemInfo.hSubMenu,
                    windowHandle,
                    itemPath,
                    processId,
                    threadId,
                    processName,
                    rows,
                    dedupeSet,
                    depth + 1);
            }
        }
    }

    void collectMenuHotkeys(
        const std::uint32_t processId,
        const QString& processName,
        std::vector<HotkeyCandidate>& rows,
        QSet<QString>& dedupeSet)
    {
        const std::vector<HWND> windows = collectWindowsForProcess(processId);
        for (HWND windowHandle : windows)
        {
            DWORD ownerProcessId = 0;
            const DWORD threadId = ::GetWindowThreadProcessId(windowHandle, &ownerProcessId);
            HMENU menuHandle = ::GetMenu(windowHandle);
            if (menuHandle == nullptr)
            {
                continue;
            }
            collectMenuHotkeysRecursive(
                menuHandle,
                windowHandle,
                windowTitleText(windowHandle),
                processId,
                threadId,
                processName,
                rows,
                dedupeSet,
                0);
        }
    }

    QStringList shortcutSearchRoots()
    {
        QStringList roots;
        const auto appendRoot = [&roots](const QString& pathText)
        {
            const QString cleaned = QDir::fromNativeSeparators(pathText.trimmed());
            if (!cleaned.isEmpty() && QDir(cleaned).exists() && !roots.contains(cleaned, Qt::CaseInsensitive))
            {
                roots << cleaned;
            }
        };

        appendRoot(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
        appendRoot(QStringLiteral("C:/Users/Public/Desktop"));

        const QString appData = qEnvironmentVariable("APPDATA");
        if (!appData.isEmpty())
        {
            appendRoot(QDir::fromNativeSeparators(appData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs")));
        }

        const QString programData = qEnvironmentVariable("PROGRAMDATA");
        if (!programData.isEmpty())
        {
            appendRoot(QDir::fromNativeSeparators(programData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs")));
        }

        return roots;
    }

    bool shortcutTargetMatchesProcess(
        const QString& shortcutTarget,
        const QString& processImagePath,
        const QString& processName)
    {
        const QString normalizedTarget = normalizedFilePath(shortcutTarget);
        const QString normalizedProcessImage = normalizedFilePath(processImagePath);
        if (!normalizedTarget.isEmpty() &&
            !normalizedProcessImage.isEmpty() &&
            normalizedTarget == normalizedProcessImage)
        {
            return true;
        }

        if (!processName.trimmed().isEmpty())
        {
            const QString targetName = QFileInfo(shortcutTarget).fileName();
            if (!targetName.isEmpty() && targetName.compare(processName, Qt::CaseInsensitive) == 0)
            {
                return true;
            }
        }
        return false;
    }

    void collectShellShortcutHotkeys(
        const std::uint32_t processId,
        const QString& processName,
        const QString& processImagePath,
        std::vector<HotkeyCandidate>& rows,
        QSet<QString>& dedupeSet,
        QString& diagnosticText)
    {
        HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool needsUninitialize = SUCCEEDED(comResult);
        const bool comUsable = SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE;
        if (!comUsable)
        {
            diagnosticText += QStringLiteral(" | ShellLink COM初始化失败:0x%1")
                .arg(static_cast<unsigned long>(comResult), 0, 16);
            return;
        }

        const QStringList roots = shortcutSearchRoots();
        for (const QString& rootPath : roots)
        {
            QDirIterator shortcutIterator(
                rootPath,
                QStringList{ QStringLiteral("*.lnk") },
                QDir::Files,
                QDirIterator::Subdirectories);
            while (shortcutIterator.hasNext())
            {
                const QString shortcutPath = shortcutIterator.next();
                IShellLinkW* shellLink = nullptr;
                HRESULT createResult = ::CoCreateInstance(
                    CLSID_ShellLink,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&shellLink));
                if (FAILED(createResult) || shellLink == nullptr)
                {
                    continue;
                }

                IPersistFile* persistFile = nullptr;
                HRESULT queryResult = shellLink->QueryInterface(IID_PPV_ARGS(&persistFile));
                if (SUCCEEDED(queryResult) && persistFile != nullptr)
                {
                    const std::wstring shortcutPathW = shortcutPath.toStdWString();
                    HRESULT loadResult = persistFile->Load(shortcutPathW.c_str(), STGM_READ);
                    if (SUCCEEDED(loadResult))
                    {
                        WORD hotkeyValue = 0;
                        wchar_t targetPathBuffer[MAX_PATH] = {};
                        WIN32_FIND_DATAW findData{};
                        shellLink->GetHotkey(&hotkeyValue);
                        shellLink->GetPath(
                            targetPathBuffer,
                            static_cast<int>(std::size(targetPathBuffer)),
                            &findData,
                            SLGP_UNCPRIORITY);

                        const QString targetPath = QString::fromWCharArray(targetPathBuffer).trimmed();
                        if (hotkeyValue != 0 &&
                            !targetPath.isEmpty() &&
                            shortcutTargetMatchesProcess(targetPath, processImagePath, processName))
                        {
                            const std::uint32_t virtualKey = LOBYTE(hotkeyValue);
                            const std::uint32_t modifiers = hotkeyModifiersFromHotkeyf(HIBYTE(hotkeyValue));
                            HotkeyCandidate row{};
                            row.objectText = shortcutPath;
                            row.hotkeyId = 0;
                            row.modifiers = modifiers;
                            row.virtualKey = virtualKey;
                            row.hotkeyText = hotkeyTextFromParts(row.modifiers, row.virtualKey);
                            row.processId = processId;
                            row.threadId = 0;
                            row.processName = processName;
                            row.sourceText = QStringLiteral("快捷方式热键");
                            row.detailText = targetPath;
                            appendCandidate(rows, dedupeSet, std::move(row));
                        }
                    }
                    persistFile->Release();
                }
                shellLink->Release();
            }
        }

        if (needsUninitialize)
        {
            ::CoUninitialize();
        }
    }

    struct AcceleratorEnumContext
    {
        HMODULE moduleHandle = nullptr;
        QString modulePath;
        std::uint32_t processId = 0;
        QString processName;
        std::vector<HotkeyCandidate>* rows = nullptr;
        QSet<QString>* dedupeSet = nullptr;
    };

    QString acceleratorResourceNameText(LPCWSTR resourceName)
    {
        if (IS_INTRESOURCE(resourceName))
        {
            return QStringLiteral("#%1").arg(static_cast<qulonglong>(reinterpret_cast<std::uintptr_t>(resourceName)));
        }
        return QString::fromWCharArray(resourceName);
    }

    BOOL CALLBACK enumerateAcceleratorResourceProc(
        HMODULE moduleHandle,
        LPCWSTR resourceType,
        LPWSTR resourceName,
        LONG_PTR parameter)
    {
        Q_UNUSED(resourceType);

        auto* context = reinterpret_cast<AcceleratorEnumContext*>(parameter);
        if (context == nullptr || context->rows == nullptr || context->dedupeSet == nullptr)
        {
            return TRUE;
        }

        HRSRC resourceHandle = ::FindResourceW(moduleHandle, resourceName, RT_ACCELERATOR);
        if (resourceHandle == nullptr)
        {
            return TRUE;
        }

        const DWORD resourceBytes = ::SizeofResource(moduleHandle, resourceHandle);
        if (resourceBytes < sizeof(ACCEL))
        {
            return TRUE;
        }

        HGLOBAL loadedResource = ::LoadResource(moduleHandle, resourceHandle);
        const auto* accelEntries = static_cast<const ACCEL*>(::LockResource(loadedResource));
        if (accelEntries == nullptr)
        {
            return TRUE;
        }

        const std::size_t entryCount = resourceBytes / sizeof(ACCEL);
        const QString resourceNameText = acceleratorResourceNameText(resourceName);
        for (std::size_t index = 0; index < entryCount; ++index)
        {
            const ACCEL& accelEntry = accelEntries[index];
            const BYTE flags = static_cast<BYTE>(accelEntry.fVirt & 0x7F);

            std::uint32_t modifiers = 0;
            if ((flags & FCONTROL) != 0U) modifiers |= MOD_CONTROL;
            if ((flags & FSHIFT) != 0U) modifiers |= MOD_SHIFT;
            if ((flags & FALT) != 0U) modifiers |= MOD_ALT;

            std::uint32_t virtualKey = 0;
            QString keyOverride;
            if ((flags & FVIRTKEY) != 0U)
            {
                virtualKey = accelEntry.key;
            }
            else
            {
                const wchar_t keyChar = static_cast<wchar_t>(accelEntry.key);
                SHORT vkScan = ::VkKeyScanW(keyChar);
                if (vkScan != -1)
                {
                    virtualKey = LOBYTE(vkScan);
                }
                keyOverride = QString(QChar(static_cast<ushort>(keyChar))).toUpper();
            }

            if (virtualKey == 0U && keyOverride.isEmpty())
            {
                continue;
            }

            HotkeyCandidate row{};
            row.objectText = QStringLiteral("%1:%2")
                .arg(context->modulePath, resourceNameText);
            row.hotkeyId = accelEntry.cmd;
            row.modifiers = modifiers;
            row.virtualKey = virtualKey;
            row.hotkeyText = hotkeyTextFromParts(row.modifiers, row.virtualKey, keyOverride);
            row.processId = context->processId;
            row.threadId = 0;
            row.processName = context->processName;
            row.sourceText = QStringLiteral("PE Accelerator");
            row.detailText = QStringLiteral("entry=%1 flags=0x%2")
                .arg(static_cast<qulonglong>(index))
                .arg(static_cast<unsigned int>(accelEntry.fVirt), 2, 16, QChar('0'))
                .toUpper();
            appendCandidate(*context->rows, *context->dedupeSet, std::move(row));
        }

        return TRUE;
    }

    void collectAcceleratorResourceHotkeys(
        const std::uint32_t processId,
        const QString& processName,
        const QString& processImagePath,
        std::vector<HotkeyCandidate>& rows,
        QSet<QString>& dedupeSet,
        QString& diagnosticText)
    {
        QStringList modulePaths;
        const auto appendModulePath = [&modulePaths](const QString& pathText)
        {
            const QString normalizedPath = normalizedFilePath(pathText);
            if (!normalizedPath.isEmpty() && QFileInfo(pathText).exists() && !modulePaths.contains(pathText, Qt::CaseInsensitive))
            {
                modulePaths << pathText;
            }
        };

        appendModulePath(processImagePath);
        const ks::process::ProcessModuleSnapshot moduleSnapshot =
            ks::process::EnumerateProcessModulesAndThreads(processId, false);
        for (const ks::process::ProcessModuleRecord& moduleRecord : moduleSnapshot.modules)
        {
            appendModulePath(QString::fromStdString(moduleRecord.modulePath));
            if (modulePaths.size() >= 96)
            {
                diagnosticText += QStringLiteral(" | Accelerator模块扫描已限制为前96个模块");
                break;
            }
        }

        for (const QString& modulePath : modulePaths)
        {
            const std::wstring modulePathW = modulePath.toStdWString();
            HMODULE moduleHandle = ::LoadLibraryExW(
                modulePathW.c_str(),
                nullptr,
                LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            if (moduleHandle == nullptr)
            {
                continue;
            }

            AcceleratorEnumContext context{};
            context.moduleHandle = moduleHandle;
            context.modulePath = modulePath;
            context.processId = processId;
            context.processName = processName;
            context.rows = &rows;
            context.dedupeSet = &dedupeSet;
            ::EnumResourceNamesW(
                moduleHandle,
                RT_ACCELERATOR,
                enumerateAcceleratorResourceProc,
                reinterpret_cast<LONG_PTR>(&context));
            ::FreeLibrary(moduleHandle);
        }
    }

    void collectDriverHotkeyTable(
        const std::uint32_t processId,
        const QString& processName,
        std::vector<HotkeyCandidate>& rows,
        QString& diagnosticText)
    {
        const unsigned long flags =
            KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_SYSTEM |
            KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_DIAGNOSTICS |
            KSWORD_ARK_KEYBOARD_ENUM_FLAG_FILTER_PROCESS;
        const ksword::ark::KeyboardHotkeyEnumResult driverResult =
            ksword::ark::DriverClient().enumerateKeyboardHotkeys(processId, flags, 2048UL);
        if (!driverResult.io.ok)
        {
            appendDiagnostic(
                diagnosticText,
                QStringLiteral("R0热键不可用:%1").arg(QString::fromStdString(driverResult.io.message)));
            return;
        }

        appendDiagnostic(
            diagnosticText,
            QStringLiteral("R0热键:%1 total=%2 returned=%3")
                .arg(keyboardEnumStatusText(driverResult.status))
                .arg(driverResult.totalCount)
                .arg(driverResult.entries.size()));

        for (const ksword::ark::KeyboardHotkeyEntry& entry : driverResult.entries)
        {
            if (entry.processId != 0U && entry.processId != processId)
            {
                continue;
            }

            HotkeyCandidate row{};
            row.objectText = QStringLiteral("tagHOTKEY %1").arg(hex64Text(entry.hotkeyObject));
            row.hotkeyId = entry.hotkeyId;
            row.modifiers = entry.modifiers;
            row.virtualKey = entry.virtualKey;
            row.hotkeyText = hotkeyTextFromParts(entry.modifiers, entry.virtualKey);
            row.processId = entry.processId == 0U ? processId : entry.processId;
            row.threadId = entry.threadId;
            row.processName = entry.processId == 0U || entry.processId == processId
                ? processName
                : QString::fromStdString(ks::process::GetProcessNameByPID(entry.processId));
            row.sourceText = keyboardHotkeySourceText(entry.source);
            row.detailText = QStringLiteral(
                "status=%1 bucket=%2 depth=%3 next=%4 threadInfo=%5 thread=%6 hwnd=%7 flags2=0x%8 %9")
                .arg(keyboardEnumStatusText(entry.status))
                .arg(entry.bucketIndex)
                .arg(entry.depth)
                .arg(hex64Text(entry.nextHotkeyObject))
                .arg(hex64Text(entry.threadInfo))
                .arg(hex64Text(entry.threadObject))
                .arg(hex64Text(entry.windowObject))
                .arg(entry.modifierFlags2, 0, 16)
                .arg(QString::fromStdWString(entry.detail))
                .toUpper();
            rows.push_back(std::move(row));
        }
    }

    std::vector<KeyboardHookCandidate> collectDriverKeyboardHooks(
        const std::uint32_t processId,
        QString& diagnosticText)
    {
        std::vector<KeyboardHookCandidate> rows;
        const unsigned long flags =
            KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_THREAD_HOOKS |
            KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_GLOBAL_HOOKS |
            KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_DIAGNOSTICS |
            KSWORD_ARK_KEYBOARD_ENUM_FLAG_FILTER_PROCESS;
        const ksword::ark::KeyboardHookEnumResult driverResult =
            ksword::ark::DriverClient().enumerateKeyboardHooks(processId, flags, 2048UL);
        if (!driverResult.io.ok)
        {
            appendDiagnostic(
                diagnosticText,
                QStringLiteral("R0键盘钩子不可用:%1").arg(QString::fromStdString(driverResult.io.message)));
            return rows;
        }

        appendDiagnostic(
            diagnosticText,
            QStringLiteral("R0键盘钩子:%1 total=%2 returned=%3")
                .arg(keyboardEnumStatusText(driverResult.status))
                .arg(driverResult.totalCount)
                .arg(driverResult.entries.size()));

        rows.reserve(driverResult.entries.size());
        for (const ksword::ark::KeyboardHookEntry& entry : driverResult.entries)
        {
            KeyboardHookCandidate row{};
            row.objectText = QStringLiteral("tagHOOK %1").arg(hex64Text(entry.hookObject));
            row.typeText = keyboardHookTypeText(entry.hookType);
            row.scopeText = keyboardHookScopeText(entry.hookScope);
            row.processId = entry.processId;
            row.threadId = entry.threadId;
            row.procedureText = entry.procedureAddress != 0U
                ? hex64Text(entry.procedureAddress)
                : QStringLiteral("offset=%1").arg(hex64Text(entry.procedureOffset));
            row.moduleText = entry.moduleBase != 0U
                ? hex64Text(entry.moduleBase)
                : QStringLiteral("moduleId=%1").arg(entry.moduleId);
            row.sourceText = keyboardHookSourceText(entry.source);
            row.flagsText = QStringLiteral("0x%1").arg(entry.flags, 0, 16).toUpper();
            row.detailText = QStringLiteral(
                "status=%1 chain=%2 next=%3 threadInfo=%4 targetThreadInfo=%5 desktop=%6 last=0x%7 %8")
                .arg(keyboardEnumStatusText(entry.status))
                .arg(hex64Text(entry.chainHead))
                .arg(hex64Text(entry.nextHookObject))
                .arg(hex64Text(entry.threadInfo))
                .arg(hex64Text(entry.targetThreadInfo))
                .arg(hex64Text(entry.desktopInfo))
                .arg(static_cast<unsigned long>(static_cast<std::uint32_t>(entry.lastStatus)), 0, 16)
                .arg(QString::fromStdWString(entry.detail))
                .toUpper();
            rows.push_back(std::move(row));
        }

        return rows;
    }

    std::vector<HotkeyCandidate> collectHotkeysForProcess(
        const std::uint32_t processId,
        QString processName,
        QString processImagePath,
        QString& diagnosticText)
    {
        if (processName.trimmed().isEmpty())
        {
            processName = QString::fromStdString(ks::process::GetProcessNameByPID(processId));
        }
        if (processImagePath.trimmed().isEmpty())
        {
            processImagePath = QString::fromStdString(ks::process::QueryProcessPathByPid(processId));
        }

        std::vector<HotkeyCandidate> rows;
        QSet<QString> dedupeSet;

        collectWindowActivationHotkeys(processId, processName, rows, dedupeSet);
        collectMenuHotkeys(processId, processName, rows, dedupeSet);
        collectAcceleratorResourceHotkeys(processId, processName, processImagePath, rows, dedupeSet, diagnosticText);
        collectShellShortcutHotkeys(processId, processName, processImagePath, rows, dedupeSet, diagnosticText);
        collectDriverHotkeyTable(processId, processName, rows, diagnosticText);

        std::sort(
            rows.begin(),
            rows.end(),
            [](const HotkeyCandidate& left, const HotkeyCandidate& right)
            {
                if (left.hotkeyText != right.hotkeyText) return left.hotkeyText < right.hotkeyText;
                if (left.sourceText != right.sourceText) return left.sourceText < right.sourceText;
                return left.objectText < right.objectText;
            });

        return rows;
    }

    QTableWidgetItem* createHotkeyCell(const QString& textValue)
    {
        auto* item = new QTableWidgetItem(textValue);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }
}

void ProcessDetailWindow::initializeHotkeyTab()
{
    kLogEvent initHotkeyTabEvent;
    info << initHotkeyTabEvent
        << "[ProcessDetailWindow] initializeHotkeyTab: 构建进程热键页面。"
        << eol;

    m_hotkeyLayout = new QVBoxLayout(m_hotkeyTab);
    m_hotkeyLayout->setContentsMargins(6, 6, 6, 6);
    m_hotkeyLayout->setSpacing(8);

    QGroupBox* hotkeyGroup = new QGroupBox(QStringLiteral("进程热键检测"), m_hotkeyTab);
    QVBoxLayout* hotkeyGroupLayout = new QVBoxLayout(hotkeyGroup);
    hotkeyGroupLayout->setContentsMargins(8, 8, 8, 8);
    hotkeyGroupLayout->setSpacing(6);

    QHBoxLayout* topBarLayout = new QHBoxLayout();
    m_refreshHotkeyButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QStringLiteral("刷新热键"), hotkeyGroup);
    m_refreshHotkeyButton->setToolTip(QStringLiteral("扫描当前进程的窗口热键、菜单快捷键、Accelerator资源、快捷方式热键和R0热键表"));
    m_hotkeyStatusLabel = new QLabel(QStringLiteral("● 尚未刷新"), hotkeyGroup);
    m_hotkeyStatusLabel->setStyleSheet(
        QStringLiteral("color:%1; font-weight:600;")
        .arg(KswordTheme::TextSecondaryHex()));
    topBarLayout->addWidget(m_refreshHotkeyButton);
    topBarLayout->addWidget(m_hotkeyStatusLabel, 1);
    hotkeyGroupLayout->addLayout(topBarLayout);

    m_hotkeyTable = new QTableWidget(hotkeyGroup);
    m_hotkeyTable->setColumnCount(9);
    m_hotkeyTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("对象"),
        QStringLiteral("热键ID"),
        QStringLiteral("热键"),
        QStringLiteral("进程ID"),
        QStringLiteral("线程ID"),
        QStringLiteral("进程名"),
        QStringLiteral("来源"),
        QStringLiteral("VK/Mod"),
        QStringLiteral("详情")
    });
    m_hotkeyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_hotkeyTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_hotkeyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_hotkeyTable->setAlternatingRowColors(true);
    m_hotkeyTable->setSortingEnabled(true);
    m_hotkeyTable->verticalHeader()->setVisible(false);
    m_hotkeyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_hotkeyTable->horizontalHeader()->setStretchLastSection(true);
    m_hotkeyTable->setColumnWidth(0, 260);
    m_hotkeyTable->setColumnWidth(1, 80);
    m_hotkeyTable->setColumnWidth(2, 130);
    m_hotkeyTable->setColumnWidth(3, 80);
    m_hotkeyTable->setColumnWidth(4, 80);
    m_hotkeyTable->setColumnWidth(5, 120);
    m_hotkeyTable->setColumnWidth(6, 130);
    m_hotkeyTable->setColumnWidth(7, 120);
    hotkeyGroupLayout->addWidget(m_hotkeyTable, 1);

    m_hotkeyLayout->addWidget(hotkeyGroup, 1);

    const QString buttonStyle = buildBlueButtonStyle();
    m_refreshHotkeyButton->setStyleSheet(buttonStyle);
}

void ProcessDetailWindow::updateHotkeyStatusLabel(const QString& statusText, const bool refreshing)
{
    if (m_hotkeyStatusLabel == nullptr)
    {
        return;
    }

    m_hotkeyStatusLabel->setText(statusText);
    if (refreshing)
    {
        m_hotkeyStatusLabel->setStyleSheet(buildStateLabelStyle(KswordTheme::PrimaryBlueColor, 700));
    }
    else if (statusText.contains(QStringLiteral("无公开API")) || statusText.contains(QStringLiteral("失败")))
    {
        m_hotkeyStatusLabel->setStyleSheet(buildStateLabelStyle(statusWarningColor(), 700));
    }
    else
    {
        m_hotkeyStatusLabel->setStyleSheet(buildStateLabelStyle(statusIdleColor(), 600));
    }
}

void ProcessDetailWindow::requestAsyncHotkeyRefresh()
{
    if (m_hotkeyRefreshing || m_baseRecord.pid == 0)
    {
        return;
    }

    m_hotkeyInitialRefreshStarted = true;
    m_hotkeyRefreshing = true;
    const std::uint64_t ticketValue = ++m_hotkeyRefreshTicket;
    const std::uint32_t pidValue = m_baseRecord.pid;
    const QString processNameText = QString::fromStdString(m_baseRecord.processName);
    const QString processImagePathText = QString::fromStdString(m_baseRecord.imagePath);

    if (m_refreshHotkeyButton != nullptr)
    {
        m_refreshHotkeyButton->setEnabled(false);
    }
    updateHotkeyStatusLabel(QStringLiteral("● 正在扫描进程热键..."), true);

    if (m_hotkeyRefreshProgressPid == 0)
    {
        m_hotkeyRefreshProgressPid = kPro.add("进程详情", "扫描进程热键");
    }
    kPro.set(m_hotkeyRefreshProgressPid, "枚举窗口/菜单/资源/快捷方式", 0, 20.0f);

    QPointer<ProcessDetailWindow> guardThis(this);
    auto* refreshTask = QRunnable::create(
        [guardThis, ticketValue, pidValue, processNameText, processImagePathText]()
        {
            HotkeyInspectRefreshResult refreshResult{};
            const auto beginTime = std::chrono::steady_clock::now();

            QString diagnosticText = QStringLiteral("R3窗口/菜单/资源/.lnk + R0 win32k热键表");
            const std::vector<HotkeyCandidate> candidates = collectHotkeysForProcess(
                pidValue,
                processNameText,
                processImagePathText,
                diagnosticText);

            refreshResult.rows.reserve(candidates.size());
            for (const HotkeyCandidate& candidate : candidates)
            {
                HotkeyInspectItem row{};
                row.objectText = candidate.objectText;
                row.hotkeyId = candidate.hotkeyId;
                row.modifiers = candidate.modifiers;
                row.virtualKey = candidate.virtualKey;
                row.hotkeyText = candidate.hotkeyText;
                row.processId = candidate.processId;
                row.threadId = candidate.threadId;
                row.processName = candidate.processName;
                row.sourceText = candidate.sourceText;
                row.detailText = candidate.detailText;
                refreshResult.rows.push_back(std::move(row));
            }

            refreshResult.diagnosticText = diagnosticText;
            refreshResult.elapsedMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - beginTime).count());

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, ticketValue, refreshResult]()
                {
                    if (guardThis == nullptr || guardThis->m_hotkeyRefreshTicket != ticketValue)
                    {
                        return;
                    }
                    guardThis->applyHotkeyRefreshResult(refreshResult);
                },
                Qt::QueuedConnection);
        });

    refreshTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(refreshTask);
}

void ProcessDetailWindow::applyHotkeyRefreshResult(const HotkeyInspectRefreshResult& refreshResult)
{
    m_hotkeyRefreshing = false;
    if (m_refreshHotkeyButton != nullptr)
    {
        m_refreshHotkeyButton->setEnabled(true);
    }

    m_hotkeyRows = refreshResult.rows;
    m_keyboardHotkeyRows = refreshResult.rows;
    rebuildHotkeyTable();
    rebuildKeyboardHotkeyTable();

    QString statusText = QStringLiteral("● 刷新完成 %1 ms | 热键:%2")
        .arg(refreshResult.elapsedMs)
        .arg(static_cast<qulonglong>(refreshResult.rows.size()));
    if (!refreshResult.diagnosticText.trimmed().isEmpty())
    {
        statusText += QStringLiteral(" | %1").arg(refreshResult.diagnosticText.trimmed());
    }
    updateHotkeyStatusLabel(statusText, false);

    if (m_hotkeyRefreshProgressPid > 0)
    {
        kPro.set(m_hotkeyRefreshProgressPid, "进程热键扫描完成", 100, 1.0f);
    }

    kLogEvent hotkeyRefreshDoneEvent;
    info << hotkeyRefreshDoneEvent
        << "[ProcessDetailWindow] 进程热键扫描完成, pid="
        << m_baseRecord.pid
        << ", rows="
        << refreshResult.rows.size()
        << ", elapsedMs="
        << refreshResult.elapsedMs
        << ", diagnostic="
        << refreshResult.diagnosticText.toStdString()
        << eol;
}

void ProcessDetailWindow::rebuildHotkeyTable()
{
    if (m_hotkeyTable == nullptr)
    {
        return;
    }

    m_hotkeyTable->setSortingEnabled(false);
    m_hotkeyTable->setRowCount(0);

    for (const HotkeyInspectItem& rowItem : m_hotkeyRows)
    {
        const int row = m_hotkeyTable->rowCount();
        m_hotkeyTable->insertRow(row);

        const QString idText = rowItem.hotkeyId == 0U
            ? QStringLiteral("0")
            : QStringLiteral("0x%1").arg(rowItem.hotkeyId, 0, 16).toUpper();
        const QString threadText = rowItem.threadId == 0U
            ? QStringLiteral("-")
            : QString::number(rowItem.threadId);
        const QString vkModText = QStringLiteral("VK=0x%1 MOD=0x%2")
            .arg(rowItem.virtualKey, 2, 16, QChar('0'))
            .arg(rowItem.modifiers, 2, 16, QChar('0'))
            .toUpper();

        m_hotkeyTable->setItem(row, 0, createHotkeyCell(rowItem.objectText));
        m_hotkeyTable->setItem(row, 1, createHotkeyCell(idText));
        m_hotkeyTable->setItem(row, 2, createHotkeyCell(rowItem.hotkeyText));
        m_hotkeyTable->setItem(row, 3, createHotkeyCell(QString::number(rowItem.processId)));
        m_hotkeyTable->setItem(row, 4, createHotkeyCell(threadText));
        m_hotkeyTable->setItem(row, 5, createHotkeyCell(rowItem.processName));
        m_hotkeyTable->setItem(row, 6, createHotkeyCell(rowItem.sourceText));
        m_hotkeyTable->setItem(row, 7, createHotkeyCell(vkModText));
        m_hotkeyTable->setItem(row, 8, createHotkeyCell(rowItem.detailText));

        for (int column = 0; column < m_hotkeyTable->columnCount(); ++column)
        {
            QTableWidgetItem* cellItem = m_hotkeyTable->item(row, column);
            if (cellItem != nullptr)
            {
                cellItem->setToolTip(cellItem->text());
            }
        }
    }

    m_hotkeyTable->setSortingEnabled(true);
}

void ProcessDetailWindow::initializeKeyboardTab()
{
    kLogEvent initKeyboardTabEvent;
    info << initKeyboardTabEvent
        << "[ProcessDetailWindow] initializeKeyboardTab: 构建键盘页面。"
        << eol;

    m_keyboardLayout = new QVBoxLayout(m_keyboardTab);
    m_keyboardLayout->setContentsMargins(6, 6, 6, 6);
    m_keyboardLayout->setSpacing(8);

    QGroupBox* keyboardGroup = new QGroupBox(QStringLiteral("键盘检测"), m_keyboardTab);
    QVBoxLayout* keyboardGroupLayout = new QVBoxLayout(keyboardGroup);
    keyboardGroupLayout->setContentsMargins(8, 8, 8, 8);
    keyboardGroupLayout->setSpacing(6);

    QHBoxLayout* topBarLayout = new QHBoxLayout();
    m_refreshKeyboardButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QStringLiteral("刷新键盘"), keyboardGroup);
    m_refreshKeyboardButton->setToolTip(QStringLiteral("扫描热键表和 WH_KEYBOARD/WH_KEYBOARD_LL 钩子链"));
    m_keyboardStatusLabel = new QLabel(QStringLiteral("● 尚未刷新"), keyboardGroup);
    m_keyboardStatusLabel->setStyleSheet(
        QStringLiteral("color:%1; font-weight:600;")
        .arg(KswordTheme::TextSecondaryHex()));
    topBarLayout->addWidget(m_refreshKeyboardButton);
    topBarLayout->addWidget(m_keyboardStatusLabel, 1);
    keyboardGroupLayout->addLayout(topBarLayout);

    m_keyboardInnerTabWidget = new QTabWidget(keyboardGroup);
    QWidget* hotkeyPage = new QWidget(m_keyboardInnerTabWidget);
    QWidget* hookPage = new QWidget(m_keyboardInnerTabWidget);
    auto* hotkeyLayout = new QVBoxLayout(hotkeyPage);
    auto* hookLayout = new QVBoxLayout(hookPage);
    hotkeyLayout->setContentsMargins(0, 0, 0, 0);
    hookLayout->setContentsMargins(0, 0, 0, 0);

    m_keyboardHotkeyTable = new QTableWidget(hotkeyPage);
    m_keyboardHotkeyTable->setColumnCount(9);
    m_keyboardHotkeyTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("对象"),
        QStringLiteral("热键ID"),
        QStringLiteral("热键"),
        QStringLiteral("进程ID"),
        QStringLiteral("线程ID"),
        QStringLiteral("进程名"),
        QStringLiteral("来源"),
        QStringLiteral("VK/Mod"),
        QStringLiteral("详情")
    });
    m_keyboardHotkeyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_keyboardHotkeyTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_keyboardHotkeyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_keyboardHotkeyTable->setAlternatingRowColors(true);
    m_keyboardHotkeyTable->setSortingEnabled(true);
    m_keyboardHotkeyTable->verticalHeader()->setVisible(false);
    m_keyboardHotkeyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_keyboardHotkeyTable->horizontalHeader()->setStretchLastSection(true);
    m_keyboardHotkeyTable->setColumnWidth(0, 260);
    m_keyboardHotkeyTable->setColumnWidth(1, 80);
    m_keyboardHotkeyTable->setColumnWidth(2, 130);
    m_keyboardHotkeyTable->setColumnWidth(3, 80);
    m_keyboardHotkeyTable->setColumnWidth(4, 80);
    m_keyboardHotkeyTable->setColumnWidth(5, 120);
    m_keyboardHotkeyTable->setColumnWidth(6, 150);
    m_keyboardHotkeyTable->setColumnWidth(7, 120);
    hotkeyLayout->addWidget(m_keyboardHotkeyTable, 1);

    m_keyboardHookTable = new QTableWidget(hookPage);
    m_keyboardHookTable->setColumnCount(10);
    m_keyboardHookTable->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("对象"),
        QStringLiteral("类型"),
        QStringLiteral("范围"),
        QStringLiteral("进程ID"),
        QStringLiteral("线程ID"),
        QStringLiteral("函数/偏移"),
        QStringLiteral("模块"),
        QStringLiteral("来源"),
        QStringLiteral("Flags"),
        QStringLiteral("详情")
    });
    m_keyboardHookTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_keyboardHookTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_keyboardHookTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_keyboardHookTable->setAlternatingRowColors(true);
    m_keyboardHookTable->setSortingEnabled(true);
    m_keyboardHookTable->verticalHeader()->setVisible(false);
    m_keyboardHookTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_keyboardHookTable->horizontalHeader()->setStretchLastSection(true);
    m_keyboardHookTable->setColumnWidth(0, 180);
    m_keyboardHookTable->setColumnWidth(1, 120);
    m_keyboardHookTable->setColumnWidth(2, 90);
    m_keyboardHookTable->setColumnWidth(3, 80);
    m_keyboardHookTable->setColumnWidth(4, 80);
    m_keyboardHookTable->setColumnWidth(5, 150);
    m_keyboardHookTable->setColumnWidth(6, 130);
    m_keyboardHookTable->setColumnWidth(7, 160);
    m_keyboardHookTable->setColumnWidth(8, 80);
    hookLayout->addWidget(m_keyboardHookTable, 1);

    m_keyboardInnerTabWidget->addTab(hotkeyPage, QIcon(":/Icon/process_hotkey.svg"), QStringLiteral("热键"));
    m_keyboardInnerTabWidget->addTab(hookPage, QIcon(":/Icon/process_critical.svg"), QStringLiteral("键盘钩子"));
    keyboardGroupLayout->addWidget(m_keyboardInnerTabWidget, 1);
    m_keyboardLayout->addWidget(keyboardGroup, 1);

    m_refreshKeyboardButton->setStyleSheet(buildBlueButtonStyle());
}

void ProcessDetailWindow::updateKeyboardStatusLabel(const QString& statusText, const bool refreshing)
{
    if (m_keyboardStatusLabel == nullptr)
    {
        return;
    }

    m_keyboardStatusLabel->setText(statusText);
    if (refreshing)
    {
        m_keyboardStatusLabel->setStyleSheet(buildStateLabelStyle(KswordTheme::PrimaryBlueColor, 700));
    }
    else if (statusText.contains(QStringLiteral("失败")) || statusText.contains(QStringLiteral("不可用")))
    {
        m_keyboardStatusLabel->setStyleSheet(buildStateLabelStyle(statusWarningColor(), 700));
    }
    else
    {
        m_keyboardStatusLabel->setStyleSheet(buildStateLabelStyle(statusIdleColor(), 600));
    }
}

void ProcessDetailWindow::requestAsyncKeyboardRefresh()
{
    if (m_keyboardRefreshing || m_baseRecord.pid == 0)
    {
        return;
    }

    m_keyboardInitialRefreshStarted = true;
    m_keyboardRefreshing = true;
    const std::uint64_t ticketValue = ++m_keyboardRefreshTicket;
    const std::uint32_t pidValue = m_baseRecord.pid;
    const QString processNameText = QString::fromStdString(m_baseRecord.processName);
    const QString processImagePathText = QString::fromStdString(m_baseRecord.imagePath);

    if (m_refreshKeyboardButton != nullptr)
    {
        m_refreshKeyboardButton->setEnabled(false);
    }
    updateKeyboardStatusLabel(QStringLiteral("● 正在扫描键盘热键与钩子..."), true);

    if (m_keyboardRefreshProgressPid == 0)
    {
        m_keyboardRefreshProgressPid = kPro.add("进程详情", "扫描键盘");
    }
    kPro.set(m_keyboardRefreshProgressPid, "枚举热键表和键盘钩子链", 0, 20.0f);

    QPointer<ProcessDetailWindow> guardThis(this);
    auto* refreshTask = QRunnable::create(
        [guardThis, ticketValue, pidValue, processNameText, processImagePathText]()
        {
            KeyboardInspectRefreshResult refreshResult{};
            const auto beginTime = std::chrono::steady_clock::now();

            QString diagnosticText = QStringLiteral("R3窗口/菜单/资源/.lnk + R0 win32k热键/钩子");
            const std::vector<HotkeyCandidate> hotkeyCandidates = collectHotkeysForProcess(
                pidValue,
                processNameText,
                processImagePathText,
                diagnosticText);
            refreshResult.hotkeyRows.reserve(hotkeyCandidates.size());
            for (const HotkeyCandidate& candidate : hotkeyCandidates)
            {
                HotkeyInspectItem row{};
                row.objectText = candidate.objectText;
                row.hotkeyId = candidate.hotkeyId;
                row.modifiers = candidate.modifiers;
                row.virtualKey = candidate.virtualKey;
                row.hotkeyText = candidate.hotkeyText;
                row.processId = candidate.processId;
                row.threadId = candidate.threadId;
                row.processName = candidate.processName;
                row.sourceText = candidate.sourceText;
                row.detailText = candidate.detailText;
                refreshResult.hotkeyRows.push_back(std::move(row));
            }

            const std::vector<KeyboardHookCandidate> hookCandidates =
                collectDriverKeyboardHooks(pidValue, diagnosticText);
            refreshResult.hookRows.reserve(hookCandidates.size());
            for (const KeyboardHookCandidate& candidate : hookCandidates)
            {
                KeyboardHookInspectItem row{};
                row.objectText = candidate.objectText;
                row.typeText = candidate.typeText;
                row.scopeText = candidate.scopeText;
                row.processId = candidate.processId;
                row.threadId = candidate.threadId;
                row.procedureText = candidate.procedureText;
                row.moduleText = candidate.moduleText;
                row.sourceText = candidate.sourceText;
                row.flagsText = candidate.flagsText;
                row.detailText = candidate.detailText;
                refreshResult.hookRows.push_back(std::move(row));
            }

            refreshResult.diagnosticText = diagnosticText;
            refreshResult.elapsedMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - beginTime).count());

            QMetaObject::invokeMethod(
                guardThis,
                [guardThis, ticketValue, refreshResult]()
                {
                    if (guardThis == nullptr || guardThis->m_keyboardRefreshTicket != ticketValue)
                    {
                        return;
                    }
                    guardThis->applyKeyboardRefreshResult(refreshResult);
                },
                Qt::QueuedConnection);
        });

    refreshTask->setAutoDelete(true);
    QThreadPool::globalInstance()->start(refreshTask);
}

void ProcessDetailWindow::applyKeyboardRefreshResult(const KeyboardInspectRefreshResult& refreshResult)
{
    m_keyboardRefreshing = false;
    if (m_refreshKeyboardButton != nullptr)
    {
        m_refreshKeyboardButton->setEnabled(true);
    }

    m_keyboardHotkeyRows = refreshResult.hotkeyRows;
    m_keyboardHookRows = refreshResult.hookRows;
    m_hotkeyRows = refreshResult.hotkeyRows;
    rebuildKeyboardHotkeyTable();
    rebuildKeyboardHookTable();
    rebuildHotkeyTable();

    QString statusText = QStringLiteral("● 刷新完成 %1 ms | 热键:%2 | 键盘钩子:%3")
        .arg(refreshResult.elapsedMs)
        .arg(static_cast<qulonglong>(refreshResult.hotkeyRows.size()))
        .arg(static_cast<qulonglong>(refreshResult.hookRows.size()));
    if (!refreshResult.diagnosticText.trimmed().isEmpty())
    {
        statusText += QStringLiteral(" | %1").arg(refreshResult.diagnosticText.trimmed());
    }
    updateKeyboardStatusLabel(statusText, false);
    if (m_hotkeyStatusLabel != nullptr)
    {
        updateHotkeyStatusLabel(statusText, false);
    }

    if (m_keyboardRefreshProgressPid > 0)
    {
        kPro.set(m_keyboardRefreshProgressPid, "键盘扫描完成", 100, 1.0f);
    }

    kLogEvent keyboardRefreshDoneEvent;
    info << keyboardRefreshDoneEvent
        << "[ProcessDetailWindow] 键盘扫描完成, pid="
        << m_baseRecord.pid
        << ", hotkeys="
        << refreshResult.hotkeyRows.size()
        << ", hooks="
        << refreshResult.hookRows.size()
        << ", elapsedMs="
        << refreshResult.elapsedMs
        << ", diagnostic="
        << refreshResult.diagnosticText.toStdString()
        << eol;
}

void ProcessDetailWindow::rebuildKeyboardHotkeyTable()
{
    if (m_keyboardHotkeyTable == nullptr)
    {
        return;
    }

    m_keyboardHotkeyTable->setSortingEnabled(false);
    m_keyboardHotkeyTable->setRowCount(0);

    for (const HotkeyInspectItem& rowItem : m_keyboardHotkeyRows)
    {
        const int row = m_keyboardHotkeyTable->rowCount();
        m_keyboardHotkeyTable->insertRow(row);

        const QString idText = rowItem.hotkeyId == 0U
            ? QStringLiteral("0")
            : QStringLiteral("0x%1").arg(rowItem.hotkeyId, 0, 16).toUpper();
        const QString threadText = rowItem.threadId == 0U
            ? QStringLiteral("-")
            : QString::number(rowItem.threadId);
        const QString vkModText = QStringLiteral("VK=0x%1 MOD=0x%2")
            .arg(rowItem.virtualKey, 2, 16, QChar('0'))
            .arg(rowItem.modifiers, 2, 16, QChar('0'))
            .toUpper();

        m_keyboardHotkeyTable->setItem(row, 0, createHotkeyCell(rowItem.objectText));
        m_keyboardHotkeyTable->setItem(row, 1, createHotkeyCell(idText));
        m_keyboardHotkeyTable->setItem(row, 2, createHotkeyCell(rowItem.hotkeyText));
        m_keyboardHotkeyTable->setItem(row, 3, createHotkeyCell(QString::number(rowItem.processId)));
        m_keyboardHotkeyTable->setItem(row, 4, createHotkeyCell(threadText));
        m_keyboardHotkeyTable->setItem(row, 5, createHotkeyCell(rowItem.processName));
        m_keyboardHotkeyTable->setItem(row, 6, createHotkeyCell(rowItem.sourceText));
        m_keyboardHotkeyTable->setItem(row, 7, createHotkeyCell(vkModText));
        m_keyboardHotkeyTable->setItem(row, 8, createHotkeyCell(rowItem.detailText));

        for (int column = 0; column < m_keyboardHotkeyTable->columnCount(); ++column)
        {
            QTableWidgetItem* cellItem = m_keyboardHotkeyTable->item(row, column);
            if (cellItem != nullptr)
            {
                cellItem->setToolTip(cellItem->text());
            }
        }
    }

    m_keyboardHotkeyTable->setSortingEnabled(true);
}

void ProcessDetailWindow::rebuildKeyboardHookTable()
{
    if (m_keyboardHookTable == nullptr)
    {
        return;
    }

    m_keyboardHookTable->setSortingEnabled(false);
    m_keyboardHookTable->setRowCount(0);

    for (const KeyboardHookInspectItem& rowItem : m_keyboardHookRows)
    {
        const int row = m_keyboardHookTable->rowCount();
        m_keyboardHookTable->insertRow(row);
        const QString threadText = rowItem.threadId == 0U
            ? QStringLiteral("-")
            : QString::number(rowItem.threadId);

        m_keyboardHookTable->setItem(row, 0, createHotkeyCell(rowItem.objectText));
        m_keyboardHookTable->setItem(row, 1, createHotkeyCell(rowItem.typeText));
        m_keyboardHookTable->setItem(row, 2, createHotkeyCell(rowItem.scopeText));
        m_keyboardHookTable->setItem(row, 3, createHotkeyCell(QString::number(rowItem.processId)));
        m_keyboardHookTable->setItem(row, 4, createHotkeyCell(threadText));
        m_keyboardHookTable->setItem(row, 5, createHotkeyCell(rowItem.procedureText));
        m_keyboardHookTable->setItem(row, 6, createHotkeyCell(rowItem.moduleText));
        m_keyboardHookTable->setItem(row, 7, createHotkeyCell(rowItem.sourceText));
        m_keyboardHookTable->setItem(row, 8, createHotkeyCell(rowItem.flagsText));
        m_keyboardHookTable->setItem(row, 9, createHotkeyCell(rowItem.detailText));

        for (int column = 0; column < m_keyboardHookTable->columnCount(); ++column)
        {
            QTableWidgetItem* cellItem = m_keyboardHookTable->item(row, column);
            if (cellItem != nullptr)
            {
                cellItem->setToolTip(cellItem->text());
            }
        }
    }

    m_keyboardHookTable->setSortingEnabled(true);
}
