#include "../KswordTotalHead.h"
#include "GUIfunction.h"
std::string protocolText = C("该版本用户使用协议\n"
"这里可以添加更多的协议内容...\n"
"协议的详细信息可以在这里继续扩展...\n"
"更多的协议内容...\n");

std::vector<std::string> coreDevelopers = { "A.b.c", "D.e.f", "G.h.i", "更多核心开发人员..." };
std::vector<std::string> contributors = { C("贡献者1"), C("贡献者2")};
std::vector<std::string> donors = { C("捐赠者1"), C("捐赠者2") };

IDirect3DTexture9* g_IconTexture = nullptr;
ImTextureID g_IconImTextureID = 0;


inline void KswordLogo5() {
	//ImGui::Text("   _  __                                      _   ____         ___  ");
	//ImGui::Text("  | |/ /  ___  __      __   ___    _ __    __| | | ___|       / _ \\ ");
	//ImGui::Text("  | ' /  / __| \\ \\ /\\ / /  / _ \\  | '__|  / _` | |___ \\      | | | |");
	//ImGui::Text("  | . \\  \\__ \\  \\ V  V /  | (_) | | |    | (_| |  ___) |  _  | |_| |");
	//ImGui::Text("  |_|\\_\\ |___/   \\_/\\_/    \\___/  |_|     \\__,_| |____/  (_)  \\___/ ");

    // 获取当前窗口位置和大小
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();

    // 计算图标在窗口内的绝对坐标（窗口内 (5, 5) 位置）
    ImVec2 iconPos = ImVec2(windowPos.x + 10, windowPos.y + 60);
    ImVec2 iconSize = ImVec2(60, 60);  // 图标尺寸

    // 使用 ImGui 的绘制列表在指定位置绘制图标
    ImGui::GetWindowDrawList()->AddImage(
        g_IconImTextureID,
        iconPos,
        ImVec2(iconPos.x + iconSize.x, iconPos.y + iconSize.y),
        ImVec2(0, 0),  // UV 坐标起点
        ImVec2(1, 1)   // UV 坐标终点
    );

    ImGui::PushFont(LOGOfont);
    ImGui::Text("  Ksword5.0");
    ImGui::PopFont();
	ImGui::Text("Ksword Release 5.0 | Based on ImGUI & Ksword Framework");
	ImGui::Text("Deved By WangWei_CM,LianYou_Alex,Ksword Dever Team,etc.");
	return;
}

// 在文件顶部定义主题枚举和函数
enum Theme
{
    Theme_Ksword,
    Theme_Dark,
    Theme_Light,
    Theme_Classic,
    Theme_COUNT
};

const char* ThemeNames[Theme_COUNT] = { "Ksword","Dark", "Light", "Classic" };

