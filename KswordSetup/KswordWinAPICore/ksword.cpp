#include "ksword.h"

#include <Lmcons.h>
#include <vector>

namespace {
// The shell AppUserModelID must be stable across launches so Windows can group
// the borderless main window in taskbar and Alt+Tab like a normal application.
constexpr wchar_t kKswordAppUserModelID[] = L"KswordFrame3.KswordFrame3_0";

// SetProcessAppIDProc mirrors SetCurrentProcessExplicitAppUserModelID without
// adding a Shell32 import dependency to the project file. Input is the app id;
// output is the HRESULT produced by shell32.
using SetProcessAppIDProc = HRESULT(WINAPI*)(PCWSTR);
}

// GetUserName wraps the Win32 GetUserNameA API and normalizes failures to an
// empty string so callers can use it without handling Windows error codes.
std::string GetUserName() {
    DWORD size = UNLEN + 1;
    std::vector<char> buffer(size, '\0');

    if (!::GetUserNameA(buffer.data(), &size)) {
        return {};
    }

    if (size > 0) {
        --size;
    }
    return std::string(buffer.data(), size);
}

// GetHostName wraps GetComputerNameA and returns the host name bytes reported by
// Windows. On failure, the function returns an empty string.
std::string GetHostName() {
    char computer_name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (!::GetComputerNameA(computer_name, &size)) {
        return {};
    }
    return std::string(computer_name, size);
}

// IsAdmin checks the current access token against the built-in Administrators
// group. It does not throw and treats API failures as "not admin".
bool IsAdmin() {
    BOOL is_admin = FALSE;
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    PSID admin_group = nullptr;

    if (::AllocateAndInitializeSid(
            &nt_authority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0,
            0,
            0,
            0,
            0,
            0,
            &admin_group)) {
        ::CheckTokenMembership(nullptr, admin_group, &is_admin);
        ::FreeSid(admin_group);
    }

    return is_admin == TRUE;
}

// GetSelfPath repeatedly grows a buffer until GetModuleFileNameA can return the
// full executable path. It returns an empty string only when the Win32 call fails.
std::string GetSelfPath() {
    std::vector<char> buffer(MAX_PATH, '\0');
    while (true) {
        const DWORD length = ::GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::string(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2, '\0');
    }
}

// KEnsureAppUserModelID dynamically calls the Windows shell API once. It keeps
// the executable link settings unchanged; older shells simply return false.
bool KEnsureAppUserModelID() {
    static bool attempted = false;
    static bool applied = false;
    if (attempted) {
        return applied;
    }

    attempted = true;
    HMODULE shell32 = ::LoadLibraryW(L"shell32.dll");
    if (!shell32) {
        return false;
    }

    SetProcessAppIDProc setAppId = reinterpret_cast<SetProcessAppIDProc>(
        ::GetProcAddress(shell32, "SetCurrentProcessExplicitAppUserModelID"));
    if (setAppId) {
        applied = SUCCEEDED(setAppId(kKswordAppUserModelID));
    }

    ::FreeLibrary(shell32);
    return applied;
}
