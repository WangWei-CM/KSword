#ifdef KSWORD_WITH_COMMAND
#include "../KswordTotalHead.h"
#define PW_VARIETY_WINDOW_WIDTH 30
#define PW_VARIETY_WINDOW_HEIGHT 3
using namespace std;
int PasswordVariety() {
	system("cls");
	//system("pause");
	DWORD exStyle = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);
	exStyle |= WS_EX_TRANSPARENT;
	SetWindowLong(GetConsoleWindow(), GWL_EXSTYLE, exStyle);
	//system("pause");
	SetTopWindow();
	//system("pause");
	SetAir(180);
	//system("pause");
	//HideSide();
	//system("pause");
	//Kpause();
	int WindowStartLocationX = ScreenX / 2 - fontWidth * PW_VARIETY_WINDOW_WIDTH / 2;
	int WindowStartLocationY = ScreenY / 2 - fontHeight * PW_VARIETY_WINDOW_HEIGHT / 2;
	SetConsoleWindowPosition(WindowStartLocationX, WindowStartLocationY); 
	std::string command = "mode con cols=" +
		std::to_string(PW_VARIETY_WINDOW_WIDTH) +
		" lines=" + std::to_string(PW_VARIETY_WINDOW_HEIGHT);
	system(command.c_str());
	//SetConsoleWindowSize(PW_VARIETY_WINDOW_WIDTH, PW_VARIETY_WINDOW_HEIGHT);
	HideSide();
	SMALL_RECT windowRect = { 0, 0, PW_VARIETY_WINDOW_WIDTH - 1, PW_VARIETY_WINDOW_HEIGHT - 1 };
	SetConsoleWindowInfo(GetStdHandle(STD_OUTPUT_HANDLE), TRUE, &windowRect);
	cout << "==";
	cprint("Password Variety", 0, 15, 1);
	for (int i = 1; i <= 30 - 18; i++)
		cout << "=";
	cout << endl << ">" << endl;
	for (int i = 1; i <= PW_VARIETY_WINDOW_WIDTH; i++)
		cout << "=";
	SetCursor(1, 1);
	HideCursor();
#ifndef KSWORD_DEVER_MODE
	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 0x00);
#endif
	string InputPassword = Kgetline();
	ShowCursor();
	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_INTENSITY | 7);

#ifndef KSWORD_DEVER_MODE
	if (LMD5(InputPassword) == "b0723e94d7eadf24ba42e7e58e5ee083") {
		KgetlineClean();
		SetConsoleWindowSize(ColumnWidth-2, ColumnHeight + 5);
		return KSWORD_SUCCESS_EXIT; 
	}
#else
	if (LMD5(InputPassword) == "e916c3ba1faaa13505ac16ba78d2a63d") {
		KgetlineClean();
		SetConsoleWindowSize(ColumnWidth-2, ColumnHeight + 5);
		return KSWORD_SUCCESS_EXIT; 
	}
#endif // !KWORD_DEVER_MODE
	else exit(1);
}
#endif