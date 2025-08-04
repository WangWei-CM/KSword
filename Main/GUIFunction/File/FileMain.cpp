#include <Wbemidl.h>
#define NTDDI_VERSION 0x06030000
#include "../../KswordTotalHead.h"
#include  "../../FileExplorer/ImGuiFileDialog.h"
#include <string>
#include <cstring>
#include <windows.h>
#include <commdlg.h>
const int vecSize = 10;
int Vector[vecSize] = {};
// 字符串转换: std::string 到 LPCWSTR
std::wstring s2ws(const std::string& s) {
    int len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, NULL, 0);
    std::wstring wstr(len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &wstr[0], len);
    return wstr;
}
// 字符串转换: LPCWSTR 到 std::string
std::string ws2s(const std::wstring& ws) {
    int len = WideCharToMultiByte(CP_ACP, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    std::string str(len, '\0');
    WideCharToMultiByte(CP_ACP, 0, ws.c_str(), -1, &str[0], len, NULL, NULL);
    return str;
}

// 提取文件名（从完整路径中）
std::string GetFileNameFromPath(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

// 提取目录路径（从完整路径中）
std::string GetDirectoryFromPath(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return "";
    return path.substr(0, pos);
}

// 系统默认程序打开文件
void OpenFileWithDefaultProgram(const std::string& filePath) {
    std::wstring wFilePath = s2ws(filePath);
    ShellExecuteW(NULL, L"open", wFilePath.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

// 重命名文件
bool RenameFile(const std::string& oldPath, const std::string& newName) {
    std::string dir = GetDirectoryFromPath(oldPath);
    std::string newPath = dir + "\\" + newName;

    std::wstring wOldPath = s2ws(oldPath);
    std::wstring wNewPath = s2ws(newPath);
    return MoveFileExW(wOldPath.c_str(), wNewPath.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// 删除文件
bool DeleteFile(const std::string& filePath) {
    std::wstring wFilePath = s2ws(filePath);

    // 检查是否为目录
    DWORD attr = GetFileAttributesW(wFilePath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        // 删除目录
        SHFILEOPSTRUCTW fileOp = { 0 };
        fileOp.wFunc = FO_DELETE;
        fileOp.pFrom = (wFilePath + L"\0").c_str();  // 双null结尾
        fileOp.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        return SHFileOperationW(&fileOp) == 0;
    }
    else {
        // 删除文件
        return DeleteFileW(wFilePath.c_str()) != 0;
    }
}

void KswordFile() {
    static const char* dialogName = "FileExplorer";
    static bool showExplorer = false;
    static std::string selectedFilePath;
    static bool showRenamePopup = false;
    static char newFileName[256] = { 0 };

    IGFD::FileDialogConfig config;
    config.path = ".";
    config.countSelectionMax = 0;
    config.flags =
        ImGuiFileDialogFlags_NoDialog |
        ImGuiFileDialogFlags_ShowDevicesButton |
        ImGuiFileDialogFlags_NaturalSorting |
        ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering;
    config.flags &= ~ImGuiFileDialogFlags_HideColumnType;
    config.flags &= ~ImGuiFileDialogFlags_HideColumnSize;
    config.flags &= ~ImGuiFileDialogFlags_HideColumnDate;

    // 显示/隐藏按钮
    if (ImGui::Button(showExplorer ? C("隐藏文件浏览器") : C("显示文件浏览器"))) {
        showExplorer = !showExplorer;
        if (showExplorer) {
            ImGuiFileDialog::Instance()->OpenDialog(dialogName, "", /*".cpp,.h,.exe,.dll"*/"*.*", config);
        }
    }

    if (showExplorer) {
        ImGui::Separator();
        ImGui::PushItemWidth(-1);

        // 先渲染文件对话框，保存返回值判断是否在绘制区域内
        bool isDialogDisplayed = ImGuiFileDialog::Instance()->Display(dialogName, 0, ImVec2(-1, 400));

        // 处理双击打开
        if (isDialogDisplayed && ImGuiFileDialog::Instance()->IsOk()) {
            selectedFilePath = ImGuiFileDialog::Instance()->GetFilePathName();
            OpenFileWithDefaultProgram(selectedFilePath);
        }

        // 关键修复：使用IsWindowHovered检测文件对话框窗口的鼠标悬停
        // 确保在对话框渲染后立即检测鼠标状态
        bool isHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) /*&&
            !ImGui::IsAnyPopupOpen()*/;

        // 右键菜单触发：必须在文件区域内右键点击且有选中文件
        if (isHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            // 重新获取当前选中的文件路径（确保准确性）
            std::string currentPath = ImGuiFileDialog::Instance()->GetFilePathName();
            if (!currentPath.empty()) {
                selectedFilePath = currentPath;
                ImGui::OpenPopup("FileContextMenu");
            }
        }

        // 右键菜单内容
        if (ImGui::BeginPopup("FileContextMenu")) {
            if (ImGui::MenuItem(C("打开"))) {
                OpenFileWithDefaultProgram(selectedFilePath);
                ImGui::CloseCurrentPopup();
            }

            if (ImGui::MenuItem(C("重命名"))) {
                std::string fileName = GetFileNameFromPath(selectedFilePath);
                strncpy_s(newFileName, fileName.c_str(), sizeof(newFileName) - 1);
                showRenamePopup = true;
                ImGui::CloseCurrentPopup();
            }

            if (ImGui::MenuItem(C("删除"))) {
                ImGui::OpenPopup("DeleteConfirm");
            }

            ImGui::EndPopup();
        }

        // 删除确认弹窗
        if (ImGui::BeginPopupModal("DeleteConfirm", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            std::string fileName = GetFileNameFromPath(selectedFilePath);
            ImGui::Text(C("确定要删除 %s 吗？\n此操作不可恢复！"), fileName.c_str());
            ImGui::Separator();

            if (ImGui::Button(C("取消"), ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(C("删除"), ImVec2(120, 0))) {
                if (DeleteFile(selectedFilePath)) {
                    // 刷新文件列表
                    ImGuiFileDialog::Instance()->Close();
                    ImGuiFileDialog::Instance()->OpenDialog(dialogName, "", "*.*", config);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // 重命名输入弹窗
        if (showRenamePopup && ImGui::BeginPopupModal("RenameFile", &showRenamePopup, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text(C("重命名为："));
            ImGui::InputText("", newFileName, IM_ARRAYSIZE(newFileName));

            if (ImGui::Button(C("确认"), ImVec2(120, 0))) {
                if (newFileName[0] != '\0' && RenameFile(selectedFilePath, newFileName)) {
                    // 刷新文件列表
                    ImGuiFileDialog::Instance()->Close();
                    ImGuiFileDialog::Instance()->OpenDialog(dialogName, "", "*.*", config);
                    showRenamePopup = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(C("取消"), ImVec2(120, 0))) {
                showRenamePopup = false;
            }
            ImGui::EndPopup();
        }

        ImGui::PopItemWidth();
    }

    ImGui::EndTabItem();
}
