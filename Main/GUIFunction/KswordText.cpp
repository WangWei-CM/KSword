#include "../KswordTotalHead.h"
void TextEditorWindow(TextEditor& editor, bool& isOpen,
    bool& showSaveDialog, bool& showOpenDialog,
    std::string& currentFilePath) {
    // 开始窗口
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("文本编辑器", &isOpen)) {

        // 菜单栏 - 放在编辑器渲染之前
        if (ImGui::BeginMenuBar()) {
            // 文件菜单
            if (ImGui::BeginMenu("文件")) {
                if (ImGui::MenuItem("新建", "Ctrl+N")) {
                    editor.SetText("");
                    currentFilePath.clear();
                }
                if (ImGui::MenuItem("打开...", "Ctrl+O")) {
                    showOpenDialog = true;
                }

                ImGui::Separator();

                if (ImGui::MenuItem("保存", "Ctrl+S", false, !currentFilePath.empty())) {
                    // 保存逻辑将在外部处理
                }
                if (ImGui::MenuItem("另存为...", "Ctrl+Shift+S")) {
                    showSaveDialog = true;
                }

                ImGui::Separator();

                if (ImGui::MenuItem("退出")) {
                    isOpen = false;
                }
                ImGui::EndMenu();
            }

            // 编辑菜单
            if (ImGui::BeginMenu("编辑")) {
                if (ImGui::MenuItem("撤销", "Ctrl+Z", false, editor.CanUndo())) {
                    editor.Undo();
                }
                if (ImGui::MenuItem("重做", "Ctrl+Y", false, editor.CanRedo())) {
                    editor.Redo();
                }

                ImGui::Separator();

                if (ImGui::MenuItem("复制", "Ctrl+C", false, editor.HasSelection())) {
                    editor.Copy();
                }
                if (ImGui::MenuItem("剪切", "Ctrl+X", false, editor.HasSelection())) {
                    editor.Cut();
                }
                if (ImGui::MenuItem("粘贴", "Ctrl+V")) {
                    editor.Paste();
                }

                ImGui::Separator();

                if (ImGui::MenuItem("全选", "Ctrl+A")) {
                    editor.SelectAll();
                }
                if (ImGui::MenuItem("删除", "Del", false, editor.HasSelection())) {
                    editor.Delete();
                }

                ImGui::EndMenu();
            }

            // 视图菜单
            if (ImGui::BeginMenu("视图")) {
                bool colorizerEnabled = editor.IsColorizerEnabled();
                if (ImGui::MenuItem("语法高亮", nullptr, &colorizerEnabled)) {
                    editor.SetColorizerEnable(colorizerEnabled);
                }

                bool showWhitespace = editor.IsShowingWhitespaces();
                if (ImGui::MenuItem("显示空白字符", nullptr, &showWhitespace)) {
                    editor.SetShowWhitespaces(showWhitespace);
                }

                ImGui::Separator();

                if (ImGui::MenuItem("浅色主题")) {
                    editor.SetPalette(TextEditor::GetLightPalette());
                }
                if (ImGui::MenuItem("深色主题")) {
                    editor.SetPalette(TextEditor::GetDarkPalette());
                }
                if (ImGui::MenuItem("复古主题")) {
                    editor.SetPalette(TextEditor::GetRetroBluePalette());
                }

                ImGui::EndMenu();
            }

            ImGui::EndMenuBar();
        }

        // 直接渲染文本编辑器 - 占据整个内容区域
        editor.Render("TextEditor", ImVec2(0, 0), true);

        // 状态栏 - 放在编辑器之后
        ImGui::Separator();
        ImGui::Text("行: %d, 列: %d | %s | %s",
            editor.GetCursorPosition().mLine + 1,
            editor.GetCursorPosition().mColumn + 1,
            editor.IsOverwrite() ? "覆盖" : "插入",
            currentFilePath.empty() ? "未保存文件" : currentFilePath.c_str());
    }
    ImGui::End();
}