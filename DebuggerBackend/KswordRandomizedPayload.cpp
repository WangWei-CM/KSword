#include "KswordRandomizedPayload.h"

#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <TlHelp32.h>
#include <cstdint>
#include <cwchar>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace ksword::randomized_payload {

namespace {

// 把 Win32 错误码+操作描述打包进 PrepareResult 的失败字段。
PrepareResult makeFailure(const char* operation, DWORD lastError)
{
    PrepareResult result{};
    result.success = false;
    result.lastError = lastError;
    result.failureDescription = operation != nullptr ? operation : "unknown";
    return result;
}

// 生成一串随机 hex。使用 BCryptGenRandom 的 CNG 熵源，不依赖
// Date.now / rand（Workflow 场景也可能回放）。
std::wstring randomHex(size_t byteCount)
{
    std::vector<unsigned char> bytes(byteCount);
    const NTSTATUS status = ::BCryptGenRandom(
        nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        return std::wstring();
    }
    static const wchar_t table[] = L"0123456789ABCDEF";
    std::wstring hex;
    hex.reserve(byteCount * 2);
    for (unsigned char value : bytes) {
        hex.push_back(table[(value >> 4) & 0x0F]);
        hex.push_back(table[value & 0x0F]);
    }
    return hex;
}

// 构造符合"原名加 hex 后缀"的新文件名。例如 L"x64dbg.exe" + L"A3F70B21"
// → L"x64dbg-A3F70B21.exe"。没有扩展名则直接拼接。
std::wstring suffixedName(const std::wstring& originalName, const std::wstring& hexSuffix)
{
    const size_t dot = originalName.find_last_of(L'.');
    if (dot == std::wstring::npos) {
        return originalName + L"-" + hexSuffix;
    }
    std::wstring stem = originalName.substr(0, dot);
    std::wstring extension = originalName.substr(dot);
    return stem + L"-" + hexSuffix + extension;
}

// 全随机 GUID 命名：32 位 hex + 原扩展名。
std::wstring fullyRandomizedName(const std::wstring& originalName, const std::wstring& hex32)
{
    const size_t dot = originalName.find_last_of(L'.');
    std::wstring extension = (dot != std::wstring::npos) ? originalName.substr(dot) : std::wstring();
    return hex32 + extension;
}

// 规范化去尾部反斜杠。
std::wstring stripTrailingSeparator(std::wstring path)
{
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

// 拼接 base + L"\\" + leaf，base 可能已有尾部斜杠。
std::wstring joinPath(const std::wstring& base, const std::wstring& leaf)
{
    if (base.empty()) {
        return leaf;
    }
    if (base.back() == L'\\' || base.back() == L'/') {
        return base + leaf;
    }
    return base + L'\\' + leaf;
}

// 取 %LOCALAPPDATA%；失败回退 %TEMP%。
std::wstring preferredContainerRoot()
{
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
            nullptr, 0, path))) {
        return std::wstring(path);
    }
    DWORD count = ::GetTempPathW(MAX_PATH, path);
    if (count > 0U && count < MAX_PATH) {
        return std::wstring(path, path + count);
    }
    return std::wstring();
}

