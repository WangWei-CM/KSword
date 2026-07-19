#include "Launcher.h"

#include <sstream>

namespace launcher {

bool LoadMarker(const RuntimePaths& paths, MarkerState* marker) {
    if (!marker) return false;
    *marker = MarkerState();
    std::string text;
    if (!ReadTextFile(paths.markerPath, &text)) return false;
    JsonValue root;
    std::string error;
    if (!ParseJson(text, &root, &error) || !root.isObject()) return false;
    marker->manifestSha256 = root.stringOr("manifestSha256", "");
    marker->kernelKey = root.stringOr("kernelKey", "");
    marker->useLight = root.booleanOr("useLight", false);
    marker->valid = !marker->manifestSha256.empty() && !marker->kernelKey.empty();
    return marker->valid;
}

bool WriteMarker(const RuntimePaths& paths, const MarkerState& marker, bool* accessDenied) {
    if (accessDenied) *accessDenied = false;
    if (!EnsureDirectory(paths.configDirectory)) {
        if (accessDenied && (GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_PRIVILEGE_NOT_HELD)) *accessDenied = true;
        return false;
    }
    std::ostringstream json;
    json << "{\n"
         << "  \"schemaVersion\": 1,\n"
         << "  \"manifestSha256\": \"" << JsonEscape(marker.manifestSha256) << "\",\n"
         << "  \"kernelKey\": \"" << JsonEscape(marker.kernelKey) << "\",\n"
         << "  \"useLight\": " << (marker.useLight ? "true" : "false") << "\n"
         << "}\n";
    const std::wstring temporary = paths.markerPath + L".tmp";
    if (!WriteTextFile(temporary, json.str())) {
        if (accessDenied && (GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_PRIVILEGE_NOT_HELD)) *accessDenied = true;
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), paths.markerPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileW(temporary.c_str());
        if (accessDenied && (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD)) *accessDenied = true;
        return false;
    }
    return true;
}

}
