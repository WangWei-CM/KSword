#ifdef KSWORD_WITH_COMMAND
#include "../KswordTotalHead.h"
using namespace std;

extern HANDLE Ksword_Pipe_1;
bool KswordPipeMode;

int KswordSelfProc1(int Runtime) {
	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags |= STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_SHOW; // 确保新窗口可见
	si.dwFlags = STARTF_USECOUNTCHARS | STARTF_USESHOWWINDOW |STARTF_USEPOSITION ;
    si.dwX = RightColumnStartLocationX;       // 窗口的 X 坐标
    si.dwY = RightColumnStartLocationY;       // 窗口的 Y 坐标
    si.dwXCountChars = ColumnWidth;           // 窗口的宽度（字符数）
    si.dwYCountChars = ColumnHeight+7;          // 窗口的高度（字符数）
    si.wShowWindow = SW_SHOW;                 // 显示窗口
	string tmp1 = GetSelfPath();
	wchar_t a[5] = L"1 ";
	wchar_t b[5] = L"999";
	if (Runtime == 999) {
		swprintf(a, sizeof(a) / sizeof(wchar_t), L"%d %d", 1, Runtime);
		// 创建新进程
		if (!CreateProcess(
			CharToWChar(tmp1.c_str()), // 模块名（可执行文件名）
			//CharToWChar("C:\\Users\\33251\\Desktop\\ksword\\test\\systemtest.exe"),
			b, // 命令行
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
			return 1;
		}

		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
	else {
		swprintf(a, sizeof(a) / sizeof(wchar_t), L"%d %d", 1, Runtime);
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
			return 1;
		}

		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
}

int KswordSelfPipe1(int Runtime) {
	if (Runtime == 999) {
		const std::string pipeName = "\\\\.\\pipe\\Ksword_Pipe_SOS";
		// 打开命名管道
		Ksword_Pipe_1 = CreateNamedPipe(
			CharToWChar(pipeName.c_str()),
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			1,
			256, // 输出缓冲区大小
			256, // 输入缓冲区大小
			0,   // 使用默认超时
			NULL // 默认安全属性
		);

		if (Ksword_Pipe_1 == INVALID_HANDLE_VALUE) {
			KMesErr("无法与子进程取得联系，打开管道失败");
			return 1;
		}
		// 连接到管道
		ConnectNamedPipe(Ksword_Pipe_1, NULL);
		return KSWORD_SUCCESS_EXIT;

	}
	else {
		const std::string pipeName = "\\\\.\\pipe\\Ksword_Pipe_1_" + to_string(Runtime);
		// 打开命名管道
		Ksword_Pipe_1 = CreateNamedPipe(
			CharToWChar(pipeName.c_str()),
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			1,
			256, // 输出缓冲区大小
			256, // 输入缓冲区大小
			0,   // 使用默认超时
			NULL // 默认安全属性
		);

		if (Ksword_Pipe_1 == INVALID_HANDLE_VALUE) {
			KMesErr("无法与子进程取得联系，打开管道失败");
			return 1;
		}
		// 连接到管道
		ConnectNamedPipe(Ksword_Pipe_1, NULL);
		return KSWORD_SUCCESS_EXIT;
	}
}


int KswordMainPipe() {
	const std::string pipeName = "\\\\.\\pipe\\Ksword_Main_Pipe";
	// 打开命名管道
	Ksword_Main_Pipe = CreateNamedPipe(
		CharToWChar(pipeName.c_str()),
		PIPE_ACCESS_DUPLEX,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		1,
		256, // 输出缓冲区大小
		256, // 输入缓冲区大小
		0,   // 使用默认超时
		NULL // 默认安全属性
	);

	if (Ksword_Main_Pipe == INVALID_HANDLE_VALUE) {
		KMesErr("无法与子进程取得联系，打开管道失败");
		return 1;
	}
	// 连接到管道
	ConnectNamedPipe(Ksword_Main_Pipe, NULL);
	return KSWORD_SUCCESS_EXIT;
}
int KswordMainSockPipe() {
	const std::string pipeName = "\\\\.\\pipe\\Ksword_Main_Sock_Pipe";
	// 打开命名管道
	Ksword_Main_Pipe = CreateNamedPipe(
		CharToWChar(pipeName.c_str()),
		PIPE_ACCESS_DUPLEX,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		1,
		256, // 输出缓冲区大小
		256, // 输入缓冲区大小
		0,   // 使用默认超时
		NULL // 默认安全属性
	);

	if (Ksword_Main_Pipe == INVALID_HANDLE_VALUE) {
		KMesErr("无法与子进程取得联系，打开管道失败");
		return 1;
	}
	// 连接到管道
	ConnectNamedPipe(Ksword_Main_Sock_Pipe, NULL);
	return KSWORD_SUCCESS_EXIT;
}


int KswordSend1(std::string Message) {
	Message += "`";
	DWORD bytesWritten=0;
	if (KswordPipeMode
		) {
	return WriteFile(
		Ksword_Main_Sock_Pipe,
		Message.c_str(),
		static_cast<DWORD>(Message.size()),
		&bytesWritten,
		NULL
	);
	}else
	return WriteFile(
		Ksword_Pipe_1,
		Message.c_str(),
		static_cast<DWORD>(Message.size()),
		&bytesWritten,
		NULL
	);
}


int KswordSendMain(std::string Message) {
	DWORD bytesWritten=0;
	return WriteFile(
		Ksword_Main_Pipe,
		Message.c_str(),
		static_cast<DWORD>(Message.size()),
		&bytesWritten,
		NULL
	);
}
#endif