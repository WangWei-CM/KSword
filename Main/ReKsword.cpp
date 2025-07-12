#ifdef KSWORD_WITH_COMMAND
#include "KswordTotalHead.h"
using namespace std;

HANDLE KswordSubProcess7 = nullptr;
HANDLE KswordSubProcess2 = nullptr;

bool IsDebug;
bool IsTop;
//关闭自己

bool sos;



class PipeStreambuf : public std::streambuf {
public:
    PipeStreambuf(HANDLE hPipe) : pipe(hPipe) {
	}


protected:
    virtual int_type overflow(int_type c) override {
		if (c != EOF) {
            char ch = c;
            DWORD written;
			string a; a += c;
			KswordSendMain(a);
        }
		return sync() == 0 ? c : EOF;;
    }

    virtual int sync() override {
        if (pptr() > pbase()) {
            DWORD written = 0;
            WriteFile(pipe, pbase(), pptr() - pbase(), &written, NULL);
            setp(pbase(), epptr()); // 重置缓冲区
			Sleep(20);
        }
		return 0;
    }

private:
    HANDLE pipe; // 管道句柄
};

// 全局流缓冲区对象
PipeStreambuf globalPipeStreambuf(Ksword_Main_Pipe);

// 函数：将 std::cout 的内容转发到 Ksword_Main_Pipe
void RedirectCoutToPipe() {
    // 使用全局流缓冲区替换 std::cout 的缓冲区
    std::cout.rdbuf(&globalPipeStreambuf);
}

void MainExit() {
	FreeConsole();
	CloseHandle(Ksword_Pipe_1);
	if(KswordSubProcess7!=NULL)
	TerminateProcessById(KswordSubProcess7);
	TerminateProcessById(KswordSubProcess2);
	return;
}
// 安装和卸载钩子的函数
HHOOK SetInputHook(HINSTANCE hInstance) {
	HHOOK hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProcInput, hInstance, 0);
	return hHook;
}


