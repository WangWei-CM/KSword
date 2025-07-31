

#include "ImGuiFileDialog.h"

#ifdef __cplusplus

#include <cstring>  // stricmp / strcasecmp
#include <cstdarg>  // variadic
#include <sstream>
#include <iomanip>
#include <ctime>
#include <memory>
#include <sys/stat.h>
#include <cstdio>
#include <cerrno>

// this option need c++17
#ifdef USE_STD_FILESYSTEM
#include <filesystem>
#include <exception>
#endif  // USE_STD_FILESYSTEM

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif  // __EMSCRIPTEN__

#ifdef _MSC_VER

#define IGFD_DEBUG_BREAK \
    if (IsDebuggerPresent()) __debugbreak()
#else
#define IGFD_DEBUG_BREAK
#endif

#if defined(__WIN32__) || \
    defined(WIN32) || \
    defined(_WIN32) || \
    defined(__WIN64__) || \
    defined(WIN64) || \
    defined(_WIN64) || \
    defined(_MSC_VER)
#define _IGFD_WIN_
#define stat _stati64
#define stricmp _stricmp
#include <cctype>
// this option need c++17
#ifdef USE_STD_FILESYSTEM
#include <windows.h>
#else                       // USE_STD_FILESYSTEM
#include "dirent/dirent.h"  // directly open the dirent file attached to this lib
#endif                      // USE_STD_FILESYSTEM
#define PATH_SEP '\\'
#ifndef PATH_MAX
#define PATH_MAX 260
#endif  // PATH_MAX
#elif defined(__linux__) || \
    defined(__FreeBSD__) || \
    defined(__DragonFly__) || \
    defined(__NetBSD__) || \
    defined(__OpenBSD__) || \
    defined(__APPLE__) ||\
    defined(__EMSCRIPTEN__)
#define _IGFD_UNIX_
#define stricmp strcasecmp
#include <sys/types.h>
// this option need c++17
#ifndef USE_STD_FILESYSTEM
#include <dirent.h>
#endif  // USE_STD_FILESYSTEM
#define PATH_SEP '/'
#endif  // _IGFD_UNIX_


#ifdef IMGUI_INTERNAL_INCLUDE
#include IMGUI_INTERNAL_INCLUDE
#else  // IMGUI_INTERNAL_INCLUDE
#include "../../imgui/imgui_internal.h"
#endif  // IMGUI_INTERNAL_INCLUDE

// legacy compatibility 1.89
#ifndef IM_TRUNC
#define IM_TRUNC IM_FLOOR
#endif

#include <cstdlib>
#include <algorithm>
#include <iostream>

///////////////////////////////
// STB IMAGE LIBS
///////////////////////////////

#ifdef USE_THUMBNAILS
#ifndef DONT_DEFINE_AGAIN__STB_IMAGE_IMPLEMENTATION
#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#endif  // STB_IMAGE_IMPLEMENTATION
#endif  // DONT_DEFINE_AGAIN__STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#ifndef DONT_DEFINE_AGAIN__STB_IMAGE_RESIZE_IMPLEMENTATION
#ifndef STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#endif  // STB_IMAGE_RESIZE_IMPLEMENTATION
#endif  // DONT_DEFINE_AGAIN__STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"
#endif  // USE_THUMBNAILS

///////////////////////////////
// FLOAT MACROS
///////////////////////////////

// float comparisons
#ifndef IS_FLOAT_DIFFERENT
#define IS_FLOAT_DIFFERENT(a, b) (fabs((a) - (b)) > FLT_EPSILON)
#endif  // IS_FLOAT_DIFFERENT
#ifndef IS_FLOAT_EQUAL
#define IS_FLOAT_EQUAL(a, b) (fabs((a) - (b)) < FLT_EPSILON)
#endif  // IS_FLOAT_EQUAL

///////////////////////////////
// COMBOBOX
///////////////////////////////

#ifndef FILTER_COMBO_AUTO_SIZE
#define FILTER_COMBO_AUTO_SIZE 1
#endif  // FILTER_COMBO_AUTO_SIZE
#ifndef FILTER_COMBO_MIN_WIDTH
#define FILTER_COMBO_MIN_WIDTH 150.0f
#endif  // FILTER_COMBO_MIN_WIDTH
#ifndef IMGUI_BEGIN_COMBO
#define IMGUI_BEGIN_COMBO ImGui::BeginCombo
#endif  // IMGUI_BEGIN_COMBO

///////////////////////////////
// BUTTON
///////////////////////////////

// for lets you define your button widget
// if you have like me a special bi-color button
#ifndef IMGUI_PATH_BUTTON
#define IMGUI_PATH_BUTTON ImGui::Button
#endif  // IMGUI_PATH_BUTTON
#ifndef IMGUI_BUTTON
#define IMGUI_BUTTON ImGui::Button
#endif  // IMGUI_BUTTON

///////////////////////////////
// locales
///////////////////////////////

#ifndef createDirButtonString
#define createDirButtonString "+"
#endif  // createDirButtonString
#ifndef okButtonString
#define okButtonString "OK"
#endif  // okButtonString
#ifndef okButtonWidth
#define okButtonWidth 0.0f
#endif  // okButtonWidth
#ifndef cancelButtonString
#define cancelButtonString "Cancel"
#endif  // cancelButtonString
#ifndef cancelButtonWidth
#define cancelButtonWidth 0.0f
#endif  // cancelButtonWidth
#ifndef okCancelButtonAlignement
#define okCancelButtonAlignement 0.0f
#endif  // okCancelButtonAlignement
#ifndef invertOkAndCancelButtons
// 0 => disabled, 1 => enabled
#define invertOkAndCancelButtons 0
#endif  // invertOkAndCancelButtons
#ifndef resetButtonString
#define resetButtonString "R"
#endif  // resetButtonString
#ifndef devicesButtonString
#define devicesButtonString "Devices"
#endif  // devicesButtonString
#ifndef editPathButtonString
#define editPathButtonString "E"
#endif  // editPathButtonString
#ifndef searchString
#define searchString "Search :"
#endif  // searchString
#ifndef dirEntryString
#define dirEntryString "[Dir]"
#endif  // dirEntryString
#ifndef linkEntryString
#define linkEntryString "[Link]"
#endif  // linkEntryString
#ifndef fileEntryString
#define fileEntryString "[File]"
#endif  // fileEntryString
#ifndef fileNameString
#define fileNameString "File Name:"
#endif  // fileNameString
#ifndef dirNameString
#define dirNameString "Directory Path:"
#endif  // dirNameString
#ifndef buttonResetSearchString
#define buttonResetSearchString "Reset search"
#endif  // buttonResetSearchString
#ifndef buttonDriveString
#define buttonDriveString "Devices"
#endif  // buttonDriveString
#ifndef buttonEditPathString
#define buttonEditPathString "Edit path\nYou can also right click on path buttons"
#endif  // buttonEditPathString
#ifndef buttonResetPathString
#define buttonResetPathString "Reset to current directory"
#endif  // buttonResetPathString
#ifndef buttonCreateDirString
#define buttonCreateDirString "Create Directory"
#endif  // buttonCreateDirString
#ifndef tableHeaderAscendingIcon
#define tableHeaderAscendingIcon "A|"
#endif  // tableHeaderAscendingIcon
#ifndef tableHeaderDescendingIcon
#define tableHeaderDescendingIcon "D|"
#endif  // tableHeaderDescendingIcon
#ifndef tableHeaderFileNameString
#define tableHeaderFileNameString "File name"
#endif  // tableHeaderFileNameString
#ifndef tableHeaderFileTypeString
#define tableHeaderFileTypeString "Type"
#endif  // tableHeaderFileTypeString
#ifndef tableHeaderFileSizeString
#define tableHeaderFileSizeString "Size"
#endif  // tableHeaderFileSizeString
#ifndef tableHeaderFileDateString
#define tableHeaderFileDateString "Date"
#endif  // tableHeaderFileDateString
#ifndef fileSizeBytes
#define fileSizeBytes "o"
#endif  // fileSizeBytes
#ifndef fileSizeKiloBytes
#define fileSizeKiloBytes "Ko"
#endif  // fileSizeKiloBytes
#ifndef fileSizeMegaBytes
#define fileSizeMegaBytes "Mo"
#endif  // fileSizeMegaBytes
#ifndef fileSizeGigaBytes
#define fileSizeGigaBytes "Go"
#endif  // fileSizeGigaBytes
#ifndef OverWriteDialogTitleString
#define OverWriteDialogTitleString "The selected file already exists!"
#endif  // OverWriteDialogTitleString
#ifndef OverWriteDialogMessageString
#define OverWriteDialogMessageString "Are you sure you want to overwrite it?"
#endif  // OverWriteDialogMessageString
#ifndef OverWriteDialogConfirmButtonString
#define OverWriteDialogConfirmButtonString "Confirm"
#endif  // OverWriteDialogConfirmButtonString
#ifndef OverWriteDialogCancelButtonString
#define OverWriteDialogCancelButtonString "Cancel"
#endif  // OverWriteDialogCancelButtonString
#ifndef DateTimeFormat
// see strftime functionin <ctime> for customize
#define DateTimeFormat "%Y/%m/%d %H:%M"
#endif  // DateTimeFormat

///////////////////////////////
//// SHORTCUTS => ctrl + KEY
///////////////////////////////

#ifndef SelectAllFilesKey
#define SelectAllFilesKey ImGuiKey_A
#endif  // SelectAllFilesKey

///////////////////////////////
// THUMBNAILS
///////////////////////////////

#ifdef USE_THUMBNAILS
#ifndef tableHeaderFileThumbnailsString
#define tableHeaderFileThumbnailsString "Thumbnails"
#endif  // tableHeaderFileThumbnailsString
#ifndef DisplayMode_FilesList_ButtonString
#define DisplayMode_FilesList_ButtonString "FL"
#endif  // DisplayMode_FilesList_ButtonString
#ifndef DisplayMode_FilesList_ButtonHelp
#define DisplayMode_FilesList_ButtonHelp "File List"
#endif  // DisplayMode_FilesList_ButtonHelp
#ifndef DisplayMode_ThumbailsList_ButtonString
#define DisplayMode_ThumbailsList_ButtonString "TL"
#endif  // DisplayMode_ThumbailsList_ButtonString
#ifndef DisplayMode_ThumbailsList_ButtonHelp
#define DisplayMode_ThumbailsList_ButtonHelp "Thumbnails List"
#endif  // DisplayMode_ThumbailsList_ButtonHelp
#ifndef DisplayMode_ThumbailsGrid_ButtonString
#define DisplayMode_ThumbailsGrid_ButtonString "TG"
#endif  // DisplayMode_ThumbailsGrid_ButtonString
#ifndef DisplayMode_ThumbailsGrid_ButtonHelp
#define DisplayMode_ThumbailsGrid_ButtonHelp "Thumbnails Grid"
#endif  // DisplayMode_ThumbailsGrid_ButtonHelp
#ifndef DisplayMode_ThumbailsList_ImageHeight
#define DisplayMode_ThumbailsList_ImageHeight 32.0f
#endif  // DisplayMode_ThumbailsList_ImageHeight
#ifndef IMGUI_RADIO_BUTTON
inline bool inRadioButton(const char* vLabel, bool vToggled) {
    bool pressed = false;
    if (vToggled) {
        ImVec4 bua = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImVec4 te  = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Button, te);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, te);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, te);
        ImGui::PushStyleColor(ImGuiCol_Text, bua);
    }
    pressed = IMGUI_BUTTON(vLabel);
    if (vToggled) {
        ImGui::PopStyleColor(4);  //-V112
    }
    return pressed;
}
#define IMGUI_RADIO_BUTTON inRadioButton
#endif  // IMGUI_RADIO_BUTTON
#endif  // USE_THUMBNAILS

///////////////////////////////
// PLACES
///////////////////////////////

