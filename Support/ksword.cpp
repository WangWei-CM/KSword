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
//               ���汣��         ����BUG
#include "../Main/KswordTotalHead.h"


#include "ksword.h"

#define KSWORD_VERSION_A 0
#define KSWORD_VERSION_B 0
#define KSWORD_VERSION_C 0
#define KSWORD_VERSION_D 0
#define KSWORD_VERSION_SPECIAL "Dev"


//**********Ksword Head File**********//
//Developed By WangWei_CM.,Explore & Kali-Sword Dev Team.
//רΪС��Ŀ������̨��Ŀ��windowsAPI��Ŀ���ں˶Կ������ṩ�ļ�ͷ�ļ�
//������Ⱥ�ģ�774070323������QQ��3325144899

//��ӡlogo

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
        cprint("��", 15, 1);
        Sleep(5);
    }
    for (int i = 1; i <= 70; i++) {
        putchar('\b');
    }*/
    std::cout << "Ksword Framework Developed by WangWei_CM.,Explore,Ksword Dev Team,etc.";
    std::cout << std::endl;
}
//��ȡ�汾����Ϣ
KVersionInfo KGetVersion() {
    KVersionInfo kVersionInfo;
    kVersionInfo.a = KSWORD_VERSION_A;
    kVersionInfo.b = KSWORD_VERSION_B;
    kVersionInfo.c = KSWORD_VERSION_C;
    kVersionInfo.d = KSWORD_VERSION_D;
    kVersionInfo.e = "KSWORD_VERSION_SPECIAL";
    return kVersionInfo;
};
//��ʽת������
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
//���������Ӧ��ֱ��const char*=<string>������������ڴ�й©���ǵ�delete
const char* StringToChar(const std::string& str) {
    char* cstr = new char[str.length() + 1];
    // �����ַ�������
    std::strcpy(cstr, str.c_str());
    // ����ָ���Ƶ�C����ַ�����ָ��
    return cstr;
}
//const char*תΪWCHAR*
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
    // ���ȼ���ַ����Ƿ�Ϊ��
    if (str.empty()) {
        return 0;
    }

    // ����ַ����Ƿ�ֻ���������ַ�
    for (char ch : str) {
        if (!std::isdigit(ch)) {
            KMesErr("�����Ϲ淶����������ת����std::string_to_int");
            return 0;
        }
    }

    // ʹ��stoi�������ַ���ת��Ϊ����
    int num = std::stoi(str);
    return num;
}
short StringToShort(std::string str)
{
    try {
        return static_cast<short>(std::stoi(str));
    }
    catch (const std::invalid_argument& e) {
        KMesErr("���������������ת��:std::string->short");
        return 0; // ���׳��쳣
    }
    catch (const std::out_of_range& e) {
        KMesErr("���������������ת��:std::string->short");
        return 0; // ���׳��쳣
    }
}
//Ksword�淶��Ϣ����=========================================
void Kpause() {
    std::cout << "�������е��ϵ㣬����ͣ�����س�����";
    Kgetline();
}
//��Ϣ��ʾ[ * ]
void KMesInfo(const char* text) {
    if(KSWORD_MES_LEVEL<=0){
    cprint("[ * ]", 9, 0);
    std::cout << text << std::endl;
    kLog.Add(Info, C(text), C("Ksword���"));
#ifdef KSWORD_WITH_COMMAND
    if(!isGUI)
    KswordSend1("������һ����Ϣ��" + std::string(text));
#endif
    }
}
void KMesInfo(std::string text) {
    KMesInfo(text.c_str());
}

//������ʾ[ ! ]
void KMesWarn(std::string text) {
    KMesWarn(text.c_str());
}
void KMesWarn(const char* text) {
    if(KSWORD_MES_LEVEL<=0){
    cprint("[ ! ]", 6, 0);
    std::cout << text << std::endl;
    kLog.Add(Warn, C(text), C("Ksword���"));
#ifdef KSWORD_WITH_COMMAND
    if (!isGUI)
    KswordSend1("������һ�����棺" + std::string(text));
#endif
    }
}

//������ʾ[ X ]
void KMesErr(std::string text) {
    KMesErr(text.c_str());
}
void KMesErr(const char* text) {
    if(KSWORD_MES_LEVEL<=0){
    cprint("[ �� ]", 4, 0);
    std::cout << text << std::endl;
    kLog.Add(Err, C(text), C("Ksword���"));
#ifdef KSWORD_WITH_COMMAND
    if (!isGUI)

        KswordSend1("������һ������" + std::string(text));
#endif
    }
}

//����̨�������Բٿغ���=====================================
//�ö���ȡ���ö�����
bool SetTopWindow() {
    HWND hWnd = GetConsoleWindow(); // ��ȡ����̨���ھ��
    if (hWnd != NULL) {
        // �������ö�
        if (!SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE)) {
            KMesErr("SetWindowPos ����ʧ��");
            return false;
        }
        return true;
    } else {
        KMesErr("��ȡ���ʧ��");
        return false;
    }
}
bool UnTopWindow() {
    HWND hWnd = GetConsoleWindow(); // ��ȡ����̨���ھ��
    if (hWnd != NULL) {
        // ȡ�������ö�
        SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        return KSWORD_SUCCESS_EXIT;
    } else {
        KMesErr("��ȡ���ʧ��");
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
    // ��ǰ̨�����߳���������ǰ�̣߳�Ҳ���ǳ���A�еĵ����̣߳�
    ::AttachThreadInput(dwCurID, dwForeID, FALSE);
    //KMesInfo("���ڳɹ��ö���");
    return KSWORD_SUCCESS_EXIT;
//ԭ�����ӣ�https://blog.csdn.net/weixin_45525272/article/details/116452142
}

//������ȡ�����غ���
bool HideWindow() {
    HWND hWnd = GetConsoleWindow();
    if (hWnd != NULL) {
        ShowWindow(hWnd, SW_HIDE);
        //KMesInfo("�����Ѿ�����");
        return KSWORD_SUCCESS_EXIT;
    } else {
        KMesErr("��ȡ���ʧ�ܣ�");
        return KSWORD_ERROR_EXIT;
    }
}
bool ShowWindow() {
    HWND hWnd = GetConsoleWindow();
    if (hWnd != NULL) {
        ShowWindow(hWnd, SW_SHOW);
        //KMesInfo("����״̬����Ϊ��ʾ");
        return KSWORD_SUCCESS_EXIT;
    } else {
        KMesErr("��ȡ���ʧ��!");
        return KSWORD_ERROR_EXIT;
    }
}
//���ش��ڱ߿�
bool HideSide() {
    // ��ȡ����̨���ڵľ��
    HWND hWnd = GetConsoleWindow();
    // ��ȡ���ڵĵ�ǰ��ʽ
    LONG style = GetWindowLong(hWnd, GWL_STYLE);
    // �Ƴ�������ʽ�еĴ��ڱ߿�ͱ�������־
        style &= ~(WS_OVERLAPPED | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_VSCROLL | WS_HSCROLL);

    // Ӧ���µ���ʽ
    SetWindowLong(hWnd, GWL_STYLE, style);
    // ���´����Է�ӳ�µ���ʽ
    SetWindowPos(hWnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    return KSWORD_SUCCESS_EXIT;
}
//����ȫ��
bool KFullScreen() {
    HWND hwnd = GetConsoleWindow();  // ʹ�� GetConsoleWindow ��ȡ��ǰ����̨����
    if (!hwnd) {
        KMesErr("�޷���ȡ����̨���ھ��");
        return KSWORD_ERROR_EXIT;
    }

    int cx = GetSystemMetrics(SM_CXSCREEN);  // ��Ļ��� ����
    int cy = GetSystemMetrics(SM_CYSCREEN);  // ��Ļ�߶� ����

    LONG l_WinStyle = GetWindowLong(hwnd, GWL_STYLE);  // ��ȡ������Ϣ
    if (l_WinStyle == 0) {
        KMesErr("�޷���ȡ������ʽ");
        return KSWORD_ERROR_EXIT;
    }

    // ���ô�����Ϣ����󻯣�ȡ�����������߿�
    SetWindowLong(hwnd, GWL_STYLE, (l_WinStyle | WS_POPUP | WS_MAXIMIZE) & ~WS_CAPTION & ~WS_THICKFRAME & ~WS_BORDER);

    // Ӧ���µĴ�����ʽ
    if (!SetWindowPos(hwnd, HWND_TOP, 0, 0, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED)) {
        KMesErr("�޷����ô���λ�úʹ�С");
        return KSWORD_ERROR_EXIT;
    }

    KMesInfo("�ɹ�ʹ��WindowsAPIȫ����");
    return KSWORD_SUCCESS_EXIT;
}
bool KResetWindow() {
    HWND hwnd = GetForegroundWindow();
    int cx = GetSystemMetrics(SM_CXSCREEN);            /* ��Ļ��� ���� */
    int cy = GetSystemMetrics(SM_CYSCREEN);            /* ��Ļ�߶� ���� */

    LONG l_WinStyle = GetWindowLong(hwnd, GWL_STYLE);   /* ��ȡ������Ϣ */
    LONG l_WinExStyle = GetWindowLong(hwnd, GWL_EXSTYLE); /* ��ȡ��չ������ʽ */

    /* �ָ�������ʽ���Ƴ������ʽ����ӱ��������߿� */
    SetWindowLong(hwnd, GWL_STYLE, (l_WinStyle & ~WS_POPUP & ~WS_MAXIMIZE) | WS_CAPTION | WS_THICKFRAME | WS_BORDER);
    SetWindowLong(hwnd, GWL_EXSTYLE, l_WinExStyle | WS_EX_WINDOWEDGE); // ��Ӵ��ڱ�Ե��ʽ

    /* �ָ����ڵ�ԭʼ��С��λ�ã��������ԭʼ��С��λ������Ļ��һ�� */
    SetWindowPos(hwnd, NULL, 0, 0, cx / 2, cy / 2, SWP_NOZORDER | SWP_FRAMECHANGED);

    KMesInfo("�ɹ�ʹ��WindowsAPI����ȫ����");
    return KSWORD_SUCCESS_EXIT;
}
//ԭ�����ӣ�https ://blog.csdn.net/linuxwuj/article/details/81165885
bool FullScreen() {
    COORD font_size;
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    /* ������Ϣ */
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

    GetCurrentConsoleFont(handle, FALSE, &cfi);             /* ��ȡ��ǰ����������Ϣ */
    font_size = GetConsoleFontSize(handle, cfi.index);      /* ��ȡ��ǰ��������Ϣ[�ַ���ȼ��߶���ռ������] */
//ԭ�����ӣ�https ://blog.csdn.net/linuxwuj/article/details/81165885
    HWND hwnd = GetForegroundWindow();
    int cx = GetSystemMetrics(SM_CXSCREEN);            /* ��Ļ��� */
    int cy = GetSystemMetrics(SM_CYSCREEN);            /* ��Ļ�߶� */
    char cmd[32] = { 0 };
    sprintf(cmd, "MODE CON: COLS=%d LINES=%d", cx / font_size.X, cy / font_size.Y);
    KMesInfo(cmd);
    system(cmd);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, cx, cy, 0);
    //KMesInfo("�ɹ�ȫ��");
    return KSWORD_SUCCESS_EXIT;
}
//��������ʾָ��
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
        KMesErr("��ȡ���ʧ�ܣ�");
        return KSWORD_ERROR_EXIT;
    }
}
//ԭ�����ӣ�https://blog.csdn.net/qq_43312665/article/details/86790176
void HideCursor()
{
    CONSOLE_CURSOR_INFO cursor_info = { 1, 0 };
    SetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &cursor_info);
}
//ԭ�����ӣ�https://blog.csdn.net/qq_33866593/article/details/104597731

