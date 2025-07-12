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

	// ��ȡ��ǰ����̨��������
	HANDLE hConsoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	// ��ȡ����̨��������Ϣ
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
		std::cerr << "�޷���ȡ����̨���ھ��" << std::endl;
		return;
	}
	// ��ȡ��ǰ���ڵ���ʽ
	DWORD style = GetWindowLong(hwnd, GWL_STYLE);
	DWORD exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
	// ȷ�������б������ͱ߿�
	style |= WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
	// ȷ�����ڲ��ǵ����͸��
	exStyle &= ~WS_EX_LAYERED;
	//exStyle &= ~WS_EX_TRANSPARENT;
	// Ӧ���µ���ʽ
	SetWindowLong(hwnd, GWL_STYLE, style);
	SetWindowLong(hwnd, GWL_EXSTYLE, exStyle);
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);

	// �����µ���Ļ��������С
	COORD newBufferSize = { 80,800 };
	if (!SetConsoleScreenBufferSize(hStdOut, newBufferSize)) {
		std::cerr << "������Ļ��������Сʧ�ܣ�������: " << GetLastError() << std::endl;
		return;
	}

	// ��ȡ����̨��Ļ����������Ϣ
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	if (!GetConsoleScreenBufferInfo(hStdOut, &csbi)) {
		std::cerr << "��ȡ����̨��Ļ��������Ϣʧ�ܣ�������: " << GetLastError() << std::endl;
		return;
	}

	// �����µĴ��ڴ�С��ȷ�����ڴ�С��������Ļ��������С
	//SMALL_RECT windowRect = { 0, 0, static_cast<SHORT>(80 - 1), static_cast<SHORT>(csbi.dwSize.Y - 1) };
	//if (!SetConsoleWindowInfo(hStdOut, TRUE, &windowRect)) {
	//	std::cerr << "���ô��ڴ�Сʧ�ܣ�������: " << GetLastError() << std::endl;
	//	return;
	//}


	// ˢ�´���
	SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
	SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}
void SetWindowKStyle() {
	SetWindowLong(GetConsoleWindow(), GWL_EXSTYLE,
		GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE) | WS_EX_LAYERED);

	// ����ָ����ɫΪ͸��ɫ
	SetLayeredWindowAttributes(GetConsoleWindow(), RGB(0, 0, 0), 0, LWA_COLORKEY);
	DWORD exStyle = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);

	// ��ӵ����͸��ʽ
	exStyle |= WS_EX_TRANSPARENT;

	// Ӧ���µ���չ��ʽ
	SetWindowLong(GetConsoleWindow(), GWL_EXSTYLE, exStyle);

	// ���´�����Ӧ�ø���
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