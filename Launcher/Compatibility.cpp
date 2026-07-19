#include "Launcher.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <winternl.h>

namespace launcher {

namespace {

// RSDS_DEBUG_DIRECTORY 保存 PDB 文件名、GUID 和 Age，是支持清单的稳定身份来源。
#pragma pack(push, 1)
struct RsdsRecord {
    DWORD signature;
    GUID guid;
    DWORD age;
    char path[1];
};
#pragma pack(pop)

struct SystemModuleEntry {
    HANDLE section;
    PVOID mappedBase;
    PVOID imageBase;
    ULONG imageSize;
    ULONG flags;
    USHORT loadOrderIndex;
    USHORT initOrderIndex;
    USHORT loadCount;
    USHORT offsetToFileName;
    CHAR fullPathName[256];
};

struct SystemModuleInformationBuffer {
    ULONG numberOfModules;
    SystemModuleEntry modules[1];
};

static_assert(offsetof(SystemModuleInformationBuffer, modules) == sizeof(PVOID), "SystemModuleInformation x64 alignment mismatch");

using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

std::wstring NormalizeKernelPath(std::wstring path) {
    if (path.rfind(L"\\SystemRoot\\", 0) == 0 || path.rfind(L"\\systemroot\\", 0) == 0) {
        wchar_t windowsDirectory[MAX_PATH] = {};
        GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
        return JoinPath(windowsDirectory, path.substr(12));
    }
    if (path.rfind(L"\\??\\", 0) == 0) return path.substr(4);
    if (path.rfind(L"\\DosDevices\\", 0) == 0) return path.substr(12);
    return path;
}

std::wstring Basename(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

DWORD RvaToFileOffset(const BYTE* image, size_t imageSize, DWORD rva, const IMAGE_NT_HEADERS64* headers) {
    (void)image;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(headers);
    for (WORD index = 0; index < headers->FileHeader.NumberOfSections; ++index, ++section) {
        const DWORD sectionSize = (std::max)(section->Misc.VirtualSize, section->SizeOfRawData);
        if (rva >= section->VirtualAddress && rva - section->VirtualAddress < sectionSize) {
            const ULONGLONG offset = static_cast<ULONGLONG>(section->PointerToRawData) + (rva - section->VirtualAddress);
            return offset < imageSize ? static_cast<DWORD>(offset) : 0;
        }
    }
    return rva < imageSize ? rva : 0;
}

int ModuleClassForName(const std::wstring& name, const SupportManifest* manifest) {
    if (!manifest) return -1;
    const std::wstring lower = LowerWide(Basename(name));
    for (const ModuleDefinition& module : manifest->modules) {
        for (const std::wstring& candidate : module.fileNames) if (lower == LowerWide(candidate)) return module.classId;
    }
    return -1;
}

const ModuleDefinition* DefinitionForClass(int classId, const SupportManifest& manifest) {
    for (const ModuleDefinition& module : manifest.modules) if (module.classId == classId) return &module;
    return nullptr;
}

bool SameIdentity(const PeIdentity& actual, const SupportProfile& profile) {
    std::string actualGuid = actual.pdbGuid;
    std::string profileGuid = profile.pdbGuid;
    actualGuid.erase(std::remove_if(actualGuid.begin(), actualGuid.end(), [](unsigned char ch) { return ch == '-' || ch == '{' || ch == '}' || std::isspace(ch); }), actualGuid.end());
    profileGuid.erase(std::remove_if(profileGuid.begin(), profileGuid.end(), [](unsigned char ch) { return ch == '-' || ch == '{' || ch == '}' || std::isspace(ch); }), profileGuid.end());
    return actual.valid && profile.machine == actual.machine && profile.timeDateStamp == actual.timeDateStamp &&
        profile.sizeOfImage == actual.sizeOfImage && profile.pdbAge == actual.pdbAge &&
        UpperAscii(profile.pdbName) == UpperAscii(actual.pdbName) && UpperAscii(profileGuid) == UpperAscii(actualGuid);
}

}

bool ProbePeIdentity(const std::wstring& path, PeIdentity* identity) {
    if (!identity) return false;
    *identity = PeIdentity();
    identity->path = path;
    identity->fileName = Basename(path);
    std::vector<BYTE> bytes;
    if (!ReadFileBytes(path, &bytes) || bytes.size() < sizeof(IMAGE_DOS_HEADER)) { identity->error = "PE file could not be read"; return false; }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > bytes.size()) { identity->error = "invalid DOS/PE header"; return false; }
    const auto* signature = reinterpret_cast<const DWORD*>(bytes.data() + dos->e_lfanew);
    if (*signature != IMAGE_NT_SIGNATURE) { identity->error = "invalid PE signature"; return false; }
    const auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(signature + 1);
    if (fileHeader->SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64)) { identity->error = "not an amd64 PE image"; return false; }
    const auto* optional = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(fileHeader + 1);
    if (reinterpret_cast<const BYTE*>(optional) + sizeof(*optional) > bytes.data() + bytes.size() || optional->Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) { identity->error = "invalid amd64 optional header"; return false; }
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(signature);
    identity->machine = fileHeader->Machine;
    identity->timeDateStamp = fileHeader->TimeDateStamp;
    identity->sizeOfImage = optional->SizeOfImage;
    const IMAGE_DATA_DIRECTORY& debugDirectory = optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (debugDirectory.VirtualAddress == 0 || debugDirectory.Size < sizeof(IMAGE_DEBUG_DIRECTORY)) { identity->error = "CodeView debug directory is missing"; return false; }
    const DWORD debugOffset = RvaToFileOffset(bytes.data(), bytes.size(), debugDirectory.VirtualAddress, headers);
    if (debugOffset == 0 || debugOffset + debugDirectory.Size > bytes.size()) { identity->error = "debug directory is outside the image"; return false; }
    const size_t count = debugDirectory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(bytes.data() + debugOffset);
    for (size_t index = 0; index < count; ++index) {
        if (entries[index].Type != IMAGE_DEBUG_TYPE_CODEVIEW || entries[index].PointerToRawData == 0 || entries[index].SizeOfData < 24 || entries[index].PointerToRawData + entries[index].SizeOfData > bytes.size()) continue;
        const auto* record = reinterpret_cast<const RsdsRecord*>(bytes.data() + entries[index].PointerToRawData);
        if (record->signature != 'SDSR') continue;
        char guid[33] = {};
        sprintf_s(guid, "%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X", record->guid.Data1, record->guid.Data2, record->guid.Data3,
            record->guid.Data4[0], record->guid.Data4[1], record->guid.Data4[2], record->guid.Data4[3], record->guid.Data4[4], record->guid.Data4[5], record->guid.Data4[6], record->guid.Data4[7]);
        const size_t pathLength = strnlen_s(record->path, entries[index].SizeOfData - 24);
        std::string pdbPath(record->path, pathLength);
        const size_t separator = pdbPath.find_last_of("\\/");
        identity->pdbName = separator == std::string::npos ? pdbPath : pdbPath.substr(separator + 1);
        identity->pdbGuid = UpperAscii(guid);
        identity->pdbAge = record->age;
        identity->valid = !identity->pdbName.empty() && !identity->pdbGuid.empty();
        if (identity->valid) return true;
    }
    identity->error = "CodeView RSDS record is missing";
    return false;
}