void ApplyTheme(Theme theme)
{
    ImGuiStyle& style = ImGui::GetStyle();

    switch (theme)
    {
    case Theme_Ksword:
        StyleColorsRedBlack();
        break;
    case Theme_Dark:
        ImGui::StyleColorsDark();
        StyleColor = ImVec4(1.00f * KSWORD_BLUE_STYLE_R / 255.0f, 1.00f * KSWORD_BLUE_STYLE_G / 255.0f, 1.00f * KSWORD_BLUE_STYLE_B / 255.0f, 0.00f);
        break;
    case Theme_Light:
        ImGui::StyleColorsLight();
		StyleColor = ImVec4(1.00f * KSWORD_BLUE_STYLE_R / 255.0f, 1.00f * KSWORD_BLUE_STYLE_G / 255.0f, 1.00f * KSWORD_BLUE_STYLE_B / 255.0f, 0.00f);
        break;
    case Theme_Classic:
        ImGui::StyleColorsClassic();
        break;
    default:
        break;
    }

    // 可选：自定义更精细的样式调整
    if (theme == Theme_Classic)
    {
        style.WindowRounding = 0.0f;
        style.ChildRounding = 0.0f;
        style.FrameRounding = 0.0f;
    }
}
void RenderBoolRow(const char* name, bool value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Text("%s", name);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextColored((value ? ImGuiColors::Green : ImGuiColors::Red), "%s", value ? "OK" : "ERR");
}
#pragma warning(disable:4996)
inline void KswordGUIShowStatus() {

    ImGui::Spacing();

    // 协议滚动区域
    ImGui::BeginChild("ProtocolScroll", ImVec2(0, 100));
    ImGui::TextUnformatted(protocolText.c_str());
    ImGui::EndChild();
    ImGui::Spacing();

    ImGui::BeginColumns("WelcomeLinkColumn", 3, 0);
    if (ImGui::Button(C("Github仓库"))) {
        ShellExecute(NULL, L"open", L"https://github.com/WangWei-CM/KSword/", NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::NextColumn();
    if (ImGui::Button(C("用户QQ群"))) {
        // 处理按钮点击事件
    }

    ImGui::NextColumn();
    if (ImGui::Button(C("开发者QQ群"))) {
        // 处理按钮点击事件
    }
    ImGui::NextColumn();
    ImGui::Spacing();
    ImGui::EndColumns();
	ImGui::BeginColumns("WelcomeDeverColumn", 3, 0);
    // 核心开发人员滚动区域
    ImGui::Text(C("核心开发人员"));
    ImGui::BeginChild("CoreDevelopersScroll", ImVec2(0, 100));
    for (const auto& dev : coreDevelopers) {
        ImGui::Text("%s", dev.c_str());
    }
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::NextColumn();
    // 贡献者滚动区域
    ImGui::Text(C("贡献者"));
    ImGui::BeginChild("ContributorsScroll", ImVec2(0, 100));
    for (const auto& contributor : contributors) {
        ImGui::Text("%s", contributor.c_str());
    }
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::NextColumn();
    // 捐赠者滚动区域
    ImGui::Text(C("捐赠者"));
    ImGui::BeginChild("DonorsScroll", ImVec2(0, 100));
    for (const auto& donor : donors) {
        ImGui::Text("%s", donor.c_str());
    }
	ImGui::EndChild();
    ImGui::EndColumns();
    // 辅助函数;
    if (ImGui::TreeNode("Themes"))
    {
        static int current_theme = Theme_Ksword;
        if (ImGui::Combo("Theme", &current_theme, ThemeNames, Theme_COUNT))
        {
            ApplyTheme((Theme)current_theme);
        }
        ImGui::TreePop();
    }

}



// 从资源加载图标并创建 DirectX 纹理
bool LoadIconTexture(HINSTANCE hInstance, IDirect3DDevice9* pDevice) {
    // 释放已有纹理
    if (g_IconTexture) {
        g_IconTexture->Release();
        g_IconTexture = nullptr;
        g_IconImTextureID = 0;
    }

    // 从资源加载图标
    HICON hIcon = (HICON)LoadImage(
        hInstance,
        MAKEINTRESOURCE(IDI_ICON1),
        IMAGE_ICON,
        100, 100,  // 指定图标尺寸为 30x30
        LR_DEFAULTCOLOR
    );

    if (!hIcon) {
        return false;
    }

    // 获取图标信息
    ICONINFO iconInfo;
    if (!GetIconInfo(hIcon, &iconInfo)) {
        DestroyIcon(hIcon);
        return false;
    }

    // 获取位图信息
    BITMAP bm;
    GetObject(iconInfo.hbmColor, sizeof(bm), &bm);

    // 创建临时表面用于复制图标数据
    IDirect3DSurface9* pSurface = nullptr;
    pDevice->CreateOffscreenPlainSurface(
        bm.bmWidth, bm.bmHeight,
        D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
        &pSurface, nullptr
    );

    if (!pSurface) {
        DestroyIcon(hIcon);
        DeleteObject(iconInfo.hbmColor);
        DeleteObject(iconInfo.hbmMask);
        return false;
    }

    // 将图标复制到表面
    HDC hdc = GetDC(NULL);
    HDC hdcSurface = NULL;
    pSurface->GetDC(&hdcSurface);
    DrawIconEx(hdcSurface, 0, 0, hIcon, bm.bmWidth, bm.bmHeight, 0, NULL, DI_NORMAL);
    pSurface->ReleaseDC(hdcSurface);
    ReleaseDC(NULL, hdc);

    // 创建纹理
    pDevice->CreateTexture(
        bm.bmWidth, bm.bmHeight,
        1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED,
        &g_IconTexture, nullptr
    );

    if (g_IconTexture) {
        // 将表面数据复制到纹理
        IDirect3DSurface9* pTextureSurface = nullptr;
        g_IconTexture->GetSurfaceLevel(0, &pTextureSurface);
        D3DXLoadSurfaceFromSurface(
            pTextureSurface, nullptr, nullptr,
            pSurface, nullptr, nullptr,
            D3DX_FILTER_NONE, 0
        );
        pTextureSurface->Release();

        // 获取 ImGui 可用的纹理 ID
        g_IconImTextureID = (ImTextureID)g_IconTexture;
    }

    // 清理资源
    pSurface->Release();
    DestroyIcon(hIcon);
    DeleteObject(iconInfo.hbmColor);
    DeleteObject(iconInfo.hbmMask);

    return g_IconTexture != nullptr;
}