#ifdef USE_PLACES_FEATURE
#ifndef defaultPlacePaneWith
#define defaultPlacePaneWith 150.0f
#endif  // defaultPlacePaneWith
#ifndef placesButtonString
#define placesButtonString "Places"
#endif  // placesButtonString
#ifndef placesButtonHelpString
#define placesButtonHelpString "Places"
#endif  // placesButtonHelpString
#ifndef placesBookmarksGroupName
#define placesBookmarksGroupName "Bookmarks"
#endif  // placesBookmarksGroupName
#ifndef PLACES_BOOKMARK_DEFAULT_OPEPEND
#define PLACES_BOOKMARK_DEFAULT_OPEPEND true
#endif  // PLACES_BOOKMARK_DEFAULT_OPEPEND
#ifndef PLACES_DEVICES_DEFAULT_OPEPEND
#define PLACES_DEVICES_DEFAULT_OPEPEND true
#endif  // PLACES_DEVICES_DEFAULT_OPEPEND
#ifndef placesBookmarksDisplayOrder
#define placesBookmarksDisplayOrder 0
#endif  // placesBookmarksDisplayOrder
#ifndef placesDevicesGroupName
#define placesDevicesGroupName "Devices"
#endif  // placesDevicesGroupName
#ifndef placesDevicesDisplayOrder
#define placesDevicesDisplayOrder 10
#endif  // placesDevicesDisplayOrder
#ifndef addPlaceButtonString
#define addPlaceButtonString "+"
#endif  // addPlaceButtonString
#ifndef removePlaceButtonString
#define removePlaceButtonString "-"
#endif  // removePlaceButtonString
#ifndef validatePlaceButtonString
#define validatePlaceButtonString "ok"
#endif  // validatePlaceButtonString
#ifndef editPlaceButtonString
#define editPlaceButtonString "E"
#endif  // editPlaceButtonString
#ifndef PLACES_PANE_DEFAULT_SHOWN
#define PLACES_PANE_DEFAULT_SHOWN false
#endif  // PLACES_PANE_DEFAULT_SHOWN
#ifndef IMGUI_TOGGLE_BUTTON
inline bool inToggleButton(const char* vLabel, bool* vToggled) {
    bool pressed = false;

    if (vToggled && *vToggled) {
        ImVec4 bua = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        // ImVec4 buh = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
        // ImVec4 bu = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        ImVec4 te = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Button, te);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, te);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, te);
        ImGui::PushStyleColor(ImGuiCol_Text, bua);
    }

    pressed = IMGUI_BUTTON(vLabel);

    if (vToggled && *vToggled) {
        ImGui::PopStyleColor(4);  //-V112
    }

    if (vToggled && pressed) *vToggled = !*vToggled;

    return pressed;
}
#define IMGUI_TOGGLE_BUTTON inToggleButton
#endif  // IMGUI_TOGGLE_BUTTON
#endif  // USE_PLACES_FEATURE

class IGFDException : public std::exception {
private:
    char const* m_msg{};

public:
    IGFDException() : std::exception() {
    }
    explicit IGFDException(char const* const vMsg)
        : std::exception(),  // std::exception(msg) is not availaiable on linux it seems... but on windos yes
          m_msg(vMsg) {
    }
    char const* what() const noexcept override {
        return m_msg;
    }
};

#ifndef CUSTOM_FILESYSTEM_INCLUDE
#ifdef USE_STD_FILESYSTEM

static std::filesystem::path stringToPath(const std::string& str) {
#ifdef _IGFD_WIN_
    return std::filesystem::path(IGFD::Utils::UTF8Decode(str));
#else
    return std::filesystem::path(str);
#endif
}

static std::string pathToString(const std::filesystem::path& path) {
#ifdef _IGFD_WIN_
    return IGFD::Utils::UTF8Encode(path.wstring());
#else
    return path.string();
#endif
}

class FileSystemStd : public IGFD::IFileSystem {
public:
    bool IsDirectoryCanBeOpened(const std::string& vName) override {
        bool bExists = false;
        if (!vName.empty()) {
            namespace fs  = std::filesystem;
            auto pathName = stringToPath(vName);
            try {
                // interesting, in the case of a protected dir or for any reason the dir cant be opened
                // this func will work but will say nothing more . not like the dirent version
                bExists = fs::is_directory(pathName);
                // test if can be opened, this function can thrown an exception if there is an issue with this dir
                // here, the dir_iter is need else not exception is thrown..
                const auto dir_iter = fs::directory_iterator(pathName);
                (void)dir_iter;  // for avoid unused warnings
            } catch (const std::exception& /*ex*/) {
                // fail so this dir cant be opened
                bExists = false;
            }
        }
        return bExists;  // this is not a directory!
    }
    bool IsDirectoryExist(const std::string& vName) override {
        if (!vName.empty()) {
            namespace fs = std::filesystem;
            return fs::is_directory(stringToPath(vName));
        }
        return false;  // this is not a directory!
    }
    bool IsFileExist(const std::string& vName) override {
        namespace fs = std::filesystem;
        return fs::is_regular_file(stringToPath(vName));
    }
    bool CreateDirectoryIfNotExist(const std::string& vName) override {
        if (vName.empty()) return false;
        if (IsDirectoryExist(vName)) return true;

#if defined(__EMSCRIPTEN__)
        std::string str = std::string("FS.mkdir('") + vName + "');";
        emscripten_run_script(str.c_str());
        bool res = true;
#else
        namespace fs = std::filesystem;
        bool res     = fs::create_directory(stringToPath(vName));
#endif  // _IGFD_WIN_
        if (!res) {
            std::cout << "Error creating directory " << vName << std::endl;
        }
        return res;
    }

    std::vector<IGFD::PathDisplayedName> GetDevicesList() override {
        std::vector<IGFD::PathDisplayedName> res;
#ifdef _IGFD_WIN_
        const DWORD mydevices = 2048;
        char lpBuffer[2048];
#define mini(a, b) (((a) < (b)) ? (a) : (b))
        const DWORD countChars = mini(GetLogicalDriveStringsA(mydevices, lpBuffer), 2047);
#undef mini
        if (countChars > 0U && countChars < 2049U) {
            std::string var = std::string(lpBuffer, (size_t)countChars);
            IGFD::Utils::ReplaceString(var, "\\", "");
            auto arr = IGFD::Utils::SplitStringToVector(var, '\0', false);
            wchar_t szVolumeName[2048];
            IGFD::PathDisplayedName path_name;
            for (auto& a : arr) {
                path_name.first = a;
                path_name.second.clear();
                std::wstring wpath = IGFD::Utils::UTF8Decode(a);
                if (GetVolumeInformationW(wpath.c_str(), szVolumeName, 2048, nullptr, nullptr, nullptr, nullptr, 0)) {
                    path_name.second = IGFD::Utils::UTF8Encode(szVolumeName);
                }
                res.push_back(path_name);
            }
        }
#endif  // _IGFD_WIN_
        return res;
    }

    IGFD::Utils::PathStruct ParsePathFileName(const std::string& vPathFileName) override {
        // https://github.com/aiekick/ImGuiFileDialog/issues/54
        namespace fs = std::filesystem;
        IGFD::Utils::PathStruct res;
        if (vPathFileName.empty()) return res;
        auto fsPath = stringToPath(vPathFileName);
        if (fs::is_directory(fsPath)) {
            res.name = "";
            res.path = pathToString(fsPath);
            res.isOk = true;
        } else if (fs::is_regular_file(fsPath)) {
            res.name = pathToString(fsPath.filename());
            res.path = pathToString(fsPath.parent_path());
            res.isOk = true;
        }
        return res;
    }

    std::vector<IGFD::FileInfos> ScanDirectory(const std::string& vPath) override {
        std::vector<IGFD::FileInfos> res;
        try {
            namespace fs          = std::filesystem;
            auto fspath           = stringToPath(vPath);
            const auto dir_iter   = fs::directory_iterator(fspath);
            IGFD::FileType fstype = IGFD::FileType(IGFD::FileType::ContentType::Directory, fs::is_symlink(fs::status(fspath)));
            {
                IGFD::FileInfos file_two_dot;
                file_two_dot.filePath    = vPath;
                file_two_dot.fileNameExt = "..";
                file_two_dot.fileType    = fstype;
                res.push_back(file_two_dot);
            }
            for (const auto& file : dir_iter) {
                try {
                    IGFD::FileType fileType;
                    if (file.is_symlink()) {
                        fileType.SetSymLink(file.is_symlink());
                        fileType.SetContent(IGFD::FileType::ContentType::LinkToUnknown);
                    }
                    if (file.is_directory()) {
                        fileType.SetContent(IGFD::FileType::ContentType::Directory);
                    }  // directory or symlink to directory
                    else if (file.is_regular_file()) {
                        fileType.SetContent(IGFD::FileType::ContentType::File);
                    }
                    if (fileType.isValid()) {
                        auto fileNameExt = pathToString(file.path().filename());
                        {
                            IGFD::FileInfos _file;
                            _file.filePath    = vPath;
                            _file.fileNameExt = fileNameExt;
                            _file.fileType    = fileType;
                            res.push_back(_file);
                        }
                    }
                } catch (const std::exception& ex) {
                    std::cout << "IGFD : " << ex.what() << std::endl;
                }
            }
        } catch (const std::exception& ex) {
            std::cout << "IGFD : " << ex.what() << std::endl;
        }
        return res;
    }
    bool IsDirectory(const std::string& vFilePathName) override {
        namespace fs = std::filesystem;
        return fs::is_directory(stringToPath(vFilePathName));
    }
};
#define FILE_SYSTEM_OVERRIDE FileSystemStd
#else
class FileSystemDirent : public IGFD::IFileSystem {
public:
    bool IsDirectoryCanBeOpened(const std::string& vName) override {
        if (!vName.empty()) {
            DIR* pDir = nullptr;
            // interesting, in the case of a protected dir or for any reason the dir cant be opened
            // this func will fail
            pDir = opendir(vName.c_str());
            if (pDir != nullptr) {
                (void)closedir(pDir);
                return true;
            }
        }
        return false;
    }
    bool IsDirectoryExist(const std::string& vName) override {
        bool bExists = false;
        if (!vName.empty()) {
            DIR* pDir = nullptr;
            pDir      = opendir(vName.c_str());
            if (pDir) {
                bExists = true;
                closedir(pDir);
            } else if (ENOENT == errno) {
                /* Directory does not exist. */
                // bExists = false;
            } else {
                /* opendir() failed for some other reason.
                   like if a dir is protected, or not accessable with user right
                */
                bExists = true;
            }
        }
        return bExists;
    }
    bool IsFileExist(const std::string& vName) override {
        std::ifstream docFile(vName, std::ios::in);
        if (docFile.is_open()) {
            docFile.close();
            return true;
        }
        return false;
    }
    bool CreateDirectoryIfNotExist(const std::string& vName) override {
        bool res = false;
        if (!vName.empty()) {
            if (!IsDirectoryExist(vName)) {
#ifdef _IGFD_WIN_
                std::wstring wname = IGFD::Utils::UTF8Decode(vName);
                if (CreateDirectoryW(wname.c_str(), nullptr)) {
                    res = true;
                }
#elif defined(__EMSCRIPTEN__)  // _IGFD_WIN_
                std::string str = std::string("FS.mkdir('") + vName + "');";
                emscripten_run_script(str.c_str());
                res = true;
#elif defined(_IGFD_UNIX_)
                char buffer[PATH_MAX] = {};
                snprintf(buffer, PATH_MAX, "mkdir -p \"%s\"", vName.c_str());
                const int dir_err = std::system(buffer);
                if (dir_err != -1) {
                    res = true;
                }
#endif  // _IGFD_WIN_
                if (!res) {
                    std::cout << "Error creating directory " << vName << std::endl;
                }
            }
        }

        return res;
    }

    std::vector<IGFD::PathDisplayedName> GetDevicesList() override {
        std::vector<IGFD::PathDisplayedName> res;
#ifdef _IGFD_WIN_
        const DWORD mydevices = 2048;
        char lpBuffer[2048];
#define mini(a, b) (((a) < (b)) ? (a) : (b))
        const DWORD countChars = mini(GetLogicalDriveStringsA(mydevices, lpBuffer), 2047);
#undef mini
        if (countChars > 0U && countChars < 2049U) {
            std::string var = std::string(lpBuffer, (size_t)countChars);
            IGFD::Utils::ReplaceString(var, "\\", "");
            auto arr = IGFD::Utils::SplitStringToVector(var, '\0', false);
            wchar_t szVolumeName[2048];
            IGFD::PathDisplayedName path_name;
            for (auto& a : arr) {
                path_name.first = a;
                path_name.second.clear();
                std::wstring wpath = IGFD::Utils::UTF8Decode(a);
                if (GetVolumeInformationW(wpath.c_str(), szVolumeName, 2048, nullptr, nullptr, nullptr, nullptr, 0)) {
                    path_name.second = IGFD::Utils::UTF8Encode(szVolumeName);
                }
                res.push_back(path_name);
            }
        }
#endif  // _IGFD_WIN_
        return res;
    }

    IGFD::Utils::PathStruct ParsePathFileName(const std::string& vPathFileName) override {
        IGFD::Utils::PathStruct res;
        if (!vPathFileName.empty()) {
            std::string pfn = vPathFileName;
            std::string separator(1u, PATH_SEP);
            IGFD::Utils::ReplaceString(pfn, "\\", separator);
            IGFD::Utils::ReplaceString(pfn, "/", separator);
            size_t lastSlash = pfn.find_last_of(separator);
            if (lastSlash != std::string::npos) {
                res.name = pfn.substr(lastSlash + 1);
                res.path = pfn.substr(0, lastSlash);
                res.isOk = true;
            }
            size_t lastPoint = pfn.find_last_of('.');
            if (lastPoint != std::string::npos) {
                if (!res.isOk) {
                    res.name = pfn;
                    res.isOk = true;
                }
                res.ext = pfn.substr(lastPoint + 1);
                IGFD::Utils::ReplaceString(res.name, "." + res.ext, "");
            }
            if (!res.isOk) {
                res.name = std::move(pfn);
                res.isOk = true;
            }
        }
        return res;
    }