//����ָ��λ�ã������ң��������£�
void SetCursor(int x, int y) {
    HANDLE hout;
    COORD coord;
    coord.X = x;
    coord.Y = y;
    hout = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleCursorPosition(hout, coord);
    return;
}
//��ɫ����ı����ı����ݣ�ǰ��ɫ������ɫ��
//0 = ��ɫ 8 = ��ɫ
//1 = ��ɫ 9 = ����ɫ
//2 = ��ɫ 10 = ����ɫ
//3 = ǳ��ɫ 11 = ��ǳ��ɫ
//4 = ��ɫ 12 = ����ɫ
//5 = ��ɫ 13 = ����ɫ
//6 = ��ɫ 14 = ����ɫ
//7 = ��ɫ 15 = ����ɫ
void cprint(std::string text, int front_color, int back_color, int special) {
#ifdef KSWORD_WITH_COMMAND
    if (KswordPipeMode) {
        std::cout << text; return;
    }
#endif
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(handle, back_color * 16 | FOREGROUND_INTENSITY | front_color);
    if (special == 1) { // б��
        // Windows ����̨��֧��б�壬���Գ��������������ԣ���Ч������
        CONSOLE_FONT_INFOEX cfi = { 0 };
        cfi.cbSize = sizeof(cfi);
        GetCurrentConsoleFontEx(handle, FALSE, &cfi);
        cfi.FontWeight = FW_NORMAL;
        SetCurrentConsoleFontEx(handle, FALSE, &cfi);
    }
    else if (special == 2) { // ����
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

//��غ�����Դ��https://blog.csdn.net/qq_42885747/article/details/103835671
//����͸����
void SetAir(BYTE alpha)
{
        // ��ȡ����̨���ڵľ��
        HWND hwnd = GetConsoleWindow();
        SetWindowLong(hwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
        // ���ô��ڵķֲ����ԣ�ʹ��͸��
        SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), alpha, LWA_ALPHA);
    
}

//���Ӳ����ȡ����

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
// �������ĺ���
void dealinput(char c) {
    if (c == '\b' && !KGetLineTemp[KGetLineTempUp].empty()) { // ����Ƿ���Backspace��
        std::cout << "\b \b";
        KGetLineTemp[KGetLineTempUp].pop_back();
    }
    else if (KGetLineTemp[KGetLineTempUp][KGetLineTemp[KGetLineTempUp].length() - 1] == '^') {
        if (c >= 'a' && c <= 'z') {
            // Сд��ĸת��Ϊ��д��ĸ
            c -= 32;
        }
        else if (c >= '0' && c <= '9') {
            // ����ת��Ϊ��Ӧ�ķ���
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
            // �������ת��Ϊ��Ӧ�ķ���
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
    else if (c >= 32 && c <= 126) { // �ɼ��ַ�
        if (c >= 'A' && c <= 'Z')  // ���������Ǵ�д��ĸ
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
        //std::cout << "KGetlinetmp�к���" << KGetLineTemp[KGetLineTempUp];
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
    //std::cout << "������";
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
            /*else*/ if (p->vkCode == VK_RETURN) { // Enter��
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

                    // ��ȡ��ǰ���λ��
                    if (GetConsoleScreenBufferInfo(hConsole, &csbi)) {
                        COORD cursorPos = csbi.dwCursorPosition;
                        // �ж��Ƿ�������
                        if (cursorPos.X == csbi.dwSize.X - 1) {
                        }
                        else {
                            // �ƶ���굽ǰһ���ַ�λ��
                            //cursorPos.X--;
                        }
                        // �����µĹ��λ��
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
            else if ((p->vkCode >= 32 && p->vkCode <= 126) || (p->vkCode == 8) || (p->vkCode == 109) || (p->vkCode == 154) || (p->vkCode == 220)) { // �ɼ��ַ����߻س�
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
                // ���������������������ַ�
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
#define KEY_DOWN_FOREMOST(hWnd,vk) (KEY_DOWN(vk) && GetForegroundWindow()==hWnd)    //��ǰ�� 
#define KEY_DOWN_FOCUSED(hWnd,vk) KEY_DOWN_FOREMOST(hWnd,vk)    //������ 
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
{   //���¼���״̬
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
// ��ⰴ���Ƿ���
bool IsKeyPressed(int keyCode) {
    return (GetAsyncKeyState(keyCode) & 0x8000) != 0;
}

// ��ⰴ���Ƿ�̧��
bool IsKeyReleased(int keyCode) {
    return (GetAsyncKeyState(keyCode) & 0x8000) == 0;
}

// ��ȡ�û�������ַ���
std::string AnsyKeyCodeGetline() {
    std::string input;
    const int keys[] = { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, // 0-9
                             0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, // A-Z
                             0x3B, 0x27, 0x2C, 0x2E, 0x2F, 0x3A, 0x3F, 0x5B, 0x5D, 0x5C, 0x2D, 0x3D, 0x20 }; // ������źͿո�
    while (true) {
        Sleep(10);
        for (int keyCode : keys) { // ; ' , . / : ? [ ] \ - =
            if (KEY_DOWN2_ONCE(keyCode)) {
                char ch = tolower(char(keyCode));
                input += ch; // ��ӵ������ַ���
                std::cout << ch; // ʵʱ���
                break;
            }
        }
        // ����˸��
        if (IsKeyPressed(VK_BACK)) { // VK_BACK ���˸�����������
            while (!IsKeyReleased(VK_BACK)) { // �ȴ��˸��̧��
                Sleep(10); // ��΢��ͣһ�£�����CPUռ�ù���
            }
            if (!input.empty()) {
                input.pop_back(); // ɾ�����һ���ַ�
                std::cout << "\b \b"; // ����˸�Ч��
            }
            continue;
        }

        // ���Enter��
        if (IsKeyPressed(VK_RETURN)) { // VK_RETURN �� Enter �����������
            while (!IsKeyReleased(VK_RETURN)) { // �ȴ�Enter��̧��
                Sleep(10); // ��΢��ͣһ�£�����CPUռ�ù���
            }
            break; // �˳�ѭ��
        }

        //// �������������������ĸ�����ֺ�������ţ�
        //for (int keyCode = 0x30; keyCode <= 0x39; ++keyCode) { // 0-9
        //	if ((KEY_DOWN2_ONCE(keyCode))) {
        //		char ch = char(keyCode);
        //		input += ch; // ��ӵ������ַ���
        //		std::cout << ch; // ʵʱ���
        //		break;
        //	}
        //}

        //for (int keyCode = 0x41; keyCode <= 0x5A; ++keyCode) { // A-Z
        //	if (KEY_DOWN2_ONCE(keyCode)) {
        //		char ch = tolower(char(keyCode)); // ת��ΪСд��ĸ
        //		input += ch; // ��ӵ������ַ���
        //		std::cout << ch; // ʵʱ���
        //		break;
        //	}
        //}

        //// ����������
        //const int specialKeys[] = { 0x3B, 0x27, 0x2C, 0x2E, 0x2F, 0x3A, 0x3F, 0x5B, 0x5D, 0x5C, 0x2D, 0x3D };
        //for (int keyCode : specialKeys) { // ; ' , . / : ? [ ] \ - =
        //	if (KEY_DOWN2_ONCE(keyCode)) {
        //		char ch = char(keyCode);
        //		input += ch; // ��ӵ������ַ���
        //		std::cout << ch; // ʵʱ���
        //		break;
        //	}
        //}

        //// ���ո��
        //if (KEY_DOWN2_ONCE(VK_SPACE)) { // VK_SPACE �ǿո�����������
        //	input += ' '; // ��ӿո������ַ���
        //	std::cout << ' '; // ʵʱ���
        //}
        UpdateKeyboardStatus();
    }

    std::cout << std::endl; // ����
    return input;
}


bool directoryExists(const std::string& path) {
    DWORD attrib = GetFileAttributesA(path.c_str());
    return (attrib != INVALID_FILE_ATTRIBUTES && (attrib & FILE_ATTRIBUTE_DIRECTORY));
}

bool fileExists(const std::string& path) {
    DWORD attrib = GetFileAttributesA(path.c_str());
    // ����Ƿ��ȡ����Ч���ԣ�����·������Ŀ¼
    return (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY));
}




int KgetlineMode ;
//0�����Ӳ����ȡ��������
//1��Ӳ�������ȡ��������
//2����ͨGetline��ʽ��ȡ����
//2����ͨGetline��ʽ��ȡ����
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
            KMesErr("��װ����ʱ��������");
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
    // ��ȡת�������軺������С
    int wlen = MultiByteToWideChar(CP_ACP, 0, gbkStr.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return "";
    wchar_t* wbuf = new wchar_t[wlen];
    MultiByteToWideChar(CP_ACP, 0, gbkStr.c_str(), -1, wbuf, wlen);

    // ת��ΪUTF-8
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
        //	KMesErr("��װ����ʱ��������");
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
        // ж�ع���
       //std::cout <<"��������" << KGetLineTemp[KGetLineTempUp] << std::endl;
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

//�޸Ŀ���̨���ڱ�������
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
            // ����Ƿ��µ��ǡ��������衤�����������ΪVK_OEM_3��
            if (p->vkCode == VK_OEM_3) {
                dotKeyCount++;
                // ���ΰ�������������һ������
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
    // ������һ������
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

//����̨������С����ȡ����С��
void MiniWindow() {
    return;
}

void UnMiniWindow() {
    return;
}
//���ÿ���̨����Ϊconsola
void SetConsola()
{
#ifdef KSWORD_WITH_COMMAND
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

    // ��������
    CONSOLE_FONT_INFOEX cfi;
    cfi.cbSize = sizeof(cfi);
    cfi.nFont = 0;
    cfi.dwFontSize.X = 8;                   // ������
    cfi.dwFontSize.Y = FONT_SIZE_HEIGHT;                  // ����߶�
    cfi.FontFamily = FF_DONTCARE;
    cfi.FontWeight = FW_NORMAL;
    wcscpy(cfi.FaceName, L"Consolas");      // ��������
    cfi.FontWeight = FW_NORMAL;

    // ���ÿ���̨����
    SetCurrentConsoleFontEx(hConsole, FALSE, &cfi);
    return;
#endif
}
void SetConsoleWindowPosition(int x, int y)
{
        // ��ȡ��ǰ����̨���ڵľ��
        HWND hwnd = GetConsoleWindow();

        // ��ȡ��ǰ����̨���ڵľ�������
        RECT rect;
        GetWindowRect(hwnd, &rect);

        // �����µĴ���λ��
        int newLeft = x;
        int newTop = y;

        // ���ô��ڵ���λ��
        SetWindowPos(hwnd, nullptr, newLeft, newTop, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

}
void SetConsoleWindowSize(int width, int height) {    // ��ȡ����̨������
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole == INVALID_HANDLE_VALUE) {
        std::cerr << "Error getting console handle." << std::endl;
        return;
    }
     //������Ļ��������С
    COORD bufferSize = { width, 1000 };
    if (!SetConsoleScreenBufferSize(hConsole, bufferSize)) {
        std::cerr << "Error setting console screen buffer size." << std::endl;
        return;
    }
       //// ��������̨���ڴ�С
    SMALL_RECT windowRect = { 0, 0, width - 1, height +1 };
    if (!SetConsoleWindowInfo(hConsole, TRUE, &windowRect)) {
        std::cerr << "Error setting console window size." << std::endl;
        return;
    }
    //std::string command = "mode con cols=" + std::to_string(width) + " lines=" + std::to_string(height);
    //system(command.c_str());
    //std::cout << "damn"; Kpause();
}
//����̽�⺯��===============================================
//̽��Windowsϵͳ�汾��
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

    //�����ж��Ƿ���win11����������������ж�
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
        KMesInfo("̽�⵽�ò���ϵͳ�汾Ϊ"); KMesInfo("Win11 or later");
    return 11;

    std::string vname;
    //���ж��Ƿ�Ϊwin8.1��win10
    typedef void(__stdcall* NTPROC)(DWORD*, DWORD*, DWORD*);
    HINSTANCE hinst = LoadLibrary(L"ntdll.dll");
    DWORD dwMajor, dwMinor, dwBuildNumber;
    NTPROC proc = (NTPROC)GetProcAddress(hinst, "RtlGetNtVersionNumbers");
    proc(&dwMajor, &dwMinor, &dwBuildNumber);
    if (dwMajor == 6 && dwMinor == 3)	//win 8.1
    {
        vname = "Microsoft Windows 8.1";
            KMesInfo("̽�⵽�ò���ϵͳ�汾Ϊ"); KMesInfo(vname);
        return 9;
    }
    if (dwMajor == 10 && dwMinor == 0)	//win 10
    {
        vname = "Microsoft Windows 10";
            KMesInfo("̽�⵽�ò���ϵͳ�汾Ϊ"); KMesInfo(vname);
        return 10;
    }
    //�ж�win8.1���µİ汾
    SYSTEM_INFO info;                //��SYSTEM_INFO�ṹ�ж�64λAMD������  
    GetSystemInfo(&info);            //����GetSystemInfo�������ṹ  
    OSVERSIONINFOEX os;
    os.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
    if (GetVersionEx((OSVERSIONINFO*)&os))
    {
        //������ݰ汾��Ϣ�жϲ���ϵͳ����  
        switch (os.dwMajorVersion)
        {                        //�ж����汾��  
        case 4:
            switch (os.dwMinorVersion)
            {                //�жϴΰ汾��  
            case 0:
                if (os.dwPlatformId == VER_PLATFORM_WIN32_NT)
                    vname = "Microsoft Windows NT 4.0";  //1996��7�·���  
                else if (os.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)
                    vname = "Microsoft Windows 95";
                    KMesInfo("̽�⵽�ò���ϵͳ�汾Ϊ"); KMesInfo(vname);
                return 1;
                break;
            case 10:
                vname = "Microsoft Windows 98";
                KMesInfo("̽�⵽�ò���ϵͳ�汾Ϊ"); KMesInfo(vname);
                return 2;
                break;
            case 90:
                vname = "Microsoft Windows Me";
                KMesInfo("̽�⵽�ò���ϵͳ�汾Ϊ"); KMesInfo(vname);
                return 3;
                break;
            }
            break;
        case 5:
            switch (os.dwMinorVersion)
            {               //�ٱȽ�dwMinorVersion��ֵ  
            case 0:
                vname = "Microsoft Windows 2000";    //1999��12�·���  
                KMesInfo("̽�⵽�ò���ϵͳ�汾Ϊ"); KMesInfo(vname);
                return 4;
                break;
            case 1:
                vname = "Microsoft Windows XP";      //2001��8�·���  
                KMesInfo("̽�⵽�ò���ϵͳ�汾Ϊ"); KMesInfo(vname);
                return 5;
                break;
            case 2:
                if (os.wProductType == VER_NT_WORKSTATION &&
                    info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
                    vname = "Microsoft Windows XP Professional x64 Edition";
                else if (GetSystemMetrics(SM_SERVERR2) == 0)
                    vname = "Microsoft Windows Server 2003";   //2003��3�·���  
                else if (GetSystemMetrics(SM_SERVERR2) != 0)
                    vname = "Microsoft Windows Server 2003 R2";
                if (KSWORD_PRINT_DEBUG_INFO) {
                    KMesInfo("̽�⵽�ò���ϵͳ�汾Ϊ"); KMesInfo(vname);
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
                    vname = "Microsoft Windows Server 2008";   //�������汾 
                    KMesInfo("̽�⵽�ò���ϵͳ�汾Ϊ"); KMesInfo(vname);
                return 6;
                break;

            case 1:
                if (os.wProductType == VER_NT_WORKSTATION)
                    vname = "Microsoft Windows 7";
                else
                    vname = "Microsoft Windows Server 2008 R2";
                    KMesInfo("̽�⵽�ò���ϵͳ�汾Ϊ"); KMesInfo(vname);
                return 7;
                break;
            case 2:
                if (os.wProductType == VER_NT_WORKSTATION)
                    vname = "Microsoft Windows 8";
                else
                    vname = "Microsoft Windows Server 2012";
                    KMesInfo("̽�⵽�ò���ϵͳ�汾Ϊ"); KMesInfo(vname);
                return 8;
                break;
            }
            break;
        default:
            vname = "δ֪����ϵͳ";
                KMesErr(vname);
            return 0;

        }
    }
    else {
        vname = "δ֪����ϵͳ";
            KMesErr(vname);
        return 0;
    }
    //ԭ�����ӣ�https ://blog.csdn.net/qq78442761/article/details/64440535
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
//̽��CPU���������ʣ�����0~100������Ϊ��ǰ�ٷֱ�ֵ
__int64 CompareFileTime(FILETIME time1, FILETIME time2)
{
    __int64 a = time1.dwHighDateTime << 32 | time1.dwLowDateTime;
    __int64 b = time2.dwHighDateTime << 32 | time2.dwLowDateTime;

    return (b - a);
}
//WIN CPUʹ�����  

int CPUUsage() {
    static FILETIME preIdleTime, preKernelTime, preUserTime;
    FILETIME idleTime, kernelTime, userTime;

    // ��һ�λ�ȡʱ��
    if (!GetSystemTimes(&preIdleTime, &preKernelTime, &preUserTime))
        return -1;

    Sleep(500);  // �ȴ�1��

    // �ڶ��λ�ȡʱ��
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime))
        return -1;

    // ��FILETIMEת��Ϊ64λ���������ֵ
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

    // ����ʱ����λ��100���룩
    ULONGLONG idleDiff = currIdle.QuadPart - preIdle.QuadPart;
    ULONGLONG kernelDiff = currKernel.QuadPart - preKernel.QuadPart;
    ULONGLONG userDiff = currUser.QuadPart - preUser.QuadPart;

    // CPU������ = (��ʱ�� - ����ʱ��) / ��ʱ�� * 100
    ULONGLONG totalTime = kernelDiff + userDiff;
    if (totalTime == 0) return 0;  // ����������

    int cpuUsage = static_cast<int>((totalTime - idleDiff) * 100.0 / totalTime);
    if (cpuUsage < 0)return 0;
    if (cpuUsage > 100)return 100;
    return cpuUsage;

}

//̽���ڴ�ٷֱȣ�����0~100������Ϊ��ǰ�ٷֱ�ֵ
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
        KMesErr("��ȡ�ڴ�ռ����ʧ��");
        return -1;
    }
}
//̽���ڴ�����������������GB��������ȡ����
int RAMSize() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);

    if (GlobalMemoryStatusEx(&memInfo)) {
        DWORDLONG totalPhys = memInfo.ullTotalPhys;
        // ת��ΪGB��1 GB = 1024 * 1024 * 1024�ֽ�
        double totalMemoryGB = totalPhys / 1073741824.0;
        // ����ȡ��
        return static_cast<int>(totalMemoryGB) + (totalMemoryGB - static_cast<int>(totalMemoryGB) > 0 ? 1 : 0);
    }
    else {
            KMesErr("��ȡ�ڴ�����ʧ��");
        return 0;
    }
    return 0;
}
//̽���ڴ�����������������MB��������ȡ����
int RAMSizeMB() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);

    if (GlobalMemoryStatusEx(&memInfo)) {
        DWORDLONG totalPhys = memInfo.ullTotalPhys;
        // ת��ΪMB��1 MB = 1024 * 1024�ֽ�
        int totalMemoryMB = static_cast<int>(totalPhys / (1024 * 1024));
        return totalMemoryMB;
    }
    else {
        KMesErr("��ȡ�ڴ�����ʧ��");
        return 0;
    }
    return 0;
}
//��ȡϵͳ���̷��������̷�
char SystemDrive() {
    char systemDrive[3] = { 0 };
    GetEnvironmentVariableA("SYSTEMDRIVE", systemDrive, sizeof(systemDrive));
    std::string msgtmp;
    msgtmp += "̽�⵽ϵͳ���̷���";
    msgtmp += systemDrive[0];
    KMesErr(msgtmp);
    return systemDrive[0];
}
//��ȡָ���̿��ÿռ䣨-1�����̲����ڣ�
int FreeSpaceOfDrive(char driveLetter) {
    ULARGE_INTEGER freeBytesAvailable;
    ULARGE_INTEGER totalNumberOfBytes;
    ULARGE_INTEGER totalNumberOfFreeBytes;
    // ����������·������ "%SYSTEMDRIVE%\"
    wchar_t drivePath[4] = { driveLetter, ':', '\\', '\0' };
    //�������dev-c++�����ߣ�������char������wchar_t
    //char drivePath[4] = { driveLetter, ':', '\\', '\0' };

    // ��ȡ��������ʣ��ռ�
    if (GetDiskFreeSpaceEx(drivePath, &freeBytesAvailable, &totalNumberOfBytes, &totalNumberOfFreeBytes)) {
        // ���ֽ�ת��Ϊ���ֽڣ�1 MB = 1024 * 1024 �ֽ�
        return static_cast<int>(freeBytesAvailable.QuadPart / (1024 * 1024));
    }
    else {
        KMesErr("�Ҳ������������޷�������ÿռ�");
        // �����������ʧ�ܣ�����-1
        return -1;
    }
}
//̽���Ƿ������
    //���أ�0���������
    // 1��VMwareϵ��
    // 2��VPCϵ��
int IsVM() {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "��غ������ڱ�д��" << std::endl;
    return 0;
}
bool IsInsideVPC() {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "��غ������ڱ�д��" << std::endl;
    return 0;
}
bool IsInsideVMWare() {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "��غ������ڱ�д��" << std::endl;
    return 0;
}

//̽������·��
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
        DWORD dwSize = MAX_PATH; // ��ʼ��С
        TCHAR szUserName[MAX_PATH];

        // ��ȡ��ǰ���̵ķ�������
        HANDLE hToken;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
            std::cerr << "OpenProcessToken Error " << GetLastError() << std::endl;
            return "";
        }

        // ��ȡ�û���
        if (!GetUserName(szUserName, &dwSize)) {
            std::cerr << "GetUserName Error " << GetLastError() << std::endl;
            CloseHandle(hToken);
            return "";
        }

        CloseHandle(hToken);

        // ��TCHAR����ת��Ϊstd::string
        return WCharToString(szUserName);

}

std::string GetHostName()
{
    TCHAR szComputerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD dwSize = sizeof(szComputerName) / sizeof(TCHAR);

    // ��ȡ�������
    if (!GetComputerName(szComputerName, &dwSize)) {
        std::cerr << "GetComputerName Error " << GetLastError() << std::endl;
        return "";
    }

    // ��TCHAR����ת��Ϊstd::string
    return WCharToString(szComputerName);
}


typedef HHOOK(WINAPI *pSetWindowsHookEx)(int, HOOKPROC, HINSTANCE, DWORD);
typedef LRESULT(WINAPI *pCallNextHookEx)(HHOOK, int, WPARAM, LPARAM);
HHOOK KSetWindowsHookEx(int idHook, HOOKPROC lpfn, HINSTANCE hInstance, DWORD dwThreadId)
{
      // ��ȡϵͳ������ַ
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

    // ����ϵͳ SetWindowsHookEx ����
    return setHook(idHook, lpfn, hInstance, dwThreadId);
}

void Ktasklist()
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
    {
        KMesErr("����ʱ���󣺴������̿���ʧ�ܣ�������룺" + std::to_string(GetLastError()));
        return;
    }
    PROCESSENTRY32 pe = { sizeof(PROCESSENTRY32) };
    if (!Process32First(hSnapshot, &pe))
    {
        KMesErr("����ʱ�����޷���ȡ�����б�" + std::to_string(GetLastError()));
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
        KMesErr("����ʱ���󣺴������̿���ʧ�ܣ�������룺" +std:: to_string(GetLastError()));
        return;
    }

    PROCESSENTRY32 pe = { sizeof(PROCESSENTRY32) };
    if (!Process32First(hSnapshot, &pe))
    {
        KMesErr("����ʱ�����޷���ȡ�����б�" +std:: to_string(GetLastError()));
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
                // �����û�е�һ�����̣������һ�����̵���Ϣ
                firstProcessName = processName;
                firstProcessID = pe.th32ProcessID;
                hasFirstProcess = true;
            }
            else
            {
                // ����Ѿ��е�һ�����̣�����������̵���Ϣ
                cprint("Proc:", 2, 0); std::cout << firstProcessName << "\t";
                cprint("PID:", 1, 0); std::cout << firstProcessID << "\t";

                cprint("Proc:", 2, 0); std::cout << processName << "\t";
                cprint("PID:", 1, 0); std::cout << pe.th32ProcessID << std::endl;

                // ���ñ�־��Ϊ��һ�Խ�����׼��
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

//��������==================================================
//��ӡ��ǰʱ��
void KPrintTime()
{
    std::time_t now = std::time(nullptr); // ��ȡ��ǰϵͳʱ��
    std::tm* ptm = std::localtime(&now); // ת��Ϊ����ʱ��
    std::cout << "["
        //<< ptm->tm_year + 1900 << '-' // ��ݴ�1900��ʼ
        //<< ptm->tm_mon + 1 << '-' // �·ݴ�0��ʼ�������Ҫ+1
        //<< ptm->tm_mday << ' '
        << std::setw(2) << std::setfill('0') << ptm->tm_hour << ':'
        << std::setw(2) << std::setfill('0') << ptm->tm_min << ':'
        << std::setw(2) << std::setfill('0') << ptm->tm_sec // ��
        << "]";
    return;
}
//
bool ReleaseResourceToFile(const char* resourceType, WORD resourceID, const char* outputFilePath) {
    // ��ȡ��ǰģ����
    HMODULE hModule = GetModuleHandle(NULL);
    if (!hModule) {
        std::cerr << "�޷���ȡģ������" << std::endl;
        return false;
    }

    // ������Դ
    HRSRC hResource = FindResource(hModule, MAKEINTRESOURCE(resourceID), CharToWChar(resourceType));
    if (!hResource) {
        std::cerr << "�޷��ҵ���Դ��" << std::endl;
        return false;
    }

    // ������Դ
    HGLOBAL hLoadedResource = LoadResource(hModule, hResource);
    if (!hLoadedResource) {
        std::cerr << "�޷�������Դ��" << std::endl;
        return false;
    }

    // ������Դ
    LPVOID lpResourceData = LockResource(hLoadedResource);
    if (!lpResourceData) {
        std::cerr << "�޷�������Դ��" << std::endl;
        return false;
    }

    // ��ȡ��Դ��С
    DWORD dwResourceSize = SizeofResource(hModule, hResource);
    if (!dwResourceSize) {
        std::cerr << "�޷���ȡ��Դ��С��" << std::endl;
        return false;
    }

    // ��Ŀ���ļ�
    HANDLE hFile = CreateFile(CharToWChar(outputFilePath), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "�޷�����Ŀ���ļ���" << std::endl;
        return false;
    }

    // д����Դ���ݵ��ļ�
    DWORD dwBytesWritten;
    if (!WriteFile(hFile, lpResourceData, dwResourceSize, &dwBytesWritten, NULL)) {
        std::cerr << "�޷�д���ļ���" << std::endl;
        CloseHandle(hFile);
        return false;
    }

    // �ر��ļ����
    CloseHandle(hFile);

    return true;
}

//�첽ִ��cmd����
//����ԭ��
void ExecuteCmdAsync(const std::wstring& cmd) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    // �����ӽ���
    if (!CreateProcessW(NULL,   // û��ģ������ʹ�������У�
        const_cast<LPWSTR>(cmd.c_str()),        // ������
        NULL,                // ���̾�����ɼ̳�
        NULL,                // �߳̾�����ɼ̳�
        FALSE,               // ���þ���̳�ѡ��
        CREATE_NEW_CONSOLE,  // �����¿���̨����
        NULL,                // ʹ�ø����̵Ļ�����
        NULL,                // ʹ�ø����̵���ʼĿ¼
        &si,                 // ָ��STARTUPINFOW�ṹ
        &pi)                 // ָ��PROCESS_INFORMATION�ṹ
    ) {
        std::wcerr << L"CreateProcess failed (" << GetLastError() << L").\n";
        return;
    }

    // ���رս��̺��߳̾������Ϊ���ǲ��ȴ��ӽ��̽���
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
//ʵʱ����ִ��cmd����
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
        //��ȡ����ִ�й����е����
        while (fgets(MsgBuff, MsgLen, fp) != NULL){
            std::cout << MsgBuff;
        }
        //�ر�ִ�еĽ���
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
        //��ȡ����ִ�й����е����
        while (fgets(MsgBuff, MsgLen, fp) != NULL) {
            std::cout << MsgBuff;
        }
        //�ر�ִ�еĽ���
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
    std::array<char, 128> buffer{}; {}; // ȷ���Ѿ������� <array>
    std::string result;
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
    if (!pipe) {
        KMesErr("����ʱ����:�޷��򿪹ܵ�");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}
std::string GetCmdResultWithUTF8(std::string cmd)
{
    cmd = "chcp 65001 && " + cmd;
    std::array<char, 128> buffer{}; {}; // ȷ���Ѿ������� <array>
    std::string result;
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
    if (!pipe) {
        KMesErr("����ʱ����:�޷��򿪹ܵ�");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}
//�������ԱȨ�޲������Լ�������·����
int RequestAdmin(std::wstring path) {
    SHELLEXECUTEINFOW sei = { 0 };
    sei.cbSize = sizeof(SHELLEXECUTEINFOW);
    sei.lpVerb = L"runas";
    sei.lpFile = path.c_str();
    sei.hwnd = NULL;
    sei.nShow = SW_NORMAL;
    // �����Թ���ԱȨ����������
    if (!ShellExecuteExW(&sei)) {
        DWORD dwError = GetLastError();
        KMesErr("��ͼ��ȡ����ԱȨ��ʱ��������!"); 
        return KSWORD_ERROR_EXIT;
    }
    else {
        return KSWORD_SUCCESS_EXIT;
    }
}

//���㳤MD5
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
    // F����
    unsigned int Ksword_Md5_F(unsigned int b, unsigned int c, unsigned int d) {
        return (b & c) | ((~b) & d);
    }
    // G����
    unsigned int Ksword_Md5_G(unsigned int b, unsigned int c, unsigned int d) {
        return (b & d) | (c & (~d));
    }
    // H����
    unsigned int Ksword_Md5_H(unsigned int b, unsigned int c, unsigned int d) {
        return b ^ c ^ d;
    }
    // I����
    unsigned int Ksword_Md5_I(unsigned int b, unsigned int c, unsigned int d) {
        return c ^ (b | (~d));
    }
    // ��λ��������
    unsigned int Ksword_Md5_shift(unsigned int a, unsigned int n) {
        return (a << n) | (a >> (32 - n));
    }
    // ���뺯��
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
    // ѭ��ѹ��
    void Ksword_Md5_iterateFunc(unsigned int* X, int size = 16) {
        unsigned int a = tempKSWORD_MD5_A,
            b = tempKSWORD_MD5_B,
            c = tempKSWORD_MD5_C,
            d = tempKSWORD_MD5_D,
            rec = 0,
            g, k;
        for (int i = 0; i < 64; i++) {
            if (i < 16) {
                // F����
                g = Ksword_Md5_F(b, c, d);
                k = i;
            }
            else if (i < 32) {
                // G����
                g = Ksword_Md5_G(b, c, d);
                k = (1 + 5 * i) % 16;
            }
            else if (i < 48) {
                // H����
                g = Ksword_Md5_H(b, c, d);
                k = (5 + 3 * i) % 16;
            }
            else {
                // I����
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
    // ����ַ���
    std::vector<unsigned int> Ksword_Md5_padding(std::string src) {
        // ��512λ,64���ֽ�Ϊһ��
        unsigned int num = ((src.length() + 8) / 64) + 1;
        std::vector<unsigned int> rec(num * 16);
        strlength = num * 16;
        for (unsigned int i = 0; i < src.length(); i++) {
            // һ��unsigned int��Ӧ4���ֽڣ�����4���ַ���Ϣ
            rec[i >> 2] |= (int)(src[i]) << ((i % 4) * 8);
        }
        // ����1000...000
        rec[src.length() >> 2] |= (0x80 << ((src.length() % 4) * 8));
        // ���ԭ�ĳ���
        rec[rec.size() - 2] = (src.length() << 3);
        return rec;
    }
    // �������
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
//�����MD5
std::string SMD5(const char* text) {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "��غ������ڱ�д��" << std::endl;
    std::string a;
    return a;
}
std::string SMD5(std::string text) {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "��غ������ڱ�д��" << std::endl;
    std::string a;
    return a;
}
//�����ϣ
std::string Hash(const char* text) {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "��غ������ڱ�д��" << std::endl;
    std::string a;
    return a;
}
std::string Hash(std::string text) {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "��غ������ڱ�д��" << std::endl;
    std::string a;
    return a;
}
//�����ļ���ϣ
std::string FileHash(std::string path) {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "��غ������ڱ�д��" << std::endl;
    std::string a;
    return a;
}
std::string FileHash(const char* path) {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "��غ������ڱ�д��" << std::endl;
    std::string a;
    return a;
}
std::string FileHash(wchar_t path) {
    if (KSWORD_PRINT_DEBUG_INFO)std::cout << std::endl << "��غ������ڱ�д��" << std::endl;
    std::string a;
    return a;
}
double BytesToMB(ULONG64 bytes) {
    const double bytesPerMB = 1024 * 1024; // 1 MB = 1024 * 1024 �ֽ�
    return static_cast<double>(bytes) / bytesPerMB;
}
HWND GetHandleByTitle(const std::wstring& windowTitle)
{
    // ʹ��FindWindow�������Ҷ��㴰��
    HWND hwnd = FindWindow(NULL, windowTitle.c_str());
    if (hwnd == NULL)
    {
        // ���û���ҵ����㴰�ڣ����Գ��Բ����Ӵ���
        HWND hwndParent = FindWindow(NULL, L"����"); // ͨ�����洰�ڵı����ǿյ�
        hwnd = FindWindowEx(hwndParent, NULL, NULL, windowTitle.c_str());
    }
    return hwnd;
}
HANDLE GetHandleByProcName(const std::wstring& processName)
{

    HANDLE hProcessSnap = INVALID_HANDLE_VALUE;
    HANDLE hProcess = NULL;
    PROCESSENTRY32 pe32;

    // �������̿���
    hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hProcessSnap == INVALID_HANDLE_VALUE) return NULL;

    // ��ʼ��PROCESSENTRY32�ṹ
    pe32.dwSize = sizeof(PROCESSENTRY32);

    // ��ȡ��һ������
    if (!Process32First(hProcessSnap, &pe32)) {
        CloseHandle(hProcessSnap);
        return NULL;
    }

    // ��������
    do {
        if (pe32.szExeFile == processName) {
            // �ҵ�ƥ��Ľ��̣��򿪽��̾��
            hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pe32.th32ProcessID);
            break;
        }
    } while (Process32Next(hProcessSnap, &pe32)); // ��ȡ��һ������

    // �رս��̿��վ��
    CloseHandle(hProcessSnap);
    return hProcess;
}
HANDLE GetProcessHandleByPID(DWORD dwProcessId)
{
    // ʹ��PROCESS_ALL_ACCESSȨ�ޣ������Ը�����Ҫ����Ȩ��
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwProcessId);
    if (hProcess == nullptr) {
        KMesErr("�򿪽���ʧ��:" + std::to_string(GetLastError()));
    }
    return hProcess;
}

BOOL IsInt(std::string str)
{
    try
    {
        // ���Խ��ַ���ת��Ϊ����
        size_t pos;
        int value = std::stoi(str, &pos);

        // ����Ƿ������ַ�������ת��
        if (pos != str.size())
        {
            return false; // �ַ����а����������ַ�
        }

        //// ����Ƿ��� int �ķ�Χ��
        //if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        //{
        //	return false; // ���� int ��Χ
        //}

        return true;
    }
    catch (const std::invalid_argument&)
    {
        return false; // �ַ����а�����Ч�ַ�
    }
    catch (const std::out_of_range&)
    {
        return false; // ���� int ��Χ
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
                // �����ַ�ӳ������ת��Ϊ std::string
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
        // ��ȡ�������Ʋ���
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
        KMesErr("��pidΪ" +std:: to_string(pid) + "�Ľ���ʧ�ܣ�������룺" + std::to_string(GetLastError()));
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
        KMesErr("ö��" +std:: to_string(pid) + "�Ľ���ʧ�ܣ�������룺" + std::to_string(GetLastError()));
    }

    CloseHandle(hProcess);
    return dllList;
}
std::string GetClipBoard()
{
    std::string clipboardContent;
    // �򿪼�����
    if (OpenClipboard(NULL))
    {
        // ��ȡ�������е����ݾ��
        HANDLE hData = GetClipboardData(CF_TEXT);
        if (hData != NULL)
        {
            // ������������ת��Ϊ�ַ���
            char* pszText = static_cast<char*>(GlobalLock(hData));
            if (pszText != NULL)
            {
                clipboardContent = pszText;
                GlobalUnlock(hData);
            }
        }
        // �رռ�����
        CloseClipboard();
    }
    return clipboardContent;
}
//����Powershell


//�ں˶Կ�����==============================================
//������ָ�ָ������
int SuspendProcess(DWORD dwProcessId) {
    HANDLE hProcessSnap;
    HANDLE hProcess;
    BOOL bNext = FALSE;

    // �������̿���
    hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hProcessSnap == INVALID_HANDLE_VALUE) {
        std::cerr << "CreateToolhelp32Snapshot failed (" << GetLastError() << ")\n";
        return KSWORD_ERROR_EXIT;
    }

    // ��ȡ������Ϣ
    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);
    if (!Process32First(hProcessSnap, &pe32)) {
        std::cerr << "Process32First failed (" << GetLastError() << ")\n";
        CloseHandle(hProcessSnap);
        return KSWORD_ERROR_EXIT;
    }

    // Ѱ��Ŀ�����
    do {
        if (pe32.th32ProcessID == dwProcessId) {
            // ��Ŀ�����
            hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwProcessId);
            if (hProcess == NULL) {
                KMesErr("�޷���ȡ���̾��");
                CloseHandle(hProcessSnap);
                return KSWORD_ERROR_EXIT;
            }

            // �����߳̿���
            HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (hThreadSnap == INVALID_HANDLE_VALUE) {
                KMesErr("�������̿���ʧ��");
                CloseHandle(hProcess);
                CloseHandle(hProcessSnap);
                return KSWORD_ERROR_EXIT;
            }

            // ��ȡ�߳���Ϣ
            THREADENTRY32 te32;
            te32.dwSize = sizeof(THREADENTRY32);
            if (!Thread32First(hThreadSnap, &te32)) {
                KMesErr("�����߳�ʱ��������");
                CloseHandle(hThreadSnap);
                CloseHandle(hProcess);
                CloseHandle(hProcessSnap);
                return KSWORD_ERROR_EXIT;
            }
            // ���������߳�
            do {
                if (te32.th32OwnerProcessID == dwProcessId) {
                    HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te32.th32ThreadID);
                    if (hThread == NULL) {
                        KMesErr("�޷���ȡ�߳̾��");
                    }
                    else {
                        SuspendThread(hThread);
                        CloseHandle(hThread);
                    }
                }
            } while (Thread32Next(hThreadSnap, &te32));

            // ����
            CloseHandle(hThreadSnap);
            CloseHandle(hProcess);
            CloseHandle(hProcessSnap);
            return KSWORD_SUCCESS_EXIT;
        }
    } while (Process32Next(hProcessSnap, &pe32));

    // ����
    CloseHandle(hProcessSnap);
    return KSWORD_ERROR_EXIT;
}
//ȡ���������
int UnSuspendProcess(DWORD dwProcessId){
    HANDLE hProcessSnap;
    HANDLE hProcess;
    BOOL bNext = FALSE;

    // �������̿���
    hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hProcessSnap == INVALID_HANDLE_VALUE) {
        KMesErr("�޷��������̿���");
        return KSWORD_ERROR_EXIT;
    }

    // ��ȡ������Ϣ
    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);
    if (!Process32First(hProcessSnap, &pe32)) {
        KMesErr("��ȡ������Ϣ����");
        CloseHandle(hProcessSnap);
        return KSWORD_ERROR_EXIT;
    }

    // Ѱ��Ŀ�����
    do {
        if (pe32.th32ProcessID == dwProcessId) {
            // ��Ŀ�����
            hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwProcessId);
            if (hProcess == NULL) {
                KMesErr("�򿪽���ʧ�ܣ��ܾ�����");
                CloseHandle(hProcessSnap);
                return KSWORD_ERROR_EXIT;
            }

            // �����߳̿���
            HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (hThreadSnap == INVALID_HANDLE_VALUE) {
                KMesErr("�����߳̿���ʧ��");
                CloseHandle(hProcess);
                CloseHandle(hProcessSnap);
                return KSWORD_ERROR_EXIT;
            }

            // ��ȡ�߳���Ϣ
            THREADENTRY32 te32;
            te32.dwSize = sizeof(THREADENTRY32);
            if (!Thread32First(hThreadSnap, &te32)) {
                KMesErr("��ȡ�߳���Ϣʧ��");
                CloseHandle(hThreadSnap);
                CloseHandle(hProcess);
                CloseHandle(hProcessSnap);
                return KSWORD_ERROR_EXIT;
            }

            // �ָ������߳�
            do {
                if (te32.th32OwnerProcessID == dwProcessId) {
                    HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te32.th32ThreadID);
                    if (hThread == NULL) {
                        KMesErr("�޷����߳̽��в������ܾ����ʡ�");
                    }
                    else {
                        ResumeThread(hThread);
                        CloseHandle(hThread);
                    }
                }
            } while (Thread32Next(hThreadSnap, &te32));

            // ����
            CloseHandle(hThreadSnap);
            CloseHandle(hProcess);
            CloseHandle(hProcessSnap);
            KMesInfo("�ɹ�����Ŀ�����");
            return KSWORD_SUCCESS_EXIT;
        }
    } while (Process32Next(hProcessSnap, &pe32));

    // ����
    CloseHandle(hProcessSnap);
    return KSWORD_ERROR_EXIT;
}
//�����ҵĳ���·������Ӧ����һ����������ȡ��
static std::string programPath;
//��ȡ����·��������һ��string
std::string GetProgramPath() {
    char path[MAX_PATH];
    DWORD result = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (result == 0 || result == MAX_PATH) {
        // �������򻺳������
        return "��������ը";
    }
    programPath = std::string(path);
    return programPath;
}
//��ȡsystemȨ�޲������Լ�����Ҫ����ԱȨ�ޣ�
int GetSystem(const char* Para) {

    std::string SelfPathTemp=GetSelfPath();
    const char* cstr = SelfPathTemp.c_str();
    //// ���������ַ����ĳ���
    //size_t len1 = std::strlen(cstr);
    //size_t len2 = std::strlen(Para);

    //// Ϊ�ϲ�����ַ��������㹻�Ŀռ䣬�����ո�ͽ�����
    //char* combined = new char[len1 + len2 + 2]; // +2 ���ڿո�ͽ�����

    //// ���Ƶ�һ���ַ����������
    //std::strncpy(combined, cstr, len1);
    //combined[len1] = ' '; // ��ӿո�
    //combined[len1 + 1] = '\0'; // ȷ������ַ����Կ��ַ���β

    //// ׷�ӵڶ����ַ���������У�ָ����󳤶�
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
    std::cout << "��ǰ·��Ϊ" << GetSelfPath()<<std::endl;
    //ö�ٽ��̻�ȡlsass.exe��ID��winlogon.exe��ID�����������еĿ���ֱ�Ӵ򿪾����ϵͳ����
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
                //std::cout << "����ƥ�䵽�ˣ���" << std::endl;
            }
            //else if ((wcscmp(pe.szExeFile, L"lsass.exe") == 0)) {
            //	idW = pe.th32ProcessID;
            //	//std::cout << "����ƥ�䵽�ˣ���" << std::endl;
            //}
            else {
                //std::cout << "���ǣ���";
            }
        } while (Process32Next(hSnapshot, &pe));
        //std::cout << 3;
    }
    else {
        std::cout << GetLastError() << std::endl;
    }
    CloseHandle(hSnapshot);
    KMesInfo("lsass.exe PIDΪ" + std::to_string(idL) + ";winlogon PIDΪ " + std::to_string(idW));
    // ��ȡ���������lsass����winlogon
    KMesInfo("�򿪹ؼ�����");
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, FALSE, idL);
    if (!hProcess) {
        hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, FALSE, idW);
        if (!hProcess) {
            KMesErr("�޷���ȡ���������ʧ�ܡ�");
            return KSWORD_ERROR_EXIT;
        }
    }
    //std::cout << "Check1" << std::endl;
    //system("pause");
    HANDLE hTokenx;
    KMesInfo("��ȡ����");
    // ��ȡ����
    if (!OpenProcessToken(hProcess, TOKEN_DUPLICATE, &hTokenx)) {
        CloseHandle(hProcess);
        KMesErr("OpenProcessToken failed with error: " + std::to_string(GetLastError()));
        return KSWORD_ERROR_EXIT;
    }
    //std::cout << "��������" << std::endl;
    // ��������
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
    // ������Ϣ
    STARTUPINFOW si = { 0 };
    si.cb = sizeof(STARTUPINFOW);
    wchar_t lpDesktopTmp[] = L"winsta0\\default";
    si.lpDesktop = lpDesktopTmp; // ��ʾ����

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
    // ������Ϣ
    KMesInfo("ʹ����������");
    PROCESS_INFORMATION pi = { 0 };
    //bool fUIAccess = 1;
    //SetTokenInformation(hToken, TokenUIAccess, &fUIAccess, sizeof(fUIAccess));
    if (!CreateProcessWithTokenW(hToken, LOGON_NETCREDENTIALS_ONLY, NULL, wideCstr, NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
        delete[] wideCstr;
        CloseHandle(hToken);
        KMesErr("CreateProcessWithTokenW failed with error: " + std::to_string(GetLastError()));
        return KSWORD_ERROR_EXIT;
    }
    //�������̣�������CreateProcessAsUser���򱨴�1314����Ȩ
    //if (CreateProcessWithTokenW(hToken, LOGON_NETCREDENTIALS_ONLY, NULL, wideCstr, NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
    //	KMesInfo("�ɹ�ʹ��SYSTEMȨ������");
    //}
    //else {
    //	KMesErr("����ʧ��,�������Ϊ��");
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
        KMesErr("����:���ȱ�����systemȨ�ޡ�����getsys����");
        return;
    }
        //��Ȩ�Ե�ǰ�û���������
        //ȡexplorer��PID
        HWND hwnd = FindWindow(L"Shell_TrayWnd", NULL);
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);
        //�򿪾������ȡ����
        HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        HANDLE token;
        OpenProcessToken(handle, TOKEN_DUPLICATE, &token);//ȡ��token
        DuplicateTokenEx(token, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &token);
        CloseHandle(handle);
        //Ϊ��������UIAccess
        BOOL fUIAccess = TRUE;
        SetTokenInformation(token, TokenUIAccess, &fUIAccess, sizeof (fUIAccess));
        TOKEN_PRIVILEGES tp;
