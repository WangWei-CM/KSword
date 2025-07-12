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
    int cx = GetSystemMetrics(SM_CXSCREEN);            /* 屏幕宽度 像素 */
    int cy = GetSystemMetrics(SM_CYSCREEN);            /* 屏幕高度 像素 */

    LONG l_WinStyle = GetWindowLong(hwnd, GWL_STYLE);   /* 获取窗口信息 */
    /* 设置窗口信息 最大化 取消标题栏及边框 */
    SetWindowLong(hwnd, GWL_STYLE, (l_WinStyle | WS_POPUP | WS_MAXIMIZE) & ~WS_CAPTION & ~WS_THICKFRAME & ~WS_BORDER);

    SetWindowPos(hwnd, HWND_TOP, 0, 0, cx, cy, 0);    //添加点击穿透
    DWORD exStyle = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);

    // 添加点击穿透样式`exit
    exStyle |= WS_EX_TRANSPARENT;

    // 应用新的扩展样式
    SetWindowLong(GetConsoleWindow(), GWL_EXSTYLE, exStyle);

    // 更新窗口以应用更改
    SetWindowPos(GetConsoleWindow(), nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    std::thread listenerThread(keyboardListener);
    LONG_PTR currentExStyle = GetWindowLongPtr(GetConsoleWindow(), GWL_EXSTYLE);
    SetWindowLongPtr(GetConsoleWindow(), GWL_EXSTYLE, currentExStyle | WS_EX_TOOLWINDOW);
    findrc();
    string bmptmp = localadd+"\\Kbackground.bmp";
    cout << bmptmp;
    FILE* file = NULL;
    file = fopen(bmptmp.c_str(), "r"); // 只能显示bmp格式图片
    if (!file)
    {
        printf("图片文件打开失败！\n");
        return -1;
    }

    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);

    LPBITMAPINFOHEADER bi = (LPBITMAPINFOHEADER)malloc(size);
    if (!bi)
    {
        printf("内存分配失败！\n");
        return -2;
    }

    fseek(file, sizeof(BITMAPFILEHEADER), SEEK_SET);
    fread(bi, 1, size, file);
    //HWND hwnd = GetConsoleWindow();
    HDC dc = GetDC(hwnd);
    SetConsoleTitleA("控制台显示图片");

    do
    {
        SetDIBitsToDevice(dc, 0, 0, bi->biWidth, bi->biHeight, 0, -0, 0, bi->biHeight, bi + 1, (LPBITMAPINFO)bi, 0);
        Sleep(250);
    } while (1);
    getchar();
	return 0;
}

#endif