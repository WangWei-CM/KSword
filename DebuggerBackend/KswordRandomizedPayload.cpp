#include "KswordRandomizedPayload.h"

#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <bcrypt.h>
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

// 判断 directory 下所有 .exe 都能用 FILE_SHARE_NONE 独占打开，等同于"没有进程把它作为 image 加载"。
// 一旦有任何 exe 打开失败就返回 false，调用方不删这个目录（可能还在用）。
bool directoryExesAreNotInUse(const std::wstring& directory)
{
    const std::wstring pattern = joinPath(directory, L"*");
    WIN32_FIND_DATAW find{};
    HANDLE finder = ::FindFirstFileW(pattern.c_str(), &find);
    if (finder == INVALID_HANDLE_VALUE) {
        return true; // 空目录视为可清。
    }
    bool freeToDelete = true;
    do {
        const wchar_t* name = find.cFileName;
        if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) {
            continue;
        }
        const std::wstring child = joinPath(directory, name);
        if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            if (!directoryExesAreNotInUse(child)) {
                freeToDelete = false;
                break;
            }
            continue;
        }
        const size_t nameLen = ::wcslen(name);
        if (nameLen < 4U) {
            continue;
        }
        if (::_wcsicmp(name + nameLen - 4U, L".exe") != 0 &&
            ::_wcsicmp(name + nameLen - 4U, L".dll") != 0) {
            continue;
        }
        HANDLE probe = ::CreateFileW(child.c_str(), DELETE, 0U, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe == INVALID_HANDLE_VALUE) {
            freeToDelete = false;
            break;
        }
        ::CloseHandle(probe);
    } while (::FindNextFileW(finder, &find) != FALSE);
    ::FindClose(finder);
    return freeToDelete;
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
        const std::wstring candidate = joinPath(sessionsRoot, name);
        if (directoryExesAreNotInUse(candidate)) {
            cleanupRandomizedPayload(candidate);
        }
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
        return makeFailure("BCryptGenRandom(session)", ::GetLastError());
    }
    const std::wstring sessionDir = joinPath(sessionsRoot, L"x-" + sessionHex);
    if (::CreateDirectoryW(sessionDir.c_str(), nullptr) == FALSE) {
        return makeFailure("CreateDirectoryW(session)", ::GetLastError());
    }

    // 主 exe 新名字。
    std::wstring renamedExe;
    if (mode == NamingMode::FullyRandomized) {
        const std::wstring fullHex = randomHex(16U);
        if (fullHex.empty()) {
            return makeFailure("BCryptGenRandom(exe)", ::GetLastError());
        }
        renamedExe = fullyRandomizedName(primaryExeName, fullHex);
    } else {
        const std::wstring suffixHex = randomHex(4U);
        if (suffixHex.empty()) {
            return makeFailure("BCryptGenRandom(suffix)", ::GetLastError());
        }
        renamedExe = suffixedName(primaryExeName, suffixHex);
    }

    // 递归克隆（硬链接优先）。
    const char* failureOp = nullptr;
    const DWORD cloneError = cloneDirectoryTree(sourceRoot, sessionDir,
        primaryExeName, renamedExe, failureOp);
    if (cloneError != ERROR_SUCCESS) {
        // 克隆失败：尽力清理半成品会话目录。
        cleanupRandomizedPayload(sessionDir);
        return makeFailure(failureOp != nullptr ? failureOp : "cloneDirectoryTree", cloneError);
    }

    PrepareResult result{};
    result.success = true;
    result.clonedDirectory = sessionDir;
    result.clonedExecutable = joinPath(sessionDir, renamedExe);
    result.lastError = ERROR_SUCCESS;
    return result;
}

void cleanupRandomizedPayload(const std::wstring& clonedDirectory)
{
    if (clonedDirectory.empty()) {
        return;
    }
    // 必须包含 KSword\\sessions\\ 前缀，否则拒绝（防误删任意路径）。
    if (clonedDirectory.find(L"\\KSword\\sessions\\") == std::wstring::npos) {
        return;
    }
    const DWORD attrs = ::GetFileAttributesW(clonedDirectory.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
        return;
    }
    // SHFileOperationW 需要双 null 结尾的路径数组。
    std::vector<wchar_t> path(clonedDirectory.begin(), clonedDirectory.end());
    path.push_back(L'\0');
    path.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = path.data();
    op.fFlags = FOF_NO_UI | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI;
    (void)::SHFileOperationW(&op);
}

} // namespace ksword::randomized_payload
