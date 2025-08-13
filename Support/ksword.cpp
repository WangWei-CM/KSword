//                       _oo0oo_
//                      o8888888o
//                      88" . "88
//                      (| -_- |)
//                      0\ = / 0
//                    ___ / `---'\___
//                  .' \\|     |// '.
//                 / \\|||  :  |||// \
//                / _||||| -:- |||||- \
//               |   | \\\  -  /// |   |
//               | \_|  ''\---/''  |_/ |
//               \  .-\__  '-'  ___/-. /
//             ___'. .' / --.--\  `. .'___
//          ."" '<  `.___\_<|>_/___.' >' "".
//         | | :  `- \`.;`\ _ /`;.`/ - ` : | |
//         \  \ `_.   \_ __\ /__ _/   .-` /  /
//     =====`-.____`.___ \_____/___.-`___.-'=====
//                       `=---='
//
//
//     ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
//               佛祖保佑         永无BUG
#include "../Main/KswordTotalHead.h"


#include "ksword.h"

#define KSWORD_VERSION_A 0
#define KSWORD_VERSION_B 0
#define KSWORD_VERSION_C 0
#define KSWORD_VERSION_D 0
#define KSWORD_VERSION_SPECIAL "Dev"


//**********Ksword Head File**********//
//Developed By WangWei_CM.,Explore & Kali-Sword Dev Team.
//专为小项目、控制台项目、windowsAPI项目、内核对抗程序提供的简化头文件
//开发者群聊：774070323；作者QQ：3325144899

//打印logo

int HookThreadID;
int KgetlineCoreThreadID;
void KPrintLogo() {
    std::cout << std::endl;	std::cout << "  _  __                                      _ "; 
    char versionInfo[100];
    sprintf(versionInfo, "%d.%d.%d.%d %s", KSWORD_VERSION_A, KSWORD_VERSION_B, KSWORD_VERSION_C, KSWORD_VERSION_D, KSWORD_VERSION_SPECIAL);
    cprint(versionInfo, 15, 1);
    std::cout << std::endl;
    std::cout << " | |/ /  Dev WinAPI C++ CUI Tool  _ __    __| |" << std::endl;
    std::cout << " | ' /  / __| \\ \\ /\\ / /  / _ \\  | '__|  / _` |" << std::endl;
    std::cout << " | . \\  \\__ \\  \\ V  V /  | (_) | | |    | (_| |" << std::endl;
    std::cout << " |_|\\_\\ |___/   \\_/\\_/    \\___/  |_|     \\__,_|"; cprint("EXE FRAMEWORK", 4, 15);std::cout << std::endl;
    /*for (int i = 1; i <= 35; i++) {
        cprint("█", 15, 1);
        Sleep(5);
    }
    for (int i = 1; i <= 70; i++) {
        putchar('\b');
    }*/
    std::cout << "Ksword Framework Developed by WangWei_CM.,Explore,Ksword Dev Team,etc.";
    std::cout << std::endl;
}
//获取版本号信息
KVersionInfo KGetVersion() {
    KVersionInfo kVersionInfo;
    kVersionInfo.a = KSWORD_VERSION_A;
    kVersionInfo.b = KSWORD_VERSION_B;
    kVersionInfo.c = KSWORD_VERSION_C;
    kVersionInfo.d = KSWORD_VERSION_D;
    kVersionInfo.e = "KSWORD_VERSION_SPECIAL";
    return kVersionInfo;
};
//格式转换函数
std::string WCharToString(const WCHAR* wstr) {
    int len = WideCharToMultiByte(CP_ACP, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    std::string str(len, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr, -1, &str[0], len, nullptr, nullptr);
    return str;
}

std::wstring StringToWString(std::string str) {
    std::wstring wstr(str.begin(), str.end());
    return wstr;
}
std::string WstringToString(std::wstring str) {
    std::string wstr(str.begin(), str.end());
    return wstr;
}
std::string CharToString(const char* charPtr) {
    return std::string(charPtr);
}
//正常情况下应该直接const char*=<string>，这个方法会内存泄漏，记得delete
const char* StringToChar(const std::string& str) {
    char* cstr = new char[str.length() + 1];
    // 复制字符串内容
    std::strcpy(cstr, str.c_str());
    // 返回指向复制的C风格字符串的指针
    return cstr;
}
//const char*转为WCHAR*
const WCHAR* CharToWChar(const char* ansiString) {
    if (ansiString == nullptr) {
        return nullptr;
    }

    int wideLength = MultiByteToWideChar(CP_ACP, 0, ansiString, -1, nullptr, 0);
    WCHAR* wideString = new WCHAR[wideLength];

    MultiByteToWideChar(CP_ACP, 0, ansiString, -1, wideString, wideLength);

    return wideString;
}
int StringToInt(std::string str)
{
    // 首先检查字符串是否为空
    if (str.empty()) {
        return 0;
    }

    // 检查字符串是否只包含数字字符
    for (char ch : str) {
        if (!std::isdigit(ch)) {
            KMesErr("不符合规范的数据类型转换：std::string_to_int");
            return 0;
        }
    }

    // 使用stoi函数将字符串转换为整数
    int num = std::stoi(str);
    return num;
}
short StringToShort(std::string str)
{
    try {
        return static_cast<short>(std::stoi(str));
    }
    catch (const std::invalid_argument& e) {
        KMesErr("不允许的数据类型转换:std::string->short");
        return 0; // 或抛出异常
    }
    catch (const std::out_of_range& e) {
        KMesErr("不允许的数据类型转换:std::string->short");
        return 0; // 或抛出异常
    }
}
//Ksword规范消息函数=========================================
void Kpause() {
    std::cout << "程序运行到断点，已暂停，按回车继续";
    Kgetline();
}
//信息提示[ * ]
void KMesInfo(const char* text) {
    if(KSWORD_MES_LEVEL<=0){
    cprint("[ * ]", 9, 0);
    std::cout << text << std::endl;
    kLog.Add(Info, C(text), C("Ksword框架"));
#ifdef KSWORD_WITH_COMMAND
    if(!isGUI)
    KswordSend1("报告了一个信息：" + std::string(text));
#endif
    }
}
void KMesInfo(std::string text) {
    KMesInfo(text.c_str());
}

//警告提示[ ! ]
void KMesWarn(std::string text) {
    KMesWarn(text.c_str());
}
void KMesWarn(const char* text) {
    if(KSWORD_MES_LEVEL<=0){
    cprint("[ ! ]", 6, 0);
    std::cout << text << std::endl;
    kLog.Add(Warn, C(text), C("Ksword框架"));
#ifdef KSWORD_WITH_COMMAND
    if (!isGUI)
    KswordSend1("报告了一个警告：" + std::string(text));
#endif
    }
}

//错误提示[ X ]
void KMesErr(std::string text) {
    KMesErr(text.c_str());
}
void KMesErr(const char* text) {
    if(KSWORD_MES_LEVEL<=0){
    cprint("[ × ]", 4, 0);
    std::cout << text << std::endl;
    kLog.Add(Err, C(text), C("Ksword框架"));
#ifdef KSWORD_WITH_COMMAND
    if (!isGUI)

        KswordSend1("报告了一个错误：" + std::string(text));
#endif
    }
}

//控制台窗口属性操控函数=====================================
//置顶与取消置顶函数
bool SetTopWindow() {
    HWND hWnd = GetConsoleWindow(); // 获取控制台窗口句柄
    if (hWnd != NULL) {
        // 将窗口置顶
        if (!SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE)) {
            KMesErr("SetWindowPos 调用失败");
            return false;
        }
        return true;
    } else {
        KMesErr("获取句柄失败");
        return false;
    }
}
bool UnTopWindow() {
    HWND hWnd = GetConsoleWindow(); // 获取控制台窗口句柄
    if (hWnd != NULL) {
        // 取消窗口置顶
        SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        return KSWORD_SUCCESS_EXIT;
    } else {
        KMesErr("获取句柄失败");
        return KSWORD_ERROR_EXIT;
    }
}
bool KTopWindow() {
    HWND hWnd = ::GetForegroundWindow();
    HWND hForeWnd = NULL;
    DWORD dwForeID = 0;
    DWORD dwCurID = 0;

    hForeWnd = ::GetForegroundWindow();
    dwCurID = ::GetCurrentThreadId();
    dwForeID = ::GetWindowThreadProcessId(hForeWnd, NULL);
    ::AttachThreadInput(dwCurID, dwForeID, TRUE);
    ::ShowWindow(hWnd, SW_SHOWNORMAL);
    ::SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
    ::SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
    ::SetForegroundWindow(hWnd);
    // 将前台窗口线程贴附到当前线程（也就是程序A中的调用线程）
    ::AttachThreadInput(dwCurID, dwForeID, FALSE);
    //KMesInfo("窗口成功置顶！");
    return KSWORD_SUCCESS_EXIT;
//原文链接：https://blog.csdn.net/weixin_45525272/article/details/116452142
}

//隐藏与取消隐藏函数
bool HideWindow() {
    HWND hWnd = GetConsoleWindow();
    if (hWnd != NULL) {
        ShowWindow(hWnd, SW_HIDE);
        //KMesInfo("窗口已经隐藏");
        return KSWORD_SUCCESS_EXIT;
    } else {
        KMesErr("获取句柄失败！");
        return KSWORD_ERROR_EXIT;
    }
}
bool ShowWindow() {
    HWND hWnd = GetConsoleWindow();
    if (hWnd != NULL) {
        ShowWindow(hWnd, SW_SHOW);
        //KMesInfo("窗口状态设置为显示");
        return KSWORD_SUCCESS_EXIT;
    } else {
        KMesErr("获取句柄失败!");
        return KSWORD_ERROR_EXIT;
    }
}
//隐藏窗口边框
bool HideSide() {
    // 获取控制台窗口的句柄
    HWND hWnd = GetConsoleWindow();
    // 获取窗口的当前样式
    LONG style = GetWindowLong(hWnd, GWL_STYLE);
    // 移除窗口样式中的窗口边框和标题栏标志
        style &= ~(WS_OVERLAPPED | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_VSCROLL | WS_HSCROLL);

    // 应用新的样式
    SetWindowLong(hWnd, GWL_STYLE, style);
    // 更新窗口以反映新的样式
    SetWindowPos(hWnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    return KSWORD_SUCCESS_EXIT;
}
//窗口全屏
bool KFullScreen() {
    HWND hwnd = GetConsoleWindow();  // 使用 GetConsoleWindow 获取当前控制台窗口
    if (!hwnd) {
        KMesErr("无法获取控制台窗口句柄");
        return KSWORD_ERROR_EXIT;
    }

    int cx = GetSystemMetrics(SM_CXSCREEN);  // 屏幕宽度 像素
    int cy = GetSystemMetrics(SM_CYSCREEN);  // 屏幕高度 像素

    LONG l_WinStyle = GetWindowLong(hwnd, GWL_STYLE);  // 获取窗口信息
    if (l_WinStyle == 0) {
        KMesErr("无法获取窗口样式");
        return KSWORD_ERROR_EXIT;
    }

    // 设置窗口信息：最大化，取消标题栏及边框
    SetWindowLong(hwnd, GWL_STYLE, (l_WinStyle | WS_POPUP | WS_MAXIMIZE) & ~WS_CAPTION & ~WS_THICKFRAME & ~WS_BORDER);

    // 应用新的窗口样式
    if (!SetWindowPos(hwnd, HWND_TOP, 0, 0, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED)) {
        KMesErr("无法设置窗口位置和大小");
        return KSWORD_ERROR_EXIT;
    }

    KMesInfo("成功使用WindowsAPI全屏！");
    return KSWORD_SUCCESS_EXIT;
}
bool KResetWindow() {
    HWND hwnd = GetForegroundWindow();
    int cx = GetSystemMetrics(SM_CXSCREEN);            /* 屏幕宽度 像素 */
    int cy = GetSystemMetrics(SM_CYSCREEN);            /* 屏幕高度 像素 */

    LONG l_WinStyle = GetWindowLong(hwnd, GWL_STYLE);   /* 获取窗口信息 */
    LONG l_WinExStyle = GetWindowLong(hwnd, GWL_EXSTYLE); /* 获取扩展窗口样式 */

    /* 恢复窗口样式，移除最大化样式，添加标题栏及边框 */
    SetWindowLong(hwnd, GWL_STYLE, (l_WinStyle & ~WS_POPUP & ~WS_MAXIMIZE) | WS_CAPTION | WS_THICKFRAME | WS_BORDER);
    SetWindowLong(hwnd, GWL_EXSTYLE, l_WinExStyle | WS_EX_WINDOWEDGE); // 添加窗口边缘样式

    /* 恢复窗口到原始大小和位置，这里假设原始大小和位置是屏幕的一半 */
    SetWindowPos(hwnd, NULL, 0, 0, cx / 2, cy / 2, SWP_NOZORDER | SWP_FRAMECHANGED);

    KMesInfo("成功使用WindowsAPI脱离全屏！");
    return KSWORD_SUCCESS_EXIT;
}
//原文链接：https ://blog.csdn.net/linuxwuj/article/details/81165885
bool FullScreen() {
    COORD font_size;
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    /* 字体信息 */
    struct CONSOLE_FONT
    {
        DWORD index;
        COORD dim;
    } cfi;
    typedef COORD(WINAPI* PROCGETCONSOLEFONTSIZE)(HANDLE, DWORD);
    typedef BOOL(WINAPI* PROCGETCURRENTCONSOLEFONT)(HANDLE, BOOL, struct CONSOLE_FONT*);

    HMODULE hKernel32 = GetModuleHandle(L"kernel32");
    PROCGETCONSOLEFONTSIZE GetConsoleFontSize = (PROCGETCONSOLEFONTSIZE)GetProcAddress(hKernel32, "GetConsoleFontSize");
    PROCGETCURRENTCONSOLEFONT GetCurrentConsoleFont = (PROCGETCURRENTCONSOLEFONT)GetProcAddress(hKernel32, "GetCurrentConsoleFont");

    GetCurrentConsoleFont(handle, FALSE, &cfi);             /* 获取当前字体索引信息 */
    font_size = GetConsoleFontSize(handle, cfi.index);      /* 获取当前字体宽高信息[字符宽度及高度所占像素数] */
//原文链接：https ://blog.csdn.net/linuxwuj/article/details/81165885
    HWND hwnd = GetForegroundWindow();
    int cx = GetSystemMetrics(SM_CXSCREEN);            /* 屏幕宽度 */
    int cy = GetSystemMetrics(SM_CYSCREEN);            /* 屏幕高度 */
    char cmd[32] = { 0 };
    sprintf(cmd, "MODE CON: COLS=%d LINES=%d", cx / font_size.X, cy / font_size.Y);
    KMesInfo(cmd);
    system(cmd);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, cx, cy, 0);
    //KMesInfo("成功全屏");
    return KSWORD_SUCCESS_EXIT;
}
//隐藏与显示指针
bool ShowCursor() {
    HANDLE h_GAME = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_GAME != NULL) {
        CONSOLE_CURSOR_INFO cursor_info;
        GetConsoleCursorInfo(h_GAME, &cursor_info);
        cursor_info.bVisible = true;
        SetConsoleCursorInfo(h_GAME, &cursor_info);;
        return KSWORD_SUCCESS_EXIT;
    }
    else {
        KMesErr("获取句柄失败！");
        return KSWORD_ERROR_EXIT;
    }
}
//原文链接：https://blog.csdn.net/qq_43312665/article/details/86790176
void HideCursor()
{
    CONSOLE_CURSOR_INFO cursor_info = { 1, 0 };
    SetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &cursor_info);
}
//原文链接：https://blog.csdn.net/qq_33866593/article/details/104597731

