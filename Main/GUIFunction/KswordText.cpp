#include "../KswordTotalHead.h"
void TextEditorWindow(TextEditor& editor, bool& isOpen,
    bool& showSaveDialog, bool& showOpenDialog,
    std::string& currentFilePath) {
    // ��ʼ����
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("�ı��༭��", &isOpen)) {

        // �˵��� - ���ڱ༭����Ⱦ֮ǰ
        if (ImGui::BeginMenuBar()) {
            // �ļ��˵�
            if (ImGui::BeginMenu("�ļ�")) {
                if (ImGui::MenuItem("�½�", "Ctrl+N")) {
                    editor.SetText("");
                    currentFilePath.clear();
                }
                if (ImGui::MenuItem("��...", "Ctrl+O")) {
                    showOpenDialog = true;
                }

                ImGui::Separator();

                if (ImGui::MenuItem("����", "Ctrl+S", false, !currentFilePath.empty())) {
                    // �����߼������ⲿ����
                }
                if (ImGui::MenuItem("���Ϊ...", "Ctrl+Shift+S")) {
                    showSaveDialog = true;
                }

                ImGui::Separator();

                if (ImGui::MenuItem("�˳�")) {
                    isOpen = false;
                }
                ImGui::EndMenu();
            }

            // �༭�˵�
            if (ImGui::BeginMenu("�༭")) {
                if (ImGui::MenuItem("����", "Ctrl+Z", false, editor.CanUndo())) {
                    editor.Undo();
                }
                if (ImGui::MenuItem("����", "Ctrl+Y", false, editor.CanRedo())) {
                    editor.Redo();
                }

                ImGui::Separator();

                if (ImGui::MenuItem("����", "Ctrl+C", false, editor.HasSelection())) {
                    editor.Copy();
                }
                if (ImGui::MenuItem("����", "Ctrl+X", false, editor.HasSelection())) {
                    editor.Cut();
                }
                if (ImGui::MenuItem("ճ��", "Ctrl+V")) {
                    editor.Paste();
                }

                ImGui::Separator();

                if (ImGui::MenuItem("ȫѡ", "Ctrl+A")) {
                    editor.SelectAll();
                }
                if (ImGui::MenuItem("ɾ��", "Del", false, editor.HasSelection())) {
                    editor.Delete();
                }

                ImGui::EndMenu();
            }

            // ��ͼ�˵�
            if (ImGui::BeginMenu("��ͼ")) {
                bool colorizerEnabled = editor.IsColorizerEnabled();
                if (ImGui::MenuItem("�﷨����", nullptr, &colorizerEnabled)) {
                    editor.SetColorizerEnable(colorizerEnabled);
                }

                bool showWhitespace = editor.IsShowingWhitespaces();
                if (ImGui::MenuItem("��ʾ�հ��ַ�", nullptr, &showWhitespace)) {
                    editor.SetShowWhitespaces(showWhitespace);
                }

                ImGui::Separator();

                if (ImGui::MenuItem("ǳɫ����")) {
                    editor.SetPalette(TextEditor::GetLightPalette());
                }
                if (ImGui::MenuItem("��ɫ����")) {
                    editor.SetPalette(TextEditor::GetDarkPalette());
                }
                if (ImGui::MenuItem("��������")) {
                    editor.SetPalette(TextEditor::GetRetroBluePalette());
                }

                ImGui::EndMenu();
            }

            ImGui::EndMenuBar();
        }

        // ֱ����Ⱦ�ı��༭�� - ռ��������������
        editor.Render("TextEditor", ImVec2(0, 0), true);

        // ״̬�� - ���ڱ༭��֮��
        ImGui::Separator();
        ImGui::Text("��: %d, ��: %d | %s | %s",
            editor.GetCursorPosition().mLine + 1,
            editor.GetCursorPosition().mColumn + 1,
            editor.IsOverwrite() ? "����" : "����",
            currentFilePath.empty() ? "δ�����ļ�" : currentFilePath.c_str());
    }
    ImGui::End();
}