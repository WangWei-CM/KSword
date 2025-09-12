#include "../KswordTotalHead.h"
inline int KswordMainSecureDesktop(){
    if (1) {
        SECURITY_ATTRIBUTES sa;
        SECURITY_DESCRIPTOR sd;
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE); // �������з���Ȩ��

        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;

        // 2. �����������棨������ǰ���̷��ʣ�
        HDESK hSecureDesktop = CreateDesktop(
            L"KswordSecureDesktop1", nullptr, nullptr, 0,
            DESKTOP_CREATEWINDOW | DESKTOP_SWITCHDESKTOP,
            &sa // Ӧ�ð�ȫ������
        );
        if (!hSecureDesktop) {
            std::cerr << "CreateDesktop failed. Error: " << GetLastError() << std::endl;
        }

        // 3. �л���������
        if (!SetThreadDesktop(hSecureDesktop) || !SwitchDesktop(hSecureDesktop)) {
            std::cerr << "SwitchDesktop failed. Error: " << GetLastError() << std::endl;
            CloseDesktop(hSecureDesktop);
        }

        // 4. ���������������ܱ�������������������棩
        STARTUPINFO si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        std::wstring desktopName = StringToWString("KswordSecureDesktop1");
        std::wstring exePath = GetSelfPath();

        si.lpDesktop = const_cast<LPWSTR>(desktopName.c_str()); // ��ȫ�Ƴ� const

        wchar_t tmp[] = L"SecureDesktopMain";
        if (!CreateProcess(
            GetSelfPath().c_str(),
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
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }

        // 5. �ָ�Ĭ������
        HDESK hDefault = OpenDesktop(L"Default", 0, FALSE, GENERIC_ALL);
        if (hDefault) {
            SwitchDesktop(hDefault);
            CloseDesktop(hDefault);
        }
        CloseDesktop(hSecureDesktop);
        return 0;
    }

}

#include <windows.h>
#include <tchar.h>
#include <strsafe.h>

// ȫ�ֱ��������ڴ洢�����ھ��
HWND g_hWndMain;

// �Զ������ɫ������ڹ��̺���
LRESULT CALLBACK BlueBannerWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // ���ñ���Ϊ��ɫ
        RECT rect;
        GetClientRect(hwnd, &rect);
        HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 255));
        FillRect(hdc, &rect, hBrush);
        DeleteObject(hBrush);

        // ������ʾ�ı�
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, TRANSPARENT);
        HFONT hFont = CreateFont(
            16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_SWISS, _T("΢���ź�")
        );
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
        TCHAR szText[] = _T("������Щ��ʾ������\n��10���ڻ�ԭΪ��ǰ����ʾ���á�");
        SIZE szTextSize;
        GetTextExtentPoint32(hdc, szText, _tcslen(szText), &szTextSize);
        int x = (rect.right - szTextSize.cx) / 2;
        int y = (rect.bottom - szTextSize.cy) / 2;
        DrawText(hdc, szText, -1, &rect, DT_CENTER | DT_VCENTER);
        SelectObject(hdc, hOldFont);
        DeleteObject(hFont);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
}

// �����ڹ��̺���
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        // ������ɫ����Ӵ���
        HWND hBlueBanner = CreateWindowEx(
            0,
            _T("STATIC"),
            NULL,
            WS_CHILD | WS_VISIBLE | SS_CENTER ,
            0, 0, 800, 100,  // ���������λ�úʹ�С
            hwnd,
            NULL,
            ((LPCREATESTRUCT)lParam)->hInstance,
            NULL
        );
        SetWindowLongPtr(hBlueBanner, GWLP_WNDPROC, (LONG_PTR)BlueBannerWndProc);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
}

inline int KswordShowSecureFlag(){/*
    static TCHAR szAppName[] = _T("BlueBannerApp");
    HWND hwnd;
    MSG msg;
    WNDCLASS wndclass;

    wndclass.style = CS_HREDRAW | CS_VREDRAW;
    wndclass.lpfnWndProc = WndProc;
    wndclass.cbClsExtra = 0;
    wndclass.cbWndExtra = 0;
    wndclass.hInstance = GetModuleHandle(NULL);
    wndclass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
    wndclass.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wndclass.lpszMenuName = NULL;
    wndclass.lpszClassName = szAppName;

    if (!RegisterClass(&wndclass)) {
        MessageBox(NULL, _T("�����޷�ע�ᴰ���࣡"), szAppName, MB_ICONERROR);
        return 0;
    }

    hwnd = CreateWindow(szAppName, _T("My Application"),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        NULL, NULL, GetModuleHandle(NULL), NULL);

    ShowWindow(hwnd, SW_SHOW );
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return msg.wParam;*/

    return 0;


}