// 深度递归：对 srcDir 下的所有条目，在 dstDir 建立同名硬链接或子目录。
// renameMap 若非空且命中某文件，则在 dstDir 下以 renameMap.second 命名。
// 返回 Win32 错误码；0 表示成功。
DWORD cloneDirectoryTree(
    const std::wstring& srcDir,
    const std::wstring& dstDir,
    const std::wstring& renameFrom,
    const std::wstring& renameTo,
    const char*& failureOp)
{
    const DWORD createDirError = ::CreateDirectoryW(dstDir.c_str(), nullptr)
        ? ERROR_SUCCESS
        : ::GetLastError();
    if (createDirError != ERROR_SUCCESS && createDirError != ERROR_ALREADY_EXISTS) {
        failureOp = "CreateDirectoryW(destination)";
        return createDirError;
    }

    const std::wstring searchPattern = joinPath(srcDir, L"*");
    WIN32_FIND_DATAW findData{};
    HANDLE finder = ::FindFirstFileW(searchPattern.c_str(), &findData);
    if (finder == INVALID_HANDLE_VALUE) {
        failureOp = "FindFirstFileW";
        return ::GetLastError();
    }

    DWORD lastError = ERROR_SUCCESS;
    do {
        const wchar_t* name = findData.cFileName;
        if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) {
            continue;
        }
        const std::wstring srcPath = joinPath(srcDir, name);
        std::wstring dstLeaf = name;
        if (!renameFrom.empty() && ::CompareStringOrdinal(name, -1,
                renameFrom.c_str(), -1, TRUE) == CSTR_EQUAL) {
            dstLeaf = renameTo;
        }
        const std::wstring dstPath = joinPath(dstDir, dstLeaf);

        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            // 子目录递归；重命名只影响顶层主 exe，不传递到子目录。
            lastError = cloneDirectoryTree(srcPath, dstPath,
                std::wstring(), std::wstring(), failureOp);
            if (lastError != ERROR_SUCCESS) {
                break;
            }
            continue;
        }

        // 先尝试硬链接（同卷 NTFS 零开销）；跨卷失败则退化到 CopyFileW。
        if (::CreateHardLinkW(dstPath.c_str(), srcPath.c_str(), nullptr) == FALSE) {
            const DWORD linkError = ::GetLastError();
            // ERROR_NOT_SAME_DEVICE=0x11、ERROR_INVALID_FUNCTION=0x01（非 NTFS）
            // 和 ERROR_ACCESS_DENIED 都降级到复制。
            if (linkError == ERROR_NOT_SAME_DEVICE ||
                linkError == ERROR_INVALID_FUNCTION ||
                linkError == ERROR_ACCESS_DENIED ||
                linkError == ERROR_NOT_SUPPORTED) {
                if (::CopyFileW(srcPath.c_str(), dstPath.c_str(), FALSE) == FALSE) {
                    failureOp = "CopyFileW(fallback)";
                    lastError = ::GetLastError();
                    break;
                }
            } else {
                failureOp = "CreateHardLinkW";
                lastError = linkError;
                break;
            }
        }
    } while (::FindNextFileW(finder, &findData) != FALSE);

    ::FindClose(finder);
    return lastError;
}

constexpr DWORD kSessionMagic = 0x5357534BU; // KSWS
constexpr DWORD kSessionVersion = 1U;
constexpr DWORD kPreparing = 1U;
constexpr DWORD kRunning = 2U;
constexpr ULONGLONG kAbandonedLaunchGrace = 120ULL * 10000000ULL;
constexpr wchar_t kMarkerName[] = L".ksword-launch";

struct SessionRecord {
    DWORD magic = kSessionMagic;
    DWORD version = kSessionVersion;
    DWORD state = kPreparing;
    DWORD ownerPid = 0;
    ULONGLONG ownerCreated = 0;
    DWORD childPid = 0;
    ULONGLONG childCreated = 0;
};

ULONGLONG fileTimeValue(const FILETIME& time)
{
    return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

ULONGLONG processCreationTime(HANDLE process)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    return process != nullptr && ::GetProcessTimes(process, &created, &exited, &kernel, &user)
        ? fileTimeValue(created) : 0;
}

bool processIdentityIsRunning(DWORD pid, ULONGLONG created)
{
    if (pid == 0 || created == 0) return false;
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) return false;
    const bool running = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT &&
        processCreationTime(process) == created;
    ::CloseHandle(process);
    return running;
}

bool writeRecord(HANDLE file, const SessionRecord& record)
{
    LARGE_INTEGER start{};
    DWORD written = 0;
    return ::SetFilePointerEx(file, start, nullptr, FILE_BEGIN) &&
        ::WriteFile(file, &record, sizeof(record), &written, nullptr) &&
        written == sizeof(record) && ::SetEndOfFile(file) && ::FlushFileBuffers(file);
}

