
#ifdef KSWORD_WITH_COMMAND
#ifndef Ksword_Main_1

#define Ksword_Main_1

#include "..\Main\KswordTotalHead.h"
#pragma comment(lib, "wintrust.lib")
#include <softpub.h>  // 补充数字签名相关的 GUID 定义

#include <wintrust.h>

const std::unordered_set<std::wstring> kSystemWhitelist = {
	L"ntdll.dll",        L"kernel32.dll",    L"kernelbase.dll",
	L"user32.dll",       L"win32u.dll",      L"gdi32.dll",
	L"gdi32full.dll",    L"msvcp_win.dll",   L"ucrtbase.dll",
	L"crypt32.dll",      L"msasn1.dll",      L"ws2_32.dll",
	L"rpcrt4.dll",       L"advapi32.dll",    L"msvcrt.dll",
	L"sechost.dll",      L"shell32.dll",     L"cfgmgr32.dll",
	L"shcore.dll",       L"combase.dll",     L"bcryptprimitives.dll",
	L"windows.storage.dll", L"profapi.dll",   L"powrprof.dll",
	L"umpdc.dll",        L"shlwapi.dll",     L"kernel.appcore.dll",
	L"cryptsp.dll",      L"wintrust.dll",	L".exe",
	L"iphlpapi.dll",	L"uxtheme.dll",	L"bcrypt.dll",
	L"imm32.dll",	L"apphelp.dll",		L"sspicli.dll"
};

// 路径标准化处理（关键函数）
std::wstring NormalizeDllPath(const std::wstring& rawPath) {
	std::wstring path = rawPath;

	// 替换相对路径标识符
	if (path.find(L"..\\") == 0) {
		path = L"C:\\Windows\\system32\\" + path.substr(3);
	}

	// 统一为小写
	std::transform(path.begin(), path.end(), path.begin(), ::towlower);

	// 提取纯文件名
	size_t pos = path.find_last_of(L"\\/");
	if (pos != std::wstring::npos) {
		return path.substr(pos + 1);
	}
	return path;
}
bool selfEnumed;
std::vector<std::wstring> GetSuspiciousDlls(DWORD pid) {
	std::vector<std::wstring> suspiciousDlls;
	HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
	if (!hProcess) return suspiciousDlls;

	HMODULE hModules[1024];
	DWORD cbNeeded;
	if (EnumProcessModules(hProcess, hModules, sizeof(hModules), &cbNeeded)) {
		const DWORD moduleCount = cbNeeded / sizeof(HMODULE);
		for (DWORD i = 0; i < moduleCount; ++i) {
			wchar_t modulePath[MAX_PATH];
			if (GetModuleFileNameEx(hProcess, hModules[i], modulePath, MAX_PATH)) {
				std::wstring normalized = NormalizeDllPath(modulePath);
				if (!selfEnumed) {
					selfEnumed = 1;
					continue;
				}
				if (kSystemWhitelist.find(normalized) == kSystemWhitelist.end()) {
					suspiciousDlls.push_back(modulePath);
				}
			}
		}
	}
	CloseHandle(hProcess);
	return suspiciousDlls;
}


