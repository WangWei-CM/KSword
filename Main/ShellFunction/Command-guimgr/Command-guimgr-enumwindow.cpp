#ifdef KSWORD_WITH_COMMAND
#include "../../KswordTotalHead.h"
using namespace std;
std::vector<std::pair<HWND, std::string>> g_windows;
int KEnumWindowOnDesktopNum;
int KEnumWindowOnDesktopTask;
int damn(int pid) {
	return KswordRegThread("����" + to_string(pid) + "���ظ������߳�");
}
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
	DWORD processId = 0;
	GetWindowThreadProcessId(hwnd, &processId);
	if (processId == (DWORD)lParam) {
		char windowTitle[256];
		GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle));
		g_windows.push_back(std::make_pair(hwnd, std::string(windowTitle)));
	}
	return TRUE;
}

BOOL CALLBACK EnumWindowsProcOnScreen(HWND hwnd, LPARAM lParam) {
    // ��ȡ���ڱ���
    KEnumWindowOnDesktopNum++;
    const int titleSize = 256;
    WCHAR title[titleSize];
    GetWindowText(hwnd, title, titleSize);
    string a=WCharToString(title);
    // ��鴰���Ƿ�ɼ�
    if (IsWindowVisible(hwnd)) {
        DWORD pid;
        GetWindowThreadProcessId(hwnd,&pid);
        string ImName = GetProcessNameByPID(pid);
        if (ImName.size() >= 12 && ImName.compare(0, 12, "explorer.exe") != 0) {
            cout << KEnumWindowOnDesktopNum << "\t";
            cprint("Proc:", 1, 0);
            cout << ImName /*<< ImName << ImName*/ << "(" << pid << ")\t";
            cprint("Title:", 2, 0); cout << a << endl;
        }
    }
    return TRUE; // ����TRUE�Լ�������
}
void ToggleWindowVisibility(HWND hwnd, int TID);

BOOL CALLBACK EnumWindowsProcOnScreenAndReturn(HWND hwnd, LPARAM lParam) {
    // ��ȡ���ڱ���
    KEnumWindowOnDesktopNum++;
        DWORD pid;
        GetWindowThreadProcessId(hwnd,&pid);
        if (KEnumWindowOnDesktopNum == KEnumWindowOnDesktopTask) {
			cout << endl << "��ѡ���Ӧ�Ĳ����������Ӧ������" << endl <<
				"1>���ش���" << endl <<
				"2>��ʾ����" << endl <<
				"3>�ö�����" << endl <<
				"4>ȡ���ö�" << endl <<
				"5>����ر�" << endl <<
				"6>����͸���ȣ�͸��0~256��͸����" << endl <<
				"7>��������" << endl <<
				"8>��������" << endl;
					int taskmethod = StringToInt(Kgetline());
					//cout << "������Ϊ" << taskmethod;
				
				if (taskmethod == 1) {
					ShowWindow(hwnd, SW_HIDE);
				}
				else if (taskmethod == 2) {
					ShowWindow(hwnd, SW_SHOW);
				}
				else if (taskmethod == 3) {
					SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 100, 100, SWP_NOMOVE | SWP_NOSIZE);
				}
				else if (taskmethod == 4) {
					SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 100, 100, SWP_NOMOVE | SWP_NOSIZE);
				}
				else if (taskmethod == 5) {
					SendMessage(hwnd, WM_CLOSE, 0, 0);
					cout << "������ܲ��ɹ���������ɹ��볢��ֱ�ӽ�������" << endl;
				}
				else if (taskmethod == 6) {int
					kswordSelfAir = StringToInt(Kgetline());
					SetWindowLong(hwnd, GWL_EXSTYLE,
						GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
					// ���ô���͸����
					SetLayeredWindowAttributes(hwnd, 0, kswordSelfAir, LWA_ALPHA);
				}
				else if (taskmethod == 7) {
#ifndef _M_IX86



					if (EndTask(hwnd, TRUE, TRUE)) {
						std::cout << "�����ѳɹ�������" << std::endl;
					}
					else {
						DWORD dwError = GetLastError();
						std::cerr << "EndTask ʧ�ܣ�������룺" << dwError << std::endl;
					}
#endif
#ifdef _M_IX86
					KMesErr("endtask��������32λ�汾��");
#endif
				}
				else if (taskmethod == 8) {
							
							int tid = KswordRegThread("����" + to_string(pid) + "���ظ������߳�");

							thread hideWindowThread(ToggleWindowVisibility,hwnd,tid);
								cout << "���뵥���̣߳��߳��嵥���Ϊ" << tid << endl;
				}
				else {
					KMesErr("δ����Ĳ�����ʽ");
				}
			return FALSE;
        }
    return TRUE; // ����TRUE�Լ�������
}
void ToggleWindowVisibility(HWND hwnd, int TID) {
    if (!hwnd) {
        std::cerr << "Invalid window handle." << std::endl;
        return;
    }
    while (!KswordThreadStop[TID]) {
        // ���ش���
        ShowWindow(hwnd, SW_HIDE);
        Sleep(100); // �ȴ� 100 ����
    }
}
#endif