LUID luid;

// ���� SeDebugPrivilege
if (LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid)) {
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
}

// ���� SeImpersonatePrivilege
if (LookupPrivilegeValue(NULL, SE_IMPERSONATE_NAME, &luid)) {
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
}

// ���� SeAssignPrimaryTokenPrivilege
if (LookupPrivilegeValue(NULL, SE_ASSIGNPRIMARYTOKEN_NAME, &luid)) {
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
}
        //������Ϣ
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(STARTUPINFOW));
        si.cb = sizeof(STARTUPINFOW);
        si.lpDesktop = LPWSTR(L"winsta0\\default");//��ʾ����
        //�������̣�������CreateProcessAsUser���򱨴�1314����Ȩ
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
//������غ���
    //װ����������������·�����������ƣ�
bool LoadWinDrive(const WCHAR * drvPath, const WCHAR * serviceName) {

    // �򿪷�����ƹ��������ݿ�
    SC_HANDLE schSCManager = OpenSCManager(
        NULL,                   // Ŀ������������,NULL�����ӱ��ؼ�����ϵķ�����ƹ�����
        NULL,                   // ������ƹ��������ݿ�����ƣ�NULL���� SERVICES_ACTIVE_DATABASE ���ݿ�
        SC_MANAGER_ALL_ACCESS   // ����Ȩ��
    );
    if (schSCManager == NULL) {
        KMesErr("�򿪷�����ƹ������ݿ�ʧ�ܣ���ȷ��Ȩ���㹻");
        return KSWORD_ERROR_EXIT;
    }

    // ����������������������ƹ��������ݿ�
    SC_HANDLE schService = CreateService(
        schSCManager,               // ����ؼ����������ݿ�ľ��
        serviceName,                // Ҫ��װ�ķ��������
        serviceName,                // �û��������������ʶ�������ʾ����
        SERVICE_ALL_ACCESS,         // �Է���ķ���Ȩ�ޣ�����ȫȨ��
        SERVICE_KERNEL_DRIVER,      // �������ͣ���������
        SERVICE_DEMAND_START,       // ��������ѡ����̵��� StartService ʱ����
        SERVICE_ERROR_IGNORE,       // ����޷����������Դ����������
        drvPath,                    // �����ļ�����·������������ո���Ҫ���˫����
        NULL,                       // ���������ĸ��ض����飺��������ĳ����
        NULL,                       // ���ն�����Ψһ���ֵ��������
        NULL,                       // �������˳�����飺����û��������
        NULL,                       // ���з�����˻�����ʹ�� LocalSystem �˻�
        NULL                        // LocalSystem �˻�����
    );
    if (schService == NULL) {
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        KMesErr("����ʱ���󣺴����������ʧ��"+std::to_string(GetLastError()));
        return KSWORD_ERROR_EXIT;
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    KMesInfo("�ɹ��Ӵ�����������������װ�ص�����");
    return KSWORD_SUCCESS_EXIT;
}
bool LoadWinDrive(const char* drvPathA, const char* serviceNameA) {
    int drvPathWSize = MultiByteToWideChar(CP_UTF8, 0, drvPathA, -1, NULL, 0);
    int serviceNameWSize = MultiByteToWideChar(CP_UTF8, 0, serviceNameA, -1, NULL, 0);

    // �����㹻�Ŀռ����洢���ַ��ַ���
    WCHAR* drvPathW = (WCHAR*)malloc(drvPathWSize * sizeof(WCHAR));
    WCHAR* serviceNameW = (WCHAR*)malloc(serviceNameWSize * sizeof(WCHAR));

    // ִ��ת��
    MultiByteToWideChar(CP_UTF8, 0, drvPathA, -1, drvPathW, drvPathWSize);
    MultiByteToWideChar(CP_UTF8, 0, serviceNameA, -1, serviceNameW, serviceNameWSize);

    // ����LoadWinDrive����
    bool result = LoadWinDrive(drvPathW, serviceNameW);

    // �ͷ��ڴ�
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
//�������񣨷������ƣ�
bool StartDriverService(const WCHAR serviceName[]) {
    // �򿪷�����ƹ��������ݿ�
    SC_HANDLE schSCManager = OpenSCManager(
        NULL,                   // Ŀ������������,NULL�����ӱ��ؼ�����ϵķ�����ƹ�����
        NULL,                   // ������ƹ��������ݿ�����ƣ�NULL���� SERVICES_ACTIVE_DATABASE ���ݿ�
        SC_MANAGER_ALL_ACCESS   // ����Ȩ��
    );
    if (schSCManager == NULL) {
        CloseServiceHandle(schSCManager);
        KMesErr("�޷��򿪷�����ƹ��������ݿ⣬��ȷ��Ȩ���㹻");
        return KSWORD_ERROR_EXIT;
    }

    // �򿪷���
    SC_HANDLE hs = OpenService(
        schSCManager,           // ����ؼ����������ݿ�ľ��
        serviceName,            // Ҫ�򿪵ķ�����
        SERVICE_ALL_ACCESS      // �������Ȩ�ޣ�����Ȩ��
    );
    if (hs == NULL) {
        CloseServiceHandle(hs);
        CloseServiceHandle(schSCManager);
        KMesErr("�޷���ȡ������");
        return KSWORD_ERROR_EXIT;
    }
    if (StartService(hs, 0, 0) == 0) {
        CloseServiceHandle(hs);
        CloseServiceHandle(schSCManager);
        KMesErr("����ʱ�����޷���������"+std::to_string(GetLastError()));
        return KSWORD_ERROR_EXIT;
    }


    CloseServiceHandle(hs);
    CloseServiceHandle(schSCManager);
    KMesInfo("�ɹ���������");
    return KSWORD_SUCCESS_EXIT;
}
bool StartDriverService(const char* text) {
    return StartDriverService(CharToWChar(text));
}
bool StartDriverService(std::string text) {
    return StartDriverService(text.c_str());
}
//ֹͣ���񣨷������ƣ�
bool StopDriverService(const WCHAR serviceName[]) {
    // �򿪷�����ƹ��������ݿ�
    SC_HANDLE schSCManager = OpenSCManager(
        NULL,                   // Ŀ������������,NULL�����ӱ��ؼ�����ϵķ�����ƹ�����
        NULL,                   // ������ƹ��������ݿ�����ƣ�NULL���� SERVICES_ACTIVE_DATABASE ���ݿ�
        SC_MANAGER_ALL_ACCESS   // ����Ȩ��
    );
    if (schSCManager == NULL) {
        CloseServiceHandle(schSCManager);
        KMesErr("�޷��򿪷�����ƹ���������ȷ��Ȩ��");
        return KSWORD_ERROR_EXIT;
    }

    // �򿪷���
    SC_HANDLE hs = OpenService(
        schSCManager,           // ����ؼ����������ݿ�ľ��
        serviceName,            // Ҫ�򿪵ķ�����
        SERVICE_ALL_ACCESS      // �������Ȩ�ޣ�����Ȩ��
    );
    if (hs == NULL) {
        CloseServiceHandle(hs);
        CloseServiceHandle(schSCManager);
        KMesErr("�޷��򿪷�����ȷ��Ȩ��");
        return KSWORD_SUCCESS_EXIT;
    }

    // ���������������
    SERVICE_STATUS status;
    if (QueryServiceStatus(hs, &status) == 0) {
        CloseServiceHandle(hs);
        CloseServiceHandle(schSCManager);
        KMesErr("������������");
        return KSWORD_ERROR_EXIT;
    }
    if (status.dwCurrentState != SERVICE_STOPPED &&
        status.dwCurrentState != SERVICE_STOP_PENDING
        ) {
        // ���͹رշ�������
        if (ControlService(
            hs,                         // ������
            SERVICE_CONTROL_STOP,       // �����룺֪ͨ����Ӧ��ֹͣ
            &status                     // �������µķ���״̬��Ϣ
        ) == 0) {
            CloseServiceHandle(hs);
            CloseServiceHandle(schSCManager);
            KMesErr("���Թرշ���ʱ��������");
            return KSWORD_ERROR_EXIT;
        }

        // �жϳ�ʱ
        INT timeOut = 0;
        while (status.dwCurrentState != SERVICE_STOPPED) {
            timeOut++;
            QueryServiceStatus(hs, &status);
            Sleep(50);
        }
        if (timeOut > 80) {
            CloseServiceHandle(hs);
            CloseServiceHandle(schSCManager);
            KMesErr("������ƹ�������ʱδ��Ӧ");
            return KSWORD_ERROR_EXIT;
        }
    }

    CloseServiceHandle(hs);
    CloseServiceHandle(schSCManager);
    KMesInfo("�ɹ�ֹͣ����");
    return KSWORD_SUCCESS_EXIT;
}
bool StopDriverService(std::string text) {
    return StopDriverService(text.c_str());
}
bool StopDriverService(const char* text) {
    return StopDriverService(CharToWChar(text));
}
//ж���������������ƣ�
bool UnLoadWinDrive(const WCHAR serviceName[]) {
    SC_HANDLE schSCManager = OpenSCManager(
        NULL,                   // Ŀ������������,NULL�����ӱ��ؼ�����ϵķ�����ƹ�����
        NULL,                   // ������ƹ��������ݿ�����ƣ�NULL���� SERVICES_ACTIVE_DATABASE ���ݿ�
        SC_MANAGER_ALL_ACCESS   // ����Ȩ��
    );
    if (schSCManager == NULL) {
        CloseServiceHandle(schSCManager);
        KMesErr("�޷��򿪷�����ƹ��������ݿ⣬��ȷ��Ȩ��");
        return KSWORD_ERROR_EXIT;
    }

    // �򿪷���
    SC_HANDLE hs = OpenService(
        schSCManager,           // ����ؼ����������ݿ�ľ��
        serviceName,            // Ҫ�򿪵ķ�����
        SERVICE_ALL_ACCESS      // �������Ȩ�ޣ�����Ȩ��
    );
    if (hs == NULL) {
        CloseServiceHandle(hs);
        CloseServiceHandle(schSCManager);
        KMesErr("�޷��򿪷���");
        return KSWORD_ERROR_EXIT;
    }

    // ɾ������
    if (DeleteService(hs) == 0) {
        CloseServiceHandle(hs);
        CloseServiceHandle(schSCManager);
        KMesErr("ɾ��������ʱ��������");
        return KSWORD_ERROR_EXIT;
    }

    CloseServiceHandle(hs);
    CloseServiceHandle(schSCManager);
    KMesInfo("�ɹ�ж������");
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
        KMesErr("�򿪵�ǰ��������ʧ�ܣ��������" + std::to_string(GetLastError()));
        return FALSE;
    }

    LUID luid;
    if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid)) {
        KMesErr("��ѯ����Ȩ��ʧ�ܣ��������" + std::to_string(GetLastError()));
        CloseHandle(hToken);
        return FALSE;
    }

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL)) {
        KMesErr("��Ȩ��debugʧ�ܣ��������" + std::to_string(GetLastError()));
        CloseHandle(hToken);
        return FALSE;
    }

    CloseHandle(hToken);
    return TRUE;
}BOOL HasDebugPrivilege()
{
    HANDLE hToken;
    DWORD dwSize;

    // ������
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return FALSE;

    // ��һ�λ�ȡ���軺������С
    GetTokenInformation(hToken, TokenPrivileges, NULL, 0, &dwSize);
    DWORD lastError = GetLastError();
    if (lastError != ERROR_INSUFFICIENT_BUFFER) {
        CloseHandle(hToken);
        return FALSE;
    }

    // ��̬�����ڴ�
    PTOKEN_PRIVILEGES pTokenPrivileges = (PTOKEN_PRIVILEGES)malloc(dwSize);
    if (!pTokenPrivileges) {
        CloseHandle(hToken);
        return FALSE;
    }

    // ʵ�ʻ�ȡȨ����Ϣ
    if (!GetTokenInformation(hToken, TokenPrivileges, pTokenPrivileges, dwSize, &dwSize)) {
        free(pTokenPrivileges);
        CloseHandle(hToken);
        return FALSE;
    }

    // ��������Ȩ�޲��� SE_DEBUG_NAME
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
//��������
    //����������
    //0���������з���ֱ�������˳����൱���ˣ�
    //1��ʹ��taskkill�����ǿ�Ʊ�ʶ����������
    //2��ʹ��taskkill�����ǿ�Ʊ�ʶ����������
    //3������TerminateProcess()������ֹ����
    //4������TerminateThread()�����ݻ������߳�
    //5������nt terminate��������
    //6: ʹ����ҵ�����������
    //7�����ں�Ȩ�ޣ�ʹ��ZwTerminateProcess()������������
    //8�����ں�Ȩ�ޣ���̬����PspTerminateThreadByPointer�������н���
    //9�����ں�Ȩ�ޣ������ڴ�
BOOL TerminateAllThreads(HANDLE hProcess);
BOOL NtTerminate(HANDLE hProcess);
BOOL TerminateProcessViaJob(HANDLE hProcess);
int KillProcess(int a, HANDLE ProcessHandle) {
    if (a == 1 || a == 2) {
        KMesErr("���÷������󣺲��ܶ�taskkill������");
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
        KMesErr("����ĵ��÷���������");
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
            KMesErr("��PIDΪ" + std::to_string(PID) + "�Ľ���ʧ�ܣ��������" + std::to_string(GetLastError()));
            return KSWORD_ERROR_EXIT;
        }
        else return KillProcess(a, hProcess);
    }
    
}

