#ifdef KSWORD_WITH_COMMAND
#include "..\Main\KswordTotalHead.h"
using namespace std;





int LeftColumnStartLocationX = K_DEFAULT_SPACE;
int LeftColumnStartLocationY = K_DEFAULT_SPACE;
int RightColumnStartLocationX = 0;
int RightColumnStartLocationY = K_DEFAULT_SPACE;
int ColumnWidth = 0;
int ColumnHeight = 0;
int fontWidth;
int fontHeight;
int MainWindow2StartLocationY;
void CalcWindowStyle() {
	KEnviProb();

	// 获取当前控制台的输出句柄
	HANDLE hConsoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	// 获取控制台的字体信息
	CONSOLE_FONT_INFOEX cfi = { 0 };
	cfi.cbSize = sizeof(cfi);
	GetCurrentConsoleFontEx(hConsoleOutput, FALSE, &cfi);
	fontWidth = cfi.dwFontSize.Y * 34 / 72;
	fontHeight = cfi.dwFontSize.Y;
	RightColumnStartLocationX = ScreenX / 2 + 10;
	ColumnWidth = (ScreenX / 2 - 20) / FONT_SIZE_WIDTH;
	ColumnHeight = (ScreenY * 3 / 4) / FONT_SIZE_HEIGHT;
	MainWindow2StartLocationY = LeftColumnStartLocationY + ColumnHeight * FONT_SIZE_HEIGHT + K_DEFAULT_SPACE;
	return;
}

void SetWindowNormal() {
	HWND hwnd = GetConsoleWindow();
	if (hwnd == NULL) {
		std::cerr << "无法获取控制台窗口句柄" << std::endl;
		return;
	}
	// 获取当前窗口的样式
	DWORD style = GetWindowLong(hwnd, GWL_STYLE);
	DWORD exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
	// 确保窗口有标题栏和边框
	style |= WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
	// 确保窗口不是点击穿透的
	exStyle &= ~WS_EX_LAYERED;
	//exStyle &= ~WS_EX_TRANSPARENT;
	// 应用新的样式
	SetWindowLong(hwnd, GWL_STYLE, style);
	SetWindowLong(hwnd, GWL_EXSTYLE, exStyle);
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);

	// 设置新的屏幕缓冲区大小
	COORD newBufferSize = { 80,800 };
	if (!SetConsoleScreenBufferSize(hStdOut, newBufferSize)) {
		std::cerr << "设置屏幕缓冲区大小失败，错误码: " << GetLastError() << std::endl;
		return;
	}

	// 获取控制台屏幕缓冲区的信息
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	if (!GetConsoleScreenBufferInfo(hStdOut, &csbi)) {
		std::cerr << "获取控制台屏幕缓冲区信息失败，错误码: " << GetLastError() << std::endl;
		return;
	}

	// 设置新的窗口大小，确保窗口大小不超过屏幕缓冲区大小
	//SMALL_RECT windowRect = { 0, 0, static_cast<SHORT>(80 - 1), static_cast<SHORT>(csbi.dwSize.Y - 1) };
	//if (!SetConsoleWindowInfo(hStdOut, TRUE, &windowRect)) {
	//	std::cerr << "设置窗口大小失败，错误码: " << GetLastError() << std::endl;
	//	return;
	//}


	// 刷新窗口
	SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
	SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}
void SetWindowKStyle() {
	SetWindowLong(GetConsoleWindow(), GWL_EXSTYLE,
		GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE) | WS_EX_LAYERED);

	// 设置指定颜色为透明色
	SetLayeredWindowAttributes(GetConsoleWindow(), RGB(0, 0, 0), 0, LWA_COLORKEY);
	DWORD exStyle = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);

	// 添加点击穿透样式
	exStyle |= WS_EX_TRANSPARENT;

	// 应用新的扩展样式
	SetWindowLong(GetConsoleWindow(), GWL_EXSTYLE, exStyle);

	// 更新窗口以应用更改
	SetWindowPos(GetConsoleWindow(), nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
	SetTopWindow();
	SetConsola();
	SetAir(200);
	//HideSide();
	SetConsoleWindowPosition(LeftColumnStartLocationX, LeftColumnStartLocationY);
	SetConsoleWindowSize(ColumnWidth, ColumnHeight);
	system("cls");
}

#endif