//更改指针位置（从左到右；从上往下）
void SetCursor(int x, int y) {
    HANDLE hout;
    COORD coord;
    coord.X = x;
    coord.Y = y;
    hout = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleCursorPosition(hout, coord);
    return;
}
//彩色输出文本（文本内容，前景色，背景色）
//0 = 黑色 8 = 灰色
//1 = 蓝色 9 = 淡蓝色
//2 = 绿色 10 = 淡绿色
//3 = 浅绿色 11 = 淡浅绿色
//4 = 红色 12 = 淡红色
//5 = 紫色 13 = 淡紫色
//6 = 黄色 14 = 淡黄色
//7 = 白色 15 = 亮白色
void cprint(std::string text, int front_color, int back_color, int special) {
#ifdef KSWORD_WITH_COMMAND
    if (KswordPipeMode) {
        std::cout << text; return;
    }
#endif
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(handle, back_color * 16 | FOREGROUND_INTENSITY | front_color);
    if (special == 1) { // 斜体
        // Windows 控制台不支持斜体，可以尝试设置字体属性，但效果有限
        CONSOLE_FONT_INFOEX cfi = { 0 };
        cfi.cbSize = sizeof(cfi);
        GetCurrentConsoleFontEx(handle, FALSE, &cfi);
        cfi.FontWeight = FW_NORMAL;
        SetCurrentConsoleFontEx(handle, FALSE, &cfi);
    }
    else if (special == 2) { // 粗体
        CONSOLE_FONT_INFOEX cfi = { 0 };
        cfi.cbSize = sizeof(cfi);
        GetCurrentConsoleFontEx(handle, FALSE, &cfi);
        cfi.FontWeight = FW_BOLD;
        SetCurrentConsoleFontEx(handle, FALSE, &cfi);
    }
    printf(text.c_str());
    SetConsoleTextAttribute(handle, FOREGROUND_INTENSITY | 7);
    return;
}

//相关函数来源：https://blog.csdn.net/qq_42885747/article/details/103835671
//设置透明度
void SetAir(BYTE alpha)
{
        // 获取控制台窗口的句柄
        HWND hwnd = GetConsoleWindow();
        SetWindowLong(hwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
        // 设置窗口的分层属性，使其透明
        SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), alpha, LWA_ALPHA);
    
}

//钩子层面获取输入

static HHOOK hKeyboardHook;
#define KGetLineTempBufferSize 1000
std::string KGetLineTemp[KGetLineTempBufferSize] = {};
int KGetLineTempUp = 0;
static int KGetLineSearchTemp;
static bool isFirstSearch = 1;
void KgetlineClean() {
    //std::cout << KGetLineTempUp ;system("pause");
    if (KGetLineTempUp == 0)KGetLineTemp[KGetLineTempBufferSize-1] = "ClEANED";
    else KGetLineTemp[KGetLineTempUp - 1] = "CLEANED";
}
// 处理按键的函数
void dealinput(char c) {
    if (c == '\b' && !KGetLineTemp[KGetLineTempUp].empty()) { // 检查是否按下Backspace键
        std::cout << "\b \b";
        KGetLineTemp[KGetLineTempUp].pop_back();
    }
    else if (KGetLineTemp[KGetLineTempUp][KGetLineTemp[KGetLineTempUp].length() - 1] == '^') {
        if (c >= 'a' && c <= 'z') {
            // 小写字母转换为大写字母
            c -= 32;
        }
        else if (c >= '0' && c <= '9') {
            // 数字转换为对应的符号
            switch (c) {
            case '1': c = '!'; break;
            case '2': c = '@'; break;
            case '3': c = '#'; break;
            case '4': c = '$'; break;
            case '5': c = '%'; break;
            case '6': c = '^'; break;
            case '7': c = '&'; break;
            case '8': c = '*'; break;
            case '9': c = '('; break;
            case '0': c = ')'; break;
            }
        }
        else if (c >= '!' && c <= '/') {
            // 特殊符号转换为对应的符号
            switch (c) {
            case '`': c = '~'; break;
            case '-': c = '_'; break;
            case '=': c = '+'; break;
            case '[': c = '{'; break;
            case ']': c = '}'; break;
            case '\\': c = '|'; break;
            case ';': c = ':'; break;
            case '\'': c = '"'; break;
            case ',': c = '<'; break;
            case '.': c = '>'; break;
            case '/': c = '?'; break;
            }
        }
        std::cout << "\b  \b\b";
        std::cout << c;
        KGetLineTemp[KGetLineTempUp].pop_back();
        KGetLineTemp[KGetLineTempUp] += c;
    }
    else if (c >= 32 && c <= 126) { // 可见字符
        if (c >= 'A' && c <= 'Z')  // 如果输入的是大写字母
            c += 32;
        else if (c == '{')c = '[';
        else if (c == '}')c = ']';
        else if (c == '|')c = '\\';
        else if (c == '?')c = '/';
        else if (c == '<')c = ',';
        else if (c == '>')c = '.';
        else if (c == ':')c = ';';
        else if (c == '_')c = '-';
        else if (c == '+')c = '=';
        else if (c == '`')c = '`';
        else if (c == '!')c = '1';
        else if (c == '@')c = '2';
        else if (c == '#')c = '3';
        else if (c == '$')c = '4';
        else if (c == '%')c = '5';
        else if (c == '^')c = '6';
        else if (c == '&')c = '7';
        else if (c == '*')c = '8';
        else if (c == '(')c = '9';
        else if (c == ')')c = '0';
        std::cout << c;
        KGetLineTemp[KGetLineTempUp] += c;
        //std::cout << "KGetlinetmp中含有" << KGetLineTemp[KGetLineTempUp];
    }
    else if (c == 47) {
        KGetLineTemp[KGetLineTempUp] += c;
        std::cout << c;
    }
    else if (c == 92) {
        KGetLineTemp[KGetLineTempUp] += c;
        std::cout << c;
    }
    else if (c == 45) {
        KGetLineTemp[KGetLineTempUp] += c;
        std::cout << c;
    }
}


LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    //std::cout << "被调用";
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        if (wParam == WM_KEYDOWN) {
            BYTE keyState[256];
            GetKeyboardState(keyState);
            //if (p->vkCode == VK_TAB) {
            //	std::string tmp;
            //	getline(std::cin, tmp);
            //	KGetLineTemp[KGetLineTempUp] += tmp;
            //	KGetLineTempUp++;
            //	PostQuitMessage(0);
            //}
            /*else*/ if (p->vkCode == VK_RETURN) { // Enter键
                if (!isFirstSearch) {
                    KGetLineTemp[KGetLineTempUp] = KGetLineTemp[KGetLineSearchTemp];
                    KGetLineSearchTemp = KGetLineTempUp;
                    isFirstSearch = 1;
                }
                if (KGetLineTempUp >= KGetLineTempBufferSize - 1) {
                    KGetLineTempUp = 0;
                }
                else {
                    KGetLineTempUp++;
                }
                std::cout << std::endl;
                PostThreadMessage(KgetlineCoreThreadID, WM_QUIT, 0, 0);
            }
            else if (p->vkCode == VK_TAB) {
                std::string clipBoard = GetClipBoard();
                std::cout << clipBoard;
                KGetLineTemp[KGetLineTempUp] += clipBoard;
            }
            else if (p->vkCode == VK_LSHIFT) {
                if (KGetLineTemp[KGetLineTempUp][KGetLineTemp[KGetLineTempUp].length()] == '^'); else {
                    KGetLineTemp[KGetLineTempUp] += "^";
                    std::cout << "^";
                    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
                    CONSOLE_SCREEN_BUFFER_INFO csbi;

                    // 获取当前光标位置
                    if (GetConsoleScreenBufferInfo(hConsole, &csbi)) {
                        COORD cursorPos = csbi.dwCursorPosition;
                        // 判断是否在行首
                        if (cursorPos.X == csbi.dwSize.X - 1) {
                        }
                        else {
                            // 移动光标到前一个字符位置
                            //cursorPos.X--;
                        }
                        // 设置新的光标位置
                        SetConsoleCursorPosition(hConsole, cursorPos);
                    }
                }
            }
            else if (p->vkCode == VK_RCONTROL || p->vkCode == VK_RMENU) {
                if (!isFirstSearch) {
                    KGetLineTemp[KGetLineTempUp] = "\\" + KGetLineTemp[KGetLineSearchTemp];
                    KGetLineSearchTemp = KGetLineTempUp;
                    isFirstSearch = 1;
                }
                if (KGetLineTempUp >= KGetLineTempBufferSize - 1) {
                    KGetLineTempUp = 0;
                    KGetLineTemp[KGetLineTempUp] = "\\" + KGetLineTemp[KGetLineTempBufferSize - 1];
                    KGetLineTemp[KGetLineTempBufferSize - 1] = "kinterrupt";
                }
                else {
                    KGetLineTempUp++;
                    KGetLineTemp[KGetLineTempUp] = "\\" + KGetLineTemp[KGetLineTempUp - 1];
                    KGetLineTemp[KGetLineTempUp - 1] = "kinterrupt";
                }
                cprint("[interrupt]", 7, 4);
                PostThreadMessage(KgetlineCoreThreadID, WM_QUIT, 0, 0);
                return CallNextHookEx(NULL, nCode, wParam, lParam);
            }
            else if (p->vkCode == VK_UP) {
                if (isFirstSearch) {
                    KGetLineSearchTemp = KGetLineTempUp;
                    isFirstSearch = 0;
                }
                for (int i = 1; i <= KGetLineTemp[KGetLineSearchTemp].size(); i++) {
                    std::cout << "\b \b";
                }
                if (KGetLineSearchTemp == 0) KGetLineSearchTemp = KGetLineTempBufferSize - 1;
                do {
                    KGetLineSearchTemp--;
                } while (KGetLineTemp[KGetLineSearchTemp] == "");
                std::cout << KGetLineTemp[KGetLineSearchTemp];
            }
            else if (p->vkCode == VK_RSHIFT) {
                if (!isFirstSearch) {
                    //KGetLineTemp[KGetLineTempUp] = "\\"+KGetLineTemp[KGetLineSearchTemp];
                    KGetLineSearchTemp = KGetLineTempUp;
                    isFirstSearch = 1;
                }
                if (KGetLineTempUp >= KGetLineTempBufferSize - 1) {
                    KGetLineTempUp = 0;
                    KGetLineTemp[KGetLineTempBufferSize - 1] = "kreinput";
                }
                else {
                    KGetLineTempUp++;
                    KGetLineTemp[KGetLineTempUp - 1] = "kreinput";
                }
                cprint("[reinput]", 7, 6);
                PostThreadMessage(KgetlineCoreThreadID, WM_QUIT, 0, 0);
                return CallNextHookEx(NULL, nCode, wParam, lParam);

            }
            else if (p->vkCode == VK_DOWN) {
                if (isFirstSearch) {
                    ;
                }
                else {
                    for (int i = 1; i <= KGetLineTemp[KGetLineSearchTemp].size(); i++) {
                        std::cout << "\b \b";
                    }
                    do {
                        if (KGetLineSearchTemp == KGetLineTempBufferSize) KGetLineSearchTemp = -1;
                        KGetLineSearchTemp++;
                    } while (KGetLineTemp[KGetLineSearchTemp] == "");
                    std::cout << KGetLineTemp[KGetLineSearchTemp];

                }
            }
            else if ((p->vkCode >= 32 && p->vkCode <= 126) || (p->vkCode == 8) || (p->vkCode == 109) || (p->vkCode == 154) || (p->vkCode == 220)) { // 可见字符或者回车
                if (isFirstSearch) {
                    BYTE keyState[256];
                    GetKeyboardState(keyState);
                    unsigned short dwResult;
                    if (ToAscii(p->vkCode, p->scanCode, keyState, &dwResult, 0) == 1) {
                        dealinput((char)dwResult);
                    }
                }
                else {
                    KGetLineTemp[KGetLineTempUp] = KGetLineTemp[KGetLineSearchTemp];
                    KGetLineSearchTemp = KGetLineTempUp;
                    isFirstSearch = 1;
                    unsigned short dwResult;
                    if (ToAscii(p->vkCode, p->scanCode, keyState, &dwResult, 0) == 1) {
                        dealinput((char)dwResult);
                    }
                }
            }
            else {
                // 处理其他键，包括特殊字符
                unsigned short dwResult;
                if (ToAscii(p->vkCode, p->scanCode, keyState, &dwResult, 0) == 1) {
                    dealinput((char)dwResult);
                }
                return CallNextHookEx(NULL, nCode, wParam, lParam);
            }
        }
        else {
            return CallNextHookEx(NULL, nCode, wParam, lParam);
        }
    }
    else {
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }
    if (nCode < 0) {
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }
    return 1;
}

std::string Getline() {
    std::string line;
    std::getline(std::cin, line);
    return line;
}


