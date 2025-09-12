#include "../KswordTotalHead.h"
static float SelectedWindowAlpha = 1.0f;
static bool showPointerWindowDetail = false;
extern LPDIRECT3DTEXTURE9 GetCachedProcessIcon(DWORD pid);
static inline BOOL CALLBACK EnumWorkerWProc(_In_ HWND hwndWorkerW, _In_ LPARAM lParam) {
    char className[256] = { 0 };
    GetClassNameA(hwndWorkerW, className, sizeof(className));

    // 只处理类名为"WorkerW"的窗口
    if (strcmp(className, "WorkerW") != 0) {
        return TRUE;
    }

    // 查找当前WorkerW下是否包含"SHELLDLL_DefView"子窗口（这是目标WorkerW）
    HWND* pHideWorkerW = (HWND*)lParam;
    HWND hwndShellDefView = FindWindowExA(hwndWorkerW, NULL, "SHELLDLL_DefView", NULL);

    if (hwndShellDefView != NULL) {
        // 记录需要保留的WorkerW（后续不隐藏）
        *pHideWorkerW = hwndWorkerW;
    }
    else {
        // 隐藏不含SHELLDLL_DefView的WorkerW（避免桌面显示异常）
        ShowWindow(hwndWorkerW, SW_HIDE);
    }

    return TRUE;
}
inline void RenderPointerWindowInfo(HWND hwnd) {
    if (!hwnd) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "[未找到窗口]");
        return;
    }


    // ===================== 左侧列：窗口信息（先绘制，自动适应宽度） =====================
    {
        float left_col_start_x = ImGui::GetCursorPosX(); // 记录左侧列起始X坐标

        // 1. 窗口标题
        char title[256];
        GetWindowTextA(hwnd, title, sizeof(title));
        ImGui::Text(GBKtoUTF8("标题: %s").c_str(), title[0] ? GBKtoUTF8(title).c_str() : C("<无标题>"));

        // 2. 窗口类名
        char class_name[256];
        GetClassNameA(hwnd, class_name, sizeof(class_name));
        ImGui::Text(GBKtoUTF8("类名: %s").c_str(), GBKtoUTF8(class_name).c_str());

        // 3. 窗口位置和尺寸
        RECT rect;
        GetWindowRect(hwnd, &rect);
        ImGui::Text(GBKtoUTF8("位置: (%d, %d)").c_str(), rect.left, rect.top);
        ImGui::Text(GBKtoUTF8("尺寸: %dx%d").c_str(), rect.right - rect.left, rect.bottom - rect.top);

        // 4. 进程信息
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);
        ImGui::Separator();
        ImGui::Text(GBKtoUTF8("进程ID: %d").c_str(), pid);
		ImGui::SameLine();
        LPDIRECT3DTEXTURE9 pTexture = GetCachedProcessIcon(pid);
        if (pTexture) {
            // ImGui::Image参数：纹理指针（转换为ImTextureID）、尺寸
            ImGui::Image(
                (ImTextureID)pTexture,  // DX9纹理指针直接作为ImTextureID
                ImVec2(16, 16)          // 16×16显示尺寸
            );
        }
		ImGui::SameLine();
        if (ImGui::Button(("-"), ImVec2(16, 16))) {
            showPointerWindowDetail = 0;
        };
        ImGui::Text(GBKtoUTF8("进程名称: %s").c_str(), C(GetProcessNameByPID(pid)));

        // 计算并设置左侧列宽度（内容+内边距）
        float left_col_width = ImGui::GetCursorPosX() - left_col_start_x;
        left_col_width += 10.0f; // 增加10px内边距，避免与右侧过近
    }

    {

        // 1. 置顶按钮
        if (ImGui::Button(GBKtoUTF8("置顶").c_str(), ImVec2(50, 25))) {
            SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
            );
        }
        ImGui::SameLine();

        // 2. 取消置顶按钮
        if (ImGui::Button(GBKtoUTF8("取消").c_str(), ImVec2(50, 25))) {
            SetWindowPos(
                hwnd,
                HWND_NOTOPMOST,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
            );
        }
        ImGui::SameLine();

        // 3. 隐藏按钮
        if (ImGui::Button(C("隐藏"), ImVec2(50, 25))) {
            ShowWindow(hwnd, SW_HIDE);
        }
        ImGui::SameLine();