    std::vector<IGFD::FileInfos> ScanDirectory(const std::string& vPath) override {
        std::vector<IGFD::FileInfos> res;
        struct dirent** files = nullptr;
        size_t n              = scandir(vPath.c_str(), &files, nullptr,                         //
                                        [](const struct dirent** a, const struct dirent** b) {  //
                               return strcoll((*a)->d_name, (*b)->d_name);
                           });
        if (n && files) {
            for (size_t i = 0; i < n; ++i) {
                struct dirent* ent = files[i];
                IGFD::FileType fileType;
                switch (ent->d_type) {
                    case DT_DIR: fileType.SetContent(IGFD::FileType::ContentType::Directory); break;
                    case DT_REG: fileType.SetContent(IGFD::FileType::ContentType::File); break;
#if defined(_IGFD_UNIX_) || (DT_LNK != DT_UNKNOWN)
                    case DT_LNK:
#endif
                    case DT_UNKNOWN: {
                        struct stat sb = {};
#ifdef _IGFD_WIN_
                        auto filePath = vPath + ent->d_name;
#else
                        auto filePath = vPath + IGFD::Utils::GetPathSeparator() + ent->d_name;
#endif
                        if (!stat(filePath.c_str(), &sb)) {
                            if (sb.st_mode & S_IFLNK) {
                                fileType.SetSymLink(true);
                                // by default if we can't figure out the target type.
                                fileType.SetContent(IGFD::FileType::ContentType::LinkToUnknown);
                            }
                            if (sb.st_mode & S_IFREG) {
                                fileType.SetContent(IGFD::FileType::ContentType::File);
                                break;
                            } else if (sb.st_mode & S_IFDIR) {
                                fileType.SetContent(IGFD::FileType::ContentType::Directory);
                                break;
                            }
                        }
                        break;
                    }
                    default: break;  // leave it invalid (devices, etc.)
                }
                if (fileType.isValid()) {
                    IGFD::FileInfos _file;
                    _file.filePath    = vPath;
                    _file.fileNameExt = ent->d_name;
                    _file.fileType    = fileType;
                    res.push_back(_file);
                }
            }
            for (size_t i = 0; i < n; ++i) {
                free(files[i]);
            }
            free(files);
        }
        return res;
    }
    bool IsDirectory(const std::string& vFilePathName) override {
        DIR* pDir = opendir(vFilePathName.c_str());
        if (pDir) {
            (void)closedir(pDir);
            return true;
        }
        return false;
    }
};
#define FILE_SYSTEM_OVERRIDE FileSystemDirent
#endif  // USE_STD_FILESYSTEM
#else
#include CUSTOM_FILESYSTEM_INCLUDE
#endif  // USE_CUSTOM_FILESYSTEM

// https://github.com/ocornut/imgui/issues/1720
bool IGFD::Utils::ImSplitter(bool split_vertically, float thickness, float* size1, float* size2, float min_size1, float min_size2, float splitter_long_axis_size) {
    auto* window = ImGui::GetCurrentWindow();
    ImGuiID id   = window->GetID("##Splitter");
    ImRect bb;
    bb.Min = window->DC.CursorPos + (split_vertically ? ImVec2(*size1, 0.0f) : ImVec2(0.0f, *size1));
    bb.Max = bb.Min + ImGui::CalcItemSize(split_vertically ? ImVec2(thickness, splitter_long_axis_size) : ImVec2(splitter_long_axis_size, thickness), 0.0f, 0.0f);
    return ImGui::SplitterBehavior(bb, id, split_vertically ? ImGuiAxis_X : ImGuiAxis_Y, size1, size2, min_size1, min_size2, 1.0f, 0.0, ImGui::GetColorU32(ImGuiCol_FrameBg));
}

// Convert a wide Unicode string to an UTF8 string
std::string IGFD::Utils::UTF8Encode(const std::wstring& wstr) {
    std::string res;
#ifdef _IGFD_WIN_
    if (!wstr.empty()) {
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), nullptr, 0, nullptr, nullptr);
        if (size_needed) {
            res = std::string(size_needed, 0);
            WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &res[0], size_needed, nullptr, nullptr);
        }
    }
#else
    // Suppress warnings from the compiler.
    (void)wstr;
#endif  // _IGFD_WIN_
    return res;
}

// Convert an UTF8 string to a wide Unicode String
std::wstring IGFD::Utils::UTF8Decode(const std::string& str) {
    std::wstring res;
#ifdef _IGFD_WIN_
    if (!str.empty()) {
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), nullptr, 0);
        if (size_needed) {
            res = std::wstring(size_needed, 0);
            MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &res[0], size_needed);
        }
    }
#else
    // Suppress warnings from the compiler.
    (void)str;
#endif  // _IGFD_WIN_
    return res;
}

bool IGFD::Utils::ReplaceString(std::string& str, const ::std::string& oldStr, const ::std::string& newStr, const size_t& vMaxRecursion) {
    if (!str.empty() && oldStr != newStr) {
        bool res             = false;
        size_t pos           = 0;
        bool found           = false;
        size_t max_recursion = vMaxRecursion;
        do {
            pos = str.find(oldStr, pos);
            if (pos != std::string::npos) {
                found = res = true;
                str.replace(pos, oldStr.length(), newStr);
                pos += newStr.length();
            } else if (found && max_recursion > 0) {  // recursion loop
                found = false;
                pos   = 0;
                --max_recursion;
            }
        } while (pos != std::string::npos);
        return res;
    }
    return false;
}

std::vector<std::string> IGFD::Utils::SplitStringToVector(const std::string& vText, const std::string& vDelimiterPattern, const bool vPushEmpty) {
    std::vector<std::string> arr;
    if (!vText.empty()) {
        size_t start = 0;
        size_t end   = vText.find(vDelimiterPattern, start);
        while (end != std::string::npos) {
            auto token = vText.substr(start, end - start);
            if (!token.empty() || (token.empty() && vPushEmpty)) {  //-V728
                arr.push_back(token);
            }
            start = end + vDelimiterPattern.size();
            end   = vText.find(vDelimiterPattern, start);
        }
        auto token = vText.substr(start);
        if (!token.empty() || (token.empty() && vPushEmpty)) {  //-V728
            arr.push_back(token);
        }
    }
    return arr;
}

std::vector<std::string> IGFD::Utils::SplitStringToVector(const std::string& vText, const char& vDelimiter, const bool vPushEmpty) {
    std::vector<std::string> arr;
    if (!vText.empty()) {
        size_t start = 0;
        size_t end   = vText.find(vDelimiter, start);
        while (end != std::string::npos) {
            auto token = vText.substr(start, end - start);
            if (!token.empty() || (token.empty() && vPushEmpty)) {  //-V728
                arr.push_back(token);
            }
            start = end + 1;
            end   = vText.find(vDelimiter, start);
        }
        auto token = vText.substr(start);
        if (!token.empty() || (token.empty() && vPushEmpty)) {  //-V728
            arr.push_back(token);
        }
    }
    return arr;
}

void IGFD::Utils::AppendToBuffer(char* vBuffer, size_t vBufferLen, const std::string& vStr) {
    std::string st = vStr;
    size_t len     = vBufferLen - 1u;
    size_t slen    = strlen(vBuffer);

    if (!st.empty() && st != "\n") {
        IGFD::Utils::ReplaceString(st, "\n", "");
        IGFD::Utils::ReplaceString(st, "\r", "");
    }
    vBuffer[slen]   = '\0';
    std::string str = std::string(vBuffer);
    // if (!str.empty()) str += "\n";
    str += vStr;
    if (len > str.size()) {
        len = str.size();
    }
#ifdef _MSC_VER
    strncpy_s(vBuffer, vBufferLen, str.c_str(), len);
#else   // _MSC_VER
    strncpy(vBuffer, str.c_str(), len);
#endif  // _MSC_VER
    vBuffer[len] = '\0';
}

void IGFD::Utils::ResetBuffer(char* vBuffer) {
    vBuffer[0] = '\0';
}

void IGFD::Utils::SetBuffer(char* vBuffer, size_t vBufferLen, const std::string& vStr) {
    ResetBuffer(vBuffer);
    AppendToBuffer(vBuffer, vBufferLen, vStr);
}

std::string IGFD::Utils::LowerCaseString(const std::string& vString) {
    auto str = vString;

    // convert to lower case
    for (char& c : str) {
        c = (char)std::tolower(c);
    }

    return str;
}

size_t IGFD::Utils::GetCharCountInString(const std::string& vString, const char& vChar) {
    size_t res = 0U;
    for (const auto& c : vString) {
        if (c == vChar) {
            ++res;
        }
    }
    return res;
}

size_t IGFD::Utils::GetLastCharPosWithMinCharCount(const std::string& vString, const char& vChar, const size_t& vMinCharCount) {
    if (vMinCharCount) {
        size_t last_dot_pos = vString.size() + 1U;
        size_t count_dots   = vMinCharCount;
        while (count_dots > 0U && last_dot_pos > 0U && last_dot_pos != std::string::npos) {
            auto new_dot = vString.rfind(vChar, last_dot_pos - 1U);
            if (new_dot != std::string::npos) {
                last_dot_pos = new_dot;
                --count_dots;
            } else {
                break;
            }
        }
        return last_dot_pos;
    }
    return std::string::npos;
}

std::string IGFD::Utils::GetPathSeparator() {
    return std::string(1U, PATH_SEP);
}

std::string IGFD::Utils::RoundNumber(double vvalue, int n) {
    std::stringstream tmp;
    tmp << std::setprecision(n) << std::fixed << vvalue;
    return tmp.str();
}

std::string IGFD::Utils::FormatFileSize(size_t vByteSize) {
    if (vByteSize != 0) {
        static double lo = 1024.0;
        static double ko = 1024.0 * 1024.0;
        static double mo = 1024.0 * 1024.0 * 1024.0;
        const auto v     = static_cast<double>(vByteSize);
        if (v < lo)
            return RoundNumber(v, 0) + " " + fileSizeBytes;  // octet
        else if (v < ko)
            return RoundNumber(v / lo, 2) + " " + fileSizeKiloBytes;  // ko
        else if (v < mo)
            return RoundNumber(v / ko, 2) + " " + fileSizeMegaBytes;  // Mo
        else
            return RoundNumber(v / mo, 2) + " " + fileSizeGigaBytes;  // Go
    }

    return "0 " fileSizeBytes;
}

// https://cplusplus.com/reference/cstdlib/strtod
bool IGFD::Utils::M_IsAValidCharExt(const char& c) {
    return c == '.' ||            // .5
           c == '-' || c == '+';  // -2.5 or +2.5;
}

// https://cplusplus.com/reference/cstdlib/strtod
bool IGFD::Utils::M_IsAValidCharSuffix(const char& c) {
    return c == 'e' || c == 'E' ||  // 1e5 or 1E5
           c == 'x' || c == 'X' ||  // 0x14 or 0X14
           c == 'p' || c == 'P';    // 6.2p2 or 3.2P-5
}

bool IGFD::Utils::M_ExtractNumFromStringAtPos(const std::string& str, size_t& pos, double& vOutNum) {
    if (!str.empty() && pos < str.size()) {
        const char fc = str.at(pos);  // first char
        // if the first char is not possible for a number we quit
        if (std::isdigit(fc) || M_IsAValidCharExt(fc)) {
            static constexpr size_t COUNT_CHAR = 64;
            char buf[COUNT_CHAR + 1];
            size_t buf_p        = 0;
            bool is_last_digit  = false;
            bool is_last_suffix = false;
            const auto& ss      = str.size();
            while (ss > 1 && pos < ss && buf_p < COUNT_CHAR) {
                const char& c = str.at(pos);
                // a suffix must be after a number
                if (is_last_digit && M_IsAValidCharSuffix(c)) {
                    is_last_suffix = true;
                    buf[buf_p++]   = c;
                } else if (std::isdigit(c)) {
                    is_last_suffix = false;
                    is_last_digit  = true;
                    buf[buf_p++]   = c;
                } else if (M_IsAValidCharExt(c)) {
                    is_last_digit = false;
                    buf[buf_p++]  = c;
                } else {
                    break;
                }
                ++pos;
            }
            // if the last char is a suffix so its not a number
            if (buf_p != 0 && !is_last_suffix) {
                buf[buf_p] = '\0';
                char* endPtr;
                vOutNum = strtod(buf, &endPtr);
                // the edge cases for numbers will be next filtered by strtod
                if (endPtr != buf) {
                    return true;
                }
            }
        }
    }
    return false;
}