#define KEY_DOWN(VK_NONAME) ((GetAsyncKeyState(VK_NONAME)& 0x8000 ? 1:0))
#define KEY_DOWN_FOREMOST(hWnd,vk) (KEY_DOWN(vk) && GetForegroundWindow()==hWnd)    //最前面 
#define KEY_DOWN_FOCUSED(hWnd,vk) KEY_DOWN_FOREMOST(hWnd,vk)    //带焦点 
#define K(sth) KEY_DOWN_FOCUSED(hwnd, sth)
#define KEY_DOWN2(sth) (int(GetKeyState(sth)) < 0)
#define KEY_DOWN2_FOCUSED(hWnd, sth) (KEY_DOWN2(sth) && GetForegroundWindow() == hWnd)
#define K2(sth) KEY_DOWN2_FOCUSED(hwnd, sth)


#define KEY_DOWN_ONCE(bVk)     (KEY_DOWN(bVk) && !keybd_status.test(bVk))
#define KEY_DOWN2_ONCE(bVk)     (KEY_DOWN2(bVk) && !keybd_status.test(bVk))
#define K_ONCE(bVk)            (K(bVk) && !keybd_status.test(bVk))
#define K2_ONCE(bVk)            (K2(bVk) && !keybd_status.test(bVk))

std::bitset<255UL> keybd_status{ 0 };


void UpdateKeyboardStatus()
{   //更新键盘状态
#ifdef KSWORD_WITH_COMMAND
    if (windowsshow)
    {
        for (short k = 0; k < 255; ++k)
            keybd_status.set(k, KEY_DOWN(k));
    }
    else {
        for (short k = 0; k < 255; ++k)
            keybd_status.set(k, KEY_DOWN2(k));
    }
#endif
}
// 检测按键是否按下
bool IsKeyPressed(int keyCode) {
    return (GetAsyncKeyState(keyCode) & 0x8000) != 0;
}

// 检测按键是否抬起
bool IsKeyReleased(int keyCode) {
    return (GetAsyncKeyState(keyCode) & 0x8000) == 0;
}

// 获取用户输入的字符串
std::string AnsyKeyCodeGetline() {
    std::string input;
    const int keys[] = { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, // 0-9
                             0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, // A-Z
                             0x3B, 0x27, 0x2C, 0x2E, 0x2F, 0x3A, 0x3F, 0x5B, 0x5D, 0x5C, 0x2D, 0x3D, 0x20 }; // 特殊符号和空格
    while (true) {
        Sleep(10);
        for (int keyCode : keys) { // ; ' , . / : ? [ ] \ - =
            if (KEY_DOWN2_ONCE(keyCode)) {
                char ch = tolower(char(keyCode));
                input += ch; // 添加到输入字符串
                std::cout << ch; // 实时输出
                break;
            }
        }
        // 检测退格键
        if (IsKeyPressed(VK_BACK)) { // VK_BACK 是退格键的虚拟键码
            while (!IsKeyReleased(VK_BACK)) { // 等待退格键抬起
                Sleep(10); // 稍微暂停一下，避免CPU占用过高
            }
            if (!input.empty()) {
                input.pop_back(); // 删除最后一个字符
                std::cout << "\b \b"; // 输出退格效果
            }
            continue;
        }

        // 检测Enter键
        if (IsKeyPressed(VK_RETURN)) { // VK_RETURN 是 Enter 键的虚拟键码
            while (!IsKeyReleased(VK_RETURN)) { // 等待Enter键抬起
                Sleep(10); // 稍微暂停一下，避免CPU占用过高
            }
            break; // 退出循环
        }

        //// 检测其他按键（包括字母、数字和特殊符号）
        //for (int keyCode = 0x30; keyCode <= 0x39; ++keyCode) { // 0-9
        //	if ((KEY_DOWN2_ONCE(keyCode))) {
        //		char ch = char(keyCode);
        //		input += ch; // 添加到输入字符串
        //		std::cout << ch; // 实时输出
        //		break;
        //	}
        //}

        //for (int keyCode = 0x41; keyCode <= 0x5A; ++keyCode) { // A-Z
        //	if (KEY_DOWN2_ONCE(keyCode)) {
        //		char ch = tolower(char(keyCode)); // 转换为小写字母
        //		input += ch; // 添加到输入字符串
        //		std::cout << ch; // 实时输出
        //		break;
        //	}
        //}

        //// 检测特殊符号
        //const int specialKeys[] = { 0x3B, 0x27, 0x2C, 0x2E, 0x2F, 0x3A, 0x3F, 0x5B, 0x5D, 0x5C, 0x2D, 0x3D };
        //for (int keyCode : specialKeys) { // ; ' , . / : ? [ ] \ - =
        //	if (KEY_DOWN2_ONCE(keyCode)) {
        //		char ch = char(keyCode);
        //		input += ch; // 添加到输入字符串
        //		std::cout << ch; // 实时输出
        //		break;
        //	}
        //}

        //// 检测空格键
        //if (KEY_DOWN2_ONCE(VK_SPACE)) { // VK_SPACE 是空格键的虚拟键码
        //	input += ' '; // 添加空格到输入字符串
        //	std::cout << ' '; // 实时输出
        //}
        UpdateKeyboardStatus();
    }

    std::cout << std::endl; // 换行
    return input;
}


bool directoryExists(const std::string& path) {
    DWORD attrib = GetFileAttributesA(path.c_str());
    return (attrib != INVALID_FILE_ATTRIBUTES && (attrib & FILE_ATTRIBUTE_DIRECTORY));
}

bool fileExists(const std::string& path) {
    DWORD attrib = GetFileAttributesA(path.c_str());
    // 检查是否获取到有效属性，并且路径不是目录
    return (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY));
}




int KgetlineMode ;
//0：钩子层面获取键盘输入
//1：硬件层面获取键盘输入
//2：普通Getline方式获取输入
//2：普通Getline方式获取输入
bool ExitKGetline = 0;
bool UseHookFucker = 1;
void KgetlineRegHookManager() {
    while (!ExitKGetline)
    {
        Sleep(1);
        PostThreadMessage(HookThreadID, WM_QUIT, 0, 0);
    }return;
}
void KgetlineRegHook() {
    HookThreadID = GetCurrentThreadId();
    while (!ExitKGetline) {
        hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
        if (hKeyboardHook == NULL) {
            KMesErr("安装钩子时发生错误");
            return;
        }
        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        UnhookWindowsHookEx(hKeyboardHook);
    }
    return;
}
std::string GBKtoUTF8(const std::string& gbkStr) {
    // 获取转换后所需缓冲区大小
    int wlen = MultiByteToWideChar(CP_ACP, 0, gbkStr.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return "";
    wchar_t* wbuf = new wchar_t[wlen];
    MultiByteToWideChar(CP_ACP, 0, gbkStr.c_str(), -1, wbuf, wlen);

    // 转换为UTF-8
    int ulen = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, nullptr, 0, nullptr, nullptr);
    char* ubuf = new char[ulen];
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, ubuf, ulen, nullptr, nullptr);

    std::string utf8Str(ubuf);
    delete[] wbuf;
    delete[] ubuf;
    return utf8Str;
}
std::string KgetlineCore()
{
    if (KgetlineMode == 0) {
        if (KGetLineTemp[KGetLineTempUp][0] != '\\') {
            //std::cout << "KGetLineTemp[KGetLineTempUp]=" << KGetLineTemp[KGetLineTempUp];
            KGetLineTemp[KGetLineTempUp] = "";
        }
        else {
            //std::cout << "KGetLineTemp[KGetLineTempUp]=" << KGetLineTemp[KGetLineTempUp];
            KGetLineTemp[KGetLineTempUp].erase(0, 1);
            std::cout << KGetLineTemp[KGetLineTempUp];
        }
        //hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
        //if (hKeyboardHook == NULL) {
        //	KMesErr("安装钩子时发生错误");
        //	return "SetWindowsHookEx failed";
        //}
        std::thread KGetlineHookerThread(KgetlineRegHook);
        std::thread KGetlineHookerManager(KgetlineRegHookManager);
        KgetlineCoreThreadID = GetCurrentThreadId();
        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        // 卸载钩子
       //std::cout <<"即将返回" << KGetLineTemp[KGetLineTempUp] << std::endl;
       /*return KGetLineTemp[KGetLineTempUp];*/
        ExitKGetline = 1;
        KGetlineHookerThread.join();
        KGetlineHookerManager.join();
        ExitKGetline = 0;
        if (KGetLineTempUp == 0)return KGetLineTemp[KGetLineTempBufferSize];
        else return KGetLineTemp[KGetLineTempUp - 1];
    }
    else if (KgetlineMode == 1) {
        return AnsyKeyCodeGetline();
    }
    else /*if (KgetlineMode == 2) */ {
        SetForegroundWindow(GetConsoleWindow());
        return Getline();
    }
}

//修改控制台窗口标题内容
//void SetTitle(const char* text) {
//	SetConsoleTitleW(text);
//	return;
//}
//void SetTitle(std::string text) {
//	SetConsoleTitleW(text.c_str());
//	return;
//}


static LRESULT CALLBACK LowLevelKeyboardProcInput(int nCode, WPARAM wParam, LPARAM lParam) {
#ifdef KSWORD_WITH_COMMAND
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        if (wParam == WM_KEYDOWN) {
            // 检查是否按下的是·键（假设·键的虚拟键码为VK_OEM_3）
            if (p->vkCode == VK_OEM_3) {
                dotKeyCount++;
                // 屏蔽按键，不调用下一个钩子
                //
                if (dotKeyCount == 3) {
                    PostQuitMessage(0);
                    dotKeyCount = 0;
                }
                return 1;
            }
            else {
                dotKeyCount = 0;
            }
        }
    }
    // 调用下一个钩子
#endif
    return CallNextHookEx(NULL, nCode, wParam, lParam);

}
static HHOOK SetInputHook(HINSTANCE hInstance) {
    HHOOK hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProcInput, hInstance, 0);
    return hHook;
}
static void Unhook(HHOOK hHook) {
    UnhookWindowsHookEx(hHook);
}
std::string Kgetline() {
    std::string returnValue = KgetlineCore();
    if (returnValue == "kreinput")
        return Kgetline();
    if (returnValue == "kinterrupt") {
        if (1 && KgetlineMode == 0)
        {
            HINSTANCE hInstanceInput = GetModuleHandle(NULL);
            HHOOK hHook = SetInputHook(hInstanceInput);

            MSG msg;
            while ((GetMessage(&msg, NULL, 0, 0))) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            Unhook(hHook);
        }
        return Kgetline();
    }
    else return returnValue;
}

//控制台窗口最小化与取消最小化
void MiniWindow() {
    return;
}

void UnMiniWindow() {
    return;
}
//设置控制台字体为consola
void SetConsola()
{
#ifdef KSWORD_WITH_COMMAND
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

    // 设置字体
    CONSOLE_FONT_INFOEX cfi;
    cfi.cbSize = sizeof(cfi);
    cfi.nFont = 0;
    cfi.dwFontSize.X = 8;                   // 字体宽度
    cfi.dwFontSize.Y = FONT_SIZE_HEIGHT;                  // 字体高度
    cfi.FontFamily = FF_DONTCARE;
    cfi.FontWeight = FW_NORMAL;
    wcscpy(cfi.FaceName, L"Consolas");      // 字体名称
    cfi.FontWeight = FW_NORMAL;

    // 设置控制台字体
    SetCurrentConsoleFontEx(hConsole, FALSE, &cfi);
    return;
#endif
}
void SetConsoleWindowPosition(int x, int y)
{
        // 获取当前控制台窗口的句柄
        HWND hwnd = GetConsoleWindow();

        // 获取当前控制台窗口的矩形区域
        RECT rect;
        GetWindowRect(hwnd, &rect);

        // 计算新的窗口位置
        int newLeft = x;
        int newTop = y;

        // 设置窗口的新位置
        SetWindowPos(hwnd, nullptr, newLeft, newTop, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

}
void SetConsoleWindowSize(int width, int height) {    // 获取控制台输出句柄
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole == INVALID_HANDLE_VALUE) {
        std::cerr << "Error getting console handle." << std::endl;
        return;
    }
     //设置屏幕缓冲区大小
    COORD bufferSize = { width, 1000 };
    if (!SetConsoleScreenBufferSize(hConsole, bufferSize)) {
        std::cerr << "Error setting console screen buffer size." << std::endl;
        return;
    }
       //// 调整控制台窗口大小
    SMALL_RECT windowRect = { 0, 0, width - 1, height +1 };
    if (!SetConsoleWindowInfo(hConsole, TRUE, &windowRect)) {
        std::cerr << "Error setting console window size." << std::endl;
        return;
    }
    //std::string command = "mode con cols=" + std::to_string(width) + " lines=" + std::to_string(height);
    //system(command.c_str());
    //std::cout << "damn"; Kpause();
}
//环境探测函数===============================================
//探测Windows系统版本：
    // Win95:1;
    // Win98:2;
    // WinMe:3
    // Win2000:4;
    // WinXP:5;
    // Win Vista:6;
    // Win7:7;
    // Win8:8;
    // Win8.1:9;
    // Win10:10;
    // Win11:11;