//���н������̵ķ�����Ӧʵ�ֺ���
bool KillProcess1(DWORD PID) {
    std::string cmd;
    std::string returnString="taskkill�������������ֵ��";
    cmd = "taskkill /pid ";
    cmd += std::to_string(PID);
    returnString+=RunCmdNow(cmd.c_str());
    KMesInfo(returnString);
    
    return KSWORD_SUCCESS_EXIT;
}
bool KillProcess2(DWORD PID) {
    std::string cmd;
    std::string returnString = "taskkill�������������ֵ��";
    cmd = "taskkill /f /pid ";
    cmd += std::to_string(PID);
    returnString += RunCmdNow(cmd.c_str());
    KMesInfo(returnString);
    return KSWORD_SUCCESS_EXIT;
}
BOOL TerminateProcessById(HANDLE hProcess, UINT uExitCode)
{
    // ������Ƿ���Ч
    if (hProcess == NULL || hProcess == INVALID_HANDLE_VALUE)
    {
        KMesErr("���̾����Ч");
        return KSWORD_ERROR_EXIT;
    }
    EnableDebugPrivilege(TRUE);
    BOOL bResult = TerminateProcess(hProcess, uExitCode);
    if (!bResult)
    {
        KMesErr("��������ʧ�ܣ������룺" + std::to_string(GetLastError()));
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
    // ���Ի�ȡ���̵��˳�����
    if (!GetExitCodeProcess(hProcess, &exitCode))
    {
        // ����޷���ȡ�˳����룬���� true�������Ǿ����Ч�����⣩
        return true;
    }
    // ��������������У����� true
    return exitCode == STILL_ACTIVE;
}


// {
// test
    void EnumProcessThreads(DWORD processID) {
        // �����߳̿���
        HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (hThreadSnap == INVALID_HANDLE_VALUE) {
            std::cerr << "Failed to create thread snapshot." << std::endl;
            return;
        }

        THREADENTRY32 te32;
        te32.dwSize = sizeof(THREADENTRY32);

        // ��ȡ��һ���߳�
        if (!Thread32First(hThreadSnap, &te32)) {
            std::cerr << "Failed to retrieve thread information." << std::endl;
            CloseHandle(hThreadSnap);
            return;
        }

        // �����߳�
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

        // ��ȡĿ����̵� ID
        DWORD processID = GetProcessId(hProcess);
        if (processID == 0) {
            return FALSE; // �޷���ȡ���� ID
        }

        // �����߳̿���
        HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (hThreadSnap == INVALID_HANDLE_VALUE) {
            return FALSE;
        }

        THREADENTRY32 te32;
        te32.dwSize = sizeof(THREADENTRY32);

        *cbNeeded = 0; // ��ʼ���������
        DWORD count = 0;

        // �����߳̿���
        if (Thread32First(hThreadSnap, &te32)) {
            do {
                if (te32.th32OwnerProcessID == processID) {
                    // ����Ƿ����㹻�Ŀռ�洢�߳� ID
                    if ((count + 1) * sizeof(DWORD) <= dwSize) {
                        dwThreadIds[count] = te32.th32ThreadID;
                        count++;
                    }
                    else {
                        // �ռ䲻��ʱ����ͳ�������ֽ���
                        *cbNeeded += sizeof(DWORD);
                    }
                }
            } while (Thread32Next(hThreadSnap, &te32));
        }

        CloseHandle(hThreadSnap);

        *cbNeeded = count * sizeof(DWORD); // ������ʹ�õ��ֽ���

        // ����Ƿ�ռ䲻��
        if (*cbNeeded > dwSize) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }

        return TRUE;
    }
