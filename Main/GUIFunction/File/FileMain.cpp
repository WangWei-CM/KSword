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
// �ַ���ת��: std::string �� LPCWSTR
std::wstring s2ws(const std::string& s) {
    int len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, NULL, 0);
    std::wstring wstr(len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &wstr[0], len);
    return wstr;
}
// �ַ���ת��: LPCWSTR �� std::string
std::string ws2s(const std::wstring& ws) {
    int len = WideCharToMultiByte(CP_ACP, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    std::string str(len, '\0');
    WideCharToMultiByte(CP_ACP, 0, ws.c_str(), -1, &str[0], len, NULL, NULL);
    return str;
}

// ��ȡ�ļ�����������·���У�
std::string GetFileNameFromPath(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

// ��ȡĿ¼·����������·���У�
std::string GetDirectoryFromPath(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return "";
    return path.substr(0, pos);
}

// ϵͳĬ�ϳ�����ļ�
void OpenFileWithDefaultProgram(const std::string& filePath) {
    std::wstring wFilePath = s2ws(filePath);
    ShellExecuteW(NULL, L"open", wFilePath.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

// �������ļ�
bool RenameFile(const std::string& oldPath, const std::string& newName) {
    std::string dir = GetDirectoryFromPath(oldPath);
    std::string newPath = dir + "\\" + newName;

    std::wstring wOldPath = s2ws(oldPath);
    std::wstring wNewPath = s2ws(newPath);
    return MoveFileExW(wOldPath.c_str(), wNewPath.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// ɾ���ļ�
bool DeleteFile(const std::string& filePath) {
    std::wstring wFilePath = s2ws(filePath);

    // ����Ƿ�ΪĿ¼
    DWORD attr = GetFileAttributesW(wFilePath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        // ɾ��Ŀ¼
        SHFILEOPSTRUCTW fileOp = { 0 };
        fileOp.wFunc = FO_DELETE;
        fileOp.pFrom = (wFilePath + L"\0").c_str();  // ˫null��β
        fileOp.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        return SHFileOperationW(&fileOp) == 0;
    }
    else {
        // ɾ���ļ�
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

    // ��ʾ/���ذ�ť
    if (ImGui::Button(showExplorer ? C("�����ļ������") : C("��ʾ�ļ������"))) {
        showExplorer = !showExplorer;
        if (showExplorer) {
            ImGuiFileDialog::Instance()->OpenDialog(dialogName, "", /*".cpp,.h,.exe,.dll"*/"*.*", config);
        }
    }

    if (showExplorer) {
        ImGui::Separator();
        ImGui::PushItemWidth(-1);

        // ����Ⱦ�ļ��Ի��򣬱��淵��ֵ�ж��Ƿ��ڻ���������
        bool isDialogDisplayed = ImGuiFileDialog::Instance()->Display(dialogName, 0, ImVec2(-1, 400));

        // ����˫����
        if (isDialogDisplayed && ImGuiFileDialog::Instance()->IsOk()) {
            selectedFilePath = ImGuiFileDialog::Instance()->GetFilePathName();
            OpenFileWithDefaultProgram(selectedFilePath);
        }

        // �ؼ��޸���ʹ��IsWindowHovered����ļ��Ի��򴰿ڵ������ͣ
        // ȷ���ڶԻ�����Ⱦ������������״̬
        bool isHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) /*&&
            !ImGui::IsAnyPopupOpen()*/;

        // �Ҽ��˵��������������ļ��������Ҽ��������ѡ���ļ�
        if (isHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            // ���»�ȡ��ǰѡ�е��ļ�·����ȷ��׼ȷ�ԣ�
            std::string currentPath = ImGuiFileDialog::Instance()->GetFilePathName();
            if (!currentPath.empty()) {
                selectedFilePath = currentPath;
                ImGui::OpenPopup("FileContextMenu");
            }
        }

        // �Ҽ��˵�����
        if (ImGui::BeginPopup("FileContextMenu")) {
            if (ImGui::MenuItem(C("��"))) {
                OpenFileWithDefaultProgram(selectedFilePath);
                ImGui::CloseCurrentPopup();
            }

            if (ImGui::MenuItem(C("������"))) {
                std::string fileName = GetFileNameFromPath(selectedFilePath);
                strncpy_s(newFileName, fileName.c_str(), sizeof(newFileName) - 1);
                showRenamePopup = true;
                ImGui::CloseCurrentPopup();
            }

            if (ImGui::MenuItem(C("ɾ��"))) {
                ImGui::OpenPopup("DeleteConfirm");
            }

            ImGui::EndPopup();
        }

        // ɾ��ȷ�ϵ���
        if (ImGui::BeginPopupModal("DeleteConfirm", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            std::string fileName = GetFileNameFromPath(selectedFilePath);
            ImGui::Text(C("ȷ��Ҫɾ�� %s ��\n�˲������ɻָ���"), fileName.c_str());
            ImGui::Separator();

            if (ImGui::Button(C("ȡ��"), ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(C("ɾ��"), ImVec2(120, 0))) {
                if (DeleteFile(selectedFilePath)) {
                    // ˢ���ļ��б�
                    ImGuiFileDialog::Instance()->Close();
                    ImGuiFileDialog::Instance()->OpenDialog(dialogName, "", "*.*", config);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ���������뵯��
        if (showRenamePopup && ImGui::BeginPopupModal("RenameFile", &showRenamePopup, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text(C("������Ϊ��"));
            ImGui::InputText("", newFileName, IM_ARRAYSIZE(newFileName));

            if (ImGui::Button(C("ȷ��"), ImVec2(120, 0))) {
                if (newFileName[0] != '\0' && RenameFile(selectedFilePath, newFileName)) {
                    // ˢ���ļ��б�
                    ImGuiFileDialog::Instance()->Close();
                    ImGuiFileDialog::Instance()->OpenDialog(dialogName, "", "*.*", config);
                    showRenamePopup = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(C("ȡ��"), ImVec2(120, 0))) {
                showRenamePopup = false;
            }
            ImGui::EndPopup();
        }

        ImGui::PopItemWidth();
    }

    ImGui::EndTabItem();
}
