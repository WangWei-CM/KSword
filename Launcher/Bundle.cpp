#include "Launcher.h"

#include <shellapi.h>
#include <algorithm>
#include <sstream>

namespace launcher {

namespace {

std::wstring FileNameOnly(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

std::wstring BundleRoot() {
    wchar_t temp[MAX_PATH] = {};
    const DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
    if (!length || length >= ARRAYSIZE(temp)) return {};
    std::wstring root(temp, length);
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    return JoinPath(root, L"CompressAndSendToDeveloper");
}

bool DeleteTree(const std::wstring& path) {
    WIN32_FIND_DATAW data = {};
    const std::wstring pattern = JoinPath(path, L"*");
    HANDLE finder = FindFirstFileW(pattern.c_str(), &data);
    if (finder != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
            const std::wstring child = JoinPath(path, data.cFileName);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!DeleteTree(child)) { FindClose(finder); return false; }
            } else if (!DeleteFileW(child.c_str())) { FindClose(finder); return false; }
        } while (FindNextFileW(finder, &data));
        FindClose(finder);
    }
    return RemoveDirectoryW(path.c_str()) != FALSE || GetLastError() == ERROR_PATH_NOT_FOUND;
}

bool CopyAttachment(const std::wstring& source, const std::wstring& destination, std::vector<std::wstring>* copied, std::string* log) {
    if (source.empty() || !FileExists(source)) {
        if (log) *log += "missing source: " + WideToUtf8(source) + "\n";
        return false;
    }
    if (!CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
        if (log) *log += "copy failed: " + WideToUtf8(source) + " (" + FormatWin32Error() + ")\n";
        return false;
    }
    copied->push_back(destination);
    return true;
}

std::string IdentityJson(const PeIdentity& identity) {
    const std::string symbolKey = UpperAscii(identity.pdbGuid) + std::to_string(identity.pdbAge);
    std::ostringstream json;
    json << "{\"fileName\":\"" << JsonEscape(WideToUtf8(identity.fileName)) << "\",\"path\":\"" << JsonEscape(WideToUtf8(identity.path))
         << "\",\"machine\":" << identity.machine << ",\"timeDateStamp\":" << identity.timeDateStamp << ",\"sizeOfImage\":" << identity.sizeOfImage
         << ",\"pdbName\":\"" << JsonEscape(identity.pdbName) << "\",\"pdbGuid\":\"" << JsonEscape(identity.pdbGuid) << "\",\"pdbAge\":" << identity.pdbAge
         << ",\"pdbSymbolKey\":\"" << JsonEscape(symbolKey) << "\""
         << ",\"valid\":" << (identity.valid ? "true" : "false") << ",\"error\":\"" << JsonEscape(identity.error) << "\"}";
    return json.str();
}

const ModuleDefinition* FindModulePolicy(const SupportManifest& manifest, int classId) {
    for (const ModuleDefinition& module : manifest.modules) if (module.classId == classId) return &module;
    return nullptr;
}

}

std::string BuildReportJson(const RuntimePaths&, const SupportManifest& manifest, const ScanResult& scan) {
    const OsInfo os = QueryOsInfo();
    std::ostringstream json;
    json << "{\n  \"schemaVersion\": 1,\n  \"os\": {\"major\": " << os.major << ", \"minor\": " << os.minor << ", \"build\": " << os.build << "},\n  \"manifestSha256\": \"" << JsonEscape(manifest.sha256) << "\",\n  \"kernel\": " << IdentityJson(scan.kernel) << ",\n  \"modules\": [\n";
    for (size_t index = 0; index < scan.inspected.size(); ++index) {
        const ModuleFinding& finding = scan.inspected[index];
        const ModuleDefinition* policy = FindModulePolicy(manifest, finding.module.classId);
        json << "    {\"classId\":" << finding.module.classId << ",\"name\":\"" << JsonEscape(WideToUtf8(finding.module.name)) << "\",\"profileFound\":" << (finding.profileFound ? "true" : "false")
             << ",\"profileComplete\":" << (finding.profileComplete ? "true" : "false")
             << ",\"compatibilityRequired\":" << (policy && policy->compatibilityRequired ? "true" : "false")
             << ",\"collectionOnly\":" << (policy && policy->collectionOnly ? "true" : "false")
             << ",\"supportSource\":\"" << JsonEscape(policy ? policy->supportSource : "unknown") << "\",\"identity\":" << IdentityJson(finding.module.identity) << "}";
        if (index + 1 != scan.inspected.size()) json << ',';
        json << '\n';
    }
    json << "  ],\n  \"missingCount\": " << scan.missing.size() << ",\n  \"collectionCandidateCount\": " << scan.collectionCandidates.size() << ",\n  \"notes\": \"PDB files are intentionally not copied; use pdbName, pdbGuid and pdbAge to download exact symbols.\"\n}\n";
    return json.str();
}