#define CURRENT_MODULE "窗口管理指针窗口"
        if (ImGui::Button(C("设为桌面子窗口"), ImVec2(100, 25))) {
            if (hwnd) {
                kLog.info(C("开始尝试将窗口设为桌面子窗口"), C(CURRENT_MODULE));

                // 步骤1：获取桌面根窗口Progman
                HWND hwndProgman = FindWindowA("Progman", "Program Manager");
                if (hwndProgman == NULL) {
                    kLog.err(C("未找到桌面根窗口Progman，操作终止"), C(CURRENT_MODULE));
                    return;
                }
                kLog.dbg(C("已找到Progman窗口"), C(CURRENT_MODULE));

                // 步骤2：发送0x052c消息给Progman
                LRESULT sendResult = SendMessageTimeoutA(
                    hwndProgman,
                    0x052C,
                    0,
                    0,
                    SMTO_NORMAL,
                    1000,
                    NULL
                );
                if (sendResult == 0) {
                    kLog.err(C("发送0x052C消息给Progman失败，可能未能创建WorkerW窗口"), C(CURRENT_MODULE));
                    // 这里可以选择return或继续
                }
                else {
                    kLog.dbg(C("已发送0x052C消息给Progman"), C(CURRENT_MODULE));
                }

                // 步骤3：枚举所有WorkerW窗口
                HWND hwndKeepWorkerW = NULL;
                if (!EnumWindows((WNDENUMPROC)EnumWorkerWProc, (LPARAM)&hwndKeepWorkerW)) {
                    kLog.err(C("枚举WorkerW窗口失败"), C(CURRENT_MODULE));
                    // 这里可以选择return或继续
                }
                else {
                    kLog.dbg(C("已枚举WorkerW窗口"), C(CURRENT_MODULE));
                }

                // 步骤4：将目标窗口设为Progman的子窗口
                HWND hRet = SetParent(hwnd, hwndProgman);
                if (hRet == NULL) {
                    DWORD err = GetLastError();
                    char msg[256];
                    snprintf(msg, sizeof(msg), "SetParent失败，错误码: %lu, hwnd=0x%p, hwndProgman=0x%p", err, hwnd, hwndProgman);
                    kLog.fatal(C(msg), C(CURRENT_MODULE));
                }
                else {
                    kLog.info(C("成功将目标窗口设为桌面子窗口"), C(CURRENT_MODULE));
                }
            }
            else {
                kLog.warn(C("hwnd无效，无法设为桌面子窗口"), C(CURRENT_MODULE));
            }
        }
#undef CURRENT_MODULE

        ImGui::Separator();

        // 4. 透明度调节（全局变量`SelectedWindowAlpha`保持状态）
        ImGui::Text(C("透明度"));
        if (ImGui::SliderFloat("##window_alpha", &SelectedWindowAlpha, 0.01f, 1.0f, "%.1f",0)) {
            LONG ex_style = GetWindowLong(hwnd, GWL_EXSTYLE);
            if (SelectedWindowAlpha < 1.0f) {
                SetWindowLong(hwnd, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
                SetLayeredWindowAttributes(hwnd, 0, (BYTE)(SelectedWindowAlpha * 255), LWA_ALPHA);
            }
            else {
                SetWindowLong(hwnd, GWL_EXSTYLE, ex_style & ~WS_EX_LAYERED);
                RedrawWindow(hwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
            }
        }
    }
}static    HWND hwnd = NULL;
    ImVec2 mouse_pos = {};