// }


BOOL TerminateAllThreads(HANDLE hProcess) {
    if (hProcess == NULL || hProcess == INVALID_HANDLE_VALUE) {
        std::cerr << "��Ч�Ľ��̾����" << std::endl;
        return FALSE;
    }

    // ��ȡ���̵��߳�����
    DWORD dwThreadIds[1024]; // ����һ���㹻����������洢�߳�ID
    DWORD cbNeeded;

    if (!EnumProcessThreads(hProcess, &dwThreadIds[0], sizeof(dwThreadIds), &cbNeeded)) {
        std::cerr << "ö���߳�ʧ�ܣ������룺" << GetLastError() << std::endl;
        return FALSE;
    }

    // ����ʵ�ʵ��߳�����
    DWORD dwThreadCount = cbNeeded / sizeof(DWORD);

    // ��������߳�
    for (DWORD i = 0; i < dwThreadCount; i++) {
        HANDLE hThread = OpenThread(THREAD_TERMINATE, FALSE, dwThreadIds[i]);
        if (hThread != NULL) {
            BOOL bResult = TerminateThread(hThread, 1); // ʹ��1��Ϊ�˳�����
            if (!bResult) {
                std::cerr << "�����߳�ʧ�ܣ��߳�ID��" << dwThreadIds[i] << "�������룺" << GetLastError() << std::endl;
            }
            CloseHandle(hThread);
        }
        else {
            std::cerr << "���߳�ʧ�ܣ��߳�ID��" << dwThreadIds[i] << "�������룺" << GetLastError() << std::endl;
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
        KMesErr("��ע�⣺�����ᵼ��ksword������һ���˳����س��Լ����������κ������Գ���");
        if (!(Kgetline() == ""))return FALSE;
        TerminateJobObject(hJob, 0);
        CloseHandle(hJob);
        bool returnValue = ExistProcess(hProcess);
        CloseHandle(hProcess);
        return !returnValue;
    }
}


// ���õ�ǰ���̵����ȼ�Ϊʵʱ
bool SetProcessToRealtimePriority() {
    HANDLE hProcess = GetCurrentProcess();
    if (!SetPriorityClass(hProcess, REALTIME_PRIORITY_CLASS)) {
        std::cerr << "Failed to set process priority to REALTIME_PRIORITY_CLASS. Error: " << GetLastError() << std::endl;
        return false;
    }
    //std::cout << "Process priority set to REALTIME_PRIORITY_CLASS." << std::endl;
    return true;
}

// ����ָ���̵߳����ȼ�Ϊ��ߣ�ʵʱ��
bool SetThreadToTimeCriticalPriority(HANDLE hThread) {
    if (!SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL)) {
        std::cerr << "Failed to set thread priority to THREAD_PRIORITY_TIME_CRITICAL. Error: " << GetLastError() << std::endl;
        return false;
    }
    return true;
}

// ��ȡ��ǰ���̵������̲߳�����Ϊʵʱ���ȼ�
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

// �����������ý��̺������̵߳����ȼ�
bool SetProcessAndThreadsToRealtimePriority() {
    if (!SetProcessToRealtimePriority()) {
        return false;
    }

    if (!SetAllThreadsToRealtimePriority()) {
        return false;
    }
    KMesInfo("��ǰ����������Ϊ�����ȼ�");
    return true;
}