bool validSessionLeaf(const std::wstring& leaf)
{
    if (leaf.size() != 18 || leaf[0] != L'x' || leaf[1] != L'-') return false;
    for (size_t i = 2; i < leaf.size(); ++i) {
        const wchar_t c = leaf[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F'))) return false;
    }
    return true;
}

bool validSessionDirectory(const std::wstring& path, std::wstring& leaf)
{
    const std::wstring appData = preferredContainerRoot();
    if (appData.empty()) return false;
    const std::wstring root = joinPath(joinPath(appData, L"KSword"), L"sessions");
    wchar_t expanded[32768];
    const DWORD length = ::GetFullPathNameW(path.c_str(), _countof(expanded), expanded, nullptr);
    if (length == 0 || length >= _countof(expanded)) return false;
    const std::wstring canonical(expanded, length);
    const std::wstring prefix = root + L'\\';
    if (canonical.size() <= prefix.size() ||
        ::_wcsnicmp(canonical.c_str(), prefix.c_str(), prefix.size()) != 0) return false;
    leaf = canonical.substr(prefix.size());
    if (!validSessionLeaf(leaf)) return false;
    const DWORD attrs = ::GetFileAttributesW(canonical.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

HANDLE lockSession(const std::wstring& leaf)
{
    const std::wstring name = L"Local\\KSword.RandomPayload." + leaf;
    HANDLE mutex = ::CreateMutexW(nullptr, FALSE, name.c_str());
    if (mutex == nullptr) return nullptr;
    const DWORD result = ::WaitForSingleObject(mutex, 0);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return mutex;
    ::CloseHandle(mutex);
    return nullptr;
}

void unlockSession(HANDLE mutex)
{
    ::ReleaseMutex(mutex);
    ::CloseHandle(mutex);
}

bool recentlyCreated(const std::wstring& directory)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    FILETIME now{};
    if (!::GetFileAttributesExW(directory.c_str(), GetFileExInfoStandard, &attributes)) return true;
    ::GetSystemTimeAsFileTime(&now);
    const ULONGLONG current = fileTimeValue(now);
    const ULONGLONG created = fileTimeValue(attributes.ftCreationTime);
    return current < created || current - created < kAbandonedLaunchGrace;
}

bool collectExeNames(const std::wstring& directory, std::vector<std::wstring>& names)
{
    WIN32_FIND_DATAW find{};
    HANDLE finder = ::FindFirstFileW(joinPath(directory, L"*").c_str(), &find);
    if (finder == INVALID_HANDLE_VALUE) return ::GetLastError() == ERROR_FILE_NOT_FOUND;
    bool complete = true;
    do {
        const wchar_t* name = find.cFileName;
        if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) continue;
        const std::wstring child = joinPath(directory, name);
        if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            if ((find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U)
                complete = collectExeNames(child, names) && complete;
        } else {
            const size_t length = ::wcslen(name);
            if (length >= 4 && ::_wcsicmp(name + length - 4, L".exe") == 0)
                names.emplace_back(name);
        }
    } while (::FindNextFileW(finder, &find));
    complete = ::GetLastError() == ERROR_NO_MORE_FILES && complete;
    ::FindClose(finder);
    return complete;
}

// Query the actual running image paths. Openability of an image file is not a
// liveness test: a child may outlive its launcher or load another exe in the
// cloned tree. Inaccessible processes with a matching cloned name are retained.
bool directoryMayContainRunningProcess(const std::wstring& directory)
{
    std::vector<std::wstring> exeNames;
    if (!collectExeNames(directory, exeNames)) return true;
    const std::wstring prefix = directory + L'\\';
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool inUse = false;
    if (!::Process32FirstW(snapshot, &entry)) {
        ::CloseHandle(snapshot);
        return true;
    }
    do {
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
            FALSE, entry.th32ProcessID);
        if (process != nullptr) {
            if (::WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                wchar_t image[32768];
                DWORD length = _countof(image);
                if (::QueryFullProcessImageNameW(process, 0, image, &length)) {
                    if (length > prefix.size() &&
                        ::_wcsnicmp(image, prefix.c_str(), prefix.size()) == 0) inUse = true;
                } else {
                    for (const auto& name : exeNames)
                        if (::_wcsicmp(entry.szExeFile, name.c_str()) == 0) inUse = true;
                }
            }
            ::CloseHandle(process);
        } else {
            for (const auto& name : exeNames)
                if (::_wcsicmp(entry.szExeFile, name.c_str()) == 0) inUse = true;
        }
        if (inUse) break;
    } while (::Process32NextW(snapshot, &entry));
    if (::GetLastError() != ERROR_NO_MORE_FILES) inUse = true;
    ::CloseHandle(snapshot);
    return inUse;
}

void deleteTreeUnchecked(const std::wstring& directory)
{
    std::vector<wchar_t> path(directory.begin(), directory.end());
    path.push_back(L'\0');
    path.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = path.data();
    op.fFlags = FOF_NO_UI | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI;
    (void)::SHFileOperationW(&op);
}

} // anonymous namespace

