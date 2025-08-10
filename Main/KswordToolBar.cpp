#include "KswordTotalHead.h"

static bool Ksword_tool_bar_already_top = 0;
static bool first_done = 1;
static TCHAR szAppName[] = _T("FlatWindowApp");
static HWND         hwnd;
       bool isGuiSuspended = false;
inline void KswordToolBar() {
    // 设置窗口位置和大小
    //const ImGuiViewport* viewport = ImGui::GetMainViewport();
    //ImVec2 window_pos = ImVec2(viewport->WorkPos.x + 10, viewport->WorkPos.y + 10);
    ImVec2 window_size = ImVec2(200, 150);

    // 设置窗口标志 - 不参与布局保存
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoResize |
        //ImGuiWindowFlags_NoSavedSettings |
        //ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking |
        //ImGuiWindowFlags_Tooltip |
        //ImGuiWindowFlags_TopMost|
        //ImGuiWindowFlags_NoFocusOnAppearing /*|*/  // 防止自动获取焦点
        //ImGuiWindowFlags_NoDecoration /*|*/       // 无标题栏、无边框
        ImGuiWindowFlags_NoBringToFrontOnFocus; // 防止被其他窗口覆盖
        ;

        //ImGui::SetNextWindowFocus();
    //ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
    //ImGuiViewport* viewport = ImGui::GetWindowViewport(); // 当前窗口所属视口
    //HWND hwnd = (HWND)viewport->PlatformHandle;          // 转换为 Windows 句柄
    //if (!Ksword_tool_bar_already_top) {
    //    //2. 调用系统 API 置顶
    //    SetWindowPos(
    //        hwnd,
    //        HWND_TOPMOST,     // 置顶标志
    //        0, 0, 0, 0,       // 保留原位置和尺寸
    //        SWP_NOMOVE | SWP_NOSIZE
    //    );
    //    Ksword_tool_bar_already_top=1;
    //}
    // 开始绘制窗口
    if (ImGui::Begin("Control Panel", nullptr, window_flags))
    {

        ImGuiViewport* viewport = ImGui::GetWindowViewport(); // 当前窗口所属视口
        HWND hwnd = (HWND)viewport->PlatformHandle;          // 转换为 Windows 句柄
        if (first_done) {

            if (!SetWindowPos(::hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE
            )) {
                const std::wstring a = L"窗口属性设置失败！" + std::to_wstring(GetLastError());
                MessageBox(NULL, a.c_str(), szAppName, MB_ICONERROR);
            }
            //2. 调用系统 API 置顶
            SetWindowPos(
                hwnd,
                HWND_TOPMOST,     // 置顶标志
                0, 0, 0, 0,       // 保留原位置和尺寸
                SWP_NOMOVE | SWP_NOSIZE
            );
            first_done = 0;
        }
        // 1. 退出按钮 - 顶部
        if (ImGui::Button("Exit", ImVec2(-1, 0)))
        {
            Ksword_main_should_exit = 1;
        }

        ImGui::Separator();

        // 2. 折叠栏 - 窗口控制按钮
        if (ImGui::CollapsingHeader("Window Controls", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // 日志窗口按钮
            if (ImGui::Button(KswordShowLogWindow ? "Hide Log Window" : "Show Log Window", ImVec2(-1, 0)))
            {
                KswordShowLogWindow = !KswordShowLogWindow;
            }

            // 指针窗口按钮
            if (ImGui::Button(KswordShowPointerWindow ? "Hide Pointer Window" : "Show Pointer Window", ImVec2(-1, 0)))
            {
                KswordShowPointerWindow = !KswordShowPointerWindow;
            }

            // 记事本窗口按钮
            if (ImGui::Button(KswordShowNotpadWindow ? "Hide Notepad Window" : "Show Notepad Window", ImVec2(-1, 0)))
            {
                KswordShowNotpadWindow = !KswordShowNotpadWindow;
            }

            // 工具栏按钮
            if (ImGui::Button(KswordShowToolBar ? "Hide Tool Bar" : "Show Tool Bar", ImVec2(-1, 0)))
            {
                KswordShowToolBar = !KswordShowToolBar;
            }
        }
    }
    ImGui::End();
}

#include <windows.h>

// 按钮ID
#define ID_BUTTON 101

// 按钮 ID 定义
#define ID_BTN_EXIT 1001
#define ID_BTN_RESTART 1002
#define ID_BTN_SUSPEND 1003
#define ID_BTN_CMD 1004
#define ID_BTN_EMERGENCY 1005
#define ID_BTN_SAFE_DESK 1006
#define ID_BTN_VERSION 1007

// 窗口过程函数声明
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
// 自定义绘制扁平化按钮函数声明
void DrawFlatButton(HDC hdc, const RECT& rect, const TCHAR* text, BOOL isPressed, HFONT hFont);
// 窗口过程函数声明
LRESULT CALLBACK KswordToolBarWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// 主函数
int KswordRegToolBarWindow()
{

    MSG          msg;
    WNDCLASS     wndclass;

    wndclass.style = CS_HREDRAW | CS_VREDRAW;
    wndclass.lpfnWndProc = KswordToolBarWindowProc;
    wndclass.cbClsExtra = 0;
    wndclass.cbWndExtra = 0;
    wndclass.hInstance = NULL;
    wndclass.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1));
    wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
    // 使用更接近系统默认、清晰的背景刷，也可根据需求调整
    wndclass.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wndclass.lpszMenuName = NULL;
    wndclass.lpszClassName = szAppName;

    if (!RegisterClass(&wndclass))
    {
        MessageBox(NULL, _T("该程序需要在 Windows 环境下运行！"),
            szAppName, MB_ICONERROR);
        return 0;
    }

    // 创建固定大小窗口，计算居中位置
    int windowWidth = 350;
    int windowHeight = 190;
    int x = 200;
    int y = 200;

    hwnd = CreateWindow(szAppName, _T("Ksword控制面板"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, // 更清晰的样式
        x, y, windowWidth, windowHeight,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    //hwnd=FindWindow(szAppName, _T("Ksword控制面板"));
    if (hwnd != NULL)
    {
        LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
        exStyle |= WS_EX_TOPMOST;  // 保留原有样式并添加置顶扩展样式
        SetWindowLong(hwnd, GWL_EXSTYLE, exStyle);

        // 2. 设置窗口置顶
        if(!SetWindowPos(hwnd, HWND_TOPMOST, 0, 0,0,0, SWP_NOMOVE | SWP_NOSIZE
        ))MessageBox(NULL, _T("窗口属性设置失败！"), szAppName, MB_ICONERROR);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    else {
        MessageBox(NULL, _T("窗口创建失败！"), szAppName, MB_ICONERROR);
    }


    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return msg.wParam;
}

// 窗口过程函数 - 处理窗口消息
LRESULT CALLBACK KswordToolBarWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static HWND hBtnExit, hBtnRestart, hBtnSuspend, hBtnCmd, hBtnEmergency, hBtnSafeDesk, hBtnVersion;
    static HFONT hFont;  // 用于存储自定义字体对象

    switch (uMsg)
    {
    case WM_CREATE:
        hFont = CreateFont(
            20,                     // 字体高度，单位是逻辑单位，可根据需求调整
            0,                      // 字体宽度，0表示由系统根据字体高度自动匹配合适的宽度
            0,                      // 字符倾斜角度，单位是十分之一度，0表示不倾斜
            0,                      // 字体倾斜角度，单位是十分之一度，0表示不倾斜
            FW_NORMAL,              // 字体粗细，FW_NORMAL表示正常粗细
            FALSE,                  // 是否为斜体，FALSE表示不是斜体
            FALSE,                  // 是否有下划线，FALSE表示没有下划线
            FALSE,                  // 是否有删除线，FALSE表示没有删除线
            GB2312_CHARSET,         // 字符集，GB2312_CHARSET表示使用GB2312字符集，适用于中文
            OUT_DEFAULT_PRECIS,     // 输出精度
            CLIP_DEFAULT_PRECIS,    // 裁剪精度
            DEFAULT_QUALITY,        // 输出质量
            DEFAULT_PITCH | FF_SWISS,// 间距和字体族，FF_SWISS表示瑞士风格字体族
            _T("微软雅黑")           // 字体名称
        );
        // 创建各个按钮，设置为不可见，后续在 WM_PAINT 中自定义绘制
        hBtnExit = CreateWindow(_T("BUTTON"), _T("退出"),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            10, 10, 150, 35,
            hwnd, (HMENU)ID_BTN_EXIT, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        hBtnRestart = CreateWindow(_T("BUTTON"), _T("重新启动"),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW  ,
            170, 10, 100, 35,
            hwnd, (HMENU)ID_BTN_RESTART, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        hBtnSuspend = CreateWindow(_T("BUTTON"), _T("挂起界面"),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            10, 60, 100, 35,
            hwnd, (HMENU)ID_BTN_SUSPEND, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        hBtnCmd = CreateWindow(_T("BUTTON"), _T("CMD"),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            120, 60, 60, 35,
            hwnd, (HMENU)ID_BTN_CMD, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        hBtnEmergency = CreateWindow(_T("BUTTON"), _T("应急按钮"),      
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            190, 60, 100, 35,
            hwnd, (HMENU)ID_BTN_EMERGENCY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        hBtnSafeDesk = CreateWindow(_T("BUTTON"), _T("安全桌面"),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            10, 110, 100, 35,
            hwnd, (HMENU)ID_BTN_SAFE_DESK, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        hBtnVersion = CreateWindow(_T("BUTTON"), _T("Ksword5.0开发者版本"),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            120, 110, 200, 35,
            hwnd, (HMENU)ID_BTN_VERSION, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_BTN_EXIT:
            Ksword_main_should_exit = 1;
            break;
        case ID_BTN_RESTART:
            WinExec(GetSelfPath().c_str(),SW_SHOW);
            Ksword_main_should_exit = 1;
            break;
        case ID_BTN_SUSPEND:
            isGuiSuspended = !isGuiSuspended;
            SetWindowText(hBtnSuspend, isGuiSuspended ? _T("恢复界面") : _T("挂起界面"));
            break;
        case ID_BTN_CMD:
            RunCmdAsyn("cmd.exe");
            break;
        case ID_BTN_EMERGENCY:
            MessageBox(hwnd, _T("你点击了应急按钮"), _T("提示"), MB_OK);
            break;
        case ID_BTN_SAFE_DESK:
        {
            STARTUPINFO si = { sizeof(si) };
            PROCESS_INFORMATION pi;
            wchar_t tmp[] = L"SecureJmp";
            if (!CreateProcess(
                CharToWChar(GetSelfPath().c_str()),
                //const_cast<LPWSTR>(exePath.c_str()),
                tmp,
                nullptr,
                nullptr,
                FALSE,
                CREATE_NEW_CONSOLE,
                nullptr,
                nullptr,
                &si,
                &pi
            )) {
                std::cerr << "CreateProcess failed. Error: " << GetLastError() << std::endl;
            }
            else {
                WaitForSingleObject(pi.hProcess, 10000);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }
            break;
        case ID_BTN_VERSION:
            MessageBox(hwnd, _T("你点击了Ksword5.0开发者版本按钮"), _T("提示"), MB_OK);
            break;
        }
        return 0;
#ifndef ODS_PRESSED
#define ODS_PRESSED 0x0001
#endif

    case WM_DRAWITEM:
    {
        LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
        TCHAR text[256];
        GetWindowText(pDIS->hwndItem, text, sizeof(text) / sizeof(TCHAR));
        DrawFlatButton(pDIS->hDC, pDIS->rcItem, text, (pDIS->itemState & ODS_PRESSED) != 0,hFont);

    }
    return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        // 可在此处添加窗口背景等额外绘制逻辑
        EndPaint(hwnd, &ps);
    }
    return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

// 自定义绘制扁平化按钮函数实现
void DrawFlatButton(HDC hdc, const RECT& rect, const TCHAR* text, BOOL isPressed, HFONT hFont)
{
    // 按钮背景颜色，可根据喜好调整，这里用浅蓝色模拟扁平化
    COLORREF bgColor = RGB(200, 220, 255);
    if (isPressed)
    {
        // 按下时稍深颜色
        bgColor = RGB(180, 200, 235);
    }
    HBRUSH hBrush = CreateSolidBrush(RGB(255,255,255));
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hBrush);

    HPEN hBorderPen = CreatePen(PS_SOLID, 2, bgColor);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hBorderPen);
    // 绘制按钮矩形
    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);

    // 设置文本颜色
    SetTextColor(hdc, RGB(0, 0, 0));
    // 设置文本背景透明
    SetBkMode(hdc, TRANSPARENT);
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    // 计算文本绘制位置，居中显示
    SIZE sz;
    GetTextExtentPoint32(hdc, text, _tcslen(text), &sz);
    int x = rect.left + (rect.right - rect.left - sz.cx) / 2;
    int y = rect.top + (rect.bottom - rect.top - sz.cy) / 2;
    if (isPressed)
    {
        // 按下时文本稍偏移，模拟按下效果
        x += 1;
        y += 1;
    }
    TextOut(hdc, x, y, text, _tcslen(text));

    SelectObject(hdc, hOldBrush);
    DeleteObject(hBrush);
}