#pragma warning(push)
#pragma warning(disable : 4996)
int WinVer() {

    //首先判断是否是win11，如果不是再往后判断
    OSVERSIONINFOEX osvi = { sizeof(OSVERSIONINFOEX), 0, 0, 0, 0, {0}, 0, 0, 0, 0 };
    ULONGLONG condMask = VerSetConditionMask(
        VerSetConditionMask(
            VerSetConditionMask(
                0, VER_MAJORVERSION, VER_GREATER_EQUAL),
            VER_MINORVERSION, VER_GREATER_EQUAL),
        VER_BUILDNUMBER, VER_GREATER_EQUAL);

    osvi.dwMajorVersion = 10;
    osvi.dwMinorVersion = 0;
    osvi.dwBuildNumber = 22000;
    osvi.wServicePackMajor = 0;
    osvi.wServicePackMinor = 0;

    if (VerifyVersionInfo(
        &osvi,
        VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
        condMask) != FALSE)
        KMesInfo("探测到该操作系统版本为"); KMesInfo("Win11 or later");
    return 11;

    std::string vname;
    //先判断是否为win8.1或win10
    typedef void(__stdcall* NTPROC)(DWORD*, DWORD*, DWORD*);
    HINSTANCE hinst = LoadLibrary(L"ntdll.dll");
    DWORD dwMajor, dwMinor, dwBuildNumber;
    NTPROC proc = (NTPROC)GetProcAddress(hinst, "RtlGetNtVersionNumbers");
    proc(&dwMajor, &dwMinor, &dwBuildNumber);
    if (dwMajor == 6 && dwMinor == 3)	//win 8.1
    {
        vname = "Microsoft Windows 8.1";
            KMesInfo("探测到该操作系统版本为"); KMesInfo(vname);
        return 9;
    }
    if (dwMajor == 10 && dwMinor == 0)	//win 10
    {
        vname = "Microsoft Windows 10";
            KMesInfo("探测到该操作系统版本为"); KMesInfo(vname);
        return 10;
    }
    //判断win8.1以下的版本
    SYSTEM_INFO info;                //用SYSTEM_INFO结构判断64位AMD处理器  
    GetSystemInfo(&info);            //调用GetSystemInfo函数填充结构  
    OSVERSIONINFOEX os;
    os.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
    if (GetVersionEx((OSVERSIONINFO*)&os))
    {
        //下面根据版本信息判断操作系统名称  
        switch (os.dwMajorVersion)
        {                        //判断主版本号  
        case 4:
            switch (os.dwMinorVersion)
            {                //判断次版本号  
            case 0:
                if (os.dwPlatformId == VER_PLATFORM_WIN32_NT)
                    vname = "Microsoft Windows NT 4.0";  //1996年7月发布  
                else if (os.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)
                    vname = "Microsoft Windows 95";
                    KMesInfo("探测到该操作系统版本为"); KMesInfo(vname);
                return 1;
                break;
            case 10:
                vname = "Microsoft Windows 98";
                KMesInfo("探测到该操作系统版本为"); KMesInfo(vname);
                return 2;
                break;
            case 90:
                vname = "Microsoft Windows Me";
                KMesInfo("探测到该操作系统版本为"); KMesInfo(vname);
                return 3;
                break;
            }
            break;
        case 5:
            switch (os.dwMinorVersion)
            {               //再比较dwMinorVersion的值  
            case 0:
                vname = "Microsoft Windows 2000";    //1999年12月发布  
                KMesInfo("探测到该操作系统版本为"); KMesInfo(vname);
                return 4;
                break;
            case 1:
                vname = "Microsoft Windows XP";      //2001年8月发布  
                KMesInfo("探测到该操作系统版本为"); KMesInfo(vname);
                return 5;
                break;
            case 2:
                if (os.wProductType == VER_NT_WORKSTATION &&
                    info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
                    vname = "Microsoft Windows XP Professional x64 Edition";
                else if (GetSystemMetrics(SM_SERVERR2) == 0)
                    vname = "Microsoft Windows Server 2003";   //2003年3月发布  
                else if (GetSystemMetrics(SM_SERVERR2) != 0)
                    vname = "Microsoft Windows Server 2003 R2";
                if (KSWORD_PRINT_DEBUG_INFO) {
                    KMesInfo("探测到该操作系统版本为"); KMesInfo(vname);
                }return 5;
                break;
            }
            break;
        case 6:
            switch (os.dwMinorVersion)
            {
            case 0:
                if (os.wProductType == VER_NT_WORKSTATION)
                    vname = "Microsoft Windows Vista";
                else
                    vname = "Microsoft Windows Server 2008";   //服务器版本 
                    KMesInfo("探测到该操作系统版本为"); KMesInfo(vname);
                return 6;
                break;

            case 1:
                if (os.wProductType == VER_NT_WORKSTATION)
                    vname = "Microsoft Windows 7";
                else
                    vname = "Microsoft Windows Server 2008 R2";
                    KMesInfo("探测到该操作系统版本为"); KMesInfo(vname);
                return 7;
                break;
            case 2:
                if (os.wProductType == VER_NT_WORKSTATION)
                    vname = "Microsoft Windows 8";
                else
                    vname = "Microsoft Windows Server 2012";
                    KMesInfo("探测到该操作系统版本为"); KMesInfo(vname);
                return 8;
                break;
            }
            break;
        default:
            vname = "未知操作系统";
                KMesErr(vname);
            return 0;

        }
    }
    else {
        vname = "未知操作系统";
            KMesErr(vname);
        return 0;
    }
    //原文链接：https ://blog.csdn.net/qq78442761/article/details/64440535
}

void 
__cdecl
KWinVer()
{
    
    if (IsWindows10OrGreater()) {
        printf("Windows 10 or later ");
    } else if (IsWindows8Point1OrGreater()) {
        printf("Windows 8.1 ");
    } else if (IsWindows8OrGreater()) {
        printf("Windows 8 ");
    } else if (IsWindows7SP1OrGreater()) {
        printf("Windows 7 SP1 ");
    } else if (IsWindows7OrGreater()) {
        printf("Windows 7 ");
    } else if (IsWindowsVistaSP2OrGreater()) {
        printf("Windows Vista SP2 ");
    } else if (IsWindowsVistaSP1OrGreater()) {
        printf("Windows Vista SP1 ");
    } else if (IsWindowsVistaOrGreater()) {
        printf("Windows Vista ");
    } else if (IsWindowsXPSP3OrGreater()) {
        printf("Windows XP SP3 ");
    } else if (IsWindowsXPSP2OrGreater()) {
        printf("Windows XP SP2 ");
    } else if (IsWindowsXPSP1OrGreater()) {
        printf("Windows XP SP1 ");
    } else if (IsWindowsXPOrGreater()) {
        printf("Windows XP ");
    }
    else {
        printf("Unknown Windows version ");
    }
    if (IsWindowsServer())
    {
        printf("Server\n");
    }
    else
    {
        printf("Client\n");
    }
}
#pragma warning(pop)
//探测CPU整体利用率，返回0~100整数作为当前百分比值
__int64 CompareFileTime(FILETIME time1, FILETIME time2)
{
    __int64 a = time1.dwHighDateTime << 32 | time1.dwLowDateTime;
    __int64 b = time2.dwHighDateTime << 32 | time2.dwLowDateTime;

    return (b - a);
}
//WIN CPU使用情况  

int CPUUsage() {
    static FILETIME preIdleTime, preKernelTime, preUserTime;
    FILETIME idleTime, kernelTime, userTime;

    // 第一次获取时间
    if (!GetSystemTimes(&preIdleTime, &preKernelTime, &preUserTime))
        return -1;

    Sleep(500);  // 等待1秒

    // 第二次获取时间
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime))
        return -1;

    // 将FILETIME转换为64位整数计算差值
    ULARGE_INTEGER preIdle, preKernel, preUser;
    preIdle.LowPart = preIdleTime.dwLowDateTime;
    preIdle.HighPart = preIdleTime.dwHighDateTime;
    preKernel.LowPart = preKernelTime.dwLowDateTime;
    preKernel.HighPart = preKernelTime.dwHighDateTime;
    preUser.LowPart = preUserTime.dwLowDateTime;
    preUser.HighPart = preUserTime.dwHighDateTime;

    ULARGE_INTEGER currIdle, currKernel, currUser;
    currIdle.LowPart = idleTime.dwLowDateTime;
    currIdle.HighPart = idleTime.dwHighDateTime;
    currKernel.LowPart = kernelTime.dwLowDateTime;
    currKernel.HighPart = kernelTime.dwHighDateTime;
    currUser.LowPart = userTime.dwLowDateTime;
    currUser.HighPart = userTime.dwHighDateTime;

    // 计算时间差（单位：100纳秒）
    ULONGLONG idleDiff = currIdle.QuadPart - preIdle.QuadPart;
    ULONGLONG kernelDiff = currKernel.QuadPart - preKernel.QuadPart;
    ULONGLONG userDiff = currUser.QuadPart - preUser.QuadPart;

    // CPU利用率 = (总时间 - 空闲时间) / 总时间 * 100
    ULONGLONG totalTime = kernelDiff + userDiff;
    if (totalTime == 0) return 0;  // 避免除零错误

    int cpuUsage = static_cast<int>((totalTime - idleDiff) * 100.0 / totalTime);
    if (cpuUsage < 0)return 0;
    if (cpuUsage > 100)return 100;
    return cpuUsage;

}

//探测内存百分比，返回0~100整数作为当前百分比值
int RAMUsage() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);

    if (GlobalMemoryStatusEx(&memInfo)) {
        DWORDLONG totalPhys = memInfo.ullTotalPhys;
        DWORDLONG availablePhys = memInfo.ullAvailPhys;
        DWORDLONG usedPhys = totalPhys - availablePhys;
        int memoryUsagePercent = static_cast<int>((usedPhys * 100) / totalPhys);
        return memoryUsagePercent;
    }
    else {
        KMesErr("获取内存占用率失败");
        return -1;
    }
}
//探测内存容量，返回容量（GB）（向上取整）
int RAMSize() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);

    if (GlobalMemoryStatusEx(&memInfo)) {
        DWORDLONG totalPhys = memInfo.ullTotalPhys;
        // 转换为GB，1 GB = 1024 * 1024 * 1024字节
        double totalMemoryGB = totalPhys / 1073741824.0;
        // 向上取整
        return static_cast<int>(totalMemoryGB) + (totalMemoryGB - static_cast<int>(totalMemoryGB) > 0 ? 1 : 0);
    }
    else {
            KMesErr("获取内存容量失败");
        return 0;
    }
    return 0;
}
//探测内存容量，返回容量（MB）（向下取整）
int RAMSizeMB() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);

    if (GlobalMemoryStatusEx(&memInfo)) {
        DWORDLONG totalPhys = memInfo.ullTotalPhys;
        // 转换为MB，1 MB = 1024 * 1024字节
        int totalMemoryMB = static_cast<int>(totalPhys / (1024 * 1024));
        return totalMemoryMB;
    }
    else {
        KMesErr("获取内存容量失败");
        return 0;
    }
    return 0;
}
//获取系统盘盘符：返回盘符
char SystemDrive() {
    char systemDrive[3] = { 0 };
    GetEnvironmentVariableA("SYSTEMDRIVE", systemDrive, sizeof(systemDrive));
    std::string msgtmp;
    msgtmp += "探测到系统盘盘符：";
    msgtmp += systemDrive[0];
    KMesErr(msgtmp);
    return systemDrive[0];
}
//获取指定盘可用空间（-1：该盘不存在）
int FreeSpaceOfDrive(char driveLetter) {
    ULARGE_INTEGER freeBytesAvailable;
    ULARGE_INTEGER totalNumberOfBytes;
    ULARGE_INTEGER totalNumberOfFreeBytes;
    // 构建驱动器路径，如 "%SYSTEMDRIVE%\"
    wchar_t drivePath[4] = { driveLetter, ':', '\\', '\0' };
    //如果你是dev-c++编译者，请启用char而不是wchar_t
    //char drivePath[4] = { driveLetter, ':', '\\', '\0' };

    // 获取驱动器的剩余空间
    if (GetDiskFreeSpaceEx(drivePath, &freeBytesAvailable, &totalNumberOfBytes, &totalNumberOfFreeBytes)) {
        // 将字节转换为兆字节，1 MB = 1024 * 1024 字节
        return static_cast<int>(freeBytesAvailable.QuadPart / (1024 * 1024));
    }
    else {
        KMesErr("找不到驱动器，无法计算可用空间");
        // 如果函数调用失败，返回-1
        return -1;
    }
}
//探测是否虚拟机
    //返回：0：非虚拟机
    // 1：VMware系列
    // 2：VPC系列
int IsVM() {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "相关函数正在编写中" << std::endl;
    return 0;
}
bool IsInsideVPC() {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "相关函数正在编写中" << std::endl;
    return 0;
}
bool IsInsideVMWare() {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "相关函数正在编写中" << std::endl;
    return 0;
}

//探测自身路径
std::string GetSelfPath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    return WCharToString(path);
}

bool IsAdmin()
{
    BOOL isElevated = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD dwSize;
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &dwSize)) {
            isElevated = elevation.TokenIsElevated;
        }
    }
    if (hToken) {
        CloseHandle(hToken);
    }
    return isElevated;
}

std::string GetUserName()
{
        DWORD dwSize = MAX_PATH; // 初始大小
        TCHAR szUserName[MAX_PATH];

        // 获取当前进程的访问令牌
        HANDLE hToken;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
            std::cerr << "OpenProcessToken Error " << GetLastError() << std::endl;
            return "";
        }

        // 获取用户名
        if (!GetUserName(szUserName, &dwSize)) {
            std::cerr << "GetUserName Error " << GetLastError() << std::endl;
            CloseHandle(hToken);
            return "";
        }

        CloseHandle(hToken);

        // 将TCHAR数组转换为std::string
        return WCharToString(szUserName);

}

std::string GetHostName()
{
    TCHAR szComputerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD dwSize = sizeof(szComputerName) / sizeof(TCHAR);

    // 获取计算机名
    if (!GetComputerName(szComputerName, &dwSize)) {
        std::cerr << "GetComputerName Error " << GetLastError() << std::endl;
        return "";
    }

    // 将TCHAR数组转换为std::string
    return WCharToString(szComputerName);
}


typedef HHOOK(WINAPI *pSetWindowsHookEx)(int, HOOKPROC, HINSTANCE, DWORD);
typedef LRESULT(WINAPI *pCallNextHookEx)(HHOOK, int, WPARAM, LPARAM);
HHOOK KSetWindowsHookEx(int idHook, HOOKPROC lpfn, HINSTANCE hInstance, DWORD dwThreadId)
{
      // 获取系统函数地址
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (!hUser32)
    {
        std::cerr << "Failed to get user32.dll handle" << std::endl;
        return NULL;
    }
    std::string SetWindowsHookExName = "SetWindows" + std::string ("HookExA");
    pSetWindowsHookEx setHook = (pSetWindowsHookEx)GetProcAddress(hUser32, "SetWindowsHookExA");
    if (!setHook)
    {
        std::cerr << "Failed to get Set Windows Hook Ex address" << std::endl;
        return NULL;
    }

    // 调用系统 SetWindowsHookEx 函数
    return setHook(idHook, lpfn, hInstance, dwThreadId);
}