std::string BuildReportText(const SupportManifest& manifest, const ScanResult& scan, bool chinese) {
    const OsInfo os = QueryOsInfo();
    std::ostringstream text;
    text << "Ksword Launcher compatibility report\n";
    text << "OS: Windows " << os.major << "." << os.minor << " Build " << os.build << " (amd64)\n";
    text << "Manifest SHA-256: " << manifest.sha256 << "\n";
    text << "Kernel: " << WideToUtf8(scan.kernel.fileName) << "\n";
    text << "  Path: " << WideToUtf8(scan.kernel.path) << "\n";
    text << "  PDB: " << scan.kernel.pdbName << "\n";
    text << "  GUID: " << scan.kernel.pdbGuid << "\n";
    text << "  Age: " << scan.kernel.pdbAge << "\n\n";
    text << "Modules inspected: " << scan.inspected.size() << "\n";
    text << "Compatibility-required modules missing or incomplete: " << scan.missing.size() << "\n";
    text << "Loaded modules selected for collection: " << scan.collectionCandidates.size() << "\n";
    for (const ModuleFinding& finding : scan.collectionCandidates) {
        text << "- " << WideToUtf8(finding.module.name) << " class=" << finding.module.classId << " profile=" << (finding.profileFound ? "incomplete" : "not published") << "\n";
        text << "  path=" << WideToUtf8(finding.module.path) << "\n";
        text << "  machine=" << finding.module.identity.machine << " timestamp=" << finding.module.identity.timeDateStamp << " imageSize=" << finding.module.identity.sizeOfImage << "\n";
        text << "  pdbName=" << finding.module.identity.pdbName << " pdbGuid=" << finding.module.identity.pdbGuid << " pdbAge=" << finding.module.identity.pdbAge
             << " pdbSymbolKey=" << UpperAscii(finding.module.identity.pdbGuid) << finding.module.identity.pdbAge << "\n";
    }
    text << "\nPDB files are intentionally not included. Download symbols with the PDB name/GUID/Age above.\n";
    text << (chinese ? "界面语言：中文\n" : "UI language: English\n");
    return text.str();
}

bool PrepareUploadBundle(const RuntimePaths& paths, const SupportManifest& manifest, const ScanResult& scan, std::wstring* bundlePath) {
    if (!bundlePath) return false;
    const std::wstring root = BundleRoot();
    if (root.empty()) return false;
    // 只删除固定的临时目录，避免把用户其它临时文件误删。
    const DWORD existingAttributes = GetFileAttributesW(root.c_str());
    if (existingAttributes != INVALID_FILE_ATTRIBUTES) {
        if (existingAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!DeleteTree(root)) return false;
        } else if (!DeleteFileW(root.c_str())) {
            return false;
        }
    }
    if (!EnsureDirectory(root)) return false;
    std::vector<std::wstring> copied;
    std::string log;
    const std::wstring readme = JoinPath(root, L"README.txt");
    const std::string readmeText = "Ksword Launcher compatibility collection\n\nCompress this folder and send it to the Ksword developer.\nThe .bin files are renamed copies of kernel modules; they are not executable replacements.\nPDB files are not included. report.json/report.txt contain PDB Name/GUID/Age/SymbolKey for symbol download.\n";
    WriteTextFile(readme, readmeText);
    WriteTextFile(JoinPath(root, L"report.json"), BuildReportJson(paths, manifest, scan));
    WriteTextFile(JoinPath(root, L"report.txt"), BuildReportText(manifest, scan, false));

    std::vector<std::wstring> sourcePaths;
    sourcePaths.push_back(scan.kernel.path);
    for (const ModuleFinding& finding : scan.collectionCandidates) if (finding.module.classId != 0) sourcePaths.push_back(finding.module.path);
    std::sort(sourcePaths.begin(), sourcePaths.end());
    sourcePaths.erase(std::unique(sourcePaths.begin(), sourcePaths.end()), sourcePaths.end());
    for (const std::wstring& source : sourcePaths) {
        if (source.empty()) continue;
        const std::wstring name = FileNameOnly(source) + L".bin";
        CopyAttachment(source, JoinPath(root, name), &copied, &log);
    }
    std::ostringstream sums;
    for (const std::wstring& file : copied) sums << Sha256File(file) << "  " << WideToUtf8(FileNameOnly(file)) << "\n";
    WriteTextFile(JoinPath(root, L"SHA256SUMS.txt"), sums.str());
    if (!log.empty()) WriteTextFile(JoinPath(root, L"launcher.log"), log);
    else WriteTextFile(JoinPath(root, L"launcher.log"), "Collection completed without copy errors.\n");
    *bundlePath = root;
    return true;
}

void OpenBundleFolder(const std::wstring& path) {
    ShellExecuteW(nullptr, L"open", L"explorer.exe", path.c_str(), nullptr, SW_SHOWNORMAL);
}

}
