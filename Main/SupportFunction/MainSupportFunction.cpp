#ifdef KSWORD_WITH_COMMAND
#include "..\KswordTotalHead.h"
using namespace std::chrono;
using namespace std;

FileInfo aptinfo[50];
version kswordversion;

//全局变量================================================================

//资源相关
int rcnumber;//存在的资源数量
string serverip = "159.75";
string localadd;//本地资源存放路径
//正在执行的命令
string nowcmd;
string cmdpara[MAX_PARA_NUMBER];
int cmdparanum;
string cmdtodo[50];
int cmdtodonum;
int cmdtodonumpointer;
//线程信号
bool ThreadTopWorking = 0;
int ctrlCount = 0;
bool windowsshow = 1;//窗口是否显示
int TopThreadID;
//其他变量
std::string path = "C:\\Windows\\system32";//终端所处的路径



// 全局变量，用于跟踪按键次数
int dotKeyCount = 0;

//进程通讯
HANDLE Ksword_Pipe_1;
HANDLE Ksword_Main_Pipe=INVALID_HANDLE_VALUE;
HANDLE Ksword_Main_Sock_Pipe;

//函数声明

int findrc();//查找本地资源存放路径
int readFile(const std::string&);
bool DirectoryExists(const std::wstring&);
void keyboardListener();
int shell();
void compressBackslashes(std::string&);
void exitlistener();
void SetWindowTopmostAndFocus();
LRESULT CALLBACK LowLevelKeyboardProcInput(int nCode, WPARAM wParam, LPARAM lParam);





