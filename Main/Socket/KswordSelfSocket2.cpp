#ifdef KSWORD_WITH_COMMAND
#include "../KswordTotalHead.h"
using namespace std;
HANDLE KswordSelfProc2(int Runtime) {

	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags |= STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_SHOW; // 确保新窗口可见
	string tmp1 = GetSelfPath();
	wchar_t a[5] = L"1 ";
	swprintf(a, sizeof(a) / sizeof(wchar_t), L"%d %d", 2, Runtime);
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

	return pi.hProcess;
	CloseHandle(pi.hThread);
}
#endif