bool QueryKernelModules(std::vector<LoadedModule>* modules) {
    if (!modules) return false;
    modules->clear();
    auto query = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!query) return false;
    ULONG size = 256 * 1024;
    std::vector<BYTE> buffer;
    constexpr NTSTATUS kInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
    NTSTATUS status = kInfoLengthMismatch;
    for (int attempt = 0; attempt < 5 && status == kInfoLengthMismatch; ++attempt) {
        buffer.resize(size);
        status = query(static_cast<SYSTEM_INFORMATION_CLASS>(11), buffer.data(), size, &size);
        if (status == kInfoLengthMismatch) size *= 2;
    }
    if (status < 0 || buffer.size() < sizeof(ULONG)) return false;
    const auto* info = reinterpret_cast<const SystemModuleInformationBuffer*>(buffer.data());
    const size_t moduleOffset = offsetof(SystemModuleInformationBuffer, modules);
    if (buffer.size() < moduleOffset) return false;
    const size_t available = (buffer.size() - moduleOffset) / sizeof(SystemModuleEntry);
    const ULONG count = std::min<ULONG>(info->numberOfModules, static_cast<ULONG>(available));
    for (ULONG index = 0; index < count; ++index) {
        const SystemModuleEntry& source = info->modules[index];
        std::string pathUtf8(source.fullPathName, strnlen_s(source.fullPathName, sizeof(source.fullPathName)));
        LoadedModule module;
        module.path = NormalizeKernelPath(Utf8ToWide(pathUtf8));
        module.name = Basename(module.path);
        if (module.path.empty() || module.name.empty()) continue;
        modules->push_back(std::move(module));
    }
    return !modules->empty();
}