// Fonction de comparaison naturelle entre deux cha�nes
bool IGFD::Utils::NaturalCompare(const std::string& vA, const std::string& vB, bool vInsensitiveCase, bool vDescending) {
    std::size_t ia = 0, ib = 0;
    double nA, nB;
    const auto& as = vA.size();
    const auto& bs = vB.size();
    while (ia < as && ib < bs) {
        const char& ca = vInsensitiveCase ? std::tolower(vA[ia]) : vA[ia];
        const char& cb = vInsensitiveCase ? std::tolower(vB[ib]) : vB[ib];
        // we cannot start a number extraction from suffixs
        const auto rA = M_ExtractNumFromStringAtPos(vA, ia, nA);
        const auto rB = M_ExtractNumFromStringAtPos(vB, ib, nB);
        if (rA && rB) {
            if (nA != nB) {
                return vDescending ? nA > nB : nA < nB;
            }
        } else {
            if (ca != cb) {
                return vDescending ? ca > cb : ca < cb;
            }
            ++ia;
            ++ib;
        }
    }
    return vDescending ? as > bs : as < bs;  // toto1 < toto1+
}

IGFD::FileStyle::FileStyle() : color(0, 0, 0, 0) {
}

IGFD::FileStyle::FileStyle(const FileStyle& vStyle) {
    color = vStyle.color;
    icon  = vStyle.icon;
    font  = vStyle.font;
    flags = vStyle.flags;
}

IGFD::FileStyle::FileStyle(const ImVec4& vColor, const std::string& vIcon, ImFont* vFont) : color(vColor), icon(vIcon), font(vFont) {
}

void IGFD::SearchManager::Clear() {
    searchTag.clear();
    IGFD::Utils::ResetBuffer(searchBuffer);
}