bool KReceiverContinue = 1;
bool IsKMain1Mini = 0;
int showSecond = 0;
bool shouldShowWindow = 0;
void KswordMain1ShowManager() {
	while (IsKMain1Mini) {
		if (showSecond > 0) {
			--showSecond;  // 减去 1
			std::this_thread::sleep_for(std::chrono::seconds(1));  // 等待 1 秒
		}
		if (showSecond == 0) {
			HideWindow();
			shouldShowWindow = 1;
		}
		else if (showSecond > 0) {
			if (shouldShowWindow) {
				ShowWindow();
				shouldShowWindow = 0;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}


void KswordMain1SetMini() {
	int WindowX = ScreenX - 20 * fontWidth - K_DEFAULT_SPACE - 50;
	int WindowY = ScreenY - GetSystemMetrics(SM_CYCAPTION) - K_DEFAULT_SPACE - 3 * fontHeight;
	SetConsoleWindowPosition(WindowX, WindowY);
	SetConsoleWindowSize(20, 3);
}

void KswordMain1SetMax() {
	SetConsoleWindowPosition(RightColumnStartLocationX, RightColumnStartLocationY);
	SetConsoleWindowSize(ColumnWidth, ColumnHeight);
}
int KswordMain1(char* argv) {

	AllocConsole();
	//SetConsoleOutputCP(CP_UTF8);
	//SetConsola();
	//system("color 97");
	DWORD exStyle = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);
	exStyle |= WS_EX_TRANSPARENT;
	SetWindowLong(GetConsoleWindow(), GWL_EXSTYLE, exStyle);
	SetWindowPos(GetConsoleWindow(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
	SetAir(200);
	HideSide();
	system(string("mode con cols=" + to_string(ColumnWidth-3) + " lines=" + to_string(ColumnHeight+1)).c_str());
	Sleep(50);
	SetConsoleWindowPosition(RightColumnStartLocationX, RightColumnStartLocationY);
	SetConsoleWindowSize(ColumnWidth, ColumnHeight-2);
	//HideSide();
		//COORD bufferSize = { ColumnWidth-3, ColumnHeight };
	//SetConsoleScreenBufferSize(GetStdHandle(STD_OUTPUT_HANDLE), bufferSize);
	LONG_PTR currentExStyle = GetWindowLongPtr(GetConsoleWindow(), GWL_EXSTYLE);
	SetWindowLongPtr(GetConsoleWindow(), GWL_EXSTYLE, currentExStyle | WS_EX_TOOLWINDOW);
	std::thread listenerThread(keyboardListener);
	KMesInfo("子进程1成功启动");
	HANDLE hProcess = GetCurrentProcess();
	HMODULE hModules[1024];
	DWORD cbNeeded;
	DWORD pid = GetCurrentProcessId();

	// 检测可疑DLL
	auto suspicious = GetSuspiciousDlls(pid);
	// 输出结果
	if (!suspicious.empty()) {
		KMesWarn("发现可疑DLL，运行环境可能不稳定：");
		for (const auto& dll : suspicious) {
			cprint("DLL:", 1, 0);
			std::cout << "• " << WstringToString(dll) << std::endl;
		}
	}
	else {
		std:cout << "未检测到异常DLL" << std::endl;
	}

	CloseHandle(hProcess);
	const std::string pipeName = "\\\\.\\pipe\\Ksword_Pipe_1_"+string(argv);
	SetWindowText(GetConsoleWindow(), L"Ksword通讯主程序");
	char buffer[256];
	DWORD bytesRead;
	HANDLE hPipe;
	//Sleep(500);
	SetTopWindow();
	// 打开命名管道
	if (strcmp(argv, "999") == 0) {
		for (int i = 1; i <= (ColumnWidth - 14) / 2; i++) {
			cprint("=", 7, 4);
		}
		cprint("安全模式已启动",7,4);
		for (int i = 1; i <= (ColumnWidth - 14) / 2; i++) {
			cprint("=", 7, 4);
		}
		cout << endl;
		hPipe = CreateFile(
			CharToWChar("\\\\.\\pipe\\Ksword_Pipe_SOS"),
			GENERIC_READ,
			0,
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			NULL
		);
	}
	else {
		hPipe = CreateFile(
			CharToWChar(pipeName.c_str()),
			GENERIC_READ,
			0,
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			NULL
		);
	}
	KswordSend1("sub1");
	if (hPipe == INVALID_HANDLE_VALUE) {
		KMesErr("无法打开通道，程序即将退出");
		exit(1);
	}
	if (!SetTopWindow()) {
		KMesErr("置顶失败");
	}
	// 读取消息
	while (KReceiverContinue) {
		std::string line;
		while (true) {
			char ch;
			BOOL success = ReadFile(
				hPipe,
				&ch,
				1, // 每次只读取一个字节
				&bytesRead,
				NULL
			);

			if (!success || bytesRead == 0) {
				KMesErr("无法从管道中获取输入");
				CloseHandle(hPipe);
				exit(1);
			}

			if (ch == '\n'||ch=='`') { // 如果读取到换行符，停止读取
				break;
			}

			line += ch; // 将字符追加到字符串中
		}
		buffer[bytesRead] = '\0'; // 确保字符串以空字符结尾

		KPrintTime();
		std::cout << line<< std::endl;
		if(!strcmp(line.c_str(), "refresh")) {
			KMain1Refresh();
		}
		else if (!strcmp(line.c_str(), "show")) {
			//IsKMain1Mini = 0;
			ShowWindow();
		}
		else if (!strcmp(line.c_str(), "hide")) {
			//cout << "出发判断";
			//KswordMain1SetMini();
			//thread t(KswordMain1ShowManager);
			//IsKMain1Mini = 1;
			HideWindow();
		}
		else {
			KReceiverContinue = 1;
		}
	}
	// 关闭管道
	CloseHandle(hPipe);
	FreeConsole();
	return 0;
}



void KMain1Refresh() {
	SetAir(200);
	HideSide();
	SetConsoleWindowPosition(RightColumnStartLocationX, RightColumnStartLocationY);
	SetConsoleWindowSize(ColumnWidth, ColumnHeight);
}

#endif
#endif