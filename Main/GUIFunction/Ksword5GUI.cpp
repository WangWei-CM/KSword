#include "../KswordTotalHead.h"
#include "GUIfunction.h"
std::string protocolText = C("�ð汾�û�ʹ��Э��\n"
"���������Ӹ����Э������...\n"
"Э�����ϸ��Ϣ���������������չ...\n"
"�����Э������...\n");

std::vector<std::string> coreDevelopers = { "A.b.c", "D.e.f", "G.h.i", "������Ŀ�����Ա..." };
std::vector<std::string> contributors = { C("������1"), C("������2")};
std::vector<std::string> donors = { C("������1"), C("������2") };

IDirect3DTexture9* g_IconTexture = nullptr;
ImTextureID g_IconImTextureID = 0;


inline void KswordLogo5() {
	//ImGui::Text("   _  __                                      _   ____         ___  ");
	//ImGui::Text("  | |/ /  ___  __      __   ___    _ __    __| | | ___|       / _ \\ ");
	//ImGui::Text("  | ' /  / __| \\ \\ /\\ / /  / _ \\  | '__|  / _` | |___ \\      | | | |");
	//ImGui::Text("  | . \\  \\__ \\  \\ V  V /  | (_) | | |    | (_| |  ___) |  _  | |_| |");
	//ImGui::Text("  |_|\\_\\ |___/   \\_/\\_/    \\___/  |_|     \\__,_| |____/  (_)  \\___/ ");

    // ��ȡ��ǰ����λ�úʹ�С
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();

    // ����ͼ���ڴ����ڵľ������꣨������ (5, 5) λ�ã�
    ImVec2 iconPos = ImVec2(windowPos.x + 10, windowPos.y + 60);
    ImVec2 iconSize = ImVec2(60, 60);  // ͼ��ߴ�

    // ʹ�� ImGui �Ļ����б���ָ��λ�û���ͼ��
    ImGui::GetWindowDrawList()->AddImage(
        g_IconImTextureID,
        iconPos,
        ImVec2(iconPos.x + iconSize.x, iconPos.y + iconSize.y),
        ImVec2(0, 0),  // UV �������
        ImVec2(1, 1)   // UV �����յ�
    );

    ImGui::PushFont(LOGOfont);
    ImGui::Text("  Ksword5.0");
    ImGui::PopFont();
	ImGui::Text("Ksword Release 5.0 | Based on ImGUI & Ksword Framework");
	ImGui::Text("Deved By WangWei_CM,LianYou_Alex,Ksword Dever Team,etc.");
	return;
}

// ���ļ�������������ö�ٺͺ���
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

    // ��ѡ���Զ������ϸ����ʽ����
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

    // Э���������
    ImGui::BeginChild("ProtocolScroll", ImVec2(0, 100));
    ImGui::TextUnformatted(protocolText.c_str());
    ImGui::EndChild();
    ImGui::Spacing();

    ImGui::BeginColumns("WelcomeLinkColumn", 3, 0);
    if (ImGui::Button(C("Github�ֿ�"))) {
        ShellExecute(NULL, L"open", L"https://github.com/WangWei-CM/KSword/", NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::NextColumn();
    if (ImGui::Button(C("�û�QQȺ"))) {
        // ����ť����¼�
    }

    ImGui::NextColumn();
    if (ImGui::Button(C("������QQȺ"))) {
        // ����ť����¼�
    }
    ImGui::NextColumn();
    ImGui::Spacing();
    ImGui::EndColumns();
	ImGui::BeginColumns("WelcomeDeverColumn", 3, 0);
    // ���Ŀ�����Ա��������
    ImGui::Text(C("���Ŀ�����Ա"));
    ImGui::BeginChild("CoreDevelopersScroll", ImVec2(0, 100));
    for (const auto& dev : coreDevelopers) {
        ImGui::Text("%s", dev.c_str());
    }
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::NextColumn();
    // �����߹�������
    ImGui::Text(C("������"));
    ImGui::BeginChild("ContributorsScroll", ImVec2(0, 100));
    for (const auto& contributor : contributors) {
        ImGui::Text("%s", contributor.c_str());
    }
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::NextColumn();
    // �����߹�������
    ImGui::Text(C("������"));
    ImGui::BeginChild("DonorsScroll", ImVec2(0, 100));
    for (const auto& donor : donors) {
        ImGui::Text("%s", donor.c_str());
    }
	ImGui::EndChild();
    ImGui::EndColumns();
    // ��������;
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



// ����Դ����ͼ�겢���� DirectX ����
bool LoadIconTexture(HINSTANCE hInstance, IDirect3DDevice9* pDevice) {
    // �ͷ���������
    if (g_IconTexture) {
        g_IconTexture->Release();
        g_IconTexture = nullptr;
        g_IconImTextureID = 0;
    }

    // ����Դ����ͼ��
    HICON hIcon = (HICON)LoadImage(
        hInstance,
        MAKEINTRESOURCE(IDI_ICON1),
        IMAGE_ICON,
        100, 100,  // ָ��ͼ��ߴ�Ϊ 30x30
        LR_DEFAULTCOLOR
    );

    if (!hIcon) {
        return false;
    }

    // ��ȡͼ����Ϣ
    ICONINFO iconInfo;
    if (!GetIconInfo(hIcon, &iconInfo)) {
        DestroyIcon(hIcon);
        return false;
    }

    // ��ȡλͼ��Ϣ
    BITMAP bm;
    GetObject(iconInfo.hbmColor, sizeof(bm), &bm);

    // ������ʱ�������ڸ���ͼ������
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

    // ��ͼ�긴�Ƶ�����
    HDC hdc = GetDC(NULL);
    HDC hdcSurface = NULL;
    pSurface->GetDC(&hdcSurface);
    DrawIconEx(hdcSurface, 0, 0, hIcon, bm.bmWidth, bm.bmHeight, 0, NULL, DI_NORMAL);
    pSurface->ReleaseDC(hdcSurface);
    ReleaseDC(NULL, hdc);

    // ��������
    pDevice->CreateTexture(
        bm.bmWidth, bm.bmHeight,
        1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED,
        &g_IconTexture, nullptr
    );

    if (g_IconTexture) {
        // ���������ݸ��Ƶ�����
        IDirect3DSurface9* pTextureSurface = nullptr;
        g_IconTexture->GetSurfaceLevel(0, &pTextureSurface);
        D3DXLoadSurfaceFromSurface(
            pTextureSurface, nullptr, nullptr,
            pSurface, nullptr, nullptr,
            D3DX_FILTER_NONE, 0
        );
        pTextureSurface->Release();

        // ��ȡ ImGui ���õ����� ID
        g_IconImTextureID = (ImTextureID)g_IconTexture;
    }

    // ������Դ
    pSurface->Release();
    DestroyIcon(hIcon);
    DeleteObject(iconInfo.hbmColor);
    DeleteObject(iconInfo.hbmMask);

    return g_IconTexture != nullptr;
}