void IGFD::SearchManager::DrawSearchBar(FileDialogInternal& vFileDialogInternal) {
    // search field
    if (IMGUI_BUTTON(resetButtonString "##BtnImGuiFileDialogSearchField")) {
        Clear();
        vFileDialogInternal.fileManager.ApplyFilteringOnFileList(vFileDialogInternal);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(buttonResetSearchString);
    ImGui::SameLine();
    ImGui::Text(searchString);
    ImGui::SameLine();
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    bool edited = ImGui::InputText("##InputImGuiFileDialogSearchField", searchBuffer, MAX_FILE_DIALOG_NAME_BUFFER);
    if (ImGui::GetItemID() == ImGui::GetActiveID()) searchInputIsActive = true;
    ImGui::PopItemWidth();
    if (edited) {
        searchTag = searchBuffer;
        vFileDialogInternal.fileManager.ApplyFilteringOnFileList(vFileDialogInternal);
    }
}

void IGFD::FilterInfos::setCollectionTitle(const std::string& vTitle) {
    title = vTitle;
}

void IGFD::FilterInfos::addFilter(const std::string& vFilter, const bool vIsRegex) {
    setCollectionTitle(vFilter);
    addCollectionFilter(vFilter, vIsRegex);
}

void IGFD::FilterInfos::addCollectionFilter(const std::string& vFilter, const bool vIsRegex) {
    if (!vIsRegex) {
        auto _count_dots = Utils::GetCharCountInString(vFilter, '.');
        if (_count_dots > IGFD::FilterInfos::count_dots) {
            IGFD::FilterInfos::count_dots = _count_dots;
        }
        if (vFilter.find('*') != std::string::npos) {
            const auto& regex_string = transformAsteriskBasedFilterToRegex(vFilter);
            addCollectionFilter(regex_string, true);
            return;
        }
        filters.try_add(vFilter);
        filters_optimized.try_add(Utils::LowerCaseString(vFilter));
    } else {
        try {
            auto rx = std::regex(vFilter);
            filters.try_add(vFilter);
            filters_regex.emplace_back(rx);
        } catch (std::exception& e) {
            const std::string msg = "IGFD : The regex \"" + vFilter + "\" parsing was failed with msg : " + e.what();
            throw IGFDException(msg.c_str());
        }
    }
}

void IGFD::FilterInfos::clear() {
    title.clear();
    filters.clear();
    filters_optimized.clear();
    filters_regex.clear();
}

bool IGFD::FilterInfos::empty() const {
    return filters.empty() || filters.begin()->empty();
}

const std::string& IGFD::FilterInfos::getFirstFilter() const {
    if (!filters.empty()) {
        return *filters.begin();
    }
    return empty_string;
}

bool IGFD::FilterInfos::exist(const FileInfos& vFileInfos, bool vIsCaseInsensitive) const {
    for (const auto& filter : filters) {
        if (vFileInfos.SearchForExt(filter, vIsCaseInsensitive, count_dots)) {
            return true;
        }
    }
    return false;
}

bool IGFD::FilterInfos::regexExist(const std::string& vFilter) const {
    for (const auto& regex : filters_regex) {
        if (std::regex_search(vFilter, regex)) {
            return true;
        }
    }
    return false;
}

std::string IGFD::FilterInfos::transformAsteriskBasedFilterToRegex(const std::string& vFilter) {
    std::string res;
    if (!vFilter.empty() && vFilter.find('*') != std::string::npos) {
        res = "((";
        for (const auto& c : vFilter) {
            if (c == '.') {
                res += "[.]";  // [.] => a dot
            } else if (c == '*') {
                res += ".*";  // .* => any char zero or many
            } else {
                res += c;  // other chars
            }
        }
        res += "$))";  // $ => end fo the string
    }
    return res;
}

const IGFD::FilterInfos& IGFD::FilterManager::GetSelectedFilter() const {
    return m_SelectedFilter;
}

void IGFD::FilterManager::ParseFilters(const char* vFilters) {
    m_ParsedFilters.clear();

    if (vFilters) {
        dLGFilters = vFilters;  // file mode
    } else {
        dLGFilters.clear();  // directory mode
    }

    if (!dLGFilters.empty()) {
        /* Rules
        0) a filter must have 2 chars mini and the first must be a .
        1) a regex must be in (( and ))
        2) a , will separate filters except if between a ( and )
        3) name{filter1, filter2} is a spetial form for collection filters
        3.1) the name can be composed of what you want except { and }
        3.2) the filter can be a regex
        4) the filters cannot integrate these chars '(' ')' '{' '}' ' ' except for a regex with respect to rule 1)
        5) the filters cannot integrate a ','
        */

        bool current_filter_found = false;
        bool started              = false;
        bool regex_started        = false;
        bool parenthesis_started  = false;

        std::string word;
        std::string filter_name;

        char last_split_char = 0;
        for (char c : dLGFilters) {
            if (c == '{') {
                if (regex_started) {
                    word += c;
                } else {
                    started = true;
                    m_ParsedFilters.emplace_back();
                    m_ParsedFilters.back().setCollectionTitle(filter_name);
                    filter_name.clear();
                    word.clear();
                }
                last_split_char = c;
            } else if (c == '}') {
                if (regex_started) {
                    word += c;
                } else {
                    if (started) {
                        if (word.size() > 1U && word[0] == '.') {
                            if (m_ParsedFilters.empty()) {
                                m_ParsedFilters.emplace_back();
                            }
                            m_ParsedFilters.back().addCollectionFilter(word, false);
                        }
                        word.clear();
                        filter_name.clear();
                        started = false;
                    }
                }
                last_split_char = c;
            } else if (c == '(') {
                word += c;
                if (last_split_char == '(') {
                    regex_started = true;
                }
                parenthesis_started = true;
                if (!started) {
                    filter_name += c;
                }
                last_split_char = c;
            } else if (c == ')') {
                word += c;
                if (last_split_char == ')') {
                    if (regex_started) {
                        if (started) {
                            m_ParsedFilters.back().addCollectionFilter(word, true);
                        } else {
                            m_ParsedFilters.emplace_back();
                            m_ParsedFilters.back().addFilter(word, true);
                        }
                        word.clear();
                        filter_name.clear();
                        regex_started = false;
                    } else {
                        if (!started) {
                            if (!m_ParsedFilters.empty()) {
                                m_ParsedFilters.erase(m_ParsedFilters.begin() + m_ParsedFilters.size() - 1U);
                            } else {
                                m_ParsedFilters.clear();
                            }
                        }
                        word.clear();
                        filter_name.clear();
                    }
                }
                parenthesis_started = false;
                if (!started) {
                    filter_name += c;
                }
                last_split_char = c;
            } else if (c == '.') {
                word += c;
                if (!started) {
                    filter_name += c;
                }
                last_split_char = c;
            } else if (c == ',') {
                if (regex_started) {
                    regex_started = false;
                    word.clear();
                    filter_name.clear();
                } else {
                    if (started) {
                        if (word.size() > 1U && word[0] == '.') {
                            m_ParsedFilters.back().addCollectionFilter(word, false);
                            word.clear();
                            filter_name.clear();
                        }
                    } else {
                        if (word.size() > 1U && word[0] == '.') {
                            m_ParsedFilters.emplace_back();
                            m_ParsedFilters.back().addFilter(word, false);
                            word.clear();
                            filter_name.clear();
                        }
                        if (parenthesis_started) {
                            filter_name += c;
                        }
                    }
                }
            } else {
                if (c != ' ') {
                    word += c;
                }
                if (!started) {
                    filter_name += c;
                }
            }
        }

        if (started) {
            if (!m_ParsedFilters.empty()) {
                m_ParsedFilters.erase(m_ParsedFilters.begin() + m_ParsedFilters.size() - 1U);
            } else {
                m_ParsedFilters.clear();
            }
        } else if (word.size() > 1U && word[0] == '.') {
            m_ParsedFilters.emplace_back();
            m_ParsedFilters.back().addFilter(word, false);
            word.clear();
        }

        for (const auto& it : m_ParsedFilters) {
            if (it.title == m_SelectedFilter.title) {
                m_SelectedFilter     = it;
                current_filter_found = true;
                break;
            }
        }

        if (!current_filter_found) {
            if (!m_ParsedFilters.empty()) {
                m_SelectedFilter = *m_ParsedFilters.begin();
            }
        }
    }
}

void IGFD::FilterManager::SetSelectedFilterWithExt(const std::string& vFilter) {
    if (!m_ParsedFilters.empty()) {
        if (!vFilter.empty()) {
            for (const auto& infos : m_ParsedFilters) {
                for (const auto& filter : infos.filters) {
                    if (vFilter == filter) {
                        m_SelectedFilter = infos;
                    }
                }
            }
        }

        if (m_SelectedFilter.empty()) {
            m_SelectedFilter = *m_ParsedFilters.begin();
        }
    }
}

void IGFD::FilterManager::SetFileStyle(const IGFD_FileStyleFlags& vFlags, const char* vCriteria, const FileStyle& vInfos) {
    std::string _criteria                  = (vCriteria != nullptr) ? std::string(vCriteria) : "";
    m_FilesStyle[vFlags][_criteria]        = std::make_shared<FileStyle>(vInfos);
    m_FilesStyle[vFlags][_criteria]->flags = vFlags;
}

// will be called internally
// will not been exposed to IGFD API
bool IGFD::FilterManager::FillFileStyle(std::shared_ptr<FileInfos> vFileInfos) const {
    // todo : better system to found regarding what style to priorize regarding other
    // maybe with a lambda fucntion for let the user use his style
    // according to his use case
    if (vFileInfos.use_count() && !m_FilesStyle.empty()) {
        for (const auto& _flag : m_FilesStyle) {
            for (const auto& _file : _flag.second) {
                if ((_flag.first & IGFD_FileStyleByTypeDir && _flag.first & IGFD_FileStyleByTypeLink && vFileInfos->fileType.isDir() && vFileInfos->fileType.isSymLink()) ||
                    (_flag.first & IGFD_FileStyleByTypeFile && _flag.first & IGFD_FileStyleByTypeLink && vFileInfos->fileType.isFile() && vFileInfos->fileType.isSymLink()) ||
                    (_flag.first & IGFD_FileStyleByTypeLink && vFileInfos->fileType.isSymLink()) || (_flag.first & IGFD_FileStyleByTypeDir && vFileInfos->fileType.isDir()) ||
                    (_flag.first & IGFD_FileStyleByTypeFile && vFileInfos->fileType.isFile())) {
                    if (_file.first.empty()) {  // for all links
                        vFileInfos->fileStyle = _file.second;
                    } else if (_file.first.find("((") != std::string::npos && std::regex_search(vFileInfos->fileNameExt,
                                                                                                std::regex(_file.first))) {  // for links who are equal to style criteria
                        vFileInfos->fileStyle = _file.second;
                    } else if (_file.first == vFileInfos->fileNameExt) {  // for links who are equal to style criteria
                        vFileInfos->fileStyle = _file.second;
                    }
                }

                if (_flag.first & IGFD_FileStyleByExtention) {
                    if (_file.first.find("((") != std::string::npos && std::regex_search(vFileInfos->fileExtLevels[0], std::regex(_file.first))) {
                        vFileInfos->fileStyle = _file.second;
                    } else if (vFileInfos->SearchForExt(_file.first, false)) {
                        vFileInfos->fileStyle = _file.second;
                    }
                }

                if (_flag.first & IGFD_FileStyleByFullName) {
                    if (_file.first.find("((") != std::string::npos && std::regex_search(vFileInfos->fileNameExt, std::regex(_file.first))) {
                        vFileInfos->fileStyle = _file.second;
                    } else if (_file.first == vFileInfos->fileNameExt) {
                        vFileInfos->fileStyle = _file.second;
                    }
                }

                if (_flag.first & IGFD_FileStyleByContainedInFullName) {
                    if (_file.first.find("((") != std::string::npos && std::regex_search(vFileInfos->fileNameExt, std::regex(_file.first))) {
                        vFileInfos->fileStyle = _file.second;
                    } else if (vFileInfos->fileNameExt.find(_file.first) != std::string::npos) {
                        vFileInfos->fileStyle = _file.second;
                    }
                }

                for (auto& functor : m_FilesStyleFunctors) {
                    if (functor) {
                        FileStyle result;
                        if (functor(*(vFileInfos.get()), result)) {
                            vFileInfos->fileStyle = std::make_shared<FileStyle>(std::move(result));
                        }
                    }
                }

                if (vFileInfos->fileStyle.use_count()) {
                    return true;
                }
            }
        }
    }

    return false;
}

void IGFD::FilterManager::SetFileStyle(const IGFD_FileStyleFlags& vFlags, const char* vCriteria, const ImVec4& vColor, const std::string& vIcon, ImFont* vFont) {
    std::string _criteria;
    if (vCriteria) _criteria = std::string(vCriteria);
    m_FilesStyle[vFlags][_criteria]        = std::make_shared<FileStyle>(vColor, vIcon, vFont);
    m_FilesStyle[vFlags][_criteria]->flags = vFlags;
}

void IGFD::FilterManager::SetFileStyle(FileStyle::FileStyleFunctor vFunctor) {
    if (vFunctor) {
        m_FilesStyleFunctors.push_back(vFunctor);
    }
}

// todo : refactor this fucking function
bool IGFD::FilterManager::GetFileStyle(const IGFD_FileStyleFlags& vFlags, const std::string& vCriteria, ImVec4* vOutColor, std::string* vOutIcon, ImFont** vOutFont) {
    if (vOutColor) {
        if (!m_FilesStyle.empty()) {
            if (m_FilesStyle.find(vFlags) != m_FilesStyle.end()) {  // found
                if (vFlags & IGFD_FileStyleByContainedInFullName) {
                    // search for vCriteria who are containing the criteria
                    for (const auto& _file : m_FilesStyle.at(vFlags)) {
                        if (vCriteria.find(_file.first) != std::string::npos) {
                            if (_file.second.use_count()) {
                                *vOutColor = _file.second->color;
                                if (vOutIcon) *vOutIcon = _file.second->icon;
                                if (vOutFont) *vOutFont = _file.second->font;
                                return true;
                            }
                        }
                    }
                } else {
                    if (m_FilesStyle.at(vFlags).find(vCriteria) != m_FilesStyle.at(vFlags).end()) {  // found
                        *vOutColor = m_FilesStyle[vFlags][vCriteria]->color;
                        if (vOutIcon) *vOutIcon = m_FilesStyle[vFlags][vCriteria]->icon;
                        if (vOutFont) *vOutFont = m_FilesStyle[vFlags][vCriteria]->font;
                        return true;
                    }
                }
            } else {
                // search for flag composition
                for (const auto& _flag : m_FilesStyle) {
                    if (_flag.first & vFlags) {
                        if (_flag.first & IGFD_FileStyleByContainedInFullName) {
                            // search for vCriteria who are containing the criteria
                            for (const auto& _file : m_FilesStyle.at(_flag.first)) {
                                if (vCriteria.find(_file.first) != std::string::npos) {
                                    if (_file.second.use_count()) {
                                        *vOutColor = _file.second->color;
                                        if (vOutIcon) *vOutIcon = _file.second->icon;
                                        if (vOutFont) *vOutFont = _file.second->font;
                                        return true;
                                    }
                                }
                            }
                        } else {
                            if (m_FilesStyle.at(_flag.first).find(vCriteria) != m_FilesStyle.at(_flag.first).end()) {  // found
                                *vOutColor = m_FilesStyle[_flag.first][vCriteria]->color;
                                if (vOutIcon) *vOutIcon = m_FilesStyle[_flag.first][vCriteria]->icon;
                                if (vOutFont) *vOutFont = m_FilesStyle[_flag.first][vCriteria]->font;
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

void IGFD::FilterManager::ClearFilesStyle() {
    m_FilesStyle.clear();
}

bool IGFD::FilterManager::IsCoveredByFilters(const FileInfos& vFileInfos, bool vIsCaseInsensitive) const {
    if (!dLGFilters.empty() && !m_SelectedFilter.empty()) {
        return (m_SelectedFilter.exist(vFileInfos, vIsCaseInsensitive) || m_SelectedFilter.regexExist(vFileInfos.fileNameExt));
    }

    return false;
}

float IGFD::FilterManager::GetFilterComboBoxWidth() const {
#if FILTER_COMBO_AUTO_SIZE
    const auto& combo_width = ImGui::CalcTextSize(m_SelectedFilter.title.c_str()).x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    return ImMax(combo_width, FILTER_COMBO_MIN_WIDTH);
#else
    return FILTER_COMBO_MIN_WIDTH;
#endif
}

bool IGFD::FilterManager::DrawFilterComboBox(FileDialogInternal& vFileDialogInternal) {
    if (!dLGFilters.empty()) {
        ImGui::SameLine();
        bool needToApllyNewFilter = false;
        ImGui::PushItemWidth(GetFilterComboBoxWidth());
        if (IMGUI_BEGIN_COMBO("##Filters", m_SelectedFilter.title.c_str(), ImGuiComboFlags_None)) {
            intptr_t i = 0;
            for (const auto& filter : m_ParsedFilters) {
                const bool item_selected = (filter.title == m_SelectedFilter.title);
                ImGui::PushID((void*)(intptr_t)i++);
                if (ImGui::Selectable(filter.title.c_str(), item_selected)) {
                    m_SelectedFilter     = filter;
                    needToApllyNewFilter = true;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        if (needToApllyNewFilter) {
            vFileDialogInternal.fileManager.OpenCurrentPath(vFileDialogInternal);
        }
        return needToApllyNewFilter;
    }
    return false;
}

std::string IGFD::FilterManager::ReplaceExtentionWithCurrentFilterIfNeeded(const std::string& vFileName, IGFD_ResultMode vFlag) const {
    auto result = vFileName;
    if (!result.empty()) {
        const auto& current_filter = m_SelectedFilter.getFirstFilter();
        if (!current_filter.empty()) {
            Utils::ReplaceString(result, "..", ".");

            // is a regex => no change
            if (current_filter.find("((") != std::string::npos) {
                return result;
            }

            // contain .* => no change
            if (current_filter.find(".*") != std::string::npos) {
                return result;
            }

            switch (vFlag) {
                case IGFD_ResultMode_KeepInputFile: {
                    return vFileName;
                }
                case IGFD_ResultMode_OverwriteFileExt: {
                    const auto& count_dots = Utils::GetCharCountInString(vFileName, '.');
                    const auto& min_dots   = ImMin<size_t>(count_dots, m_SelectedFilter.count_dots);
                    const auto& lp         = Utils::GetLastCharPosWithMinCharCount(vFileName, '.', min_dots);
                    if (lp != std::string::npos) {  // there is a user extention
                        const auto& file_name_without_user_ext = vFileName.substr(0, lp);
                        result                                 = file_name_without_user_ext + current_filter;
                    } else {  // add extention
                        result = vFileName + current_filter;
                    }
                    break;
                }
                case IGFD_ResultMode_AddIfNoFileExt: {
                    const auto& count_dots = Utils::GetCharCountInString(vFileName, '.');
                    const auto& min_dots   = ImMin<size_t>(count_dots, m_SelectedFilter.count_dots);
                    const auto& lp         = Utils::GetLastCharPosWithMinCharCount(vFileName, '.', min_dots);
                    if (lp == std::string::npos ||        // there is no user extention
                        lp == (vFileName.size() - 1U)) {  // or this pos is also the last char => considered like no user extention
                        const auto& file_name_without_user_ext = vFileName.substr(0, lp);
                        result                                 = file_name_without_user_ext + current_filter;
                    }
                    break;
                }
                default: break;
            }

            Utils::ReplaceString(result, "..", ".");
        }
    }
    return result;
}

void IGFD::FilterManager::SetDefaultFilterIfNotDefined() {
    if (m_SelectedFilter.empty() &&                   // no filter selected
        !m_ParsedFilters.empty()) {                   // filter exist
        m_SelectedFilter = *m_ParsedFilters.begin();  // we take the first filter
    }
}

IGFD::FileType::FileType() = default;
IGFD::FileType::FileType(const ContentType& vContentType, const bool vIsSymlink) : m_Content(vContentType), m_Symlink(vIsSymlink) {
}
void IGFD::FileType::SetContent(const ContentType& vContentType) {
    m_Content = vContentType;
}
void IGFD::FileType::SetSymLink(const bool vIsSymlink) {
    m_Symlink = vIsSymlink;
}
bool IGFD::FileType::isValid() const {
    return m_Content != ContentType::Invalid;
}
bool IGFD::FileType::isDir() const {
    return m_Content == ContentType::Directory;
}
bool IGFD::FileType::isFile() const {
    return m_Content == ContentType::File;
}
bool IGFD::FileType::isLinkToUnknown() const {
    return m_Content == ContentType::LinkToUnknown;
}
bool IGFD::FileType::isSymLink() const {
    return m_Symlink;
}
// Comparisons only care about the content type, ignoring whether it's a symlink or not.
bool IGFD::FileType::operator==(const FileType& rhs) const {
    return m_Content == rhs.m_Content;
}
bool IGFD::FileType::operator!=(const FileType& rhs) const {
    return m_Content != rhs.m_Content;
}
bool IGFD::FileType::operator<(const FileType& rhs) const {
    return m_Content < rhs.m_Content;
}
bool IGFD::FileType::operator>(const FileType& rhs) const {
    return m_Content > rhs.m_Content;
}

std::shared_ptr<IGFD::FileInfos> IGFD::FileInfos::create() {
    return std::make_shared<IGFD::FileInfos>();
}

bool IGFD::FileInfos::SearchForTag(const std::string& vTag) const {
    if (!vTag.empty()) {
        if (fileNameExt_optimized == "..") return true;
        return fileNameExt_optimized.find(vTag) != std::string::npos ||  // first try without case and accents
               fileNameExt.find(vTag) != std::string::npos;              // second if searched with case and accents
    }

    // if tag is empty => its a special case but all is found
    return true;
}

bool IGFD::FileInfos::SearchForExt(const std::string& vExt, const bool vIsCaseInsensitive, const size_t& vMaxLevel) const {
    if (!vExt.empty()) {
        const auto& ext_to_check = vIsCaseInsensitive ? Utils::LowerCaseString(vExt) : vExt;
        const auto& ext_levels   = vIsCaseInsensitive ? fileExtLevels_optimized : fileExtLevels;
        if (vMaxLevel >= 1 && countExtDot >= vMaxLevel) {
            for (const auto& ext : ext_levels) {
                if (!ext.empty() && ext == ext_to_check) {
                    return true;
                }
            }
        } else {
            return (fileExtLevels[0] == vExt);
        }
    }
    return false;
}

bool IGFD::FileInfos::SearchForExts(const std::string& vComaSepExts, const bool vIsCaseInsensitive, const size_t& vMaxLevel) const {
    if (!vComaSepExts.empty()) {
        const auto& arr = Utils::SplitStringToVector(vComaSepExts, ',', false);
        for (const auto& a : arr) {
            if (SearchForExt(a, vIsCaseInsensitive, vMaxLevel)) {
                return true;
            }
        }
    }
    return false;
}

bool IGFD::FileInfos::FinalizeFileTypeParsing(const size_t& vMaxDotToExtract) {
    if (fileType.isFile() || fileType.isLinkToUnknown()) {  // link can have the same extention of a file
        countExtDot = Utils::GetCharCountInString(fileNameExt, '.');
        size_t lpt  = 0U;
        if (countExtDot > 1U) {  // multi layer ext
            size_t max_dot_to_extract = vMaxDotToExtract;
            if (max_dot_to_extract > countExtDot) {
                max_dot_to_extract = countExtDot;
            }
            lpt = Utils::GetLastCharPosWithMinCharCount(fileNameExt, '.', max_dot_to_extract);
        } else {
            lpt = fileNameExt.find_first_of('.');
        }
        if (lpt != std::string::npos) {
            size_t lvl                   = 0U;
            fileNameLevels[lvl]          = fileNameExt.substr(0, lpt);
            fileNameLevels[lvl]          = Utils::LowerCaseString(fileNameLevels[lvl]);
            fileExtLevels[lvl]           = fileNameExt.substr(lpt);
            fileExtLevels_optimized[lvl] = Utils::LowerCaseString(fileExtLevels[lvl]);
            if (countExtDot > 1U) {  // multi layer ext
                auto count = countExtDot;
                while (count > 0 && lpt != std::string::npos && lvl < fileExtLevels.size()) {
                    ++lpt;
                    ++lvl;
                    if (fileNameExt.size() > lpt) {
                        lpt = fileNameExt.find_first_of('.', lpt);
                        if (lpt != std::string::npos) {
                            fileNameLevels[lvl]          = fileNameExt.substr(0, lpt);
                            fileNameLevels[lvl]          = Utils::LowerCaseString(fileNameLevels[lvl]);
                            fileExtLevels[lvl]           = fileNameExt.substr(lpt);
                            fileExtLevels_optimized[lvl] = Utils::LowerCaseString(fileExtLevels[lvl]);
                        }
                    }
                }
            }
        }
        return true;
    }
    return false;
}

IGFD::FileManager::FileManager() {
    fsRoot = IGFD::Utils::GetPathSeparator();
#define STR(x) #x
#define STR_AFTER_EXPAND(x) STR(x)
    m_FileSystemName = STR_AFTER_EXPAND(FILE_SYSTEM_OVERRIDE);
#undef STR_AFTER_EXPAND
#undef STR
    // std::make_unique is not available un cpp11
    m_FileSystemPtr = std::unique_ptr<FILE_SYSTEM_OVERRIDE>(new FILE_SYSTEM_OVERRIDE());
    // m_FileSystemPtr = std::make_unique<FILE_SYSTEM_OVERRIDE>();
}

void IGFD::FileManager::OpenCurrentPath(const FileDialogInternal& vFileDialogInternal) {
    showDevices = false;
    ClearComposer();
    ClearFileLists();
    if (dLGDirectoryMode) {  // directory mode
        SetDefaultFileName(".");
    } else {
        SetDefaultFileName(dLGDefaultFileName);
    }
    ScanDir(vFileDialogInternal, GetCurrentPath());
}

void IGFD::FileManager::SortFields(const FileDialogInternal& vFileDialogInternal) {
    m_SortFields(vFileDialogInternal, m_FileList, m_FilteredFileList);
}

bool IGFD::FileManager::M_SortStrings(const FileDialogInternal& vFileDialogInternal, const bool vInsensitiveCase, const bool vDescendingOrder, const std::string& vA, const std::string& vB) {
    if (vFileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_NaturalSorting) {
        return IGFD::Utils::NaturalCompare(vA, vB, vInsensitiveCase, vDescendingOrder);
    } else if (vInsensitiveCase) {
        const auto ret = stricmp(vA.c_str(), vB.c_str());
        return vDescendingOrder ? (ret > 0) : (ret < 0);
    } else {
        const auto ret = strcmp(vA.c_str(), vB.c_str());
        return vDescendingOrder ? (ret > 0) : (ret < 0);
    }
}

void IGFD::FileManager::m_SortFields(const FileDialogInternal& vFileDialogInternal, std::vector<std::shared_ptr<FileInfos> >& vFileInfosList, std::vector<std::shared_ptr<FileInfos> >& vFileInfosFilteredList) {
    if (sortingField != SortingFieldEnum::FIELD_NONE) {
        headerFileName = tableHeaderFileNameString;
        headerFileType = tableHeaderFileTypeString;
        headerFileSize = tableHeaderFileSizeString;
        headerFileDate = tableHeaderFileDateString;
#ifdef USE_THUMBNAILS
        headerFileThumbnails = tableHeaderFileThumbnailsString;
#endif  // #ifdef USE_THUMBNAILS
    }
    if (sortingField == SortingFieldEnum::FIELD_FILENAME) {
        if (sortingDirection[0]) {
#ifdef USE_CUSTOM_SORTING_ICON
            headerFileName = tableHeaderAscendingIcon + headerFileName;
#endif                                                               // USE_CUSTOM_SORTING_ICON
            std::sort(vFileInfosList.begin(), vFileInfosList.end(),  //
                      [&vFileDialogInternal](const std::shared_ptr<FileInfos>& a, const std::shared_ptr<FileInfos>& b) -> bool {
                          if (!a.use_count() || !b.use_count()) return false;
                          if (a->fileType != b->fileType) return (a->fileType < b->fileType);                      // directories first
                          return M_SortStrings(vFileDialogInternal, true, false, a->fileNameExt, b->fileNameExt);  // sort in insensitive case
                      });
        } else {
#ifdef USE_CUSTOM_SORTING_ICON
            headerFileName = tableHeaderDescendingIcon + headerFileName;
#endif                                                               // USE_CUSTOM_SORTING_ICON
            std::sort(vFileInfosList.begin(), vFileInfosList.end(),  //
                      [&vFileDialogInternal](const std::shared_ptr<FileInfos>& a, const std::shared_ptr<FileInfos>& b) -> bool {
                          if (!a.use_count() || !b.use_count()) return false;
                          if (a->fileType != b->fileType) return (a->fileType > b->fileType);                     // directories last
                          return M_SortStrings(vFileDialogInternal, true, true, a->fileNameExt, b->fileNameExt);  // sort in insensitive case
                      });
        }
    } else if (sortingField == SortingFieldEnum::FIELD_TYPE) {
        if (sortingDirection[1]) {
#ifdef USE_CUSTOM_SORTING_ICON
            headerFileType = tableHeaderAscendingIcon + headerFileType;
#endif  // USE_CUSTOM_SORTING_ICON
            std::sort(vFileInfosList.begin(), vFileInfosList.end(), [&vFileDialogInternal](const std::shared_ptr<FileInfos>& a, const std::shared_ptr<FileInfos>& b) -> bool {
                if (!a.use_count() || !b.use_count()) return false;
                if (a->fileType != b->fileType) return (a->fileType < b->fileType);                                // directory in first
                return M_SortStrings(vFileDialogInternal, true, false, a->fileExtLevels[0], b->fileExtLevels[0]);  // sort in sensitive case
            });
        } else {
#ifdef USE_CUSTOM_SORTING_ICON
            headerFileType = tableHeaderDescendingIcon + headerFileType;
#endif  // USE_CUSTOM_SORTING_ICON
            std::sort(vFileInfosList.begin(), vFileInfosList.end(), [&vFileDialogInternal](const std::shared_ptr<FileInfos>& a, const std::shared_ptr<FileInfos>& b) -> bool {
                if (!a.use_count() || !b.use_count()) return false;
                if (a->fileType != b->fileType) return (a->fileType > b->fileType);                               // directory in last
                return M_SortStrings(vFileDialogInternal, true, true, a->fileExtLevels[0], b->fileExtLevels[0]);  // sort in sensitive case
            });
        }
    } else if (sortingField == SortingFieldEnum::FIELD_SIZE) {
        if (sortingDirection[2]) {
#ifdef USE_CUSTOM_SORTING_ICON
            headerFileSize = tableHeaderAscendingIcon + headerFileSize;
#endif  // USE_CUSTOM_SORTING_ICON
            std::sort(vFileInfosList.begin(), vFileInfosList.end(), [](const std::shared_ptr<FileInfos>& a, const std::shared_ptr<FileInfos>& b) -> bool {
                if (!a.use_count() || !b.use_count()) return false;
                if (a->fileType != b->fileType) return (a->fileType < b->fileType);  // directory in first
                return (a->fileSize < b->fileSize);                                  // else
            });
        } else {
#ifdef USE_CUSTOM_SORTING_ICON
            headerFileSize = tableHeaderDescendingIcon + headerFileSize;
#endif  // USE_CUSTOM_SORTING_ICON
            std::sort(vFileInfosList.begin(), vFileInfosList.end(), [](const std::shared_ptr<FileInfos>& a, const std::shared_ptr<FileInfos>& b) -> bool {
                if (!a.use_count() || !b.use_count()) return false;
                if (a->fileType != b->fileType) return (a->fileType > b->fileType);  // directory in last
                return (a->fileSize > b->fileSize);                                  // else
            });
        }
    } else if (sortingField == SortingFieldEnum::FIELD_DATE) {
        if (sortingDirection[3]) {
#ifdef USE_CUSTOM_SORTING_ICON
            headerFileDate = tableHeaderAscendingIcon + headerFileDate;
#endif  // USE_CUSTOM_SORTING_ICON
            std::sort(vFileInfosList.begin(), vFileInfosList.end(), [](const std::shared_ptr<FileInfos>& a, const std::shared_ptr<FileInfos>& b) -> bool {
                if (!a.use_count() || !b.use_count()) return false;
                if (a->fileType != b->fileType) return (a->fileType < b->fileType);  // directory in first
                return (a->fileModifDate < b->fileModifDate);                        // else
            });
        } else {
#ifdef USE_CUSTOM_SORTING_ICON
            headerFileDate = tableHeaderDescendingIcon + headerFileDate;
#endif  // USE_CUSTOM_SORTING_ICON
            std::sort(vFileInfosList.begin(), vFileInfosList.end(), [](const std::shared_ptr<FileInfos>& a, const std::shared_ptr<FileInfos>& b) -> bool {
                if (!a.use_count() || !b.use_count()) return false;
                if (a->fileType != b->fileType) return (a->fileType > b->fileType);  // directory in last
                return (a->fileModifDate > b->fileModifDate);                        // else
            });
        }
    }
#ifdef USE_THUMBNAILS
    else if (sortingField == SortingFieldEnum::FIELD_THUMBNAILS) {
        // we will compare thumbnails by :
        // 1) width
        // 2) height

        if (sortingDirection[4]) {
#ifdef USE_CUSTOM_SORTING_ICON
            headerFileThumbnails = tableHeaderAscendingIcon + headerFileThumbnails;
#endif  // USE_CUSTOM_SORTING_ICON
            std::sort(vFileInfosList.begin(), vFileInfosList.end(), [](const std::shared_ptr<FileInfos>& a, const std::shared_ptr<FileInfos>& b) -> bool {
                if (!a.use_count() || !b.use_count()) return false;
                if (a->fileType != b->fileType) return (a->fileType.isDir());  // directory in first
                if (a->thumbnailInfo.textureWidth == b->thumbnailInfo.textureWidth) return (a->thumbnailInfo.textureHeight < b->thumbnailInfo.textureHeight);
                return (a->thumbnailInfo.textureWidth < b->thumbnailInfo.textureWidth);
            });
        }

        else {
#ifdef USE_CUSTOM_SORTING_ICON
            headerFileThumbnails = tableHeaderDescendingIcon + headerFileThumbnails;
#endif  // USE_CUSTOM_SORTING_ICON
            std::sort(vFileInfosList.begin(), vFileInfosList.end(), [](const std::shared_ptr<FileInfos>& a, const std::shared_ptr<FileInfos>& b) -> bool {
                if (!a.use_count() || !b.use_count()) return false;
                if (a->fileType != b->fileType) return (!a->fileType.isDir());  // directory in last
                if (a->thumbnailInfo.textureWidth == b->thumbnailInfo.textureWidth) return (a->thumbnailInfo.textureHeight > b->thumbnailInfo.textureHeight);
                return (a->thumbnailInfo.textureWidth > b->thumbnailInfo.textureWidth);
            });
        }
    }
#endif  // USE_THUMBNAILS

    m_ApplyFilteringOnFileList(vFileDialogInternal, vFileInfosList, vFileInfosFilteredList);
}

bool IGFD::FileManager::m_CompleteFileInfosWithUserFileAttirbutes(const FileDialogInternal& vFileDialogInternal, const std::shared_ptr<FileInfos>& vInfos) {
    if (vFileDialogInternal.getDialogConfig().userFileAttributes != nullptr) {
        if (!vFileDialogInternal.getDialogConfig().userFileAttributes(vInfos.get(), vFileDialogInternal.getDialogConfig().userDatas)) {
            return false;  // the file will be ignored, so not added to the file list, so not displayed
        } else {
            if (!vInfos->fileType.isDir()) {
                vInfos->formatedFileSize = IGFD::Utils::FormatFileSize(vInfos->fileSize);
            }
        }
    }
    return true;  // file will be added to file list, so displayed
}

void IGFD::FileManager::ClearFileLists() {
    m_FilteredFileList.clear();
    m_FileList.clear();
}

void IGFD::FileManager::ClearPathLists() {
    m_FilteredPathList.clear();
    m_PathList.clear();
}

void IGFD::FileManager::m_AddFile(const FileDialogInternal& vFileDialogInternal, const std::string& vPath, const std::string& vFileName, const FileType& vFileType) {
    auto infos_ptr = FileInfos::create();

    infos_ptr->filePath              = vPath;
    infos_ptr->fileNameExt           = vFileName;
    infos_ptr->fileNameExt_optimized = Utils::LowerCaseString(infos_ptr->fileNameExt);
    infos_ptr->fileType              = vFileType;

    if (infos_ptr->fileNameExt.empty() || (infos_ptr->fileNameExt == "." && !vFileDialogInternal.filterManager.dLGFilters.empty())) {  // filename empty or filename is the current dir '.' //-V807
        return;
    }

    if (infos_ptr->fileNameExt != ".." && (vFileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_DontShowHiddenFiles) && infos_ptr->fileNameExt[0] == '.') {  // dont show hidden files
        if (!vFileDialogInternal.filterManager.dLGFilters.empty() || (vFileDialogInternal.filterManager.dLGFilters.empty() && infos_ptr->fileNameExt != ".")) {            // except "." if in directory mode //-V728
            return;
        }
    }

    if (infos_ptr->FinalizeFileTypeParsing(vFileDialogInternal.filterManager.GetSelectedFilter().count_dots)) {
        if (!vFileDialogInternal.filterManager.IsCoveredByFilters(*infos_ptr.get(),  //
                                                                  (vFileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering) != 0)) {
            return;
        }
    }

    vFileDialogInternal.filterManager.FillFileStyle(infos_ptr);

    m_CompleteFileInfos(infos_ptr);

    if (m_CompleteFileInfosWithUserFileAttirbutes(vFileDialogInternal, infos_ptr)) {
        m_FileList.push_back(infos_ptr);
    }
}

void IGFD::FileManager::m_AddPath(const FileDialogInternal& vFileDialogInternal, const std::string& vPath, const std::string& vFileName, const FileType& vFileType) {
    if (!vFileType.isDir()) return;

    auto infos_ptr = FileInfos::create();

    infos_ptr->filePath              = vPath;
    infos_ptr->fileNameExt           = vFileName;
    infos_ptr->fileNameExt_optimized = Utils::LowerCaseString(infos_ptr->fileNameExt);
    infos_ptr->fileType              = vFileType;

    if (infos_ptr->fileNameExt.empty() || (infos_ptr->fileNameExt == "." && !vFileDialogInternal.filterManager.dLGFilters.empty())) {  // filename empty or filename is the current dir '.' //-V807
        return;
    }

    if (infos_ptr->fileNameExt != ".." && (vFileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_DontShowHiddenFiles) && infos_ptr->fileNameExt[0] == '.') {  // dont show hidden files
        if (!vFileDialogInternal.filterManager.dLGFilters.empty() || (vFileDialogInternal.filterManager.dLGFilters.empty() && infos_ptr->fileNameExt != ".")) {            // except "." if in directory mode //-V728
            return;
        }
    }

    vFileDialogInternal.filterManager.FillFileStyle(infos_ptr);

    m_CompleteFileInfos(infos_ptr);

    if (m_CompleteFileInfosWithUserFileAttirbutes(vFileDialogInternal, infos_ptr)) {
        m_PathList.push_back(infos_ptr);
    }
}

void IGFD::FileManager::ScanDir(const FileDialogInternal& vFileDialogInternal, const std::string& vPath) {
    std::string path = vPath;

    if (m_CurrentPathDecomposition.empty()) {
        SetCurrentDir(path);
    }

    if (!m_CurrentPathDecomposition.empty()) {
#ifdef _IGFD_WIN_
        if (path == fsRoot) {
            path += IGFD::Utils::GetPathSeparator();
        }
#endif  // _IGFD_WIN_

        ClearFileLists();

        const auto& files = m_FileSystemPtr->ScanDirectory(path);
        for (const auto& file : files) {
            m_AddFile(vFileDialogInternal, path, file.fileNameExt, file.fileType);
        }

        m_SortFields(vFileDialogInternal, m_FileList, m_FilteredFileList);
    }
}

void IGFD::FileManager::m_ScanDirForPathSelection(const FileDialogInternal& vFileDialogInternal, const std::string& vPath) {
    std::string path = vPath;

    if (!path.empty()) {
#ifdef _IGFD_WIN_
        if (path == fsRoot) path += IGFD::Utils::GetPathSeparator();
#endif  // _IGFD_WIN_

        ClearPathLists();

        const auto& files = m_FileSystemPtr->ScanDirectory(path);
        for (const auto& file : files) {
            if (file.fileType.isDir()) {
                m_AddPath(vFileDialogInternal, path, file.fileNameExt, file.fileType);
            }
        }

        m_SortFields(vFileDialogInternal, m_PathList, m_FilteredPathList);
    }
}

void IGFD::FileManager::m_OpenPathPopup(const FileDialogInternal& vFileDialogInternal, std::vector<std::string>::iterator vPathIter) {
    const auto path = ComposeNewPath(vPathIter);
    m_ScanDirForPathSelection(vFileDialogInternal, path);
    m_PopupComposedPath = vPathIter;
    ImGui::OpenPopup("IGFD_Path_Popup");
}

bool IGFD::FileManager::GetDevices() {
    auto devices = m_FileSystemPtr->GetDevicesList();
    if (!devices.empty()) {
        m_CurrentPath.clear();
        m_CurrentPathDecomposition.clear();
        ClearFileLists();
        for (auto& drive : devices) {
            auto info_ptr                   = FileInfos::create();
            info_ptr->fileNameExt           = drive.first;
            info_ptr->fileNameExt_optimized = Utils::LowerCaseString(drive.first);
            info_ptr->deviceInfos           = drive.second;
            info_ptr->fileType.SetContent(FileType::ContentType::Directory);
            if (!info_ptr->fileNameExt.empty()) {
                m_FileList.push_back(info_ptr);
                showDevices = true;
            }
        }
        return true;
    }
    return false;
}

bool IGFD::FileManager::IsComposerEmpty() const {
    return m_CurrentPathDecomposition.empty();
}

size_t IGFD::FileManager::GetComposerSize() const {
    return m_CurrentPathDecomposition.size();
}

bool IGFD::FileManager::IsFileListEmpty() const {
    return m_FileList.empty();
}

bool IGFD::FileManager::IsPathListEmpty() const {
    return m_PathList.empty();
}

size_t IGFD::FileManager::GetFullFileListSize() const {
    return m_FileList.size();
}

std::shared_ptr<IGFD::FileInfos> IGFD::FileManager::GetFullFileAt(size_t vIdx) {
    if (vIdx < m_FileList.size()) return m_FileList[vIdx];
    return nullptr;
}

bool IGFD::FileManager::IsFilteredListEmpty() const {
    return m_FilteredFileList.empty();
}

bool IGFD::FileManager::IsPathFilteredListEmpty() const {
    return m_FilteredPathList.empty();
}

size_t IGFD::FileManager::GetFilteredListSize() const {
    return m_FilteredFileList.size();
}

size_t IGFD::FileManager::GetPathFilteredListSize() const {
    return m_FilteredPathList.size();
}

std::shared_ptr<IGFD::FileInfos> IGFD::FileManager::GetFilteredFileAt(size_t vIdx) {
    if (vIdx < m_FilteredFileList.size()) return m_FilteredFileList[vIdx];
    return nullptr;
}

std::shared_ptr<IGFD::FileInfos> IGFD::FileManager::GetFilteredPathAt(size_t vIdx) {
    if (vIdx < m_FilteredPathList.size()) return m_FilteredPathList[vIdx];
    return nullptr;
}

std::vector<std::string>::iterator IGFD::FileManager::GetCurrentPopupComposedPath() const {
    return m_PopupComposedPath;
}

bool IGFD::FileManager::IsFileNameSelected(const std::string& vFileName) {
    return m_SelectedFileNames.find(vFileName) != m_SelectedFileNames.end();
}

std::string IGFD::FileManager::GetBack() {
    return m_CurrentPathDecomposition.back();
}

void IGFD::FileManager::ClearComposer() {
    m_CurrentPathDecomposition.clear();
}

void IGFD::FileManager::ClearAll() {
    ClearComposer();
    ClearFileLists();
    ClearPathLists();
}
void IGFD::FileManager::ApplyFilteringOnFileList(const FileDialogInternal& vFileDialogInternal) {
    m_ApplyFilteringOnFileList(vFileDialogInternal, m_FileList, m_FilteredFileList);
}

void IGFD::FileManager::m_ApplyFilteringOnFileList(const FileDialogInternal& vFileDialogInternal, std::vector<std::shared_ptr<FileInfos> >& vFileInfosList, std::vector<std::shared_ptr<FileInfos> >& vFileInfosFilteredList) {
    vFileInfosFilteredList.clear();
    for (const auto& file : vFileInfosList) {
        if (!file.use_count()) continue;
        bool show = true;
        if (!file->SearchForTag(vFileDialogInternal.searchManager.searchTag))  // if search tag
            show = false;
        if (dLGDirectoryMode && !file->fileType.isDir()) show = false;
        if (show) vFileInfosFilteredList.push_back(file);
    }
}

void IGFD::FileManager::m_CompleteFileInfos(const std::shared_ptr<FileInfos>& vInfos) {
    if (!vInfos.use_count()) return;

    if (vInfos->fileNameExt != "." && vInfos->fileNameExt != "..") {
        // _stat struct :
        // dev_t     st_dev;     /* ID of device containing file */
        // ino_t     st_ino;     /* inode number */
        // mode_t    st_mode;    /* protection */
        // nlink_t   st_nlink;   /* number of hard links */
        // uid_t     st_uid;     /* user ID of owner */
        // gid_t     st_gid;     /* group ID of owner */
        // dev_t     st_rdev;    /* device ID (if special file) */
        // off_t     st_size;    /* total size, in bytes */
        // blksize_t st_blksize; /* blocksize for file system I/O */
        // blkcnt_t  st_blocks;  /* number of 512B blocks allocated */
        // time_t    st_atime;   /* time of last access - not sure out of ntfs */
        // time_t    st_mtime;   /* time of last modification - not sure out of ntfs */
        // time_t    st_ctime;   /* time of last status change - not sure out of ntfs */

        std::string fpn;

        // FIXME: so the condition is always true?
        if (vInfos->fileType.isFile() || vInfos->fileType.isLinkToUnknown() || vInfos->fileType.isDir()) {
            fpn = vInfos->filePath + IGFD::Utils::GetPathSeparator() + vInfos->fileNameExt;
        }

        struct stat statInfos = {};
        char timebuf[100];
        int result = stat(fpn.c_str(), &statInfos);
        if (!result) {
            if (!vInfos->fileType.isDir()) {
                vInfos->fileSize         = (size_t)statInfos.st_size;
                vInfos->formatedFileSize = IGFD::Utils::FormatFileSize(vInfos->fileSize);
            }

            size_t len = 0;
#ifdef _MSC_VER
            struct tm _tm;
            errno_t err = localtime_s(&_tm, &statInfos.st_mtime);
            if (!err) len = strftime(timebuf, 99, DateTimeFormat, &_tm);
#else   // _MSC_VER
            struct tm* _tm = localtime(&statInfos.st_mtime);
            if (_tm) len = strftime(timebuf, 99, DateTimeFormat, _tm);
#endif  // _MSC_VER
            if (len) {
                vInfos->fileModifDate = std::string(timebuf, len);
            }
        }
    }
}

void IGFD::FileManager::m_RemoveFileNameInSelection(const std::string& vFileName) {
    m_SelectedFileNames.erase(vFileName);

    if (m_SelectedFileNames.size() == 1) {
        snprintf(fileNameBuffer, MAX_FILE_DIALOG_NAME_BUFFER, "%s", vFileName.c_str());
    } else {
        snprintf(fileNameBuffer, MAX_FILE_DIALOG_NAME_BUFFER, "%zu files Selected", m_SelectedFileNames.size());
    }
}

void IGFD::FileManager::m_AddFileNameInSelection(const std::string& vFileName, bool vSetLastSelectionFileName) {
    if (vFileName == "." || vFileName == "..") {
        return;
    }
    m_SelectedFileNames.emplace(vFileName);

    if (m_SelectedFileNames.size() == 1) {
        snprintf(fileNameBuffer, MAX_FILE_DIALOG_NAME_BUFFER, "%s", vFileName.c_str());
    } else {
        snprintf(fileNameBuffer, MAX_FILE_DIALOG_NAME_BUFFER, "%zu files Selected", m_SelectedFileNames.size());
    }

    if (vSetLastSelectionFileName) {
        m_LastSelectedFileName = vFileName;
    }
}

void IGFD::FileManager::SetCurrentDir(const std::string& vPath) {
    std::string path = vPath;
#ifdef _IGFD_WIN_
    if (fsRoot == path) path += IGFD::Utils::GetPathSeparator();
#endif  // _IGFD_WIN_

    bool dir_opened = m_FileSystemPtr->IsDirectory(path);
    if (!dir_opened) {
        path       = ".";
        dir_opened = m_FileSystemPtr->IsDirectory(path);
    }
    if (dir_opened) {
#ifdef _IGFD_WIN_
        DWORD numchar      = 0;
        std::wstring wpath = IGFD::Utils::UTF8Decode(path);
        numchar            = GetFullPathNameW(wpath.c_str(), 0, nullptr, nullptr);
        std::wstring fpath(numchar, 0);
        GetFullPathNameW(wpath.c_str(), numchar, (wchar_t*)fpath.data(), nullptr);
        std::string real_path = IGFD::Utils::UTF8Encode(fpath);
        while (real_path.back() == '\0')  // for fix issue we can have with std::string concatenation.. if there is a \0 at end
            real_path = real_path.substr(0, real_path.size() - 1U);
        if (!real_path.empty())
#elif defined(_IGFD_UNIX_)  // _IGFD_UNIX_ is _IGFD_WIN_ or APPLE
        char real_path[PATH_MAX];
        char* numchar = realpath(path.c_str(), real_path);
        if (numchar != nullptr)
#endif                      // _IGFD_WIN_
        {
            m_CurrentPath = std::move(real_path);
            if (m_CurrentPath[m_CurrentPath.size() - 1] == PATH_SEP) {
                m_CurrentPath = m_CurrentPath.substr(0, m_CurrentPath.size() - 1);
            }
            IGFD::Utils::SetBuffer(inputPathBuffer, MAX_PATH_BUFFER_SIZE, m_CurrentPath);
            m_CurrentPathDecomposition = IGFD::Utils::SplitStringToVector(m_CurrentPath, PATH_SEP, false);
#ifdef _IGFD_UNIX_  // _IGFD_UNIX_ is _IGFD_WIN_ or APPLE
            m_CurrentPathDecomposition.insert(m_CurrentPathDecomposition.begin(), IGFD::Utils::GetPathSeparator());
#endif  // _IGFD_UNIX_
            if (!m_CurrentPathDecomposition.empty()) {
#ifdef _IGFD_WIN_
                fsRoot = m_CurrentPathDecomposition[0];
#endif  // _IGFD_WIN_
            }
        }
    }
}

bool IGFD::FileManager::CreateDir(const std::string& vPath) {
    if (!vPath.empty()) {
        std::string path = m_CurrentPath + IGFD::Utils::GetPathSeparator() + vPath;
        return m_FileSystemPtr->CreateDirectoryIfNotExist(path);
    }
    return false;
}

std::string IGFD::FileManager::ComposeNewPath(std::vector<std::string>::iterator vIter) {
    std::string res;

    while (true) {
        if (!res.empty()) {
#ifdef _IGFD_WIN_
            res = *vIter + IGFD::Utils::GetPathSeparator() + res;
#elif defined(_IGFD_UNIX_)  // _IGFD_UNIX_ is _IGFD_WIN_ or APPLE
            if (*vIter == fsRoot)
                res = *vIter + res;
            else
                res = *vIter + PATH_SEP + res;
#endif                      // _IGFD_WIN_
        } else
            res = *vIter;

        if (vIter == m_CurrentPathDecomposition.begin()) {
#ifdef _IGFD_UNIX_  // _IGFD_UNIX_ is _IGFD_WIN_ or APPLE
            if (res[0] != PATH_SEP) res = PATH_SEP + res;
#else
            if (res.back() != PATH_SEP) res.push_back(PATH_SEP);
#endif  // defined(_IGFD_UNIX_)
            break;
        }

        --vIter;
    }

    return res;
}

bool IGFD::FileManager::SetPathOnParentDirectoryIfAny() {
    if (m_CurrentPathDecomposition.size() > 1) {
        m_CurrentPath = ComposeNewPath(m_CurrentPathDecomposition.end() - 2);
        return true;
    }
    return false;
}

std::string IGFD::FileManager::GetCurrentPath() {
    if (m_CurrentPath.empty()) m_CurrentPath = ".";
    return m_CurrentPath;
}

void IGFD::FileManager::SetCurrentPath(const std::string& vCurrentPath) {
    if (vCurrentPath.empty())
        m_CurrentPath = ".";
    else
        m_CurrentPath = vCurrentPath;
}

void IGFD::FileManager::SetDefaultFileName(const std::string& vFileName) {
    dLGDefaultFileName = vFileName;
    IGFD::Utils::SetBuffer(fileNameBuffer, MAX_FILE_DIALOG_NAME_BUFFER, vFileName);
}

bool IGFD::FileManager::SelectDirectory(const std::shared_ptr<FileInfos>& vInfos) {
    if (!vInfos.use_count()) return false;

    bool pathClick = false;

    if (vInfos->fileNameExt == "..") {
        pathClick = SetPathOnParentDirectoryIfAny();
    } else {
        std::string newPath;

        if (showDevices) {
            newPath = vInfos->fileNameExt + IGFD::Utils::GetPathSeparator();
        } else {
#ifdef __linux__
            if (fsRoot == m_CurrentPath)
                newPath = m_CurrentPath + vInfos->fileNameExt;
            else
#endif  // __linux__
                newPath = m_CurrentPath + IGFD::Utils::GetPathSeparator() + vInfos->fileNameExt;
        }

        if (m_FileSystemPtr->IsDirectoryCanBeOpened(newPath)) {
            if (showDevices) {
                m_CurrentPath = vInfos->fileNameExt;
                fsRoot        = m_CurrentPath;
            } else {
                m_CurrentPath = newPath;  //-V820
            }
            pathClick = true;
        }
    }

    return pathClick;
}

void IGFD::FileManager::SelectAllFileNames() {
    m_SelectedFileNames.clear();
    for (const auto& infos_ptr : m_FilteredFileList) {
        if (infos_ptr != nullptr) {
            m_AddFileNameInSelection(infos_ptr->fileNameExt, true);
        }
    }
}

void IGFD::FileManager::SelectFileName(const std::shared_ptr<FileInfos>& vInfos) {
    if (!vInfos.use_count()) {
        return;
    }
    m_AddFileNameInSelection(vInfos->fileNameExt, true);
}

void IGFD::FileManager::SelectOrDeselectFileName(const FileDialogInternal& vFileDialogInternal, const std::shared_ptr<FileInfos>& vInfos) {
    if (!vInfos.use_count()) {
        return;
    }

    if (ImGui::IsKeyDown(ImGuiMod_Ctrl)) {
        if (dLGcountSelectionMax == 0) {                                                       // infinite selection
            if (m_SelectedFileNames.find(vInfos->fileNameExt) == m_SelectedFileNames.end()) {  // not found +> add
                m_AddFileNameInSelection(vInfos->fileNameExt, true);
            } else {  // found +> remove
                m_RemoveFileNameInSelection(vInfos->fileNameExt);
            }
        } else {  // selection limited by size
            if (m_SelectedFileNames.size() < dLGcountSelectionMax) {
                if (m_SelectedFileNames.find(vInfos->fileNameExt) == m_SelectedFileNames.end()) {  // not found +> add
                    m_AddFileNameInSelection(vInfos->fileNameExt, true);
                } else {  // found +> remove
                    m_RemoveFileNameInSelection(vInfos->fileNameExt);
                }
            }
        }
    } else if (ImGui::IsKeyDown(ImGuiMod_Shift)) {
        if (dLGcountSelectionMax != 1) {
            m_SelectedFileNames.clear();
            // we will iterate filelist and get the last selection after the start selection
            bool startMultiSelection     = false;
            std::string fileNameToSelect = vInfos->fileNameExt;
            std::string savedLastSelectedFileName;  // for invert selection mode
            for (const auto& file : m_FileList) {
                if (!file.use_count()) {
                    continue;
                }
                bool canTake = true;
                if (!file->SearchForTag(vFileDialogInternal.searchManager.searchTag)) canTake = false;
                if (canTake) {  // if not filtered, we will take files who are filtered by the dialog
                    if (file->fileNameExt == m_LastSelectedFileName) {
                        startMultiSelection = true;
                        m_AddFileNameInSelection(m_LastSelectedFileName, false);
                    } else if (startMultiSelection) {
                        if (dLGcountSelectionMax == 0) {  // infinite selection
                            m_AddFileNameInSelection(file->fileNameExt, false);
                        } else {  // selection limited by size
                            if (m_SelectedFileNames.size() < dLGcountSelectionMax) {
                                m_AddFileNameInSelection(file->fileNameExt, false);
                            } else {
                                startMultiSelection = false;
                                if (!savedLastSelectedFileName.empty()) m_LastSelectedFileName = savedLastSelectedFileName;
                                break;
                            }
                        }
                    }

                    if (file->fileNameExt == fileNameToSelect) {
                        if (!startMultiSelection) {  // we are before the last Selected FileName, so we must inverse
                            savedLastSelectedFileName = m_LastSelectedFileName;
                            m_LastSelectedFileName    = fileNameToSelect;
                            fileNameToSelect          = savedLastSelectedFileName;
                            startMultiSelection       = true;
                            m_AddFileNameInSelection(m_LastSelectedFileName, false);
                        } else {
                            startMultiSelection = false;
                            if (!savedLastSelectedFileName.empty()) m_LastSelectedFileName = savedLastSelectedFileName;
                            break;
                        }
                    }
                }
            }
        }
    } else {
        m_SelectedFileNames.clear();
        IGFD::Utils::ResetBuffer(fileNameBuffer);
        m_AddFileNameInSelection(vInfos->fileNameExt, true);
    }
}

void IGFD::FileManager::DrawDirectoryCreation(const FileDialogInternal& vFileDialogInternal) {
    if (vFileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_DisableCreateDirectoryButton) return;

    if (IMGUI_BUTTON(createDirButtonString)) {
        if (!m_CreateDirectoryMode) {
            m_CreateDirectoryMode = true;
            IGFD::Utils::ResetBuffer(directoryNameBuffer);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(buttonCreateDirString);

    if (m_CreateDirectoryMode) {
        ImGui::SameLine();

        ImGui::PushItemWidth(100.0f);
        ImGui::InputText("##DirectoryFileName", directoryNameBuffer, MAX_FILE_DIALOG_NAME_BUFFER);
        ImGui::PopItemWidth();

        ImGui::SameLine();

        if (IMGUI_BUTTON(okButtonString)) {
            std::string newDir = std::string(directoryNameBuffer);
            if (CreateDir(newDir)) {
                SetCurrentPath(m_CurrentPath + IGFD::Utils::GetPathSeparator() + newDir);
                OpenCurrentPath(vFileDialogInternal);
            }

            m_CreateDirectoryMode = false;
        }

        ImGui::SameLine();

        if (IMGUI_BUTTON(cancelButtonString)) {
            m_CreateDirectoryMode = false;
        }
    }

    ImGui::SameLine();
}

void IGFD::FileManager::DrawPathComposer(const FileDialogInternal& vFileDialogInternal) {
    if (IMGUI_BUTTON(resetButtonString)) {
        SetCurrentPath(".");
        OpenCurrentPath(vFileDialogInternal);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(buttonResetPathString);
    }
    if (vFileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_ShowDevicesButton) {
        ImGui::SameLine();
        if (IMGUI_BUTTON(devicesButtonString)) {
            devicesClicked = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(buttonDriveString);
        }
    }

    ImGui::SameLine();

    if (IMGUI_BUTTON(editPathButtonString)) {
        inputPathActivated = !inputPathActivated;
        if (inputPathActivated) {
            if (!m_CurrentPathDecomposition.empty()) {
                auto endIt    = m_CurrentPathDecomposition.end();
                m_CurrentPath = ComposeNewPath(--endIt);
                IGFD::Utils::SetBuffer(inputPathBuffer, MAX_PATH_BUFFER_SIZE, m_CurrentPath);
            }
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(buttonEditPathString);

    ImGui::SameLine();

    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);

    // show current path
    if (!m_CurrentPathDecomposition.empty()) {
        ImGui::SameLine();

        if (inputPathActivated) {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::InputText("##pathedition", inputPathBuffer, MAX_PATH_BUFFER_SIZE);
            ImGui::PopItemWidth();
        } else {
            int _id = 0;
            for (auto itPathDecomp = m_CurrentPathDecomposition.begin(); itPathDecomp != m_CurrentPathDecomposition.end(); ++itPathDecomp) {
                if (itPathDecomp != m_CurrentPathDecomposition.begin()) {
#if defined(CUSTOM_PATH_SPACING)
                    ImGui::SameLine(0, CUSTOM_PATH_SPACING);
#else
                    ImGui::SameLine();
#endif  // USE_CUSTOM_PATH_SPACING
                    if (!(vFileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_DisableQuickPathSelection)) {
#if defined(_IGFD_WIN_)
                        const char* sep = "\\";
#elif defined(_IGFD_UNIX_)
                        const char* sep = "/";
                        if (itPathDecomp != m_CurrentPathDecomposition.begin() + 1)
#endif
                        {
                            ImGui::PushID(_id++);
                            bool click = IMGUI_PATH_BUTTON(sep);
                            ImGui::PopID();

#if defined(CUSTOM_PATH_SPACING)
                            ImGui::SameLine(0, CUSTOM_PATH_SPACING);
#else
                            ImGui::SameLine();
#endif  // USE_CUSTOM_PATH_SPACING

                            if (click) {
                                m_OpenPathPopup(vFileDialogInternal, itPathDecomp - 1);
                            } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                                m_SetCurrentPath(itPathDecomp - 1);
                                break;
                            }
                        }
                    }
                }

                ImGui::PushID(_id++);
                bool click = IMGUI_PATH_BUTTON((*itPathDecomp).c_str());
                ImGui::PopID();
                if (click) {
                    m_CurrentPath = ComposeNewPath(itPathDecomp);
                    pathClicked   = true;
                    break;
                } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {  // activate input for path
                    m_SetCurrentPath(itPathDecomp);
                    break;
                }
            }
        }
    }
}

