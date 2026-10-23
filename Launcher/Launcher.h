#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "Json.h"

#include <windows.h>
#include <string>
#include <vector>

namespace launcher {

struct OsInfo {
    DWORD major = 0;
    DWORD minor = 0;
    DWORD build = 0;
    bool isWindows10OrLater() const { return major >= 10; }
    bool isEarlyQtUnsupported() const { return major == 10 && build < 17763; }
};

struct PeIdentity {
    std::wstring fileName;
    std::wstring path;
    DWORD machine = 0;
    DWORD timeDateStamp = 0;
    DWORD sizeOfImage = 0;
    std::string pdbName;
    std::string pdbGuid;
    DWORD pdbAge = 0;
    bool valid = false;
    std::string error;
};

struct ModuleDefinition {
    int classId = -1;
    std::string className;
    std::vector<std::wstring> fileNames;
    bool alwaysCollect = false;
    bool compatibilityRequired = false;
    bool collectionOnly = false;
    std::string supportSource;
    int publishedProfileCount = 0;
    int completeProfileCount = 0;
    std::string coverageStatus;
};

struct SupportProfile {
    int classId = -1;
    DWORD machine = 0;
    DWORD timeDateStamp = 0;
    DWORD sizeOfImage = 0;
    std::string pdbName;
    std::string pdbGuid;
    DWORD pdbAge = 0;
    bool complete = false;
    double coveragePercent = 0.0;
    std::string profileName;
};

struct SupportManifest {
    int schemaVersion = 0;
    std::string generatedUtc;
    std::string product;
    DWORD minimumWindowsMajor = 10;
    DWORD qtMinimumBuild = 17763;
    DWORD advertisedMaximumBuild = 0; // OS build 上限防护已停用。
    bool allowNewerWindows11 = true;
    std::vector<ModuleDefinition> modules;
    std::vector<SupportProfile> profiles;
    std::string sha256;
    bool valid = false;
    std::string error;
};

struct LoadedModule {
    std::wstring name;
    std::wstring path;
    PeIdentity identity;
    int classId = -1;
};

struct ModuleFinding {
    LoadedModule module;
    bool profileFound = false;
    bool profileComplete = false;
    const SupportProfile* profile = nullptr;
};

struct ScanResult {
    std::vector<ModuleFinding> inspected;
    std::vector<ModuleFinding> collectionCandidates;
    std::vector<ModuleFinding> missing;
    PeIdentity kernel;
    std::string error;
};

struct LauncherOptions {
    bool checkOnly = false;
    bool targetOverride = false;
    bool useLight = false;
    bool internalUpload = false;
    bool internalMarker = false;
    bool crashReport = false;
    bool crashDumpWritten = false;
    bool crashRepeat = false;
    DWORD crashProcessId = 0;
    DWORD crashExceptionCode = 0;
    unsigned long long crashExceptionAddress = 0;
    std::wstring crashDumpPath;
    std::wstring crashReadyEventName;
    std::vector<std::wstring> forwardedArguments;
};

struct MarkerState {
    std::string manifestSha256;
    std::string kernelKey;
    bool useLight = false;
    bool valid = false;
};

struct RuntimePaths {
    std::wstring launcherDirectory;
    std::wstring configDirectory;
    std::wstring manifestPath;
    std::wstring mainPath;
    std::wstring lightPath;
    std::wstring markerPath;
};

struct CollectionProgressState {
    std::mutex mutex;
    int percent = 0;
    std::wstring text;
    std::uint64_t revision = 0;
};

struct CollectionProgress {
    HWND window = nullptr;
    HWND label = nullptr;
    HWND bar = nullptr;
    // The worker receives only this channel; HWNDs remain on the owner thread.
    std::shared_ptr<CollectionProgressState> state;
    std::uint64_t appliedRevision = 0;
};

bool IsChineseUi();
std::wstring Utf8ToWide(const std::string& text);
std::string WideToUtf8(const std::wstring& text);
std::wstring LowerWide(std::wstring value);
std::string UpperAscii(std::string value);
std::wstring JoinPath(const std::wstring& parent, const std::wstring& child);
bool FileExists(const std::wstring& path);
bool EnsureDirectory(const std::wstring& path);
bool ReadFileBytes(const std::wstring& path, std::vector<BYTE>* bytes);
bool WriteTextFile(const std::wstring& path, const std::string& text);
bool ReadTextFile(const std::wstring& path, std::string* text);
std::string Sha256Bytes(const BYTE* data, size_t size);
std::string Sha256File(const std::wstring& path);
std::string FormatWin32Error(DWORD error = GetLastError());
std::string Hex32(DWORD value);

OsInfo QueryOsInfo();
RuntimePaths ResolveRuntimePaths();
bool LoadSupportManifest(const RuntimePaths& paths, SupportManifest* manifest);
std::string KernelIdentityKey(const PeIdentity& identity);
bool QueryKernelModules(std::vector<LoadedModule>* modules);
bool ProbePeIdentity(const std::wstring& path, PeIdentity* identity);
ScanResult ScanCompatibility(const SupportManifest& manifest);

bool LoadMarker(const RuntimePaths& paths, MarkerState* marker);
bool WriteMarker(const RuntimePaths& paths, const MarkerState& marker, bool* accessDenied);

int ShowUnsupportedOsDialog(const OsInfo& os, bool chinese);
int ShowUnsupportedBuildDialog(const OsInfo& os, DWORD maximumBuild, bool chinese);
int ShowEarlyWindowsChoiceDialog(bool chinese);
int ShowMissingDataDialog(bool chinese);
int ShowUploadElevationFailureDialog(bool chinese);
void ShowSimpleMessage(const std::wstring& title, const std::wstring& body, bool chinese);
void ShowCheckingWindow(const std::wstring& text, HWND* window);
void CloseCheckingWindow(HWND window);
void ShowCollectionProgress(bool chinese, CollectionProgress* progress);
void UpdateCollectionProgress(CollectionProgress* progress, int percent, const std::wstring& text);
void CloseCollectionProgress(CollectionProgress* progress);
void PresentCollectionProgress(CollectionProgress* progress);
bool IsLauncherObserverAlive(HWND window, const void* owner);
// Accepted value work completes while the caller continues dispatching messages.
// Closing the observer never cancels an accepted collection filesystem transaction.
bool RunLauncherValueTask(std::function<void()> work,
    const std::function<void()>& present, const std::function<bool()>& observerAlive);

bool RelaunchElevated(const LauncherOptions& options, bool forUpload);
bool LaunchTarget(const RuntimePaths& paths, const LauncherOptions& options);
int HandleCrashReportMode(const RuntimePaths& paths, const LauncherOptions& options, bool chinese);
bool PrepareUploadBundle(const RuntimePaths& paths, const SupportManifest& manifest, const ScanResult& scan, std::wstring* bundlePath, CollectionProgress* progress, bool chinese);
void OpenBundleFolder(const std::wstring& path);
std::string BuildReportJson(const RuntimePaths& paths, const SupportManifest& manifest, const ScanResult& scan);
std::string BuildReportText(const SupportManifest& manifest, const ScanResult& scan, bool chinese);

}