std::string KernelIdentityKey(const PeIdentity& identity) {
    return Hex32(identity.machine) + ":" + Hex32(identity.timeDateStamp) + ":" + Hex32(identity.sizeOfImage) + ":" + UpperAscii(identity.pdbGuid) + ":" + std::to_string(identity.pdbAge) + ":" + UpperAscii(identity.pdbName);
}

ScanResult ScanCompatibility(const SupportManifest& manifest) {
    ScanResult result;
    std::vector<LoadedModule> modules;
    QueryKernelModules(&modules);
    const std::wstring systemRoot = [] {
        wchar_t buffer[MAX_PATH] = {};
        GetWindowsDirectoryW(buffer, MAX_PATH);
        return std::wstring(buffer);
    }();
    bool sawKernel = false;
    for (LoadedModule& module : modules) {
        module.classId = ModuleClassForName(module.name, &manifest);
        if (module.classId < 0) continue;
        module.identity.fileName = module.name;
        if (!ProbePeIdentity(module.path, &module.identity) && module.classId == 0) {
            module.path = JoinPath(systemRoot, L"System32\\ntoskrnl.exe");
            ProbePeIdentity(module.path, &module.identity);
        }
        if (module.classId == 0 && !sawKernel) { result.kernel = module.identity; sawKernel = true; }
        ModuleFinding finding;
        finding.module = std::move(module);
        for (const SupportProfile& profile : manifest.profiles) {
            if (profile.classId == finding.module.classId && SameIdentity(finding.module.identity, profile)) {
                finding.profileFound = true;
                finding.profileComplete = profile.complete;
                finding.profile = &profile;
                break;
            }
        }
        const ModuleDefinition* definition = DefinitionForClass(finding.module.classId, manifest);
        const bool profileGap = !finding.profileFound || !finding.profileComplete;
        result.inspected.push_back(finding);
        if (profileGap && definition && (definition->compatibilityRequired || definition->collectionOnly)) result.collectionCandidates.push_back(finding);
        if (profileGap && definition && definition->compatibilityRequired) result.missing.push_back(finding);
    }
    if (!sawKernel) {
        LoadedModule kernel;
        kernel.name = L"ntoskrnl.exe";
        kernel.path = JoinPath(systemRoot, L"System32\\ntoskrnl.exe");
        kernel.classId = 0;
        ProbePeIdentity(kernel.path, &kernel.identity);
        result.kernel = kernel.identity;
        ModuleFinding finding;
        finding.module = std::move(kernel);
        for (const SupportProfile& profile : manifest.profiles) if (SameIdentity(finding.module.identity, profile) && profile.classId == 0) { finding.profileFound = true; finding.profileComplete = profile.complete; finding.profile = &profile; break; }
        const ModuleDefinition* definition = DefinitionForClass(finding.module.classId, manifest);
        const bool profileGap = !finding.profileFound || !finding.profileComplete;
        result.inspected.push_back(finding);
        if (profileGap && definition && (definition->compatibilityRequired || definition->collectionOnly)) result.collectionCandidates.push_back(finding);
        if (profileGap && definition && definition->compatibilityRequired) result.missing.push_back(finding);
    }
    return result;
}

}
