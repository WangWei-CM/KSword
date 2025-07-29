
#include <d3d9.h>
#include <gdiplus.h> // 需链接gdiplus.lib
#pragma comment(lib, "gdiplus.lib")
#include "KswordTotalHead.h"
#include <winsock.h>
#include "TextEditor/TextEditor.h"
//
bool isGUI;
ImFont* LOGOfont;
TextEditor m_editor;
WorkProgressManager kItem;
//日志
bool KswordShowLogWindow = 1;
bool KswordShowPointerWindow = 1;
bool KswordShowNotpadWindow = 1;
bool KswordShowToolBar = 1;
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
int KswordStyle;
bool showSaveDialog = false;
bool showOpenDialog = false;
std::string currentFilePath;

extern IDirect3DTexture9* g_IconTexture;
extern ImTextureID g_IconImTextureID ;

// 从资源加载图标并创建 DirectX 纹理
bool LoadIconTexture(HINSTANCE hInstance, IDirect3DDevice9* pDevice);
bool ExtractGUIINIResourceToFile();
bool DeleteReleasedGUIINIFile();
bool DeleteReleasedD3DX9DLLFile();
// Data
static LPDIRECT3D9              g_pD3D = nullptr;
static LPDIRECT3DDEVICE9        g_pd3dDevice = nullptr;
static bool                     g_DeviceLost = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static D3DPRESENT_PARAMETERS    g_d3dpp = {};
HWND MainWindow;
// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void ResetDevice();



bool Ksword_main_should_exit = false;
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#ifdef KSWORD_WITH_COMMAND
extern int KSCMDmain(int, char* []);
#endif
#include <D3dx9tex.h>
#pragma comment(lib, "D3dx9")



// Simple helper function to load an image into a DX9 texture with common settings
#ifdef KSWORD_GUI_USE_BACKGROUND
bool LoadTextureFromFile(const char* filename, PDIRECT3DTEXTURE9* out_texture, int* out_width, int* out_height)
{
    // Load texture from disk
    PDIRECT3DTEXTURE9 texture;
    // 使用支持alpha的格式
    HRESULT hr = D3DXCreateTextureFromFileExA(
        g_pd3dDevice,
        filename,
        D3DX_DEFAULT, D3DX_DEFAULT, // 使用原始尺寸
        1,                          // Mip层级
        0,                          // 使用标志
        D3DFMT_A8R8G8B8,            // 强制使用带alpha的格式[6](@ref)
        D3DPOOL_MANAGED,
        D3DX_DEFAULT,
        D3DX_DEFAULT,
        0,                          // 颜色键（透明色）
        NULL,
        NULL,
        &texture
    );

    if (hr != S_OK)
        return false;

    // Retrieve description of the texture surface so we can access its size
    D3DSURFACE_DESC my_image_desc;
    texture->GetLevelDesc(0, &my_image_desc);
    *out_texture = texture;
    *out_width = (int)my_image_desc.Width;
    *out_height = (int)my_image_desc.Height;
    return true;
}
#endif
int my_image_width = 1920;
int my_image_height = 1080;
PDIRECT3DTEXTURE9 my_texture = NULL;
bool ret;

// 全局日志容器
// 使用示例
Logger kLog;





int GUImain(int argc, char* argv[]);
int main(int argc, char* argv[]) {
#ifdef KSWORD_WITH_COMMAND
    //bool isLeftCtrl = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0;
    bool isLeftCapital = (GetAsyncKeyState(VK_CAPITAL) & 0x8000) != 0;
    //bool isTab = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
    if (/*isLeftCtrl ||*/ isLeftCapital/* || isTab*/){
        //如果左CTRL按下那么启动命令行版本
        return KSCMDmain(argc, argv);
    } else {
        //就算没有按下，带有以下参数的被视为命令行版本
        if ((strcmp(argv[0], "998") == 0)||
        ((argc == 1)&&((strcmp(argv[0], "sos") == 0)||
                        (strcmp(argv[0], "999") == 0)||
                        (strcmp(argv[0], "211") == 0)||
                        (strcmp(argv[0], "top") == 0) ||
                        (strcmp(argv[0], "topsock") == 0)))
        ||((argc == 2)&&((strcmp(argv[0], "1") == 0) ||
                        (strcmp(argv[0], "7") == 0) ||
                        (strcmp(argv[0], "2") == 0)))
        ||(strcmp(argv[0], "sethc.exe") == 0))
            return KSCMDmain(argc, argv);
        else
            //否则启动图形界面版本
#endif
        return GUImain(argc, argv);
#ifdef KSWORD_WITH_COMMAND

    }
#endif
}