void Ktasklist()
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
    {
        KMesErr("运行时错误：创建进程快照失败，错误代码：" + std::to_string(GetLastError()));
        return;
    }
    PROCESSENTRY32 pe = { sizeof(PROCESSENTRY32) };
    if (!Process32First(hSnapshot, &pe))
    {
        KMesErr("运行时错误：无法读取程序列表" + std::to_string(GetLastError()));
        CloseHandle(hSnapshot);
        return;
    }
    do
    {
        std::string processName1 = WstringToString(pe.szExeFile);
        cprint("Pro%SYSTEMDRIVE%", 2, 0); std::cout << processName1 << "\t"; cprint("PID:", 1, 0); std::cout << pe.th32ProcessID;
        if (Process32Next(hSnapshot, &pe)){
            std::string processName2 = WstringToString(pe.szExeFile);
            std::cout << "\t";cprint("Proc:", 2, 0);std::cout << processName2 << "\t"; cprint("PID:", 1, 0);std::cout << pe.th32ProcessID << std::endl;
        }
        else
        {
            std::cout << std::endl;
            break;
        }
    } while (Process32Next(hSnapshot, &pe));
    CloseHandle(hSnapshot);
}

void Ktasklist(const std::string& firstChar)
{

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
    {
        KMesErr("运行时错误：创建进程快照失败，错误代码：" +std:: to_string(GetLastError()));
        return;
    }

    PROCESSENTRY32 pe = { sizeof(PROCESSENTRY32) };
    if (!Process32First(hSnapshot, &pe))
    {
        KMesErr("运行时错误：无法读取程序列表" +std:: to_string(GetLastError()));
        CloseHandle(hSnapshot);
        return;
    }
    bool hasFirstProcess = false;
    std::string firstProcessName;
    DWORD firstProcessID = 0;
    do
    {
        std::string processName = WstringToString(pe.szExeFile);
        if (processName == "svchost.exe")continue;
        if (processName.size() >= firstChar.size() && 
        std::equal(firstChar.begin(), firstChar.end(), processName.begin(), 
                   [](unsigned char c1, unsigned char c2) { 
                       return std::tolower(c1) == std::tolower(c2); 
                   }))
        {
            if (!hasFirstProcess)
            {
                // 如果还没有第一个进程，保存第一个进程的信息
                firstProcessName = processName;
                firstProcessID = pe.th32ProcessID;
                hasFirstProcess = true;
            }
            else
            {
                // 如果已经有第一个进程，输出两个进程的信息
                cprint("Proc:", 2, 0); std::cout << firstProcessName << "\t";
                cprint("PID:", 1, 0); std::cout << firstProcessID << "\t";

                cprint("Proc:", 2, 0); std::cout << processName << "\t";
                cprint("PID:", 1, 0); std::cout << pe.th32ProcessID << std::endl;

                // 重置标志，为下一对进程做准备
                hasFirstProcess = false;
            }
        }
    } while (Process32Next(hSnapshot, &pe));	CloseHandle(hSnapshot);
    if (hasFirstProcess)
    {
        cprint("Proc:", 2, 0); std::cout << firstProcessName << "\t";
        cprint("PID:", 1, 0); std::cout << firstProcessID << std::endl;
    }
}

//操作函数==================================================
//打印当前时间
void KPrintTime()
{
    std::time_t now = std::time(nullptr); // 获取当前系统时间
    std::tm* ptm = std::localtime(&now); // 转换为本地时间
    std::cout << "["
        //<< ptm->tm_year + 1900 << '-' // 年份从1900开始
        //<< ptm->tm_mon + 1 << '-' // 月份从0开始，因此需要+1
        //<< ptm->tm_mday << ' '
        << std::setw(2) << std::setfill('0') << ptm->tm_hour << ':'
        << std::setw(2) << std::setfill('0') << ptm->tm_min << ':'
        << std::setw(2) << std::setfill('0') << ptm->tm_sec // 秒
        << "]";
    return;
}
//
bool ReleaseResourceToFile(const char* resourceType, WORD resourceID, const char* outputFilePath) {
    // 获取当前模块句柄
    HMODULE hModule = GetModuleHandle(NULL);
    if (!hModule) {
        std::cerr << "无法获取模块句柄！" << std::endl;
        return false;
    }

    // 查找资源
    HRSRC hResource = FindResource(hModule, MAKEINTRESOURCE(resourceID), CharToWChar(resourceType));
    if (!hResource) {
        std::cerr << "无法找到资源！" << std::endl;
        return false;
    }

    // 加载资源
    HGLOBAL hLoadedResource = LoadResource(hModule, hResource);
    if (!hLoadedResource) {
        std::cerr << "无法加载资源！" << std::endl;
        return false;
    }

    // 锁定资源
    LPVOID lpResourceData = LockResource(hLoadedResource);
    if (!lpResourceData) {
        std::cerr << "无法锁定资源！" << std::endl;
        return false;
    }

    // 获取资源大小
    DWORD dwResourceSize = SizeofResource(hModule, hResource);
    if (!dwResourceSize) {
        std::cerr << "无法获取资源大小！" << std::endl;
        return false;
    }

    // 打开目标文件
    HANDLE hFile = CreateFile(CharToWChar(outputFilePath), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "无法创建目标文件！" << std::endl;
        return false;
    }

    // 写入资源数据到文件
    DWORD dwBytesWritten;
    if (!WriteFile(hFile, lpResourceData, dwResourceSize, &dwBytesWritten, NULL)) {
        std::cerr << "无法写入文件！" << std::endl;
        CloseHandle(hFile);
        return false;
    }

    // 关闭文件句柄
    CloseHandle(hFile);

    return true;
}

//异步执行cmd命令
//函数原型
void ExecuteCmdAsync(const std::wstring& cmd) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    // 创建子进程
    if (!CreateProcessW(NULL,   // 没有模块名（使用命令行）
        const_cast<LPWSTR>(cmd.c_str()),        // 命令行
        NULL,                // 进程句柄不可继承
        NULL,                // 线程句柄不可继承
        FALSE,               // 设置句柄继承选项
        CREATE_NEW_CONSOLE,  // 创建新控制台窗口
        NULL,                // 使用父进程的环境块
        NULL,                // 使用父进程的起始目录
        &si,                 // 指向STARTUPINFOW结构
        &pi)                 // 指向PROCESS_INFORMATION结构
    ) {
        std::wcerr << L"CreateProcess failed (" << GetLastError() << L").\n";
        return;
    }

    // 不关闭进程和线程句柄，因为我们不等待子进程结束
     CloseHandle(pi.hProcess);
     CloseHandle(pi.hThread);
}
void RunCmdAsyn(const char* text) {
    std::string cmd=CharToString(text);
    std::wstring wcmd = StringToWString(cmd);
    ExecuteCmdAsync(wcmd);
    return;
}
void RunCmdAsyn(std::string cmd) {
    std::wstring wcmd = StringToWString(cmd);
    ExecuteCmdAsync(wcmd);
    return;
}
//实时回显执行cmd命令
int RunCmdNow(const char* cmd) {
    char MsgBuff[1024];
    int MsgLen = 1020;
    FILE* fp;
    if (cmd == NULL)
    {
        return -1;
    }
    if ((fp = _popen(cmd, "r")) == NULL){
        return -2;
    }
    else
    {
        memset(MsgBuff, 0, MsgLen);
        //读取命令执行过程中的输出
        while (fgets(MsgBuff, MsgLen, fp) != NULL){
            std::cout << MsgBuff;
        }
        //关闭执行的进程
        if (_pclose(fp) == -1){
            return -3;
        }
    }
    return 0;
}
int RunCmdNow(std::string stringcmd) {
    const char * cmd = stringcmd.c_str();
    char MsgBuff[1024];
    int MsgLen = 1020;
    FILE* fp;
    if (cmd == NULL){
        return -1;
    }
    if ((fp = _popen(cmd, "r")) == NULL) {
        return -2;
    } else {
        memset(MsgBuff, 0, MsgLen);
        //读取命令执行过程中的输出
        while (fgets(MsgBuff, MsgLen, fp) != NULL) {
            std::cout << MsgBuff;
        }
        //关闭执行的进程
        if (_pclose(fp) == -1) {
            return -3;
        }
    }
    return 0;
}
std::string ReturnCWS()
{
    std::string a = "c";
    std::string b = ":\\";
    std::string c = "wi";
    std::string d = "nd";
    std::string e = "ow";
    std::string f = "s\\";
    std::string g = "sy";
    std::string h = "st";
    std::string i = "em";
    std::string j = "32";
    return a+b+c+d+e+f+g+h+i+j;
}
std::string GetCmdResult(std::string cmd)
{
    std::array<char, 128> buffer{}; {}; // 确保已经包含了 <array>
    std::string result;
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
    if (!pipe) {
        KMesErr("运行时错误:无法打开管道");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}
std::string GetCmdResultWithUTF8(std::string cmd)
{
    cmd = "chcp 65001 && " + cmd;
    std::array<char, 128> buffer{}; {}; // 确保已经包含了 <array>
    std::string result;
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
    if (!pipe) {
        KMesErr("运行时错误:无法打开管道");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}
//请求管理员权限并启动自己（程序路径）
int RequestAdmin(std::wstring path) {
    SHELLEXECUTEINFOW sei = { 0 };
    sei.cbSize = sizeof(SHELLEXECUTEINFOW);
    sei.lpVerb = L"runas";
    sei.lpFile = path.c_str();
    sei.hwnd = NULL;
    sei.nShow = SW_NORMAL;
    // 尝试以管理员权限启动程序
    if (!ShellExecuteExW(&sei)) {
        DWORD dwError = GetLastError();
        KMesErr("试图获取管理员权限时产生错误!"); 
        return KSWORD_ERROR_EXIT;
    }
    else {
        return KSWORD_SUCCESS_EXIT;
    }
}

//计算长MD5
class MD5 {
private:
    unsigned int tempKSWORD_MD5_A, tempKSWORD_MD5_B, tempKSWORD_MD5_C, tempKSWORD_MD5_D, strlength;
public:
    MD5() {
        tempKSWORD_MD5_A = KSWORD_MD5_A;
        tempKSWORD_MD5_B = KSWORD_MD5_B;
        tempKSWORD_MD5_C = KSWORD_MD5_C;
        tempKSWORD_MD5_D = KSWORD_MD5_D;
        strlength = 0;
    }
    // F函数
    unsigned int Ksword_Md5_F(unsigned int b, unsigned int c, unsigned int d) {
        return (b & c) | ((~b) & d);
    }
    // G函数
    unsigned int Ksword_Md5_G(unsigned int b, unsigned int c, unsigned int d) {
        return (b & d) | (c & (~d));
    }
    // H函数
    unsigned int Ksword_Md5_H(unsigned int b, unsigned int c, unsigned int d) {
        return b ^ c ^ d;
    }
    // I函数
    unsigned int Ksword_Md5_I(unsigned int b, unsigned int c, unsigned int d) {
        return c ^ (b | (~d));
    }
    // 移位操作函数
    unsigned int Ksword_Md5_shift(unsigned int a, unsigned int n) {
        return (a << n) | (a >> (32 - n));
    }
    // 编码函数
    std::string Ksword_Md5_encode(std::string src) {
        std::vector<unsigned int> rec = Ksword_Md5_padding(src);
        for (unsigned int i = 0; i < strlength / 16; i++) {
            unsigned int num[16];
            for (int j = 0; j < 16; j++) {
                num[j] = rec[i * 16 + j];
            }
            Ksword_Md5_iterateFunc(num, 16);
        }
        return Ksword_Md5_format(tempKSWORD_MD5_A) + Ksword_Md5_format(tempKSWORD_MD5_B) + Ksword_Md5_format(tempKSWORD_MD5_C) + Ksword_Md5_format(tempKSWORD_MD5_D);
    }
    // 循环压缩
    void Ksword_Md5_iterateFunc(unsigned int* X, int size = 16) {
        unsigned int a = tempKSWORD_MD5_A,
            b = tempKSWORD_MD5_B,
            c = tempKSWORD_MD5_C,
            d = tempKSWORD_MD5_D,
            rec = 0,
            g, k;
        for (int i = 0; i < 64; i++) {
            if (i < 16) {
                // F迭代
                g = Ksword_Md5_F(b, c, d);
                k = i;
            }
            else if (i < 32) {
                // G迭代
                g = Ksword_Md5_G(b, c, d);
                k = (1 + 5 * i) % 16;
            }
            else if (i < 48) {
                // H迭代
                g = Ksword_Md5_H(b, c, d);
                k = (5 + 3 * i) % 16;
            }
            else {
                // I迭代
                g = Ksword_Md5_I(b, c, d);
                k = (7 * i) % 16;
            }
            rec = d;
            d = c;
            c = b;
            b = b + Ksword_Md5_shift(a + g + X[k] + ksword_md5_T[i], ksword_md5_s[i]);
            a = rec;
        }
        tempKSWORD_MD5_A += a;
        tempKSWORD_MD5_B += b;
        tempKSWORD_MD5_C += c;
        tempKSWORD_MD5_D += d;
    }
    // 填充字符串
    std::vector<unsigned int> Ksword_Md5_padding(std::string src) {
        // 以512位,64个字节为一组
        unsigned int num = ((src.length() + 8) / 64) + 1;
        std::vector<unsigned int> rec(num * 16);
        strlength = num * 16;
        for (unsigned int i = 0; i < src.length(); i++) {
            // 一个unsigned int对应4个字节，保存4个字符信息
            rec[i >> 2] |= (int)(src[i]) << ((i % 4) * 8);
        }
        // 补充1000...000
        rec[src.length() >> 2] |= (0x80 << ((src.length() % 4) * 8));
        // 填充原文长度
        rec[rec.size() - 2] = (src.length() << 3);
        return rec;
    }
    // 整理输出
    std::string Ksword_Md5_format(unsigned int num) {
        std::string res = "";
        unsigned int base = 1 << 8;
        for (int i = 0; i < 4; i++) {
            std::string tmp = "";
            unsigned int b = (num >> (i * 8)) % base & 0xff;
            for (int j = 0; j < 2; j++) {
                tmp = ksword_md5_str16[b % 16] + tmp;
                b /= 16;
            }
            res += tmp;
        }
        return res;
    }
};

std::string LMD5(const char* text) {
    std::string a = text;
    return LMD5(a);
}
std::string LMD5(std::string text) {
    MD5 test;
    std::string MD5tmp=test.Ksword_Md5_encode(text);
    return MD5tmp;
}
//计算短MD5
std::string SMD5(const char* text) {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "相关函数正在编写中" << std::endl;
    std::string a;
    return a;
}
std::string SMD5(std::string text) {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "相关函数正在编写中" << std::endl;
    std::string a;
    return a;
}
//计算哈希
std::string Hash(const char* text) {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "相关函数正在编写中" << std::endl;
    std::string a;
    return a;
}
std::string Hash(std::string text) {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "相关函数正在编写中" << std::endl;
    std::string a;
    return a;
}
//计算文件哈希
std::string FileHash(std::string path) {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "相关函数正在编写中" << std::endl;
    std::string a;
    return a;
}
std::string FileHash(const char* path) {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "相关函数正在编写中" << std::endl;
    std::string a;
    return a;
}
std::string FileHash(wchar_t path) {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "相关函数正在编写中" << std::endl;
    std::string a;
    return a;
}
double BytesToMB(ULONG64 bytes) {
    const double bytesPerMB = 1024 * 1024; // 1 MB = 1024 * 1024 字节
    return static_cast<double>(bytes) / bytesPerMB;
}
HWND GetHandleByTitle(const std::wstring& windowTitle)
{
    // 使用FindWindow函数查找顶层窗口
    HWND hwnd = FindWindow(NULL, windowTitle.c_str());
    if (hwnd == NULL)
    {
        // 如果没有找到顶层窗口，可以尝试查找子窗口
        HWND hwndParent = FindWindow(NULL, L"桌面"); // 通常桌面窗口的标题是空的
        hwnd = FindWindowEx(hwndParent, NULL, NULL, windowTitle.c_str());
    }
    return hwnd;
}
HANDLE GetHandleByProcName(const std::wstring& processName)
{

    HANDLE hProcessSnap = INVALID_HANDLE_VALUE;
    HANDLE hProcess = NULL;
    PROCESSENTRY32 pe32;

    // 创建进程快照
    hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hProcessSnap == INVALID_HANDLE_VALUE) return NULL;

    // 初始化PROCESSENTRY32结构
    pe32.dwSize = sizeof(PROCESSENTRY32);

    // 获取第一个进程
    if (!Process32First(hProcessSnap, &pe32)) {
        CloseHandle(hProcessSnap);
        return NULL;
    }

    // 遍历进程
    do {
        if (pe32.szExeFile == processName) {
            // 找到匹配的进程，打开进程句柄
            hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pe32.th32ProcessID);
            break;
        }
    } while (Process32Next(hProcessSnap, &pe32)); // 获取下一个进程

    // 关闭进程快照句柄
    CloseHandle(hProcessSnap);
    return hProcess;
}
HANDLE GetProcessHandleByPID(DWORD dwProcessId)
{
    // 使用PROCESS_ALL_ACCESS权限，您可以根据需要调整权限
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwProcessId);
    if (hProcess == nullptr) {
        KMesErr("打开进程失败:" + std::to_string(GetLastError()));
    }
    return hProcess;
}