extern void PointerWindow() {
    POINT mouse_pos_windows;

    //ImVec2 mouse_pos = ImGui::GetMousePos();
    if (!GetAsyncKeyState(VK_CONTROL)) {
        GetCursorPos(&mouse_pos_windows);

    // 获取当前窗口句柄
        hwnd = WindowFromPoint(mouse_pos_windows);
        mouse_pos = ImVec2((float)(mouse_pos_windows.x + 2), (float)(mouse_pos_windows.y + 2));
    }
    // -------------------------------
    // 核心设置：让窗口跟随鼠标位置
    // -------------------------------
    // 设置窗口的期望位置为鼠标当前位置
    //ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
        ImGui::SetNextWindowPos(mouse_pos);
        float max_width = 1000;
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(0, 0),           // 最小宽度高度（0表示自适应）
            ImVec2(max_width, FLT_MAX) // 最大宽度不限高度
        );
    

    // -------------------------------
    // 定义窗口的显示参数和样式
    // -------------------------------
    // 组合多个标志确保窗口无法交互且透明
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDecoration       // 无标题栏、无边框
          | ImGuiWindowFlags_AlwaysAutoResize   // 自动调整窗口大小以适应内容
         | ImGuiWindowFlags_NoSavedSettings
        ;            // 禁止手动拖动窗口（已通过代码控制位置）
        //ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.3f));

    // -------------------------------
    // 绘制窗口内容
    // -------------------------------
    // 设置透明背景（可选）
    //ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.3f)); // 半透明黑色背景

    // 开始绘制窗口
    if (ImGui::Begin("Mouse Following Window", nullptr, window_flags)) {
                    // 前三个参数为 RGB（示例为半透明白色，可根据需求调整）
                    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.0f, 1.0f, 1.0f, 0.8f));
        {
                ImGuiWindow* current_window = ImGui::GetCurrentWindow();
                if (current_window && current_window->Viewport) {

                    HWND current_imgui_hwnd = (HWND)current_window->Viewport->PlatformHandle;
                    //SetLayeredWindowAttributes(current_imgui_hwnd, 0, 128, LWA_ALPHA);
                    LONG exStyle = GetWindowLongPtr(current_imgui_hwnd, GWL_EXSTYLE);
                    if (!(exStyle & WS_EX_LAYERED))
                    {
                        SetWindowLongPtr(current_imgui_hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
                        SetLayeredWindowAttributes(current_imgui_hwnd, 0, (BYTE)(255 * 0.65f), LWA_ALPHA);
                    }

                    // 3. 设置透明度（80% 不透明度对应 alpha 值为 204，范围 0~255）
                    // 最后一个参数 LWA_ALPHA 表示修改 alpha 通道透明度

                    SetWindowPos(
                        current_imgui_hwnd,
                        HWND_TOPMOST,
                        0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
                    );
                }
            }
        if(showPointerWindowDetail){


            RenderPointerWindowInfo(hwnd);
            ImGui::Text(C("按住")); ImGui::SameLine(); if (ImGui::Button("Ctrl")); ImGui::SameLine(); ImGui::Text(C("以固定此窗口"));
        }// 可选：添加其他动态内容
        else {
            DWORD pid;
            GetWindowThreadProcessId(hwnd, &pid);
            ImGui::Text(GBKtoUTF8("   %d").c_str(), pid); ImGui::SameLine();


            LPDIRECT3DTEXTURE9 pTexture = GetCachedProcessIcon(pid);
            if (pTexture) {
                // ImGui::Image参数：纹理指针（转换为ImTextureID）、尺寸
                ImGui::Image(
                    (ImTextureID)pTexture,  // DX9纹理指针直接作为ImTextureID
                    ImVec2(16, 16)          // 16×16显示尺寸
                );
            }
			ImGui::SameLine();
            if(ImGui::Button(("+"), ImVec2(16, 16))) {
                showPointerWindowDetail = 1;
            };
            ImGui::Text(GBKtoUTF8("%s").c_str(), C(GetProcessNameByPID(pid)));

        }
    }
    // 弹出之前推送的样式（恢复原窗口背景）
    ImGui::PopStyleColor();
    //ImGui::PopStyleColor();
    ImGui::End();

    // 恢复样式
    //ImGui::PopStyleColor();
}