int GUImain(int argc, char* argv[]) {    // Create application window
    //ImGui_ImplWin32_EnableDpiAwareness();
        // 从资源中加载图标
    
    SetDllDirectoryW(L".");
    HRSRC D3DX9DLLResource = FindResource(NULL, MAKEINTRESOURCE(IDR_DLL1), _T("DLL"));
    if (D3DX9DLLResource == NULL)
    {
        DWORD err = GetLastError();
        return false;
    }

    // 加载资源
    HGLOBAL hResourceD3DX9 = LoadResource(NULL, D3DX9DLLResource);
    if (hResourceD3DX9 == NULL)
        return false;

    // 锁定资源获取数据指针
    LPVOID pResourceData = LockResource(hResourceD3DX9);
    if (pResourceData == NULL)
        return false;

    // 获取资源大小
    DWORD dwResourceSize = SizeofResource(NULL, D3DX9DLLResource);
    if (dwResourceSize == 0)
        return false;

    // 获取程序当前目录
    TCHAR szExePath[MAX_PATH];
    if (GetModuleFileName(NULL, szExePath, MAX_PATH) == 0)
        return false;

    // 提取目录路径
    TCHAR* pLastSlash = _tcsrchr(szExePath, _T('\\'));
    if (pLastSlash == NULL)
        return false;
    *pLastSlash = _T('\0');

    // 构建目标DLL路径
    std::basic_string<TCHAR> strOutputPath = std::basic_string<TCHAR>(szExePath) + _T("\\d3dx9_43.dll");

    // 写入DLL文件（二进制模式）
    std::ofstream KswordD3DX9dllRelease(strOutputPath.c_str(), std::ios::binary);
    if (!KswordD3DX9dllRelease.is_open())
        return false;

    KswordD3DX9dllRelease.write(static_cast<const char*>(pResourceData), dwResourceSize);
    if (!KswordD3DX9dllRelease.good())
    {
        KswordD3DX9dllRelease.close();
        DeleteFile(strOutputPath.c_str()); // 写入失败时删除不完整文件
        return false;
    }

    KswordD3DX9dllRelease.close();
    HMODULE hD3DX9 = LoadLibrary(_T("d3dx9_43.dll"));
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) {
        std::cerr << "获取标准输出句柄失败!" << std::endl;
        return 1;
    }

    // 设置控制台缓冲区大小为40x12
    SMALL_RECT srctWindow = { 0, 0, 39, 11 }; // 坐标从0开始，宽度40和高度12对应的最大索引是39和11
    if (!SetConsoleWindowInfo(hOut, TRUE, &srctWindow)) {
        std::cerr << "设置窗口信息失败!" << std::endl;
        return 1;
    }

    COORD coord = { 40, 12 }; // 缓冲区大小设置为40列12行
    if (!SetConsoleScreenBufferSize(hOut, coord)) {
        std::cerr << "设置缓冲区大小失败!" << std::endl;
        return 1;
    }

    // 获取当前控制台字体信息
    CONSOLE_FONT_INFOEX cfi;
    cfi.cbSize = sizeof(cfi);
    if (!GetCurrentConsoleFontEx(hOut, FALSE, &cfi)) {
        std::cerr << "获取当前字体信息失败!" << std::endl;
        return 1;
    }

    // 获取字体尺寸
    int fontWidth = cfi.dwFontSize.X;
    int fontHeight = cfi.dwFontSize.Y;

    // 获取屏幕尺寸
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    // 计算控制台窗口像素尺寸
    int windowWidth = 40 * fontWidth;
    int windowHeight = 12 * fontHeight;

    // 计算窗口左上角位置（居中）
    int windowX = (screenWidth - windowWidth) / 2;
    int windowY = (screenHeight - windowHeight) / 2;

    // 获取控制台窗口句柄
    HWND Consolehwnd = GetConsoleWindow();
    if (!Consolehwnd) {
        std::cerr << "获取控制台窗口句柄失败!" << std::endl;
        return 1;
    }

    // 隐藏标题栏 (WS_POPUP 样式没有标题栏和边框)
    LONG Consolestyle = GetWindowLong(Consolehwnd, GWL_STYLE);
    Consolestyle &= ~WS_OVERLAPPEDWINDOW; // 移除标准窗口样式
    Consolestyle |= WS_POPUP;             // 添加弹出窗口样式
    SetWindowLong(Consolehwnd, GWL_STYLE, Consolestyle);

    // 应用新样式
    SetWindowPos(
        Consolehwnd, NULL, windowX, windowY,
        windowWidth, windowHeight,
        SWP_FRAMECHANGED | SWP_SHOWWINDOW
    );

    // 设置白色背景 (黑底白字)
    WORD textAttribute = BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE;
    if (!SetConsoleTextAttribute(hOut, textAttribute)) {
        std::cerr << "设置颜色属性失败!" << std::endl;
        return 1;
    }

    // 清除屏幕并填充白色背景
    COORD origin = { 0, 0 };
    DWORD charsWritten;
    FillConsoleOutputCharacter(hOut, ' ', 40 * 12, origin, &charsWritten);
    FillConsoleOutputAttribute(hOut, textAttribute, 40 * 12, origin, &charsWritten);
    system("cls");
    for (int i = 1; i <= 40 * 12; i++) {
        std::cout << " ";
    }
    SetCursor(0, 11);
    std::cout << "[ * ]Ksword5.0 开发者版本 正在启动……"; Sleep(50);
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    HICON hIcon = (HICON)LoadImage(
        GetModuleHandle(NULL),
        MAKEINTRESOURCE(IDI_ICON1),
        IMAGE_ICON,
        256,
        256,
        LR_DEFAULTCOLOR
    );
    if (!hIcon) {
        printf("加载图标失败！错误代码: %d\n", GetLastError());
        return -2;
    }

    // 获取图标尺寸
    ICONINFO iconInfo;
    if (!GetIconInfo(hIcon, &iconInfo)) {
        printf("获取图标信息失败！\n");
        DestroyIcon(hIcon);
        return -3;
    }

    // 获取图标宽度和高度
    BITMAP bm;
    GetObject(iconInfo.hbmColor, sizeof(bm), &bm);
    int originalWidth = bm.bmWidth;
    int originalHeight = bm.bmHeight;

    // 释放图标信息中创建的位图
    if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
    if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);

    // 计算放大5倍后的尺寸
    int scaledWidth = originalWidth * 0.5;
    int scaledHeight = originalHeight * 0.55;

    // 获取控制台窗口客户区尺寸
    RECT rect;
    GetClientRect(GetConsoleWindow(), &rect);
    int clientWidth = rect.right - rect.left;
    int clientHeight = rect.bottom - rect.top;

    // 计算图标居中显示的位置
    int x = (clientWidth - scaledWidth) / 2 ;
    int y = 24;
    HDC hdc = GetDC(GetConsoleWindow());
    if (!hdc) {
        printf("获取设备上下文失败！\n");
        DestroyIcon(hIcon);
        return -4;
    }

    // 创建GDI+ Graphics对象
    {
        Gdiplus::Graphics graphics(hdc);

        // 设置高质量插值模式
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

        // 从HICON创建GDI+ Bitmap
        Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromHICON(hIcon);
        if (!pBitmap || pBitmap->GetLastStatus() != Gdiplus::Ok) {
            printf("从图标创建位图失败！\n");
            ReleaseDC(GetConsoleWindow(), hdc);
            DestroyIcon(hIcon);
            return -5;
        }

        // 使用GDI+高质量缩放绘制图标
        graphics.DrawImage(pBitmap, x, y, scaledWidth, scaledHeight);

        // 清理资源
        delete pBitmap;
    }
    ReleaseDC(GetConsoleWindow(), hdc);
    DestroyIcon(hIcon);

    //Ksword逻辑初始化
    isGUI = TRUE;
    KEnviProb();
    KswordGUIInit();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(
        wc.lpszClassName,
        L"Ksword5Internal",
        WS_OVERLAPPEDWINDOW, 
        100, 100,
        100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    MainWindow = hwnd;
    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Show the window
    //::ShowWindow(hwnd, SW_HIDE);
    ::UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ExtractGUIINIResourceToFile();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.IniFilename = "KswordGUI.ini";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows
    //io.ConfigViewportsNoAutoMerge = true;
    //io.ConfigViewportsNoTaskBarIcon = true;

    // Setup Dear ImGui style
    //ImGui::StyleColorsDark();
    //StyleColorsRedBlack(nullptr);
    ImGui::StyleColorsLight();
    KswordStyle = Light;
    StyleColor = ImVec4(1.00f * KSWORD_BLUE_STYLE_R / 255.0f, 1.00f * KSWORD_BLUE_STYLE_G / 255.0f, 1.00f * KSWORD_BLUE_STYLE_B / 255.0f, 0.00f);
    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f; // 全局禁用圆角
    // 消除水平和垂直方向间距
    //style.ItemSpacing = ImVec2(0, 5);       // 控件间外部间距归零[7](@ref)
    //style.ItemInnerSpacing = ImVec2(0, 5);  // 控件内元素间距归零[7](@ref)
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(g_pd3dDevice);

    // Load Fonts
    // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return a nullptr. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    //io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\msyh.ttc", 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    LOGOfont = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\consola.ttf", 72.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    IM_ASSERT(font != nullptr);

    // Our state
    bool show_demo_window = true;
    bool show_another_window = false;
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    // Main loop
    bool done = false;
#ifdef KSWORD_GUI_USE_BACKGROUND
    // 加载背景纹理
    ret = LoadTextureFromFile("C:\\Users\\WangWei_CM\\Desktop\\Ksword5.0\\x64\\Release\\fengyuanwanye.jpg", &my_texture, &my_image_width, &my_image_height);
#endif
    m_editor.SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
    m_editor.SetText("// Welcome to Text Editor\n#include <iostream>\n\nint main() {\n  std::cout << \"Hello World!\\n\";\n  return 0;\n}");


    LoadIconTexture(wc.hInstance, g_pd3dDevice);

    bool firstRun = 1;
    
    while (!done)
    {
        // Poll and handle messages (inputs, window resize, etc.)
        // See the WndProc() function below for our to dispatch events to the Win32 backend.
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Handle lost D3D9 device
        if (g_DeviceLost)
        {
            HRESULT hr = g_pd3dDevice->TestCooperativeLevel();
            if (hr == D3DERR_DEVICELOST)
            {
                ::Sleep(10);
                continue;
            }
            if (hr == D3DERR_DEVICENOTRESET)
                ResetDevice();
            g_DeviceLost = false;
        }

        // Handle window resize (we don't resize directly in the WM_SIZE handler)
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            g_d3dpp.BackBufferWidth = g_ResizeWidth;
            g_d3dpp.BackBufferHeight = g_ResizeHeight;
            g_ResizeWidth = g_ResizeHeight = 0;
            ResetDevice();
        }

        //ImGui::SetNextWindowClass(nullptr);
        // Start the Dear ImGui frame

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        bool window_collapsed = 0;
        ImGui::NewFrame(); 
        auto colorLuminance = [](const ImVec4& color) {//计算文本编辑器应该用深色还是浅色模式
            return 0.299f * color.x + 0.587f * color.y + 0.114f * color.z;
            };

        ImVec4 bgColor = ImGui::ColorConvertU32ToFloat4(
            ImGui::GetColorU32(ImGuiCol_WindowBg)
        );
        float luminance = colorLuminance(bgColor);
        if(luminance < 0.5f)m_editor.SetPalette(
            TextEditor::GetDarkPalette());
		else m_editor.SetPalette(TextEditor::GetLightPalette());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f); {
            Ksword5Title();
            if(KswordShowPointerWindow)
            PointerWindow();
            kItem.Render();
            if (KswordShowNotpadWindow) m_editor.Render("TextEditor", ImVec2(0, 0), true);
            
            
            ImGui::Begin("Ksword 5 Dever",nullptr,
                //ImGuiWindowFlags_NoCollapse |
                //ImGuiWindowFlags_NoBackground /*|*/
            //ImGuiWindowFlags_NoSavedSettings
                0
            );
            // 绘制背景
            // 在ImGui渲染循环中
            ImVec2 window_size = ImGui::GetContentRegionAvail();

            //if (my_texture) {
            //    // 计算原始图片宽高比
            //    float image_aspect = (float)my_image_width / (float)my_image_height;

            //    // 计算窗口宽高比
            //    float window_aspect = window_size.x / window_size.y;

            //    // 计算自适应尺寸（保持纵横比）
            //    ImVec2 display_size;
            //    if (window_aspect > image_aspect) {
            //        // 窗口更宽，按高度填充
            //        display_size.y = window_size.y;
            //        display_size.x = window_size.y * image_aspect;
            //    }
            //    else {
            //        // 窗口更高，按宽度填充
            //        display_size.x = window_size.x;
            //        display_size.y = window_size.x / image_aspect;
            //    }

            //    // 计算居中位置
            //    ImVec2 pos = ImVec2(
            //        (window_size.x - display_size.x) * 0.5f,
            //        (window_size.y - display_size.y) * 0.5f
            //    );
            //    ImGui::Image((ImTextureID)(intptr_t)my_texture, display_size);
            //}
            //if (my_texture) {
            //    ImVec4 tint_color = ImVec4(1.0f, 1.0f, 1.0f, 0.5f); // RGB=白色, Alpha=0.3
            //    ImVec2 window_size = ImGui::GetContentRegionAvail();
            //    window_size.x += 40;
            //    window_size.y += 40;
            //    // 计算图片原始宽高比
            //    float image_aspect = (float)my_image_width / (float)my_image_height;

            //    // 计算窗口宽高比
            //    float window_aspect = window_size.x / window_size.y;

            //    // 计算覆盖填充的裁剪尺寸
            //    ImVec2 display_size;
            //    if (window_aspect > image_aspect) {
            //        // 窗口更宽 -> 按宽度填充，高度裁剪
            //        display_size.x = window_size.x;
            //        display_size.y = window_size.x / image_aspect;  // 图片高度需要扩展

            //        // 超出窗口高度的部分会被裁剪
            //    }
            //    else {
            //        // 窗口更高 -> 按高度填充，宽度裁剪
            //        display_size.y = window_size.y;
            //        display_size.x = window_size.y * image_aspect;  // 图片宽度需要扩展

            //        // 超出窗口宽度的部分会被裁剪
            //    }

            //    // 计算偏移量（使裁剪居中）
            //    ImVec2 offset = ImVec2(
            //        (display_size.x - window_size.x) * 0.5f,
            //        (display_size.y - window_size.y) * 0.5f
            //    );

            //    // 反转偏移量得到UV坐标
            //    ImVec2 uv0 = ImVec2( 
            //        offset.x > 0 ? offset.x / display_size.x : 0.0f,
            //        offset.y > 0 ? offset.y / display_size.y : 0.0f
            //    );

            //    ImVec2 uv1 = ImVec2(
            //        offset.x > 0 ? 1.0f - offset.x / display_size.x : 1.0f,
            //        offset.y > 0 ? 1.0f - offset.y / display_size.y : 1.0f
            //    );

            //    // 使用完整的窗口大小绘制图片（自动裁剪超出部分）
            //    ImGui::SetCursorPos(ImVec2(0, 0));
            //    ImGui::Image(
            //        (ImTextureID)(intptr_t)my_texture,
            //        window_size,   // 使用窗口尺寸（裁剪效果来源于此）
            //        uv0,           // UV起始坐标（定义裁剪区域）
            //        uv1,            // UV结束坐标（定义裁剪区域）
            //        tint_color,
            //        ImVec4(0, 0, 0, 0)
            //    );
            //}
            ImGui::SetCursorPos(ImVec2(0, 30)); // 重置到同一位置
            
            if (ImGui::BeginTabBar(C("MainTabs")))
            {
            if (ImGui::BeginTabItem(C("欢迎")))
                {
                     KswordLogo5();
                     KswordGUIShowStatus();
                    ImGui::EndTabItem();
                }
            if (ImGui::BeginTabItem(C("进程")))
            {
                KswordGUIProcess();
            }
            if (ImGui::BeginTabItem(C("监控")))
            {
                KswordMonitorMain();
            }
            // Spy++ 标签
            if (ImGui::BeginTabItem("Test"))
            {
                ImGui::Text(C("增加日志信息"));
                if (ImGui::Button("Info"))kLog.Add(Info, C("测试消息类型Info")); ImGui::SameLine();
                if (ImGui::Button("Warn"))kLog.Add(Warn, C("测试消息类型Warn")); ImGui::SameLine();
                if (ImGui::Button("Err"))kLog.Add(Err, C("测试消息类型Err")); ImGui::SameLine();
                
                ImGui::EndTabItem();
            }

            // Exit 标签（带退出功能）
            if (ImGui::BeginTabItem("Exit"))
            {
                Ksword_main_should_exit = true;
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
            if (Ksword_main_should_exit)
                done = 1;
            ImGui::End();

            //日志窗口
            if (KswordShowLogWindow) {
                kLog.Draw();
            }
        }

        KswordToolBar();
        // Rendering
        ImGui::PopStyleVar();
        if (firstRun) {
            firstRun = 0;
            HideWindow();
        }
        ImGui::EndFrame();
        g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        D3DCOLOR clear_col_dx = D3DCOLOR_RGBA((int)(clear_color.x * clear_color.w * 255.0f), (int)(clear_color.y * clear_color.w * 255.0f), (int)(clear_color.z * clear_color.w * 255.0f), (int)(clear_color.w * 255.0f));
        g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clear_col_dx, 1.0f, 0);
        if (g_pd3dDevice->BeginScene() >= 0)
        {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_pd3dDevice->EndScene();
        }

        // Update and Render additional Platform Windows
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        HRESULT result = g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
        if (result == D3DERR_DEVICELOST)
            g_DeviceLost = true;
    }

    // Cleanup
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_IconTexture) {
        g_IconTexture->Release();
        g_IconTexture = nullptr;
        g_IconImTextureID = 0;
    }//释放图标资源代码
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
	Gdiplus::GdiplusShutdown(gdiplusToken);
    DeleteReleasedGUIINIFile();
    FreeLibrary(hD3DX9);
    DeleteReleasedD3DX9DLLFile();
    return 0;

}



