#include "KswordTotalHead.h"

static bool Ksword_tool_bar_already_top = 0;
static bool first_done = 1;
static TCHAR szAppName[] = _T("FlatWindowApp");
static HWND         hwnd;
       bool isGuiSuspended = false;
inline void KswordToolBar() {
    // ���ô���λ�úʹ�С
    //const ImGuiViewport* viewport = ImGui::GetMainViewport();
    //ImVec2 window_pos = ImVec2(viewport->WorkPos.x + 10, viewport->WorkPos.y + 10);
    ImVec2 window_size = ImVec2(200, 150);

    // ���ô��ڱ�־ - �����벼�ֱ���
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoResize |
        //ImGuiWindowFlags_NoSavedSettings |
        //ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking |
        //ImGuiWindowFlags_Tooltip |
        //ImGuiWindowFlags_TopMost|
        //ImGuiWindowFlags_NoFocusOnAppearing /*|*/  // ��ֹ�Զ���ȡ����
        //ImGuiWindowFlags_NoDecoration /*|*/       // �ޱ��������ޱ߿�
        ImGuiWindowFlags_NoBringToFrontOnFocus; // ��ֹ���������ڸ���
        ;

        //ImGui::SetNextWindowFocus();
    //ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
    //ImGuiViewport* viewport = ImGui::GetWindowViewport(); // ��ǰ���������ӿ�
    //HWND hwnd = (HWND)viewport->PlatformHandle;          // ת��Ϊ Windows ���
    //if (!Ksword_tool_bar_already_top) {
    //    //2. ����ϵͳ API �ö�
    //    SetWindowPos(
    //        hwnd,
    //        HWND_TOPMOST,     // �ö���־
    //        0, 0, 0, 0,       // ����ԭλ�úͳߴ�
    //        SWP_NOMOVE | SWP_NOSIZE
    //    );
    //    Ksword_tool_bar_already_top=1;
    //}
    // ��ʼ���ƴ���
    if (ImGui::Begin("Control Panel", nullptr, window_flags))
    {

        ImGuiViewport* viewport = ImGui::GetWindowViewport(); // ��ǰ���������ӿ�
        HWND hwnd = (HWND)viewport->PlatformHandle;          // ת��Ϊ Windows ���
        if (first_done) {

            if (!SetWindowPos(::hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE
            )) {
                const std::wstring a = L"������������ʧ�ܣ�" + std::to_wstring(GetLastError());
                MessageBox(NULL, a.c_str(), szAppName, MB_ICONERROR);
            }
            //2. ����ϵͳ API �ö�
            SetWindowPos(
                hwnd,
                HWND_TOPMOST,     // �ö���־
                0, 0, 0, 0,       // ����ԭλ�úͳߴ�
                SWP_NOMOVE | SWP_NOSIZE
            );
            first_done = 0;
        }
        // 1. �˳���ť - ����
        if (ImGui::Button("Exit", ImVec2(-1, 0)))
        {
            Ksword_main_should_exit = 1;
        }

        ImGui::Separator();

        // 2. �۵��� - ���ڿ��ư�ť
        if (ImGui::CollapsingHeader("Window Controls", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // ��־���ڰ�ť
            if (ImGui::Button(KswordShowLogWindow ? "Hide Log Window" : "Show Log Window", ImVec2(-1, 0)))
            {
                KswordShowLogWindow = !KswordShowLogWindow;
            }

            // ָ�봰�ڰ�ť
            if (ImGui::Button(KswordShowPointerWindow ? "Hide Pointer Window" : "Show Pointer Window", ImVec2(-1, 0)))
            {
                KswordShowPointerWindow = !KswordShowPointerWindow;
            }

            // ���±����ڰ�ť
            if (ImGui::Button(KswordShowNotpadWindow ? "Hide Notepad Window" : "Show Notepad Window", ImVec2(-1, 0)))
            {
                KswordShowNotpadWindow = !KswordShowNotpadWindow;
            }

            // ��������ť
            if (ImGui::Button(KswordShowToolBar ? "Hide Tool Bar" : "Show Tool Bar", ImVec2(-1, 0)))
            {
                KswordShowToolBar = !KswordShowToolBar;
            }
        }
    }
    ImGui::End();
}

#include <windows.h>

// ��ťID
#define ID_BUTTON 101

// ��ť ID ����
#define ID_BTN_EXIT 1001
#define ID_BTN_RESTART 1002
#define ID_BTN_SUSPEND 1003
#define ID_BTN_CMD 1004
#define ID_BTN_EMERGENCY 1005
#define ID_BTN_SAFE_DESK 1006
#define ID_BTN_VERSION 1007

// ���ڹ��̺�������
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
// �Զ�����Ʊ�ƽ����ť��������
void DrawFlatButton(HDC hdc, const RECT& rect, const TCHAR* text, BOOL isPressed, HFONT hFont);
// ���ڹ��̺�������
LRESULT CALLBACK KswordToolBarWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// ������
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
    // ʹ�ø��ӽ�ϵͳĬ�ϡ������ı���ˢ��Ҳ�ɸ����������
    wndclass.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wndclass.lpszMenuName = NULL;
    wndclass.lpszClassName = szAppName;

    if (!RegisterClass(&wndclass))
    {
        MessageBox(NULL, _T("�ó�����Ҫ�� Windows ���������У�"),
            szAppName, MB_ICONERROR);
        return 0;
    }

    // �����̶���С���ڣ��������λ��
    int windowWidth = 350;
    int windowHeight = 190;
    int x = 200;
    int y = 200;

    hwnd = CreateWindow(szAppName, _T("Ksword�������"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, // ����������ʽ
        x, y, windowWidth, windowHeight,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    //hwnd=FindWindow(szAppName, _T("Ksword�������"));
    if (hwnd != NULL)
    {
        LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
        exStyle |= WS_EX_TOPMOST;  // ����ԭ����ʽ������ö���չ��ʽ
        SetWindowLong(hwnd, GWL_EXSTYLE, exStyle);

        // 2. ���ô����ö�
        if(!SetWindowPos(hwnd, HWND_TOPMOST, 0, 0,0,0, SWP_NOMOVE | SWP_NOSIZE
        ))MessageBox(NULL, _T("������������ʧ�ܣ�"), szAppName, MB_ICONERROR);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    else {
        MessageBox(NULL, _T("���ڴ���ʧ�ܣ�"), szAppName, MB_ICONERROR);
    }


    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return msg.wParam;
}

// ���ڹ��̺��� - ��������Ϣ
LRESULT CALLBACK KswordToolBarWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static HWND hBtnExit, hBtnRestart, hBtnSuspend, hBtnCmd, hBtnEmergency, hBtnSafeDesk, hBtnVersion;
    static HFONT hFont;  // ���ڴ洢�Զ����������

    switch (uMsg)
    {
    case WM_CREATE:
        hFont = CreateFont(
            20,                     // ����߶ȣ���λ���߼���λ���ɸ����������
            0,                      // �����ȣ�0��ʾ��ϵͳ��������߶��Զ�ƥ����ʵĿ��
            0,                      // �ַ���б�Ƕȣ���λ��ʮ��֮һ�ȣ�0��ʾ����б
            0,                      // ������б�Ƕȣ���λ��ʮ��֮һ�ȣ�0��ʾ����б
            FW_NORMAL,              // �����ϸ��FW_NORMAL��ʾ������ϸ
            FALSE,                  // �Ƿ�Ϊб�壬FALSE��ʾ����б��
            FALSE,                  // �Ƿ����»��ߣ�FALSE��ʾû���»���
            FALSE,                  // �Ƿ���ɾ���ߣ�FALSE��ʾû��ɾ����
            GB2312_CHARSET,         // �ַ�����GB2312_CHARSET��ʾʹ��GB2312�ַ���������������
            OUT_DEFAULT_PRECIS,     // �������
            CLIP_DEFAULT_PRECIS,    // �ü�����
            DEFAULT_QUALITY,        // �������
            DEFAULT_PITCH | FF_SWISS,// ���������壬FF_SWISS��ʾ��ʿ���������
            _T("΢���ź�")           // ��������
        );
        // ����������ť������Ϊ���ɼ��������� WM_PAINT ���Զ������
        hBtnExit = CreateWindow(_T("BUTTON"), _T("�˳�"),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            10, 10, 150, 35,
            hwnd, (HMENU)ID_BTN_EXIT, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        hBtnRestart = CreateWindow(_T("BUTTON"), _T("��������"),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW  ,
            170, 10, 100, 35,
            hwnd, (HMENU)ID_BTN_RESTART, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        hBtnSuspend = CreateWindow(_T("BUTTON"), _T("�������"),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            10, 60, 100, 35,
            hwnd, (HMENU)ID_BTN_SUSPEND, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        hBtnCmd = CreateWindow(_T("BUTTON"), _T("CMD"),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            120, 60, 60, 35,
            hwnd, (HMENU)ID_BTN_CMD, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        hBtnEmergency = CreateWindow(_T("BUTTON"), _T("Ӧ����ť"),      
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            190, 60, 100, 35,
            hwnd, (HMENU)ID_BTN_EMERGENCY, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        hBtnSafeDesk = CreateWindow(_T("BUTTON"), _T("��ȫ����"),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            10, 110, 100, 35,
            hwnd, (HMENU)ID_BTN_SAFE_DESK, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        hBtnVersion = CreateWindow(_T("BUTTON"), _T("Ksword5.0�����߰汾"),
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
            SetWindowText(hBtnSuspend, isGuiSuspended ? _T("�ָ�����") : _T("�������"));
            break;
        case ID_BTN_CMD:
            RunCmdAsyn("cmd.exe");
            break;
        case ID_BTN_EMERGENCY:
            MessageBox(hwnd, _T("������Ӧ����ť"), _T("��ʾ"), MB_OK);
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
            MessageBox(hwnd, _T("������Ksword5.0�����߰汾��ť"), _T("��ʾ"), MB_OK);
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
        // ���ڴ˴���Ӵ��ڱ����ȶ�������߼�
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

// �Զ�����Ʊ�ƽ����ť����ʵ��
void DrawFlatButton(HDC hdc, const RECT& rect, const TCHAR* text, BOOL isPressed, HFONT hFont)
{
    // ��ť������ɫ���ɸ���ϲ�õ�����������ǳ��ɫģ���ƽ��
    COLORREF bgColor = RGB(200, 220, 255);
    if (isPressed)
    {
        // ����ʱ������ɫ
        bgColor = RGB(180, 200, 235);
    }
    HBRUSH hBrush = CreateSolidBrush(RGB(255,255,255));
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hBrush);

    HPEN hBorderPen = CreatePen(PS_SOLID, 2, bgColor);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hBorderPen);
    // ���ư�ť����
    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);

    // �����ı���ɫ
    SetTextColor(hdc, RGB(0, 0, 0));
    // �����ı�����͸��
    SetBkMode(hdc, TRANSPARENT);
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    // �����ı�����λ�ã�������ʾ
    SIZE sz;
    GetTextExtentPoint32(hdc, text, _tcslen(text), &sz);
    int x = rect.left + (rect.right - rect.left - sz.cx) / 2;
    int y = rect.top + (rect.bottom - rect.top - sz.cy) / 2;
    if (isPressed)
    {
        // ����ʱ�ı���ƫ�ƣ�ģ�ⰴ��Ч��
        x += 1;
        y += 1;
    }
    TextOut(hdc, x, y, text, _tcslen(text));

    SelectObject(hdc, hOldBrush);
    DeleteObject(hBrush);
}