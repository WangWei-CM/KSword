#ifdef KSWORD_WITH_COMMAND
#include "..\Main\KswordTotalHead.h"
using namespace std;
int KswordMain7() {
    AllocConsole();
    //Sleep(300);
    //SetConsoleOutputCP(CP_UTF8);
    FullScreen();
    SetAir(200);
    HideCursor();
    HWND hwnd = GetConsoleWindow();
    int cx = GetSystemMetrics(SM_CXSCREEN);            /* ��Ļ��� ���� */
    int cy = GetSystemMetrics(SM_CYSCREEN);            /* ��Ļ�߶� ���� */

    LONG l_WinStyle = GetWindowLong(hwnd, GWL_STYLE);   /* ��ȡ������Ϣ */
    /* ���ô�����Ϣ ��� ȡ�����������߿� */
    SetWindowLong(hwnd, GWL_STYLE, (l_WinStyle | WS_POPUP | WS_MAXIMIZE) & ~WS_CAPTION & ~WS_THICKFRAME & ~WS_BORDER);

    SetWindowPos(hwnd, HWND_TOP, 0, 0, cx, cy, 0);    //��ӵ����͸
    DWORD exStyle = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);

    // ��ӵ����͸��ʽ`exit
    exStyle |= WS_EX_TRANSPARENT;

    // Ӧ���µ���չ��ʽ
    SetWindowLong(GetConsoleWindow(), GWL_EXSTYLE, exStyle);

    // ���´�����Ӧ�ø���
    SetWindowPos(GetConsoleWindow(), nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    std::thread listenerThread(keyboardListener);
    LONG_PTR currentExStyle = GetWindowLongPtr(GetConsoleWindow(), GWL_EXSTYLE);
    SetWindowLongPtr(GetConsoleWindow(), GWL_EXSTYLE, currentExStyle | WS_EX_TOOLWINDOW);
    findrc();
    string bmptmp = localadd+"\\Kbackground.bmp";
    cout << bmptmp;
    FILE* file = NULL;
    file = fopen(bmptmp.c_str(), "r"); // ֻ����ʾbmp��ʽͼƬ
    if (!file)
    {
        printf("ͼƬ�ļ���ʧ�ܣ�\n");
        return -1;
    }

    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);

    LPBITMAPINFOHEADER bi = (LPBITMAPINFOHEADER)malloc(size);
    if (!bi)
    {
        printf("�ڴ����ʧ�ܣ�\n");
        return -2;
    }

    fseek(file, sizeof(BITMAPFILEHEADER), SEEK_SET);
    fread(bi, 1, size, file);
    //HWND hwnd = GetConsoleWindow();
    HDC dc = GetDC(hwnd);
    SetConsoleTitleA("����̨��ʾͼƬ");

    do
    {
        SetDIBitsToDevice(dc, 0, 0, bi->biWidth, bi->biHeight, 0, -0, 0, bi->biHeight, bi + 1, (LPBITMAPINFO)bi, 0);
        Sleep(250);
    } while (1);
    getchar();
	return 0;
}

#endif