void sweepStaleSessions()
{
    const std::wstring appData = preferredContainerRoot();
    if (appData.empty()) {
        return;
    }
    const std::wstring sessionsRoot = joinPath(joinPath(appData, L"KSword"), L"sessions");
    const DWORD attrs = ::GetFileAttributesW(sessionsRoot.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
        return;
    }
    const std::wstring pattern = joinPath(sessionsRoot, L"x-*");
    WIN32_FIND_DATAW find{};
    HANDLE finder = ::FindFirstFileW(pattern.c_str(), &find);
    if (finder == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
            continue;
        }
        const wchar_t* name = find.cFileName;
        if (name[0] == L'.') {
            continue;
        }
        cleanupRandomizedPayload(joinPath(sessionsRoot, name));
    } while (::FindNextFileW(finder, &find) != FALSE);
    ::FindClose(finder);
}

PrepareResult prepareRandomizedPayload(
    const std::wstring& sourcePayloadRoot,
    const std::wstring& primaryExeName,
    NamingMode mode)
{
    // 入参基本校验。primaryExeName 不许带路径分隔符，避免越层。
    if (sourcePayloadRoot.empty() || primaryExeName.empty()) {
        return makeFailure("input empty", ERROR_INVALID_PARAMETER);
    }
    if (primaryExeName.find(L'\\') != std::wstring::npos ||
        primaryExeName.find(L'/') != std::wstring::npos) {
        return makeFailure("primaryExeName contains separator", ERROR_INVALID_PARAMETER);
    }
    const std::wstring sourceRoot = stripTrailingSeparator(sourcePayloadRoot);
    const DWORD sourceAttrs = ::GetFileAttributesW(sourceRoot.c_str());
    if (sourceAttrs == INVALID_FILE_ATTRIBUTES ||
        (sourceAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
        return makeFailure("source payload directory missing", ERROR_PATH_NOT_FOUND);
    }
    const std::wstring primaryExePath = joinPath(sourceRoot, primaryExeName);
    if (::GetFileAttributesW(primaryExePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return makeFailure("primary exe missing", ERROR_FILE_NOT_FOUND);
    }

    // 容器根 %LOCALAPPDATA%\KSword\sessions 存在性保证。
    const std::wstring appData = preferredContainerRoot();
    if (appData.empty()) {
        return makeFailure("SHGetFolderPathW(LocalAppData)", ::GetLastError());
    }
    const std::wstring kswordRoot = joinPath(appData, L"KSword");
    (void)::CreateDirectoryW(kswordRoot.c_str(), nullptr);
    const std::wstring sessionsRoot = joinPath(kswordRoot, L"sessions");
    (void)::CreateDirectoryW(sessionsRoot.c_str(), nullptr);

    // 建新会话前先回收无进程占用的旧会话。CE 的 launch 命令是 fire-and-forget，
    // Launcher 自己无从在 CE 退出时清理，靠这里兜底。
    sweepStaleSessions();

    // 本次会话目录名：16 字节 hex。GUID 32 字符太长，容易让日志难读。
    const std::wstring sessionHex = randomHex(8U);
    if (sessionHex.empty()) {
        return makeFailure("BCryptGenRandom(session)", ERROR_GEN_FAILURE);
    }
    const std::wstring sessionDir = joinPath(sessionsRoot, L"x-" + sessionHex);
    // The named mutex closes the small gap between directory creation and
    // opening its marker. Other launchers cannot sweep this new directory.
    HANDLE creationMutex = lockSession(L"x-" + sessionHex);
    if (creationMutex == nullptr) {
        return makeFailure("CreateMutexW(session)", ::GetLastError());
    }
    if (::CreateDirectoryW(sessionDir.c_str(), nullptr) == FALSE) {
        const DWORD error = ::GetLastError();
        unlockSession(creationMutex);
        return makeFailure("CreateDirectoryW(session)", error);
    }
    const std::wstring markerPath = joinPath(sessionDir, kMarkerName);
    HANDLE launchLease = ::CreateFileW(markerPath.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (launchLease != INVALID_HANDLE_VALUE) {
        SessionRecord record{};
        record.ownerPid = ::GetCurrentProcessId();
        record.ownerCreated = processCreationTime(::GetCurrentProcess());
        if (!writeRecord(launchLease, record)) {
            ::CloseHandle(launchLease);
            (void)::DeleteFileW(markerPath.c_str());
            launchLease = INVALID_HANDLE_VALUE;
        }
    }
    // The mutex remains owned until CreateProcess finishes. If the marker
    // cannot be written, launches still work and cleanup falls back to the
    // bounded age window plus exact running-image path check.

    // 主 exe 新名字。
    std::wstring renamedExe;
    if (mode == NamingMode::FullyRandomized) {
        const std::wstring fullHex = randomHex(16U);
        if (fullHex.empty()) {
            if (launchLease != INVALID_HANDLE_VALUE) ::CloseHandle(launchLease);
            deleteTreeUnchecked(sessionDir);
            unlockSession(creationMutex);
            return makeFailure("BCryptGenRandom(exe)", ERROR_GEN_FAILURE);
        }
        renamedExe = fullyRandomizedName(primaryExeName, fullHex);
    } else {
        const std::wstring suffixHex = randomHex(4U);
        if (suffixHex.empty()) {
            if (launchLease != INVALID_HANDLE_VALUE) ::CloseHandle(launchLease);
            deleteTreeUnchecked(sessionDir);
            unlockSession(creationMutex);
            return makeFailure("BCryptGenRandom(suffix)", ERROR_GEN_FAILURE);
        }
        renamedExe = suffixedName(primaryExeName, suffixHex);
    }

    // 递归克隆（硬链接优先）。
    const char* failureOp = nullptr;
    const DWORD cloneError = cloneDirectoryTree(sourceRoot, sessionDir,
        primaryExeName, renamedExe, failureOp);
    if (cloneError != ERROR_SUCCESS) {
        // 克隆失败：尽力清理半成品会话目录。
        if (launchLease != INVALID_HANDLE_VALUE) ::CloseHandle(launchLease);
        deleteTreeUnchecked(sessionDir);
        unlockSession(creationMutex);
        return makeFailure(failureOp != nullptr ? failureOp : "cloneDirectoryTree", cloneError);
    }

    PrepareResult result{};
    result.success = true;
    result.clonedDirectory = sessionDir;
    result.clonedExecutable = joinPath(sessionDir, renamedExe);
    result.lastError = ERROR_SUCCESS;
    result.launchLease = launchLease;
    result.launchMutex = creationMutex;
    return result;
}

void finishRandomizedPayloadLaunch(PrepareResult& prepared, HANDLE launchedProcess)
{
    if (prepared.launchLease != INVALID_HANDLE_VALUE && launchedProcess != nullptr) {
        SessionRecord record{};
        record.ownerPid = ::GetCurrentProcessId();
        record.ownerCreated = processCreationTime(::GetCurrentProcess());
        record.childPid = ::GetProcessId(launchedProcess);
        record.childCreated = processCreationTime(launchedProcess);
        record.state = kRunning;
        if (record.childPid != 0 && record.childCreated != 0)
            (void)writeRecord(prepared.launchLease, record);
    }
    if (prepared.launchLease != INVALID_HANDLE_VALUE) {
        ::CloseHandle(prepared.launchLease);
        prepared.launchLease = INVALID_HANDLE_VALUE;
    }
    if (prepared.launchMutex != nullptr) {
        unlockSession(prepared.launchMutex);
        prepared.launchMutex = nullptr;
    }
}

void cleanupRandomizedPayload(const std::wstring& clonedDirectory)
{
    std::wstring leaf;
    if (!validSessionDirectory(clonedDirectory, leaf)) return;
    HANDLE mutex = lockSession(leaf);
    if (mutex == nullptr) return;
    const std::wstring markerPath = joinPath(clonedDirectory, kMarkerName);
    HANDLE marker = ::CreateFileW(markerPath.c_str(), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_NOT_FOUND || recentlyCreated(clonedDirectory)) {
            unlockSession(mutex);
            return;
        }
    } else {
        SessionRecord record{};
        DWORD read = 0;
        const bool valid = ::ReadFile(marker, &record, sizeof(record), &read, nullptr) &&
            read == sizeof(record) && record.magic == kSessionMagic &&
            record.version == kSessionVersion;
        ::CloseHandle(marker);
        if (valid) {
            if ((record.state == kPreparing && record.ownerPid != ::GetCurrentProcessId() &&
                    (processIdentityIsRunning(record.ownerPid, record.ownerCreated) ||
                     recentlyCreated(clonedDirectory))) ||
                (record.state == kRunning &&
                    processIdentityIsRunning(record.childPid, record.childCreated))) {
                unlockSession(mutex);
                return;
            }
        } else if (recentlyCreated(clonedDirectory)) {
            unlockSession(mutex);
            return;
        }
    }
    if (!directoryMayContainRunningProcess(clonedDirectory))
        deleteTreeUnchecked(clonedDirectory);
    unlockSession(mutex);
}

} // namespace ksword::randomized_payload
