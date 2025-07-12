#ifdef KSWORD_WITH_COMMAND
#include "..\KswordTotalHead.h"
using namespace std::chrono;
using namespace std;

FileInfo aptinfo[50];
version kswordversion;

//ȫ�ֱ���================================================================

//��Դ���
int rcnumber;//���ڵ���Դ����
string serverip = "159.75";
string localadd;//������Դ���·��
//����ִ�е�����
string nowcmd;
string cmdpara[MAX_PARA_NUMBER];
int cmdparanum;
string cmdtodo[50];
int cmdtodonum;
int cmdtodonumpointer;
//�߳��ź�
bool ThreadTopWorking = 0;
int ctrlCount = 0;
bool windowsshow = 1;//�����Ƿ���ʾ
int TopThreadID;
//��������
std::string path = "C:\\Windows\\system32";//�ն�������·��



// ȫ�ֱ��������ڸ��ٰ�������
int dotKeyCount = 0;

//����ͨѶ
HANDLE Ksword_Pipe_1;
HANDLE Ksword_Main_Pipe=INVALID_HANDLE_VALUE;
HANDLE Ksword_Main_Sock_Pipe;

//��������

int findrc();//���ұ�����Դ���·��
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
		KMesErr("�������ļ�ʱʧ��");
		return KSWORD_ERROR_EXIT;
	}
	string poo;
	getline(file, poo);
	file >> kswordversion.a >> kswordversion.b;
	file >> ws;
	if ((kswordversion.a > KSWORD_MAIN_VER_A) || ((kswordversion.a == KSWORD_MAIN_VER_A) && (kswordversion.b > KSWORD_MAIN_VER_B))) {
		KMesErr("�ó��������°汾�����������˳���");
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
	KMesInfo("���²���б����");
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
	KMesErr("�Ҳ������ܵ���Դ��ַ�����Ժ��ֶ�����");
	localadd = "c:\\";
	return KSWORD_ERROR_EXIT;
}




// ��ȡ��ǰ���ھ�����ö���Ϊ���㴰�ڵĺ���
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
	result.reserve(len); // Ԥ�����ڴ����������

	bool inEscape = false; // ����Ƿ���ת��������
	for (size_t i = 0; i < len; ++i) {
		if (cmdpara[i] == '\\') {
			if (!inEscape) { // �����ǰ����ת�������У������һ����б��
				result += '\\';
				inEscape = true; // �������Ǵ���ת��������
			}
		}
		else {
			result += cmdpara[i]; // �����ͨ�ַ�
			inEscape = false; // �뿪ת������
		}
	}
	cmdpara = result; // �ô������ַ�������ԭ�ַ���
}

// ���̹��ӵĻص�����``
LRESULT CALLBACK LowLevelKeyboardProcInput(int nCode, WPARAM wParam, LPARAM lParam) {
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

	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

void keyboardListener() {
	//using namespace std::chrono;
	//milliseconds keyPressInterval(2000); // ����2��ļ����
	//milliseconds keyRepeatInterval(10); // ����100ms�İ��������
	//steady_clock::time_point lastKeyPressTime = steady_clock::now();
	//while (1) {
	//	if (GetAsyncKeyState(VK_RCONTROL) & 0x8000) { // ���Ctrl���Ƿ񱻰���
	//		auto currentTime = steady_clock::now();
	//		if (currentTime - lastKeyPressTime < keyPressInterval) {
	//			ctrlCount++; // ���Ӱ�������
	//		}
	//		else {
	//			ctrlCount = 0; // �������2�룬���ð�������
	//		}
	//		lastKeyPressTime = currentTime; // ���������ʱ��
	//		while (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
	//			std::this_thread::sleep_for(milliseconds(50)); // �ȴ�Ctrl���ͷţ�����CPUռ��
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
	//					// ���ڲ�����С����
	//				}
	//				windowsshow = true;
	//			}
	//			ctrlCount = 0; // ���ð�������
	//		}
	//	}
	//	std::this_thread::sleep_for(keyRepeatInterval); // ÿ100ms���һ��
	//}
	using namespace std::chrono;
	milliseconds keyRepeatInterval(50); // ����100ms�İ��������
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
					// ���ڲ�����С����
				}
				windowsshow = true;
			}
		std::this_thread::sleep_for(keyContinue);
		}std::this_thread::sleep_for(keyRepeatInterval); // ÿ100ms���һ��
	}
}

// �ȼ� ID
const int HOTKEY_ID = 1;

// �ȼ������̺߳���
DWORD WINAPI HotkeyThread(LPVOID lpParam) {
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_HOTKEY) {
            // ����Ƿ��� Ctrl+G �ȼ�
            if (msg.wParam == MOD_CONTROL && msg.lParam == 'G') {
                // ��ȡ����̨���ھ��
                HWND hWnd = GetConsoleWindow();
                if (hWnd) {
                    // �Ƴ� WS_EX_TRANSPARENT ��ʽ
                    LONG_PTR exStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
                    exStyle &= ~WS_EX_TRANSPARENT;
                    SetWindowLongPtr(hWnd, GWL_EXSTYLE, exStyle);

                    // ���´����Է�ӳ�µ���ʽ
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
		cprint("��", 15, 1);
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
	milliseconds keyRepeatInterval(150); // ����100ms�İ��������
	while (1) {
		if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
			MainExit(); // �˳�����
			exit(0);
		}
		std::this_thread::sleep_for(keyRepeatInterval); // ÿ100ms���һ��
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
//        this_thread::sleep_for(seconds(3)); // �ȴ�3����
//        // ���ü�����
//        ctrlCount = 0;
//    }
//}
#endif