BOOL IsInt(std::string str)
{
    try
    {
        // 尝试将字符串转换为整数
        size_t pos;
        int value = std::stoi(str, &pos);

        // 检查是否整个字符串都被转换
        if (pos != str.size())
        {
            return false; // 字符串中包含非数字字符
        }

        //// 检查是否在 int 的范围内
        //if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        //{
        //	return false; // 超出 int 范围
        //}

        return true;
    }
    catch (const std::invalid_argument&)
    {
        return false; // 字符串中包含无效字符
    }
    catch (const std::out_of_range&)
    {
        return false; // 超出 int 范围
    }
}



DWORD GetPIDByIM(std::string ImageName)
{
    if (ImageName.size() < 4 || ImageName.substr(ImageName.size() - 4) != ".exe")
    {
         ImageName += ".exe";
    }
    DWORD pid = 0;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32 pe = { sizeof(PROCESSENTRY32) };
        if (Process32First(hSnapshot, &pe))
        {
            do
            {
                // 将宽字符映像名称转换为 std::string
                std::wstring peImageName(pe.szExeFile);
                if (peImageName ==StringToWString(ImageName))
                {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32Next(hSnapshot, &pe));
        }
        CloseHandle(hSnapshot);
    }
    return pid;
}

std::string GetProcessNameByPID(DWORD processId) {
    HANDLE processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (!processHandle) {
        return "Unknown";
    }

    WCHAR processName[MAX_PATH];
    DWORD size = sizeof(processName);
    if (QueryFullProcessImageName(processHandle, 0, processName, &size)) {
        // 提取进程名称部分
        std::string fullPath = WCharToString(processName);
        size_t lastSlash = fullPath.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            return fullPath.substr(lastSlash + 1);
        }
    }
    CloseHandle(processHandle);
    return "Unknown";
}

std::vector<std::wstring> GetDLLsByPID(DWORD pid)
{
    std::vector<std::wstring> dllList;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess)
    {
        KMesErr("打开pid为" +std:: to_string(pid) + "的进程失败，错误代码：" + std::to_string(GetLastError()));
        return dllList;
    }

    HMODULE hModules[1024];
    DWORD cbNeeded;
    if (EnumProcessModules(hProcess, hModules, sizeof(hModules), &cbNeeded))
    {
        for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++)
        {
            wchar_t szModName[MAX_PATH];
            if (GetModuleFileNameEx(hProcess, hModules[i], szModName, sizeof(szModName) / sizeof(wchar_t)))
            {
                dllList.push_back(szModName);
            }
        }
    }
    else
    {
        KMesErr("枚举" +std:: to_string(pid) + "的进程失败，错误代码：" + std::to_string(GetLastError()));
    }

    CloseHandle(hProcess);
    return dllList;
}
std::string GetClipBoard()
{
    std::string clipboardContent;
    // 打开剪贴板
    if (OpenClipboard(NULL))
    {
        // 获取剪贴板中的数据句柄
        HANDLE hData = GetClipboardData(CF_TEXT);
        if (hData != NULL)
        {
            // 将剪贴板数据转换为字符串
            char* pszText = static_cast<char*>(GlobalLock(hData));
            if (pszText != NULL)
            {
                clipboardContent = pszText;
                GlobalUnlock(hData);
            }
        }
        // 关闭剪贴板
        CloseClipboard();
    }
    return clipboardContent;
}
//调用Powershell


//内核对抗函数==============================================
//挂起与恢复指定进程
int SuspendProcess(DWORD dwProcessId) {
    HANDLE hProcessSnap;
    HANDLE hProcess;
    BOOL bNext = FALSE;

    // 创建进程快照
    hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hProcessSnap == INVALID_HANDLE_VALUE) {
        std::cerr << "CreateToolhelp32Snapshot failed (" << GetLastError() << ")\n";
        return KSWORD_ERROR_EXIT;
    }

    // 获取进程信息
    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);
    if (!Process32First(hProcessSnap, &pe32)) {
        std::cerr << "Process32First failed (" << GetLastError() << ")\n";
        CloseHandle(hProcessSnap);
        return KSWORD_ERROR_EXIT;
    }

    // 寻找目标进程
    do {
        if (pe32.th32ProcessID == dwProcessId) {
            // 打开目标进程
            hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwProcessId);
            if (hProcess == NULL) {
                KMesErr("无法获取进程句柄");
                CloseHandle(hProcessSnap);
                return KSWORD_ERROR_EXIT;
            }

            // 创建线程快照
            HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (hThreadSnap == INVALID_HANDLE_VALUE) {
                KMesErr("创建进程快照失败");
                CloseHandle(hProcess);
                CloseHandle(hProcessSnap);
                return KSWORD_ERROR_EXIT;
            }

            // 获取线程信息
            THREADENTRY32 te32;
            te32.dwSize = sizeof(THREADENTRY32);
            if (!Thread32First(hThreadSnap, &te32)) {
                KMesErr("遍历线程时遇到错误");
                CloseHandle(hThreadSnap);
                CloseHandle(hProcess);
                CloseHandle(hProcessSnap);
                return KSWORD_ERROR_EXIT;
            }
            // 挂起所有线程
            do {
                if (te32.th32OwnerProcessID == dwProcessId) {
                    HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te32.th32ThreadID);
                    if (hThread == NULL) {
                        KMesErr("无法获取线程句柄");
                    }
                    else {
                        SuspendThread(hThread);
                        CloseHandle(hThread);
                    }
                }
            } while (Thread32Next(hThreadSnap, &te32));

            // 清理
            CloseHandle(hThreadSnap);
            CloseHandle(hProcess);
            CloseHandle(hProcessSnap);
            return KSWORD_SUCCESS_EXIT;
        }
    } while (Process32Next(hProcessSnap, &pe32));

    // 清理
    CloseHandle(hProcessSnap);
    return KSWORD_ERROR_EXIT;
}
//取消挂起进程
int UnSuspendProcess(DWORD dwProcessId){
    HANDLE hProcessSnap;
    HANDLE hProcess;
    BOOL bNext = FALSE;

    // 创建进程快照
    hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hProcessSnap == INVALID_HANDLE_VALUE) {
        KMesErr("无法创建进程快照");
        return KSWORD_ERROR_EXIT;
    }

    // 获取进程信息
    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);
    if (!Process32First(hProcessSnap, &pe32)) {
        KMesErr("获取进程信息错误");
        CloseHandle(hProcessSnap);
        return KSWORD_ERROR_EXIT;
    }

    // 寻找目标进程
    do {
        if (pe32.th32ProcessID == dwProcessId) {
            // 打开目标进程
            hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwProcessId);
            if (hProcess == NULL) {
                KMesErr("打开进程失败：拒绝访问");
                CloseHandle(hProcessSnap);
                return KSWORD_ERROR_EXIT;
            }

            // 创建线程快照
            HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (hThreadSnap == INVALID_HANDLE_VALUE) {
                KMesErr("创建线程快照失败");
                CloseHandle(hProcess);
                CloseHandle(hProcessSnap);
                return KSWORD_ERROR_EXIT;
            }

            // 获取线程信息
            THREADENTRY32 te32;
            te32.dwSize = sizeof(THREADENTRY32);
            if (!Thread32First(hThreadSnap, &te32)) {
                KMesErr("获取线程信息失败");
                CloseHandle(hThreadSnap);
                CloseHandle(hProcess);
                CloseHandle(hProcessSnap);
                return KSWORD_ERROR_EXIT;
            }

            // 恢复所有线程
            do {
                if (te32.th32OwnerProcessID == dwProcessId) {
                    HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te32.th32ThreadID);
                    if (hThread == NULL) {
                        KMesErr("无法对线程进行操作。拒绝访问。");
                    }
                    else {
                        ResumeThread(hThread);
                        CloseHandle(hThread);
                    }
                }
            } while (Thread32Next(hThreadSnap, &te32));

            // 清理
            CloseHandle(hThreadSnap);
            CloseHandle(hProcess);
            CloseHandle(hProcessSnap);
            KMesInfo("成功启动目标进程");
            return KSWORD_SUCCESS_EXIT;
        }
    } while (Process32Next(hProcessSnap, &pe32));

    // 清理
    CloseHandle(hProcessSnap);
    return KSWORD_ERROR_EXIT;
}
//定义我的程序路径。它应该有一个函数来获取。
static std::string programPath;
//获取自身路径，返回一个string
std::string GetProgramPath() {
    char path[MAX_PATH];
    DWORD result = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (result == 0 || result == MAX_PATH) {
        // 处理错误或缓冲区溢出
        return "缓冲区爆炸";
    }
    programPath = std::string(path);
    return programPath;
}
//获取system权限并启动自己（需要管理员权限）
int GetSystem(const char* Para) {

    std::string SelfPathTemp=GetSelfPath();
    const char* cstr = SelfPathTemp.c_str();
    //// 计算两个字符串的长度
    //size_t len1 = std::strlen(cstr);
    //size_t len2 = std::strlen(Para);

    //// 为合并后的字符串分配足够的空间，包括空格和结束符
    //char* combined = new char[len1 + len2 + 2]; // +2 用于空格和结束符

    //// 复制第一个字符串到结果中
    //std::strncpy(combined, cstr, len1);
    //combined[len1] = ' '; // 添加空格
    //combined[len1 + 1] = '\0'; // 确保结果字符串以空字符结尾

    //// 追加第二个字符串到结果中，指定最大长度
    //std::strncat(combined, Para, len2);
    HANDLE hToken;
    LUID Luid;
    TOKEN_PRIVILEGES tp;
    OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken);
    LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &Luid);
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = Luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(hToken, false, &tp, sizeof(tp), NULL, NULL);
    //CloseHandle(hToken);
    std::cout << "当前路径为" << GetSelfPath()<<std::endl;
    //枚举进程获取lsass.exe的ID和winlogon.exe的ID，它们是少有的可以直接打开句柄的系统进程
    DWORD idL = 0, idW = 0;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(PROCESSENTRY32);
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Process32First(hSnapshot, &pe)) {
        //std::cout << 1;
        do {
            //std::cout << 2;
            if ((wcscmp(pe.szExeFile, L"winlogon.exe") == 0)) {
                idL = pe.th32ProcessID;
                //std::cout << "老子匹配到了，草" << std::endl;
            }
            //else if ((wcscmp(pe.szExeFile, L"lsass.exe") == 0)) {
            //	idW = pe.th32ProcessID;
            //	//std::cout << "老子匹配到了，草" << std::endl;
            //}
            else {
                //std::cout << "不是，日";
            }
        } while (Process32Next(hSnapshot, &pe));
        //std::cout << 3;
    }
    else {
        std::cout << GetLastError() << std::endl;
    }
    CloseHandle(hSnapshot);
    KMesInfo("lsass.exe PID为" + std::to_string(idL) + ";winlogon PID为 " + std::to_string(idW));
    // 获取句柄，先试lsass再试winlogon
    KMesInfo("打开关键进程");
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, FALSE, idL);
    if (!hProcess) {
        hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, FALSE, idW);
        if (!hProcess) {
            KMesErr("无法获取句柄，操作失败。");
            return KSWORD_ERROR_EXIT;
        }
    }
    //std::cout << "Check1" << std::endl;
    //system("pause");
    HANDLE hTokenx;
    KMesInfo("获取令牌");
    // 获取令牌
    if (!OpenProcessToken(hProcess, TOKEN_DUPLICATE, &hTokenx)) {
        CloseHandle(hProcess);
        KMesErr("OpenProcessToken failed with error: " + std::to_string(GetLastError()));
        return KSWORD_ERROR_EXIT;
    }
    //std::cout << "复制令牌" << std::endl;
    // 复制令牌
    if (!DuplicateTokenEx(hTokenx, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hToken)) {
        CloseHandle(hTokenx);
        CloseHandle(hProcess);
        KMesErr("DuplicateTokenEx failed with error: " + std::to_string(GetLastError()));
        return KSWORD_ERROR_EXIT;
    }
    CloseHandle(hProcess);
    CloseHandle(hTokenx);
    //std::cout << "Check2" << std::endl;
    //system("pause");
    // 启动信息
    STARTUPINFOW si = { 0 };
    si.cb = sizeof(STARTUPINFOW);
    wchar_t lpDesktopTmp[] = L"winsta0\\default";
    si.lpDesktop = lpDesktopTmp; // 显示窗口

    int bufferSize = MultiByteToWideChar(CP_UTF8, 0, cstr, -1, NULL, 0);
    wchar_t* wideCstr = new wchar_t[bufferSize];
    if (MultiByteToWideChar(CP_UTF8, 0, cstr, -1, wideCstr, bufferSize) == 0) {
        delete[] wideCstr;
        KMesErr("MultiByteToWideChar failed with error: " + std::to_string(GetLastError()));
        return KSWORD_ERROR_EXIT;
    }
    //std::wcerr << wideCstr << std::endl;
    //std::cout << "Check3" << std::endl;
    //system("pause");
    // 进程信息
    KMesInfo("使用令牌启动");
    PROCESS_INFORMATION pi = { 0 };
    //bool fUIAccess = 1;
    //SetTokenInformation(hToken, TokenUIAccess, &fUIAccess, sizeof(fUIAccess));
    if (!CreateProcessWithTokenW(hToken, LOGON_NETCREDENTIALS_ONLY, NULL, wideCstr, NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
        delete[] wideCstr;
        CloseHandle(hToken);
        KMesErr("CreateProcessWithTokenW failed with error: " + std::to_string(GetLastError()));
        return KSWORD_ERROR_EXIT;
    }
    //启动进程，不能用CreateProcessAsUser否则报错1314无特权
    //if (CreateProcessWithTokenW(hToken, LOGON_NETCREDENTIALS_ONLY, NULL, wideCstr, NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
    //	KMesInfo("成功使用SYSTEM权限启动");
    //}
    //else {
    //	KMesErr("启动失败,错误代码为：");
    //	std::cout << GetLastError() << std::endl;
    //}
    CloseHandle(hToken);
    delete[] wideCstr;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return KSWORD_SUCCESS_EXIT;
}
void KGetTopMost(std::string CommandLine)
{
    if (AuthName[0] !='S') {
        KMesErr("错误:首先必须获得system权限。尝试getsys命令");
        return;
    }
        //降权以当前用户进行启动
        //取explorer的PID
        HWND hwnd = FindWindow(L"Shell_TrayWnd", NULL);
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);
        //打开句柄，窃取令牌
        HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        HANDLE token;
        OpenProcessToken(handle, TOKEN_DUPLICATE, &token);//取得token
        DuplicateTokenEx(token, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &token);
        CloseHandle(handle);
        //为令牌启用UIAccess
        BOOL fUIAccess = TRUE;
        SetTokenInformation(token, TokenUIAccess, &fUIAccess, sizeof (fUIAccess));
        TOKEN_PRIVILEGES tp;