// Helper functions
bool CreateDeviceD3D(HWND hWnd)
{
    if ((g_pD3D = Direct3DCreate9(D3D_SDK_VERSION)) == nullptr)
        return false;

    // Create the D3DDevice
    ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
    g_d3dpp.Windowed = TRUE;
    g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN; // Need to use an explicit format with alpha if needing per-pixel alpha composition.
    g_d3dpp.EnableAutoDepthStencil = TRUE;
    g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
    g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;           // Present with vsync
    //g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;   // Present without vsync, maximum unthrottled framerate
    if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_d3dpp, &g_pd3dDevice) < 0)
        return false;

    return true;
}

void CleanupDeviceD3D()
{
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    if (g_pD3D) { g_pD3D->Release(); g_pD3D = nullptr; }
}

void ResetDevice()
{
    ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
    if (hr == D3DERR_INVALIDCALL)
        IM_ASSERT(0);
    ImGui_ImplDX9_CreateDeviceObjects();
}

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0 // From Windows SDK 8.1+ headers
#endif

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    case WM_DPICHANGED:
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DpiEnableScaleViewports)
        {
            //const int dpi = HIWORD(wParam);
            //printf("WM_DPICHANGED to %d (%.0f%%)\n", dpi, (float)dpi / 96.0f * 100.0f);
            const RECT* suggested_rect = (RECT*)lParam;
            ::SetWindowPos(hWnd, nullptr, suggested_rect->left, suggested_rect->top, suggested_rect->right - suggested_rect->left, suggested_rect->bottom - suggested_rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        break;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