void Unhook(HHOOK hHook) {
	UnhookWindowsHookEx(hHook);
}
	int KMainRunTime = 0;//ksword主程序被第几次启动。这决定管道名字。

	//dll审查
	
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

	
	int KSCMDmain(int argc, char* argv[]) {
		//system("pause");
		//_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
		//std::string params;
		//for (int i = 0; i < argc; ++i) {
		//	params += argv[i];
		//	if (i < argc - 1) {
		//		params += " ";
		//	}
		//}
		//std::wstring wparams(params.begin(), params.end());
		//MessageBox(NULL, CharToWChar(to_string(argc).c_str()), L"程序参数", MB_OK | MB_ICONINFORMATION);
		//Sleep(1000);
		//SetConsoleOutputCP(CP_UTF8);
		if (strcmp(argv[0], "998") == 0) {
			SECURITY_ATTRIBUTES sa;
			SECURITY_DESCRIPTOR sd;
			InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
			SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE); // 禁用所有访问权限

			sa.nLength = sizeof(SECURITY_ATTRIBUTES);
			sa.lpSecurityDescriptor = &sd;
			sa.bInheritHandle = FALSE;

			// 2. 创建隔离桌面（仅允许当前进程访问）
			HDESK hSecureDesktop = CreateDesktop(
				L"KswordSecureDesktop1", nullptr, nullptr, 0,
				DESKTOP_CREATEWINDOW | DESKTOP_SWITCHDESKTOP,
				&sa // 应用安全描述符
			);
			if (!hSecureDesktop) {
				std::cerr << "CreateDesktop failed. Error: " << GetLastError() << std::endl;
			}

			// 3. 切换到新桌面
			if (!SetThreadDesktop(hSecureDesktop) || !SwitchDesktop(hSecureDesktop)) {
				std::cerr << "SwitchDesktop failed. Error: " << GetLastError() << std::endl;
				CloseDesktop(hSecureDesktop);
			}

			// 4. 在新桌面中运行受保护程序（如密码输入界面）
			STARTUPINFO si = { sizeof(si) };
			PROCESS_INFORMATION pi;
			std::wstring desktopName = StringToWString("KswordSecureDesktop1");
			std::wstring exePath = StringToWString(GetSelfPath());

			si.lpDesktop = const_cast<LPWSTR>(desktopName.c_str()); // 安全移除 const

			wchar_t tmp[5] = L"sos";
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
				WaitForSingleObject(pi.hProcess, INFINITE);
				CloseHandle(pi.hProcess);
				CloseHandle(pi.hThread);
			}

			// 5. 恢复默认桌面
			HDESK hDefault = OpenDesktop(L"Default", 0, FALSE, GENERIC_ALL);
			if (hDefault) {
				SwitchDesktop(hDefault);
				CloseDesktop(hDefault);
			}
			CloseDesktop(hSecureDesktop);
			return 0;

		}

		CalcWindowStyle();
		SetProcessAndThreadsToRealtimePriority();
		serverip += ".66.16";
		//for (int i = 1; i <= argc - 1; i++)cout << argv[i] << ' ';
		if (argc == 1) {
			if (strcmp(argv[0], "sos") == 0) {
				sos = 1;
			}
			else if (strcmp(argv[0], "999") == 0) {
				return KswordMain1(argv[0]);
			}
			else if (strcmp(argv[0], "top") == 0) {
				SetTopWindow();
				//Kpause();
				DWORD exStyle = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);
				exStyle |= WS_EX_TRANSPARENT;
				SetConsoleWindowSize(ColumnWidth, ColumnHeight);
				SetWindowLong(GetConsoleWindow(), GWL_EXSTYLE, exStyle);
				SetWindowPos(GetConsoleWindow(), nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
				SetTopWindow();
				SetAir(200);
				SetConsoleWindowSize(ColumnWidth, ColumnHeight + 7);
				LONG_PTR currentExStyle = GetWindowLongPtr(GetConsoleWindow(), GWL_EXSTYLE);
				SetWindowLongPtr(GetConsoleWindow(), GWL_EXSTYLE, currentExStyle | WS_EX_TOOLWINDOW);
				HideSide();
				SetConsoleWindowPosition(LeftColumnStartLocationX, LeftColumnStartLocationY);
				SetWindowDisplayAffinity(GetConsoleWindow(), WDA_MONITOR);
				KMesWarn("彩色输出无法通过管道显示");
				HANDLE hDAMNPipe = CreateFile(
					L"\\\\.\\pipe\\Ksword_Main_Pipe",
					GENERIC_READ,
					0,
					NULL,
					OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL,
					NULL
				);
				const size_t BUFFER_SIZE = 1024;
				char buffer[BUFFER_SIZE];
				DWORD bytesRead;
				while (true) {
					// 清空缓冲区
					memset(buffer, 0, BUFFER_SIZE);
					// 从管道读取数据
					if (!ReadFile(hDAMNPipe, buffer, BUFFER_SIZE - 1, &bytesRead, NULL)) {
						DWORD error = GetLastError();
						if (error == ERROR_BROKEN_PIPE) {
							std::cout << "Pipe closed by the other end." << std::endl;
							break;
						}
						else {
							std::cerr << "Error reading from pipe: " << error << std::endl;
							break;
						}
					}
					if (bytesRead > 0) {
						buffer[bytesRead] = '\0'; // 确保字符串以null结尾
						std::cout << buffer;
					}
				}
				exit(0);
			}
			else if (strcmp(argv[0], "topsock") == 0) {
				SetTopWindow();
				//Kpause();
				AllocConsole();
				//SetConsoleOutputCP(CP_UTF8);
				//SetConsola();
				//system("color 97");
				DWORD exStyle = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);
				exStyle |= WS_EX_TRANSPARENT;
				SetWindowLong(GetConsoleWindow(), GWL_EXSTYLE, exStyle);
				SetWindowPos(GetConsoleWindow(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
				SetAir(200);
				//HideSide();
				Sleep(50);
				SetConsoleWindowPosition(RightColumnStartLocationX, RightColumnStartLocationY);
				SetConsoleWindowSize(ColumnWidth, ColumnHeight + 7);
				LONG_PTR currentExStyle = GetWindowLongPtr(GetConsoleWindow(), GWL_EXSTYLE);
				SetWindowLongPtr(GetConsoleWindow(), GWL_EXSTYLE, currentExStyle | WS_EX_TOOLWINDOW);
				std::thread listenerThread(keyboardListener);
				KMesInfo("子进程1成功启动");
				SetWindowText(GetConsoleWindow(), L"Ksword通讯主程序");
				HANDLE hDAMNPipe = CreateFile(
					L"\\\\.\\pipe\\Ksword_Main_Sock_Pipe",
					GENERIC_READ,
					0,
					NULL,
					OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL,
					NULL
				);
				const size_t BUFFER_SIZE = 1024;
				char buffer[BUFFER_SIZE];
				DWORD bytesRead;
				while (true) {
					// 清空缓冲区
					memset(buffer, 0, BUFFER_SIZE);
					// 从管道读取数据
					if (!ReadFile(hDAMNPipe, buffer, BUFFER_SIZE - 1, &bytesRead, NULL)) {
						DWORD error = GetLastError();
						if (error == ERROR_BROKEN_PIPE) {
							std::cout << "Pipe closed by the other end." << std::endl;
							break;
						}
						else {
							std::cerr << "Error reading from pipe: " << error << std::endl;
							break;
						}
					}
					if (bytesRead > 0) {
						buffer[bytesRead] = '\0'; // 确保字符串以null结尾
						std::cout << buffer;
					}
				}
				exit(0);
			}
		}
		if (argc == 2)
		{
			if (strcmp(argv[0], "1") == 0) {
				return KswordMain1(argv[1]);
			}
			else if (strcmp(argv[0], "7") == 0) {
				return KswordMain7();
			}
			else if (strcmp(argv[0], "2") == 0) {
				return KswordMain2();
			}
			else if (strcmp(argv[0], "sethc.exe") == 0) {

				//MessageBox(NULL, L"你好，我是LCR", L"提示", MB_OK);
				STARTUPINFO si;
				PROCESS_INFORMATION pi;

				ZeroMemory(&si, sizeof(si));
				si.cb = sizeof(si);
				si.dwFlags |= STARTF_USESHOWWINDOW;
				si.wShowWindow = SW_SHOW; // 确保新窗口可见
				string tmp1 = GetSelfPath();
				wchar_t a[5] = {};
				// 创建新进程
				if (!CreateProcess(
					CharToWChar(tmp1.c_str()), // 模块名（可执行文件名）
					//CharToWChar("C:\\Users\\33251\\Desktop\\ksword\\test\\systemtest.exe"),
					a, // 命令行
					NULL, // 进程安全性
					NULL, // 线程安全性
					FALSE, // 继承句柄
					CREATE_NEW_CONSOLE, // 创建新控制台
					NULL, // 使用父进程的环境块
					NULL, // 使用父进程的当前目录
					&si, // 指向STARTUPINFO结构的指针
					&pi // 指向PROCESS_INFORMATION结构的指针
				)) {
					KMesErr("无法打开子进程，请重新启动Ksword工作区");
					return NULL;
				}
				CloseHandle(pi.hThread);
				exit(0);
			}//否则就是首次正常启动
		}
		if (argc == 1) {
			//表明这是主程序第argv次被启动
			if (strcmp(argv[0], "211") != 0) {
				KMainRunTime = atoi(argv[0]);
			}
			else {
				KMainRunTime = 0;
			}

		}
		//system("pause");
		//这是初次启动，管道应该采用x_1
		PasswordVariety();
		//system("color 97");
		//添加点击穿透
		DWORD exStyle = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);
		exStyle |= WS_EX_TRANSPARENT;
		SetWindowLong(GetConsoleWindow(), GWL_EXSTYLE, exStyle);
		SetWindowPos(GetConsoleWindow(), nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
		SetTopWindow();
		//RegisterHotKey(NULL, HOTKEY_ID, MOD_CONTROL, 'G');
		CreateThread(NULL, 0, HotkeyThread, NULL, 0, NULL);
		thread listenerThread(keyboardListener);
		KswordRegThread("Ctrl显示/隐藏线程");
		TopThreadID = KswordRegThread("重复置顶线程");
		thread Top(SetWindowTopmostAndFocus, TopThreadID);
		TopThreadID = KswordRegThread("ESC监听线程");
		thread ExitListener(exitlistener);
		SetConsola();
		SetAir(200);
		COORD bufferSize = { ColumnWidth-3, ColumnHeight+5 };
SetConsoleScreenBufferSize(GetStdHandle(STD_OUTPUT_HANDLE), bufferSize);
SetConsoleWindowSize(ColumnWidth - 2, ColumnHeight + 5);
std::string command = "mode con cols=" + std::to_string(ColumnWidth - 1) + " lines=" + std::to_string( ColumnHeight + 6);
system(command.c_str());

//LONG_PTR currentExStyle = GetWindowLongPtr(GetConsoleWindow(), GWL_EXSTYLE);
//SetWindowLongPtr(GetConsoleWindow(), GWL_EXSTYLE, currentExStyle | WS_EX_TOOLWINDOW);
		HideSide();
		SetConsoleWindowPosition(LeftColumnStartLocationX, LeftColumnStartLocationY);
		SetWindowDisplayAffinity(GetConsoleWindow(), WDA_MONITOR);
		//bool ctrlPressed = false;
		//int pressCount = 0;
		//const int requiredPresses = 3;
		//const int waitTime = 3000;
		//for (int i = 0; i < 3000; i += 100) {
		//	if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
		//		pressCount++;
		//		if (pressCount >= 3) {
		//			break; // 达到3次按下，退出循环
		//		}
		//	}
		//	Sleep(100); // 休眠100毫秒，减少CPU占用
		//}
		//// 根据按下次数决定是否显示控制台窗口
		//if (pressCount >= requiredPresses) {
		//	ShowWindow();
		//}
		//else {
		//	KMesErr("指定的触发方式未能完成，程序正在退出");
		//}

		findrc();
		//system("pause");
		system("cls");
		if (sos) {
			for (int i = 1; i <= (ColumnWidth - 14) / 2 -1; i++) {
				cprint("=", 7, 4);
			}
			cprint("安全模式已启动", 7, 4);
			for (int i = 1; i <= (ColumnWidth - 14) / 2 -1; i++) {
				cprint("=", 7, 4);
			}
			for (int i = 1; i <= (ColumnWidth - 22) / 2 - 1; i++) {
				cprint("=", 7, 4);
			}
			cprint("退出前务必关闭所有程序", 7, 4);
			for (int i = 1; i <= (ColumnWidth - 22) / 2 - 1; i++) {
				cprint("=", 7, 4);
			}cout << endl;
			RunCmdAsyn("osk.exe");
			cout << endl;
		
		}

	KswordPrintLogo();

	//打开子进程===========================================================================
	if (sos)KswordSelfProc1(999);
	else KswordSelfProc1(KMainRunTime);
	if(fileExists(localadd+"kbackground.bmp"))
	KswordSubProcess7=KswordSelfProcess7(KMainRunTime);
	KswordSubProcess2 = KswordSelfProc2(KMainRunTime);

	//建立与子进程的管道=================================================================
	if (sos)KswordSelfPipe1(999);
	else KswordSelfPipe1(KMainRunTime);
	KswordSend1("测试信息：与子进程1成功建立连接");
	cprint("当前进程PID", 1, 0);
	cout << GetCurrentProcessId() << endl;

	const std::string pipeName = "\\\\.\\pipe\\Ksword_Pipe_1_"+to_string(KMainRunTime);
	char buffer[256];
	DWORD bytesRead;
	HANDLE hPipe = nullptr;

	ReadFile(
		hPipe,
		buffer,
		sizeof(buffer) - 1,
		&bytesRead,
		NULL
	);
	if (!strcmp(buffer, "sub1")) {
		KMesErr("与子进程通信异常：收到不符合预期的信息");
	}
	else {
		KMesInfo("与子进程1成功建立连接");
	}
	KMesWarn("如果你是第一次使用，请输入help查询帮助，并阅读README。");
	SetForegroundWindow(GetConsoleWindow());
	IsDebug = EnableDebugPrivilege(TRUE);
	//IsDebug = HasDebugPrivilege();
	//进入Ksword Shell主函数===================================================================================
	shell();
	//_CrtDumpMemoryLeaks();
	return KSWORD_SUCCESS_EXIT;
}
int shell() {
shellstart:
	//信息前缀输出=====================================================
	std::cout << endl;
	KEnviProb();
	cout << "┌ ";
	cout << "[Ksword " << KSWORD_MAIN_VER_A << "." << KSWORD_MAIN_VER_B << "]";
	if (KswordPipeMode)cprint(">Pipe", 6, 0);
	cout << "~[";
	if (!strcmp(AuthName.c_str(), "SYSTEM"))cprint("System", 4, 0);
	else cout << AuthName;
	cout << '@' << HostName;
	if (KswordPipeMode)Sleep(10);
	cout << "]";
	if (IsAuthAdmin)cprint("[Admin]", 2 ,0);
	if (IsDebug)cprint("[Debug]", 6, 0);
	cprint("[R3]", 8, 0);
	if (IsTop)cprint("[Top]", 4, 0);
	//cout << "[" << CPUUsage() << "|" << RAMUsage() << "]" << endl;
	std::cout << endl;
	cout << "└ ";
	//cout << 1;
	string result;
	bool inConsecutive = false; // 标记是否处于连续反斜杠序列中
	for (size_t i = 0; i < path.length(); ++i) {
		// 检查当前字符是否为反斜杠
		if (path[i] == '\\') {
			if (inConsecutive)continue;
			result += '\\';
			inConsecutive = true;
		}
		else {
			result += path[i];
			inConsecutive = false;
		}
	}
	//cout << 2;
	path = result;
	string tmpcmd;
	if (path[0] == 'c') {
		//这里是为了处理傻逼的cmd兼容问题，
		// 这里通过调用cmd执行一次切换目录
		// +打印目录的方式更新，但是如果是
		// 根目录的话该命令会报错，因为cmd使用
		// ```d:```来切换盘符。因此这里直接输出单层路径。 
		tmpcmd = "cd " + path + " && cd";
	}
	else {
		tmpcmd += path[0];
		tmpcmd += ": && cd " + path + " && cd";
	}
	//cout << 3;
	const string& refpath = tmpcmd;
	if (path.length() > 3) {
		string pathtmp=GetCmdResult(tmpcmd);
		if (!pathtmp.empty() && pathtmp.back() == '\n')
			pathtmp.pop_back();
		cout << pathtmp;
	}
	
	else cout << path;
	cout << ">";
	
	nowcmd = "";
	cmdparanum = 0;
	for (int i = 1; i <= MAX_PARA_NUMBER; i++) {
		cmdpara[i - 1] = "";
	}


	//获取用户输入=================================
	//cout << cmdtodonum << cmdtodonumpointer;
	if(cmdtodonum<=cmdtodonumpointer){
		for (int i = 0; i < cmdtodonum; i++) {
			cmdtodo[i] = "";
		}
		cmdtodonum = 0;
		cmdtodonumpointer = 0;
		//cin.ignore(numeric_limits<std::streamsize>::max(), '\n');
		//getline(cin, nowcmd);
		//判断是否按下三次~按键
		nowcmd = Kgetline();
		//getline(cin, nowcmd);
		KswordSend1("Ksword主程序：命令输入" + nowcmd);
		//请在ksword.cpp中搜索return CallNextHookEx(NULL, nCode, wParam, lParam);并且注释掉来防止钩子向下传递
		//cout << endl;

		std::string CmdDealresult = nowcmd;
		size_t pos = 0;

		// 找到第一个不是反引号的字符的位置
		while (pos < CmdDealresult.length() && CmdDealresult[pos] == '`') {
			++pos;
		}

		// 如果字符串以反引号开头，截取字符串
		if (pos > 0) {
			CmdDealresult = CmdDealresult.substr(pos);
		}


		stringstream ss(CmdDealresult);
		string token;

		while (getline(ss, token, '`')) {
			size_t first = token.find_first_not_of(' ');
			if (string::npos != first) {
				size_t last = token.find_last_not_of(' ');
				cmdtodo[cmdtodonum++] = token.substr(first, (last - first + 1));
			}
		}
		//for (int i = 0; i < cmdtodonum; i++) {
		//	cout << "a[" << i << "] = " << cmdtodo[i] << endl;
		//}
	}
	//cout << cmdtodonum << cmdtodonumpointer;
		nowcmd = cmdtodo[cmdtodonumpointer];
		cmdtodonumpointer++;
	
	cout << nowcmd << endl;
	int wordCount = 0;
	int spaceCount = 0;

	std::string word;
	size_t start = 0;
	size_t end = 0;

	// 遍历字符串，按空格分割单词
	while ((end = nowcmd.find(' ', start)) != std::string::npos && wordCount < MAX_PARA_NUMBER - 1) {
		//cout << "cmdparanum=" << cmdparanum;
		word = nowcmd.substr(start, end - start);
		if (!word.empty()) {
			cmdpara[wordCount++] = word;
		}
		start = end + 1;
		spaceCount++;
	}

	// 添加最后一个单词
	if (wordCount < MAX_PARA_NUMBER) {
		cmdpara[wordCount++] = nowcmd.substr(start);
	}
	//cout << "cmdparanum=" << cmdparanum;

	// 计算空格数量，最后一个单词后不应该有空格
	spaceCount = nowcmd.length() > 0 && nowcmd.back() != ' ' ? spaceCount : spaceCount - 1;

	nowcmd = cmdpara[0];
	cmdparanum = spaceCount;

	//for (int i = 1; i <= 10; i++) {
	//	cout << i << "	" << cmdpara[i];
	//}
	//cout << "cmdparanum=" << cmdparanum;
	SetWindowText(GetConsoleWindow(), L"Ksword主程序");
	string title="Ksword主程序";
	title += path;
	title += " > ";
	title += nowcmd;
	SetWindowText(GetConsoleWindow(), CharToWChar(title.c_str()));
	size_t spacePos = nowcmd.find(' ');
	//cout << "nowcmd=" << nowcmd<<endl;

	if (nowcmd == "cmd") {
		system("c:\\windows\\system32\\cmd.exe");
	}
	else if (nowcmd == "taskmgr") {
		system("c:\\windows\\system32\\taskmgr.exe");
	}
	else if (nowcmd == "help") {
		KswordMainHelp();
	}
	else if (nowcmd == "refresh") {
		KMainRefresh();
		KswordSend1("refresh");
		system("cls");
		KswordPrintLogo();
		KMesInfo("已刷新窗口布局");
	}
	else if (nowcmd == "sos") {

		STARTUPINFO si = { sizeof(si) };
		PROCESS_INFORMATION pi;
		wchar_t tmp[5] = L"998";
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
	else if (nowcmd == "time") {
		time_t timep;
		time(&timep);
		printf("%s", ctime(&timep));
	}
	else if (nowcmd == "whereami") {
		cout << path << endl;
	}
	else if (nowcmd == "lock") {
		PasswordVariety();
		SetConsoleWindowSize(ColumnWidth, ColumnHeight);
		SetConsoleWindowPosition(LeftColumnStartLocationX, LeftColumnStartLocationY);
		SetWindowKStyle();
		goto shellstart;
	}
	else if (nowcmd == "cd")
	{
		if (cmdpara[1] == "") {//没有命令参数，只输入了cd，那么进行处理，回到根目录 
			KInlineForCommandcd1();
			goto shellstart;
		}
		if (cmdpara[1] == "..") {//cd ..，回到上一个目录 
			KInlineForCommandcd2();
			goto shellstart;
		}
		const std::string& constpath = cmdpara[1];
		if (cmdpara[1][cmdpara[1].length() - 1] != '\\')cmdpara[1] += '\\';
		if (directoryExists(constpath))
		{
			path = cmdpara[1];
			goto shellstart;
		}//进入下一级或者多层目录，先检验目录是否存在，然后修改全局变量 
		else {
			string tmppath = path;
			tmppath += cmdpara[1];
			const std::string& constpath = tmppath;
			if (directoryExists(constpath)) { path = tmppath; goto shellstart; }
		}
	}
	else if (nowcmd == "cd..") {//兼容cd..，实际和cd ..没有任何区别。 
		KInlineForCommandcd3();
	}
	else if ((nowcmd.length() == 2) && (nowcmd[1] == ':')) {
		path = nowcmd + "\\";
	}//比如a:,b:,c:等等，切换盘符。应该没有哪个大聪明来个1:吧？ 
	else if (nowcmd=="ocp") {
		cout << "请输入指定文件前缀：";
		//thread occupy(GetFileOccupyInformation, Kgetline());
		//if (WaitForSingleObject(occupy.native_handle(), 2000) == WAIT_TIMEOUT) {
		//	//TerminateThread(occupy.native_handle(), -1);
		//	KMesErr("调用超时，已自动结束任务。输出的信息可能不完整。");
		//}
		GetFileOccupyInformation(Kgetline());
		goto shellstart;
	}
	else if (nowcmd == "untop")
	{
		ThreadTopWorking = 0;
		Sleep(100);
		UnTopWindow();

		// 获取窗口的扩展样式
		LONG style = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);

		// 检查是否设置了WS_EX_TOPMOST标志
		if (style & WS_EX_TOPMOST) {
			std::cout << "控制台窗口被置顶。" << std::endl;
		}
		else {
			std::cout << "控制台窗口没有被置顶。" << std::endl;
		}
	}
	else if (nowcmd == "cls")
	{
		system("cls");
		goto shellstart;
	}
	else if (nowcmd == "wintop")
	{
		SetTopWindow();
		ThreadTopWorking = 1;
		goto shellstart;
	}
	else if (nowcmd == "avkill") {
	avkill();
	goto shellstart;
	}
	else if (nowcmd == "tasklist") {
		if (cmdparanum == 0) {
			SetWindowNormal();
			Ktasklist();
		}
		else {
			for (int i = 1; i <= cmdparanum; i++) {
				Ktasklist(cmdpara[i]);
			}
		}
		goto shellstart;
	}
	else if (nowcmd == "getsys") {
		if (!IsAdmin()) {
			if (RequestAdmin(StringToWString(GetSelfPath())) == KSWORD_ERROR_EXIT) {
				KMesErr("出现错误，无法启动");
				goto shellstart;
			}
			else {
				MainExit();
				exit(0);
			}
		}
		GetProgramPath();
		if (GetSystem(to_string(KMainRunTime + 1).c_str()) == KSWORD_SUCCESS_EXIT) {
			MainExit();
			return KSWORD_SUCCESS_EXIT;
		}
		goto shellstart;
	}
	else if (nowcmd == "apt") {
		if (cmdpara[1] == "update") {
			KInlineForCommandapt1();
		}
		else if (cmdpara[1] == "upgrade") {
			KInlineForCommandapt2();
		}
		else if (cmdpara[1]=="install") {
			string path; path = localadd + "version.txt";
			KMesInfo("开始更新...");
			readFile(path);
			string pkgname;
			pkgname = cmdpara[2];
			for (int i = 1; i <= rcnumber; i++) {
				//cprint("[ * ]", 9); cout << aptinfo[i].filename << aptinfo[i].year << aptinfo[i].month << aptinfo[i].day << aptinfo[i].filesize << endl;
				if (pkgname == aptinfo[i].filename) {
					string installcmd;
					installcmd += "curl -o " + localadd + aptinfo[i].filename + " " + "http://" + serverip + ":80/kswordrc/" + aptinfo[i].filename;
					cout<<installcmd;
					//Sleep(500);
					cout<<endl<<"Recources found." <<to_string(aptinfo[i].filesize) <<"MB disk space will be used.Continue ? (y / n)";

					string yon = Kgetline();
					if (yon[0] == 'n')break;
					if (yon[0] == 'y') {
						RunCmdNow(installcmd.c_str());
						goto shellstart;
					}
				}
			}
			KMesErr("找不到指定的资源文件。");
		}
	}
	else if (nowcmd == "setrcpath") {
		KInlineForCommandapt3();
		goto shellstart;
	}
	else if (nowcmd == "ai") {
		cout << "请输入API KEY："; string API_KEY = Kgetline();
		cout << "请输入请求问题（不支持上下文）："; string post = Kgetline();
		KimiAPI(post, API_KEY);
		goto shellstart;
	}
	else if (nowcmd == "inputmode") {
		cout << "选择你的输入方式：" << endl <<
			"0>钩子获取输入（推荐，较快）" << endl <<
			"1>硬件获取输入（安全，较卡）" << endl <<
			"2>正常获取输入（无法后台输入）" << endl <<
			">";
		KgetlineMode = StringToInt(Kgetline());
		goto shellstart;
	}
	else if (nowcmd == "")
	{
		goto shellstart;
	}
	else if (nowcmd == "asyn") {

	string cmdcmd;
	//if (path != "") {
	//	if (path.length() <= 3) {
	//		if (path[0] == 'c' || path[0] == 'C') {
	//			cmdcmd = "cd\\ && ";
	//			for (int i = 1; i <= cmdparanum; i++) {
	//				cmdcmd += cmdpara[i];
	//				cmdcmd += " ";
	//			}
	//		}
	//		else {
	//			cmdcmd = path[0];
	//			cmdcmd += ": && ";
	//			for (int i = 1; i <= cmdparanum; i++) {
	//				cmdcmd += cmdpara[i];
	//				cmdcmd += " ";
	//			}
	//		}
	//		cout << "Running command:" << cmdcmd << endl;
	//	}
	//	else {
	//		if (path[0] == 'c') {
	//			cmdcmd = "cd " + path + " && ";
	//			for (int i = 1; i <= cmdparanum; i++) {
	//				cmdcmd += cmdpara[i];
	//				cmdcmd += " ";
	//			}
	//		}
	//		else {
	//			cmdcmd += path[0];
	//			cmdcmd += ": && cd \"" + path;
	//			cmdcmd += "\" && ";
	//			for (int i = 1; i <= cmdparanum; i++) {
	//				cmdcmd += cmdpara[i];
	//				cmdcmd += " ";
	//			}
	//		}
	//		cout << "Running command:" << cmdcmd << endl;
	//	}
		//const char* cstr = cmdcmd.c_str();
				for (int i = 1; i <= cmdparanum; i++) {
				cmdcmd += cmdpara[i];
				cmdcmd += " ";
			}
		RunCmdAsyn(cmdcmd);
	//}
	goto shellstart;

	}
	else if (nowcmd == "sethc") {
		string takeOwnerShipCommand;
		takeOwnerShipCommand = "c:\\windows\\sy" + string("stem32\\se") + "thc.exe";
		TakeOwnership(takeOwnerShipCommand.c_str());
		::DeleteFileW(CharToWChar(takeOwnerShipCommand.c_str()));
		//ReleaseResourceToFile("EXE", IDR_EXE1, "c:\\windows\\system32\\sethc.exe");
		string tmpcmd;
		tmpcmd = "copy \""; tmpcmd += GetSelfPath().c_str(); tmpcmd += "\" "+takeOwnerShipCommand+" / Y";
		cout << tmpcmd << endl;
		RunCmdNow(tmpcmd);
		//string a = "\\sethc_runner.b";
		//string b = "at";
		//string c = ReturnCWS() + a + b;
		//HANDLE hFile = CreateFileA(c.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		//if (hFile == INVALID_HANDLE_VALUE) {
			//std::cerr << "无法创建文件！" << std::endl;
		//}
		//else {
			//cout << "成功写入目标文件" << endl;
		//}
		// 写入内容到文件
		//DWORD dwBytesWritten;
		//if (!WriteFile(hFile, "sethc_ks.exe", strlen("sethc_ks.exe"), &dwBytesWritten, NULL)) {
		//	std::cerr << "写入文件失败！" << std::endl;
		//	CloseHandle(hFile);
		//}
		//else {
		//	cout << "成功复制到%syspath%sethc_ks.exe" << endl;
		//}
		//const string rcName = "1";
		//HMODULE hModule = GetModuleHandle(NULL);
		//HRSRC hResource = FindResource(hModule, MAKEINTRESOURCE(1), RT_MANIFEST);
		//if (hResource) {
		//	HGLOBAL hLoadedResource = LoadResource(hModule, hResource);
		//	if (hLoadedResource) {
		//		void* pResourceData = LockResource(hLoadedResource);
		//		DWORD dwResourceSize = SizeofResource(hModule, hResource);

		//		std::ofstream outFile("c:\\windows\\system32\\sethc_ks.exe.1.manifest", std::ios::binary);
		//		if (outFile) {
		//			outFile.write(static_cast<char*>(pResourceData), dwResourceSize);
		//			outFile.close();
		//			std::cout << "Manifest file extracted successfully." << std::endl;
		//		}
		//		else {
		//			std::cerr << "Failed to open output file." << std::endl;
		//		}
		//	}
		//	else {
		//		std::cerr << "Failed to load resource." << std::endl;
		//	}
		//}
		//else {
		//	std::cerr << "Resource not found." << std::endl;
		//}
		//// 关闭文件句柄
		//CloseHandle(hFile);


		goto shellstart;
	}
	else if (nowcmd == "drivermgr") {
		drivermgr();
		goto shellstart;
	}
	else if (nowcmd == "scr") {
		string scriptcmd = "";
		if (localadd[0] != 'c') {
			scriptcmd += localadd[0];
			scriptcmd += ": && ";
		}
		scriptcmd += "cd ";
		scriptcmd += localadd;
		scriptcmd += " && ";
		for (int i = 1; i <= cmdparanum; i++) {
			scriptcmd += cmdpara[i];
			scriptcmd += " ";
		}
		cout << scriptcmd << endl;
		RunCmdNow(scriptcmd);
	}
	else if (nowcmd == "exit") {
		MainExit();
		exit(0);
	}
	else if (nowcmd == "guimgr") {
		KswordGUIManagerInline1();
		goto shellstart;
	}
	else if (nowcmd == "netmgr") {
		KswordNetManager();
		goto shellstart;
	}
	else if (nowcmd == "procmgr"){
		KswordProcManager();
		goto shellstart;
	}
	else if (nowcmd == "threadmgr") {
		KswordThreadMgr();
		goto shellstart;
	}
	else if (nowcmd == "selfmgr") {
		KswordSelfManager();
		goto shellstart;
	}
	else if (nowcmd == "topmost") {
		KGetTopMost("top");
		KswordMainPipe();
		//KGetTopMost("topsock");
		//KswordMainSockPipe();
		RedirectCoutToPipe();
		KswordPipeMode = 1;
		IsTop = 1;
		goto shellstart;
	}
	else if (nowcmd == "nt") {
		string ntcmd = "";
		for (int i = 1; i <= cmdparanum; i++) {
			ntcmd += cmdpara[i]; ntcmd += " ";
		}cout << "内核命令：" << ntcmd << endl;
		KswordDriverCommand(ntcmd);
		goto shellstart;
	}
	else
	{
		string cmdcmd;
		if (path != "") {
			if (path.length() <= 3) {
				if (path[0] == 'c'|| path[0]=='C') {
					cmdcmd = "cd\\ && " + nowcmd + ' ';
					for (int i = 1; i <= cmdparanum; i++) {
						cmdcmd += cmdpara[i];
						cmdcmd += " ";
					}
				}
				else {
					cmdcmd = path[0];
					cmdcmd += ": && ";
					cmdcmd += nowcmd + ' ';
					for (int i = 1; i <= cmdparanum; i++) {
						cmdcmd += cmdpara[i];
						cmdcmd += " ";
					}
				}
				cout << "Running command:" << cmdcmd << endl;
			}
			else {
				if (path[0] == 'c') {
					cmdcmd = "cd " + path + " && " + nowcmd + ' ';
					for (int i = 1; i <= cmdparanum; i++) {
						cmdcmd += cmdpara[i];
						cmdcmd += " ";
					}
				}
				else {
					cmdcmd += path[0];
					cmdcmd += ": && cd \"" + path;
					cmdcmd += "\" && ";
					cmdcmd += nowcmd + ' ';
					for (int i = 1; i <= cmdparanum; i++) {
						cmdcmd += cmdpara[i];
						cmdcmd += " ";
					}
				}
				cout << "Running command:" << cmdcmd << endl;
			}
			const char* cstr = cmdcmd.c_str();
			RunCmdNow(cstr);
		}
	}
	goto shellstart;
}
#endif