int readFile(const std::string& path) {
	ifstream file(path);
	std::cout << path << endl;
	if (!file.is_open()) {
		KMesErr("打开配置文件时失败");
		return KSWORD_ERROR_EXIT;
	}
	string poo;
	getline(file, poo);
	file >> kswordversion.a >> kswordversion.b;
	file >> ws;
	if ((kswordversion.a > KSWORD_MAIN_VER_A) || ((kswordversion.a == KSWORD_MAIN_VER_A) && (kswordversion.b > KSWORD_MAIN_VER_B))) {
		KMesErr("该程序不是最新版本，请先升级此程序。");
		return KSWORD_ERROR_EXIT;
	}
	getline(file, poo);
	file >> rcnumber;
	file >> ws;
	getline(file, poo);
#undef max
	for (int i = 1; i <= rcnumber; i++) {
		file >> aptinfo[i].filename >> aptinfo[i].year >> aptinfo[i].month >> aptinfo[i].day >> aptinfo[i].filesize; file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	}
	KMesInfo("更新插件列表完成");
	file.close();
	return KSWORD_SUCCESS_EXIT;
}
bool DirectoryExists(const std::wstring& path) {
	DWORD dwAttrib = GetFileAttributesW(path.c_str());
	return (dwAttrib != INVALID_FILE_ATTRIBUTES &&
		(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}
int findrc() {
	wchar_t drive[100] = L"C:\\";
	for (wchar_t letter = 'C'; letter <= 'Z'; ++letter) {
		drive[0] = letter;
		if (DirectoryExists(std::wstring(drive) + L"kswordrc")) {
			std::wcout << L"Directory found at: " << drive << L"kswordrc" << std::endl;
			char narrow_letter = static_cast<char>(letter);
			localadd = ""; localadd += narrow_letter; localadd += ":\\kswordrc\\";
			return KSWORD_SUCCESS_EXIT;
		}
	}
	KMesErr("找不到可能的资源地址，请稍后手动输入");
	localadd = "c:\\";
	return KSWORD_ERROR_EXIT;
}




// 获取当前窗口句柄并置顶设为焦点窗口的函数
void SetWindowTopmostAndFocus(int TID) {
	KswordThreadStop[TID] = 1;
	HWND hwnd = GetConsoleWindow();
	milliseconds keyRepeatInterval(150);
	while (1) {
		if (KswordThreadStop[TID])
			std::this_thread::sleep_for(keyRepeatInterval);
		else {
			//SetForegroundWindow(hwnd);
			SetTopWindow();
		}
	}
}
void compressBackslashes(std::string& cmdpara) {
	size_t len = cmdpara.length();
	std::string result;
	result.reserve(len); // 预分配内存以提高性能

	bool inEscape = false; // 标记是否处于转义序列中
	for (size_t i = 0; i < len; ++i) {
		if (cmdpara[i] == '\\') {
			if (!inEscape) { // 如果当前不在转义序列中，则添加一个反斜杠
				result += '\\';
				inEscape = true; // 现在我们处于转义序列中
			}
		}
		else {
			result += cmdpara[i]; // 添加普通字符
			inEscape = false; // 离开转义序列
		}
	}
	cmdpara = result; // 用处理后的字符串覆盖原字符串
}

// 键盘钩子的回调函数``
LRESULT CALLBACK LowLevelKeyboardProcInput(int nCode, WPARAM wParam, LPARAM lParam) {
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

	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

void keyboardListener() {
	//using namespace std::chrono;
	//milliseconds keyPressInterval(2000); // 设置2秒的检测间隔
	//milliseconds keyRepeatInterval(10); // 设置100ms的按键检测间隔
	//steady_clock::time_point lastKeyPressTime = steady_clock::now();
	//while (1) {
	//	if (GetAsyncKeyState(VK_RCONTROL) & 0x8000) { // 检测Ctrl键是否被按下
	//		auto currentTime = steady_clock::now();
	//		if (currentTime - lastKeyPressTime < keyPressInterval) {
	//			ctrlCount++; // 增加按键计数
	//		}
	//		else {
	//			ctrlCount = 0; // 如果超过2秒，重置按键计数
	//		}
	//		lastKeyPressTime = currentTime; // 更新最后按下时间
	//		while (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
	//			std::this_thread::sleep_for(milliseconds(50)); // 等待Ctrl键释放，减少CPU占用
	//		}
	//		if (ctrlCount >= 3) {
	//			if (windowsshow) {
	//				HideWindow();
	//				windowsshow = false;
	//			}
	//			else {
	//				ShowWindow();
	//				if (IsIconic(GetConsoleWindow())) {
	//					ShowWindow(GetConsoleWindow(), SW_RESTORE);
	//				}
	//				else {
	//					// 窗口不是最小化的
	//				}
	//				windowsshow = true;
	//			}
	//			ctrlCount = 0; // 重置按键计数
	//		}
	//	}
	//	std::this_thread::sleep_for(keyRepeatInterval); // 每100ms检测一次
	//}
	using namespace std::chrono;
	milliseconds keyRepeatInterval(50); // 设置100ms的按键检测间隔
	milliseconds keyContinue(500);
	while (1) {
		if (((GetAsyncKeyState(VK_RCONTROL) & 0x8000))|| (GetAsyncKeyState(VK_RMENU) & 0x8000)) {
			if (windowsshow) {
				HideWindow();
				windowsshow = false;
			}
			else {
				ShowWindow();
				if (IsIconic(GetConsoleWindow())) {
					ShowWindow(GetConsoleWindow(), SW_RESTORE);
				}
				else {
					// 窗口不是最小化的
				}
				windowsshow = true;
			}
		std::this_thread::sleep_for(keyContinue);
		}std::this_thread::sleep_for(keyRepeatInterval); // 每100ms检测一次
	}
}

// 热键 ID
const int HOTKEY_ID = 1;

// 热键处理线程函数
DWORD WINAPI HotkeyThread(LPVOID lpParam) {
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_HOTKEY) {
            // 检查是否是 Ctrl+G 热键
            if (msg.wParam == MOD_CONTROL && msg.lParam == 'G') {
                // 获取控制台窗口句柄
                HWND hWnd = GetConsoleWindow();
                if (hWnd) {
                    // 移除 WS_EX_TRANSPARENT 样式
                    LONG_PTR exStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
                    exStyle &= ~WS_EX_TRANSPARENT;
                    SetWindowLongPtr(hWnd, GWL_EXSTYLE, exStyle);

                    // 更新窗口以反映新的样式
                    SetWindowPos(hWnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

                    std::cout << "Click-through effect removed." << std::endl;
                }
            }
        }
    }
    return 0;
}

void KswordPrintLogo() {
	std::cout << std::endl;	std::cout << "  _  __                                      _ ";	
	std::cout << std::endl;
	std::cout << " | |/ /  Dev WinAPI C++ CUI Tool  _ __    __| |" << std::endl;
	std::cout << " | ' /  / __| \\ \\ /\\ / /  / _ \\  | '__|  / _` |" << std::endl;
	std::cout << " | . \\  \\__ \\  \\ V  V /  | (_) | | |    | (_| |" << std::endl;
	std::cout << " |_|\\_\\ |___/   \\_/\\_/    \\___/  |_|     \\__,_|"; std::cout << std::endl;
	/*for (int i = 1; i <= 35; i++) {
		cprint("█", 15, 1);
		Sleep(5);
	}
	for (int i = 1; i <= 70; i++) {
		putchar('\b');
	}*/
	std::cout << "Ksword Internal Release V"
		<<KSWORD_MAIN_VER_A<<"."<<KSWORD_MAIN_VER_B<<"."<<KSWORD_MAIN_VER_C
		;

	std::cout << "||Based on Ksword Exe Framework.";
	std::cout << std::endl;
}

void exitlistener() {
	using namespace std::chrono;
	milliseconds keyRepeatInterval(150); // 设置100ms的按键检测间隔
	while (1) {
		if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
			MainExit(); // 退出程序
			exit(0);
		}
		std::this_thread::sleep_for(keyRepeatInterval); // 每100ms检测一次
	}
}

void KMainRefresh() {
	SetConsola();
	SetAir(200);
	HideSide();
	SetConsoleWindowPosition(LeftColumnStartLocationX, LeftColumnStartLocationY);
	SetConsoleWindowSize(ColumnWidth, ColumnHeight);
}
//void timerFunction() {
//    while (true) {
//        this_thread::sleep_for(seconds(3)); // 等待3秒钟
//        // 重置计数器
//        ctrlCount = 0;
//    }
//}
#endif