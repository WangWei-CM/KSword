#ifdef KSWORD_WITH_COMMAND
#include "..\Main\KswordTotalHead.h"
#define _WIN32_WINNT 0x0A00  // 或更高版本，例如 0x0A00 表示 Windows 10
using namespace std;

#include <windows.h>
#include <stdio.h>
#include <VersionHelpers.h>

bool wideConsole;

// 获取当前控制台光标位置的x坐标
int GetCursorX() {
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
	return csbi.dwCursorPosition.X;
}

// 检测字符▉的宽度（1或2）
int DetectBlockCharWidth() {
	// 记录初始位置
	int x1 = GetCursorX();

	// 输出测试字符
	std::cout << "▉";

	// 获取新位置
	int x2 = GetCursorX();

	// 计算差值
	int width = x2 - x1;

	// 如果输出导致换行，需要特殊处理
	if (width < 0) {
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
		width = csbi.dwSize.X - x1 + x2;
	}
	for (int i = 1; i <= width; i++) {
		cout << '\b';
	}
	return width;
}
//int 
//__cdecl
//wwmain(
//    __in int argc, 
//    __in_ecount(argc) PCWSTR argv[]
//    )
//{
//    UNREFERENCED_PARAMETER(argc);
//    UNREFERENCED_PARAMETER(argv);
//
//    if (IsWindowsXPOrGreater())
//    {
//        printf("XPOrGreater\n");
//    }
//
//    if (IsWindowsXPSP1OrGreater())
//    {
//        printf("XPSP1OrGreater\n");
//    }
//
//    if (IsWindowsXPSP2OrGreater())
//    {
//        printf("XPSP2OrGreater\n");
//    }
//
//    if (IsWindowsXPSP3OrGreater())
//    {
//        printf("XPSP3OrGreater\n");
//    }
//
//    if (IsWindowsVistaOrGreater())
//    {
//        printf("VistaOrGreater\n");
//    }
//
//    if (IsWindowsVistaSP1OrGreater())
//    {
//        printf("VistaSP1OrGreater\n");
//    }
//
//    if (IsWindowsVistaSP2OrGreater())
//    {
//        printf("VistaSP2OrGreater\n");
//    }
//
//    if (IsWindows7OrGreater())
//    {
//        printf("Windows7OrGreater\n");
//    }
//
//    if (IsWindows7SP1OrGreater())
//    {
//        printf("Windows7SP1OrGreater\n");
//    }
//
//    if (IsWindows8OrGreater())
//    {
//        printf("Windows8OrGreater\n");
//    }
//
//    if (IsWindows8Point1OrGreater())
//    {
//        printf("Windows8Point1OrGreater\n");
//    }
//
//    if (IsWindows10OrGreater())
//    {
//        printf("Windows10OrGreater\n");
//    }
//
//    if (IsWindowsServer())
//    {
//        printf("Server\n");
//    }
//    else
//    {
//        printf("Client\n");
//    }
//}
// 获取CPU核心数
int GetCPUCores() {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return sysInfo.dwNumberOfProcessors;
}

// 获取系统内存信息
std::string GetSystemMemory() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    unsigned long long totalMemory = memInfo.ullTotalPhys / (1024 * 1024); // 转换为MB
    return std::to_string(totalMemory) + " MB";
}

// 获取系统架构信息
std::string GetSystemArchitecture() {
    SYSTEM_INFO sysInfo;
    GetNativeSystemInfo(&sysInfo);
    switch (sysInfo.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_INTEL:
            return "x86";
        case PROCESSOR_ARCHITECTURE_AMD64:
            return "x64";
        case PROCESSOR_ARCHITECTURE_ARM:
            return "ARM";
        case PROCESSOR_ARCHITECTURE_ARM64:
            return "ARM64";
        default:
            return "Unknown";
    }
}

void cprint(int s, int color)
{
	HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
	SetConsoleTextAttribute(handle, FOREGROUND_INTENSITY | color);
	cout << s;
	SetConsoleTextAttribute(handle, FOREGROUND_INTENSITY | 7);
}
void printcpu(int c, int m) {
	if (DetectBlockCharWidth() == 1)wideConsole = 0; else wideConsole = 1;
	SetCursor(0,0);
    for (int i = 1; i <= ColumnWidth; i++)cout << "  ";
    SetCursor(0,0);
	int num = c * (ColumnWidth - 4) / 200;
	if (c < 10) {
		cout << "CPU"; cprint(c, 47);
	}
	else if (c <= 70) {
		cout << "CP"; cprint(c, 47);
	}
	else if (c <= 90) {
		cout << "CP"; cprint(c, 109);
	}
	else if (c <= 99) {
		cout << "CP"; cprint(c, 71);
	}
	else if (c >= 100) {
		cout << "C"; cprint(100, 71);
	}
	for (int i = 1; i <= num; i++) {
		if (wideConsole)
			cout << "▉"; else cout << "▉ ";
	}

	cout << endl;
	int numb = m * (ColumnWidth - 4) / 200;
	if (m < 10) {
		cout << "RAM"; cprint(m, 47);
	}
	else if (m <= 70) {
		cout << "RA"; cprint(m, 47);
	}
	else if (m <= 90) {
		cout << "RA"; cprint(m, 109);
	}
	else if (m <= 99) {
		cout << "RA"; cprint(m, 71);
	}
	else if (m == 100) {
		cout << "R"; cprint(m, 71);
	}
    for (int i = 1; i <= numb; i++) {
		if (wideConsole)
			cout << "▉"; else cout << "▉ ";
    }cout << endl;
    cprint("系统版本号", 1, 0);
    std::cout << "1. "; KWinVer();
     cprint("处理器核心", 1, 0);
    std::cout << "2. CPU Cores: " << GetCPUCores() << std::endl;
     cprint("内存总容量", 1, 0);
    std::cout << "3. Total Memory: " << GetSystemMemory() << std::endl;
     cprint("系统  架构", 1, 0);
    std::cout << "4. System Architecture: " << GetSystemArchitecture();
}


int GetCPUAndRAM()
{
	printcpu(CPUUsage(), RAMUsage());
	return 0;
}


int KswordMain2() {
	AllocConsole();
	//SetConsoleOutputCP(CP_UTF8);
	//SetConsola();
	//system("color 97");
	DWORD exStyle = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);
	// 添加点击穿透样式
	exStyle |= WS_EX_TRANSPARENT;
	// 应用新的扩展样式
	SetWindowLong(GetConsoleWindow(), GWL_EXSTYLE, exStyle);
	// 更新窗口以应用更改
	SetWindowPos(GetConsoleWindow(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
	SetAir(200);
	HideSide();
	Sleep(101);
	SetConsoleWindowPosition(RightColumnStartLocationX, MainWindow2StartLocationY);
	SetConsoleWindowSize(ColumnWidth, 6);
	//mode con cols=80 lines=25
	system(string("mode con cols=" + to_string(ColumnWidth) + " lines=" + to_string(6)).c_str());
	thread ListenerThread(keyboardListener);
	SetTopWindow();
	while (1) {
		//Sleep(500);
		int ret = 0;
		ret = GetCPUAndRAM();
	}
	return 0;
}


#endif