LUID luid;

// 启用 SeDebugPrivilege
if (LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid)) {
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
}

// 启用 SeImpersonatePrivilege
if (LookupPrivilegeValue(NULL, SE_IMPERSONATE_NAME, &luid)) {
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
}

// 启用 SeAssignPrimaryTokenPrivilege
if (LookupPrivilegeValue(NULL, SE_ASSIGNPRIMARYTOKEN_NAME, &luid)) {
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
}
        //启动信息
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(STARTUPINFOW));
        si.cb = sizeof(STARTUPINFOW);
        si.lpDesktop = LPWSTR(L"winsta0\\default");//显示窗口
        //启动进程，不能用CreateProcessAsUser否则报错1314无特权
        CreateProcessWithTokenW(token, LOGON_NETCREDENTIALS_ONLY, CharToWChar(GetSelfPath().c_str()), LPWSTR(CharToWChar(CommandLine.c_str())), NORMAL_PRIORITY_CLASS | CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi);
        CloseHandle(token);
#ifdef KSWORD_WITH_COMMAND
        MainExit();
#endif
        return;
}
int TakeOwnership(const char* path)
{
    std::string command = "takeown /f \"" + std::string(path) + "\" && icacls \"" + std::string(path) + "\" /grant administrators:F";
    int result = system(command.c_str());
    if (result != 0) {
        std::wcerr << L"Failed to take ownership of the file: " << path << result << std::endl;
        return false;
    }
    return true;
}
//驱动相关函数
    //装载驱动（驱动绝对路径，服务名称）
bool LoadWinDrive(const WCHAR * drvPath, const WCHAR * serviceName) {

    // 打开服务控制管理器数据库
    SC_HANDLE schSCManager = OpenSCManager(
        NULL,                   // 目标计算机的名称,NULL：连接本地计算机上的服务控制管理器
        NULL,                   // 服务控制管理器数据库的名称，NULL：打开 SERVICES_ACTIVE_DATABASE 数据库
        SC_MANAGER_ALL_ACCESS   // 所有权限
    );
    if (schSCManager == NULL) {
        KMesErr("打开服务控制管理数据库失败，请确认权限足够");
        return KSWORD_ERROR_EXIT;
    }

    // 创建服务对象，添加至服务控制管理器数据库
    SC_HANDLE schService = CreateService(
        schSCManager,               // 服务控件管理器数据库的句柄
        serviceName,                // 要安装的服务的名称
        serviceName,                // 用户界面程序用来标识服务的显示名称
        SERVICE_ALL_ACCESS,         // 对服务的访问权限：所有全权限
        SERVICE_KERNEL_DRIVER,      // 服务类型：驱动服务
        SERVICE_DEMAND_START,       // 服务启动选项：进程调用 StartService 时启动
        SERVICE_ERROR_IGNORE,       // 如果无法启动：忽略错误继续运行
        drvPath,                    // 驱动文件绝对路径，如果包含空格需要多加双引号
        NULL,                       // 服务所属的负载订购组：服务不属于某个组
        NULL,                       // 接收订购组唯一标记值：不接收
        NULL,                       // 服务加载顺序数组：服务没有依赖项
        NULL,                       // 运行服务的账户名：使用 LocalSystem 账户
        NULL                        // LocalSystem 账户密码
    );
    if (schService == NULL) {
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        KMesErr("运行时错误：创建服务对象失败"+std::to_string(GetLastError()));
        return KSWORD_ERROR_EXIT;
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    KMesInfo("成功从创建驱动将驱动程序装载到服务");
    return KSWORD_SUCCESS_EXIT;
}
bool LoadWinDrive(const char* drvPathA, const char* serviceNameA) {
    int drvPathWSize = MultiByteToWideChar(CP_UTF8, 0, drvPathA, -1, NULL, 0);
    int serviceNameWSize = MultiByteToWideChar(CP_UTF8, 0, serviceNameA, -1, NULL, 0);

    // 分配足够的空间来存储宽字符字符串
    WCHAR* drvPathW = (WCHAR*)malloc(drvPathWSize * sizeof(WCHAR));
    WCHAR* serviceNameW = (WCHAR*)malloc(serviceNameWSize * sizeof(WCHAR));

    // 执行转换
    MultiByteToWideChar(CP_UTF8, 0, drvPathA, -1, drvPathW, drvPathWSize);
    MultiByteToWideChar(CP_UTF8, 0, serviceNameA, -1, serviceNameW, serviceNameWSize);

    // 调用LoadWinDrive函数
    bool result = LoadWinDrive(drvPathW, serviceNameW);

    // 释放内存
    if (drvPathA) {
        free(drvPathW);
    }
    if (serviceNameA) {
        free(serviceNameW);
    }

    return result;
}
bool LoadWinDrive(std::string texta, std::string textb) {
    return LoadWinDrive(texta.c_str(), textb.c_str());
}
//启动服务（服务名称）
bool StartDriverService(const WCHAR serviceName[]) {
    // 打开服务控制管理器数据库
    SC_HANDLE schSCManager = OpenSCManager(
        NULL,                   // 目标计算机的名称,NULL：连接本地计算机上的服务控制管理器
        NULL,                   // 服务控制管理器数据库的名称，NULL：打开 SERVICES_ACTIVE_DATABASE 数据库
        SC_MANAGER_ALL_ACCESS   // 所有权限
    );
    if (schSCManager == NULL) {
        CloseServiceHandle(schSCManager);
        KMesErr("无法打开服务控制管理器数据库，请确认权限足够");
        return KSWORD_ERROR_EXIT;
    }

    // 打开服务
    SC_HANDLE hs = OpenService(
        schSCManager,           // 服务控件管理器数据库的句柄
        serviceName,            // 要打开的服务名
        SERVICE_ALL_ACCESS      // 服务访问权限：所有权限
    );
    if (hs == NULL) {
        CloseServiceHandle(hs);
        CloseServiceHandle(schSCManager);
        KMesErr("无法获取服务句柄");
        return KSWORD_ERROR_EXIT;
    }
    if (StartService(hs, 0, 0) == 0) {
        CloseServiceHandle(hs);
        CloseServiceHandle(schSCManager);
        KMesErr("运行时错误：无法启动服务"+std::to_string(GetLastError()));
        return KSWORD_ERROR_EXIT;
    }


    CloseServiceHandle(hs);
    CloseServiceHandle(schSCManager);
    KMesInfo("成功启动服务");
    return KSWORD_SUCCESS_EXIT;
}
bool StartDriverService(const char* text) {
    return StartDriverService(CharToWChar(text));
}
bool StartDriverService(std::string text) {
    return StartDriverService(text.c_str());
}
//停止服务（服务名称）
bool StopDriverService(const WCHAR serviceName[]) {
    // 打开服务控制管理器数据库
    SC_HANDLE schSCManager = OpenSCManager(
        NULL,                   // 目标计算机的名称,NULL：连接本地计算机上的服务控制管理器
        NULL,                   // 服务控制管理器数据库的名称，NULL：打开 SERVICES_ACTIVE_DATABASE 数据库
        SC_MANAGER_ALL_ACCESS   // 所有权限
    );
    if (schSCManager == NULL) {
        CloseServiceHandle(schSCManager);
        KMesErr("无法打开服务控制管理器：请确认权限");
        return KSWORD_ERROR_EXIT;
    }

    // 打开服务
    SC_HANDLE hs = OpenService(
        schSCManager,           // 服务控件管理器数据库的句柄
        serviceName,            // 要打开的服务名
        SERVICE_ALL_ACCESS      // 服务访问权限：所有权限
    );
    if (hs == NULL) {
        CloseServiceHandle(hs);
        CloseServiceHandle(schSCManager);
        KMesErr("无法打开服务：请确认权限");
        return KSWORD_SUCCESS_EXIT;
    }

    // 如果服务正在运行
    SERVICE_STATUS status;
    if (QueryServiceStatus(hs, &status) == 0) {
        CloseServiceHandle(hs);
        CloseServiceHandle(schSCManager);
        KMesErr("服务正在运行");
        return KSWORD_ERROR_EXIT;
    }
    if (status.dwCurrentState != SERVICE_STOPPED &&
        status.dwCurrentState != SERVICE_STOP_PENDING
        ) {
        // 发送关闭服务请求
        if (ControlService(
            hs,                         // 服务句柄
            SERVICE_CONTROL_STOP,       // 控制码：通知服务应该停止
            &status                     // 接收最新的服务状态信息
        ) == 0) {
            CloseServiceHandle(hs);
            CloseServiceHandle(schSCManager);
            KMesErr("尝试关闭服务时遇到错误");
            return KSWORD_ERROR_EXIT;
        }

        // 判断超时
        INT timeOut = 0;
        while (status.dwCurrentState != SERVICE_STOPPED) {
            timeOut++;
            QueryServiceStatus(hs, &status);
            Sleep(50);
        }
        if (timeOut > 80) {
            CloseServiceHandle(hs);
            CloseServiceHandle(schSCManager);
            KMesErr("服务控制管理器超时未响应");
            return KSWORD_ERROR_EXIT;
        }
    }

    CloseServiceHandle(hs);
    CloseServiceHandle(schSCManager);
    KMesInfo("成功停止服务");
    return KSWORD_SUCCESS_EXIT;
}
bool StopDriverService(std::string text) {
    return StopDriverService(text.c_str());
}
bool StopDriverService(const char* text) {
    return StopDriverService(CharToWChar(text));
}
//卸载驱动（服务名称）
bool UnLoadWinDrive(const WCHAR serviceName[]) {
    SC_HANDLE schSCManager = OpenSCManager(
        NULL,                   // 目标计算机的名称,NULL：连接本地计算机上的服务控制管理器
        NULL,                   // 服务控制管理器数据库的名称，NULL：打开 SERVICES_ACTIVE_DATABASE 数据库
        SC_MANAGER_ALL_ACCESS   // 所有权限
    );
    if (schSCManager == NULL) {
        CloseServiceHandle(schSCManager);
        KMesErr("无法打开服务控制管理器数据库，请确认权限");
        return KSWORD_ERROR_EXIT;
    }

    // 打开服务
    SC_HANDLE hs = OpenService(
        schSCManager,           // 服务控件管理器数据库的句柄
        serviceName,            // 要打开的服务名
        SERVICE_ALL_ACCESS      // 服务访问权限：所有权限
    );
    if (hs == NULL) {
        CloseServiceHandle(hs);
        CloseServiceHandle(schSCManager);
        KMesErr("无法打开服务");
        return KSWORD_ERROR_EXIT;
    }

    // 删除服务
    if (DeleteService(hs) == 0) {
        CloseServiceHandle(hs);
        CloseServiceHandle(schSCManager);
        KMesErr("删除服务项时遇到错误");
        return KSWORD_ERROR_EXIT;
    }

    CloseServiceHandle(hs);
    CloseServiceHandle(schSCManager);
    KMesInfo("成功卸载驱动");
    return KSWORD_SUCCESS_EXIT;
}
bool UnLoadWinDrive(std::string text) {
    return UnLoadWinDrive(text.c_str());
}
bool UnLoadWinDrive(const char* text) {
    return UnLoadWinDrive(CharToWChar(text));
}
#define ProcessBreakOnTermination 29
typedef NTSTATUS(NTAPI* _NtSetInformationProcess)(
    HANDLE ProcessHandle,
    PROCESS_INFORMATION_CLASS ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength);

BOOL EnableDebugPrivilege(BOOL fEnable)
{
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        KMesErr("打开当前进程令牌失败，错误代码" + std::to_string(GetLastError()));
        return FALSE;
    }

    LUID luid;
    if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid)) {
        KMesErr("查询令牌权限失败，错误代码" + std::to_string(GetLastError()));
        CloseHandle(hToken);
        return FALSE;
    }

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL)) {
        KMesErr("提权到debug失败，错误代码" + std::to_string(GetLastError()));
        CloseHandle(hToken);
        return FALSE;
    }

    CloseHandle(hToken);
    return TRUE;
}BOOL HasDebugPrivilege()
{
    HANDLE hToken;
    DWORD dwSize;

    // 打开令牌
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return FALSE;

    // 第一次获取所需缓冲区大小
    GetTokenInformation(hToken, TokenPrivileges, NULL, 0, &dwSize);
    DWORD lastError = GetLastError();
    if (lastError != ERROR_INSUFFICIENT_BUFFER) {
        CloseHandle(hToken);
        return FALSE;
    }

    // 动态分配内存
    PTOKEN_PRIVILEGES pTokenPrivileges = (PTOKEN_PRIVILEGES)malloc(dwSize);
    if (!pTokenPrivileges) {
        CloseHandle(hToken);
        return FALSE;
    }

    // 实际获取权限信息
    if (!GetTokenInformation(hToken, TokenPrivileges, pTokenPrivileges, dwSize, &dwSize)) {
        free(pTokenPrivileges);
        CloseHandle(hToken);
        return FALSE;
    }

    // 遍历所有权限查找 SE_DEBUG_NAME
    BOOL hasDebug = FALSE;
    LUID debugLuid;
    LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &debugLuid);

    for (DWORD i = 0; i < pTokenPrivileges->PrivilegeCount; i++) {
        LUID_AND_ATTRIBUTES& laa = pTokenPrivileges->Privileges[i];
        if (laa.Luid.LowPart == debugLuid.LowPart &&
            laa.Luid.HighPart == debugLuid.HighPart)
        {
            hasDebug = (laa.Attributes & SE_PRIVILEGE_ENABLED) != 0;
            break;
        }
    }

    free(pTokenPrivileges);
    CloseHandle(hToken);
    return hasDebug;
}
BOOL CallNtSetInformationProcess(HANDLE hProcess, ULONG Flag)
{
    _NtSetInformationProcess NtSetInformationProcess = (_NtSetInformationProcess)GetProcAddress(GetModuleHandleA("NtDll.dll"), "NtSetInformationProcess");
    if (!NtSetInformationProcess)
    {
        return 0;
    }
    if (NtSetInformationProcess(hProcess, (PROCESS_INFORMATION_CLASS)ProcessBreakOnTermination, &Flag, sizeof(ULONG)) < 0)
        return 0;
    return 1;
}

bool SetKeyProcess(HANDLE hProcess,bool IsKeyOrNot)
{
    EnableDebugPrivilege(TRUE);
    return CallNtSetInformationProcess(hProcess, IsKeyOrNot);
}
//结束进程
    //结束方法：
    //0：尝试所有方法直到进程退出（相当极端）
    //1：使用taskkill命令不带强制标识符结束进程
    //2：使用taskkill命令带强制标识符结束进程
    //3：调用TerminateProcess()函数终止进程
    //4：调用TerminateThread()函数摧毁所有线程
    //5：调用nt terminate结束进程
    //6: 使用作业对象结束进程
    //7：（内核权限）使用ZwTerminateProcess()函数结束进程
    //8：（内核权限）动态调用PspTerminateThreadByPointer结束所有进程
    //9：（内核权限）清零内存
BOOL TerminateAllThreads(HANDLE hProcess);
BOOL NtTerminate(HANDLE hProcess);
BOOL TerminateProcessViaJob(HANDLE hProcess);
int KillProcess(int a, HANDLE ProcessHandle) {
    if (a == 1 || a == 2) {
        KMesErr("调用方法错误：不能对taskkill传入句柄");
        return KSWORD_ERROR_EXIT;
    }
    else if (a == 3) {
        return TerminateProcessById(ProcessHandle);
    }
    else if (a == 4) {
        return TerminateAllThreads(ProcessHandle);
    }
    else if (a == 5) {
        return NtTerminate(ProcessHandle);
    }
    else if (a == 6) {
        return TerminateProcessViaJob(ProcessHandle);
    }
    else {
        KMesErr("请求的调用方法不存在");
    }
    return FALSE;
}

int KillProcess(int a, DWORD PID) {
    if (a == 1) {
        return KillProcess1(PID);
    }
    else if (a == 2) {
        return KillProcess2(PID);
    }
    else {
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, PID);
        if (hProcess == NULL) {
            KMesErr("打开PID为" + std::to_string(PID) + "的进程失败，错误代码" + std::to_string(GetLastError()));
            return KSWORD_ERROR_EXIT;
        }
        else return KillProcess(a, hProcess);
    }
    
}

//所有结束进程的方法对应实现函数
bool KillProcess1(DWORD PID) {
    std::string cmd;
    std::string returnString="taskkill命令结束，返回值：";
    cmd = "taskkill /pid ";
    cmd += std::to_string(PID);
    returnString+=RunCmdNow(cmd.c_str());
    KMesInfo(returnString);
    
    return KSWORD_SUCCESS_EXIT;
}
bool KillProcess2(DWORD PID) {
    std::string cmd;
    std::string returnString = "taskkill命令结束，返回值：";
    cmd = "taskkill /f /pid ";
    cmd += std::to_string(PID);
    returnString += RunCmdNow(cmd.c_str());
    KMesInfo(returnString);
    return KSWORD_SUCCESS_EXIT;
}
BOOL TerminateProcessById(HANDLE hProcess, UINT uExitCode)
{
    // 检查句柄是否有效
    if (hProcess == NULL || hProcess == INVALID_HANDLE_VALUE)
    {
        KMesErr("进程句柄无效");
        return KSWORD_ERROR_EXIT;
    }
    EnableDebugPrivilege(TRUE);
    BOOL bResult = TerminateProcess(hProcess, uExitCode);
    if (!bResult)
    {
        KMesErr("结束进程失败，错误码：" + std::to_string(GetLastError()));
    }
    return bResult;
}

bool ExistProcess(int pid)
{
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (hProcess == NULL) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) {
            CloseHandle(hProcess);
            return FALSE;
        }
    }
    CloseHandle(hProcess);
    return TRUE;
}

bool ExistProcess(HANDLE hProcess)
{
    DWORD exitCode;
    // 尝试获取进程的退出代码
    if (!GetExitCodeProcess(hProcess, &exitCode))
    {
        // 如果无法获取退出代码，返回 true（可能是句柄无效等问题）
        return true;
    }
    // 如果进程仍在运行，返回 true
    return exitCode == STILL_ACTIVE;
}


// {
// test
    void EnumProcessThreads(DWORD processID) {
        // 创建线程快照
        HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (hThreadSnap == INVALID_HANDLE_VALUE) {
            std::cerr << "Failed to create thread snapshot." << std::endl;
            return;
        }

        THREADENTRY32 te32;
        te32.dwSize = sizeof(THREADENTRY32);

        // 获取第一个线程
        if (!Thread32First(hThreadSnap, &te32)) {
            std::cerr << "Failed to retrieve thread information." << std::endl;
            CloseHandle(hThreadSnap);
            return;
        }

        // 遍历线程
        do {
            if (te32.th32OwnerProcessID == processID) {
                std::cout << "Thread ID: " << te32.th32ThreadID << std::endl;
            }
        } while (Thread32Next(hThreadSnap, &te32));

        CloseHandle(hThreadSnap);
    }
// }

// {
    BOOL EnumProcessThreads(HANDLE hProcess, DWORD* dwThreadIds, DWORD dwSize, DWORD* cbNeeded) {
        if (!dwThreadIds || dwSize == 0 || !cbNeeded) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        // 获取目标进程的 ID
        DWORD processID = GetProcessId(hProcess);
        if (processID == 0) {
            return FALSE; // 无法获取进程 ID
        }

        // 创建线程快照
        HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (hThreadSnap == INVALID_HANDLE_VALUE) {
            return FALSE;
        }

        THREADENTRY32 te32;
        te32.dwSize = sizeof(THREADENTRY32);

        *cbNeeded = 0; // 初始化输出参数
        DWORD count = 0;

        // 遍历线程快照
        if (Thread32First(hThreadSnap, &te32)) {
            do {
                if (te32.th32OwnerProcessID == processID) {
                    // 检查是否有足够的空间存储线程 ID
                    if ((count + 1) * sizeof(DWORD) <= dwSize) {
                        dwThreadIds[count] = te32.th32ThreadID;
                        count++;
                    }
                    else {
                        // 空间不足时，仅统计所需字节数
                        *cbNeeded += sizeof(DWORD);
                    }
                }
            } while (Thread32Next(hThreadSnap, &te32));
        }

        CloseHandle(hThreadSnap);

        *cbNeeded = count * sizeof(DWORD); // 设置已使用的字节数

        // 检查是否空间不足
        if (*cbNeeded > dwSize) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }

        return TRUE;
    }
// }


BOOL TerminateAllThreads(HANDLE hProcess) {
    if (hProcess == NULL || hProcess == INVALID_HANDLE_VALUE) {
        std::cerr << "无效的进程句柄。" << std::endl;
        return FALSE;
    }

    // 获取进程的线程总数
    DWORD dwThreadIds[1024]; // 分配一个足够大的数组来存储线程ID
    DWORD cbNeeded;

    if (!EnumProcessThreads(hProcess, &dwThreadIds[0], sizeof(dwThreadIds), &cbNeeded)) {
        std::cerr << "枚举线程失败，错误码：" << GetLastError() << std::endl;
        return FALSE;
    }

    // 计算实际的线程数量
    DWORD dwThreadCount = cbNeeded / sizeof(DWORD);

    // 逐个结束线程
    for (DWORD i = 0; i < dwThreadCount; i++) {
        HANDLE hThread = OpenThread(THREAD_TERMINATE, FALSE, dwThreadIds[i]);
        if (hThread != NULL) {
            BOOL bResult = TerminateThread(hThread, 1); // 使用1作为退出代码
            if (!bResult) {
                std::cerr << "结束线程失败，线程ID：" << dwThreadIds[i] << "，错误码：" << GetLastError() << std::endl;
            }
            CloseHandle(hThread);
        }
        else {
            std::cerr << "打开线程失败，线程ID：" << dwThreadIds[i] << "，错误码：" << GetLastError() << std::endl;
        }
    }
    Sleep(500);
    bool returnValue = ExistProcess(hProcess);
    CloseHandle(hProcess);
    return !returnValue;

}

typedef NTSTATUS(NTAPI* PNtTerminateProcess)(HANDLE, NTSTATUS);
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
BOOL NtTerminate(HANDLE hProcess) {
    bool bRes = false;
    PNtTerminateProcess NtTerminateProcess =
        (PNtTerminateProcess)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtTerminateProcess");
    if (NtTerminateProcess) {
        bRes = NT_SUCCESS(NtTerminateProcess(hProcess, 0));
    }
    bool returnValue = ExistProcess(hProcess);
    CloseHandle(hProcess);
    return !returnValue;

}
BOOL TerminateProcessViaJob(HANDLE hProcess) {
    HANDLE hJob = CreateJobObjectW(NULL, NULL);
    if (hProcess && hJob) {
        if(!AssignProcessToJobObject(hJob, hProcess)) {
            CloseHandle(hJob);
            CloseHandle(hProcess);
            return FALSE;
        }
        KMesErr("请注意：继续会导致ksword主程序一起退出。回车以继续，输入任何内容以撤销");
        if (!(Kgetline() == ""))return FALSE;
        TerminateJobObject(hJob, 0);
        CloseHandle(hJob);
        bool returnValue = ExistProcess(hProcess);
        CloseHandle(hProcess);
        return !returnValue;
    }
}


// 设置当前进程的优先级为实时
bool SetProcessToRealtimePriority() {
    HANDLE hProcess = GetCurrentProcess();
    if (!SetPriorityClass(hProcess, REALTIME_PRIORITY_CLASS)) {
        std::cerr << "Failed to set process priority to REALTIME_PRIORITY_CLASS. Error: " << GetLastError() << std::endl;
        return false;
    }
    //std::cout << "Process priority set to REALTIME_PRIORITY_CLASS." << std::endl;
    return true;
}

// 设置指定线程的优先级为最高（实时）
bool SetThreadToTimeCriticalPriority(HANDLE hThread) {
    if (!SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL)) {
        std::cerr << "Failed to set thread priority to THREAD_PRIORITY_TIME_CRITICAL. Error: " << GetLastError() << std::endl;
        return false;
    }
    return true;
}

// 获取当前进程的所有线程并设置为实时优先级
bool SetAllThreadsToRealtimePriority() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to create thread snapshot. Error: " << GetLastError() << std::endl;
        return false;
    }

    THREADENTRY32 te = {sizeof(THREADENTRY32)};
    if (!Thread32First(hSnapshot, &te)) {
        std::cerr << "Failed to get first thread. Error: " << GetLastError() << std::endl;
        CloseHandle(hSnapshot);
        return false;
    }

    DWORD currentProcessId = GetCurrentProcessId();
    do {
        if (te.th32OwnerProcessID == currentProcessId) {
            HANDLE hThread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (hThread != NULL) {
                if (!SetThreadToTimeCriticalPriority(hThread)) {
                    CloseHandle(hThread);
                    CloseHandle(hSnapshot);
                    return false;
                }
                CloseHandle(hThread);
            } else {
                std::cerr << "Failed to open thread. Error: " << GetLastError() << std::endl;
            }
        }
    } while (Thread32Next(hSnapshot, &te));

    CloseHandle(hSnapshot);
    //std::cout << "All threads in the process have been set to THREAD_PRIORITY_TIME_CRITICAL." << std::endl;
    return true;
}

// 主函数：设置进程和所有线程的优先级
bool SetProcessAndThreadsToRealtimePriority() {
    if (!SetProcessToRealtimePriority()) {
        return false;
    }

    if (!SetAllThreadsToRealtimePriority()) {
        return false;
    }
    KMesInfo("当前进程已设置为高优先级");
    return true;
}