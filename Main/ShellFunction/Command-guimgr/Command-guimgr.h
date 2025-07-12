#ifdef KSWORD_WITH_COMMAND
#pragma once
#include "../../KswordTotalHead.h"
using namespace std;

extern std::vector<std::pair<HWND, std::string>> g_windows;
// �ص�����������ö�ٴ���
int damn(int);
extern BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam);
BOOL CALLBACK EnumWindowsProcOnScreen(HWND hwnd, LPARAM lParam);
BOOL CALLBACK EnumWindowsProcOnScreenAndReturn(HWND hwnd, LPARAM lParam);
// ������PID���������д���
inline void FindWindowsByPID(DWORD pid) {
	g_windows.clear();
	EnumWindows(EnumWindowsProc, (LPARAM)pid);
}
extern int KEnumWindowOnDesktopNum;
extern int KEnumWindowOnDesktopTask;
inline void KswordGUIManagerInline1() {
	GUIManagerStart:
static bool kswordGUIManagerCmdinput = 0;
void ToggleWindowVisibility(HWND hwnd, int TID);
	//if (cmdparanum != 0) {
	//	kswordGUIManagerCmdinput = 1;
	//	for (int i = 1; i <= cmdparanum; i++) {
	//		if (cmdpara[i] == "-pid" || cmdpara[i] == "/pid") {
	//			if (i + 1 <= cmdparanum) {
	//				userinput = cmdpara[i + 1];
	//			}
	//		}
	//		else if (cmdpara[i] == "-air" || cmdpara[i] == "/air") {
	//			kswordSelfAir = StringToInt(cmdpara[i+1]);
	//			taskmethod = 6;
	//		}
	//		else if (cmdpara[i] == "-self" || cmdpara[i] == "/self") {
	//			userinput = "self";
	//			if (i + 1 <= cmdparanum) {
	//				if (cmdpara[i+1] == "-normal")taskmethod = 3;
	//				else if (cmdpara[i+1] == "-kstyle")taskmethod = 4;
	//				else if (cmdpara[i+1] == "-top" || cmdpara[i+1] == "/top")taskmethod = 2;
	//				else if (cmdpara[i+1] == "-air" || cmdpara[i+1] == "/air") {
	//					kswordSelfAir = StringToInt(cmdpara[i+2]); taskmethod = 6;
	//				}
	//				else { Kpause(); }
	//			}
	//		}
	//		else if (cmdpara[i] == "-top" || cmdpara[i] == "/top") {
	//			taskmethod = 3;
	//		}
	//		else if (cmdpara[i] == "-untop" || cmdpara[i] == "/untop") {
	//			taskmethod = 4;
	//		}
	//		else if (cmdpara[i] == "-show" || cmdpara[i] == "/show") {
	//			taskmethod = 2;
	//		}else if (cmdpara[i] == "-hide" || cmdpara[i] == "/hide") {
	//			taskmethod = 1;
	//		}
	//		else if (cmdpara[i] == "-kill" || cmdpara[i] == "/kill"||cmdpara[i] == "-end" || cmdpara[i] == "/end") {
	//			taskmethod = 7;
	//		}
	//	}
	//}
	bool WindowStyleChanged = 0;
	DWORD pid;
	g_windows.clear();
		std::cout << "ѡ����Ҫ�����Ķ���PID��������self�Զ��Լ�����������tasklist�Բ鿴��ǰ�Ľ��̿���,����scr�Ա�����Ļ�ϵĴ��ڣ�����exit���˳�: ";
		string userinput = Kgetline();
		//cout << "userinput.length()=" << userinput.length();
		if (userinput == "self") {
				cout << "��ѡ���Ӧ�Ĳ����������Ӧ������" << endl <<
					"1>��ͨ�ö�" << endl <<
					"2>�����ö�" << endl <<
					"3>�ָ���������̨��ʽ" << endl <<
					"4>�ָ�Ksword��ʽ" << endl <<
					"5>ͳһ��ʾ���д���" << endl <<
					"6>���ô˴���͸����" << endl;
				int taskmethod = StringToInt(Kgetline());
			
			if (taskmethod < 1 || taskmethod>6) {
				KMesErr("���Ϸ��Ĵ��ڲ�����ʽ����"); return;
			}
			else {
				if (taskmethod == 6) {
						cout << "������Ŀ��͸���ȣ�͸��0~255��͸����>" << endl;
						int kswordSelfAir = StringToInt(Kgetline());
					SetAir(kswordSelfAir);
				}
				else if (taskmethod == 5) {
					ShowWindow();
					KswordSend1("show");
				}
				else if (taskmethod == 4) {
					SetWindowKStyle();
				}
				else if (taskmethod == 3) {
					SetWindowNormal();
				}
				else if (taskmethod == 2) {
					SetTopWindow();
					ThreadTopWorking = 1;
				}
				else if (taskmethod == 1) {
					SetTopWindow();
				}
			}
			return;
		}
		else if (userinput == "scr") {
			EnumWindows(EnumWindowsProcOnScreen, 0);
			KMesWarn("���Զ�����explorer.exe���д���");
			KEnumWindowOnDesktopNum = 0;
			cout << "������Ŀ�괰�����:>";
			KEnumWindowOnDesktopTask = StringToInt(Kgetline());
			KEnumWindowOnDesktopNum = 0;
			EnumWindows(EnumWindowsProcOnScreenAndReturn, 0);
			KEnumWindowOnDesktopNum = 0;
			goto GUIManagerStart;
		}
		else if (userinput == "tasklist") {
			SetWindowNormal();
			Ktasklist();
			goto GUIManagerStart;
		}
		else if (userinput == "exit") {
			return;
		}
		else if (userinput.length() == 1) {
			Ktasklist(userinput);
			goto GUIManagerStart;
		}
		if (!IsInt(userinput) && userinput.length() != 1) {
			pid = GetPIDByIM(userinput);
			cout << "�Զ�����PIDΪ" << pid << "�Ľ���" << endl;
		}
		else
			pid = StringToInt(userinput);

		FindWindowsByPID(pid);

		if (g_windows.empty()) {
			std::cout << "�ý��̲����ڻ�û�д���" << std::endl; 
			return;
		}
		if (!kswordGUIManagerCmdinput)
		{
			std::cout << "�ҵ����´��ڣ�" << pid << ":" << std::endl;
			if (g_windows.size() > ColumnHeight) {
				WindowStyleChanged = 1;
				SetWindowNormal();
			}
			for (size_t i = 0; i < g_windows.size(); ++i) {
				cprint(to_string(i + 1), 1, 0);
				std::cout << "\t" << g_windows[i].second << std::endl;
			}
		}

		//for (size_t i = 0; i < g_windows.size(); ++i) {
		//	HWND hwnd = g_windows[i - 1].first;
		//	ShowWindow(hwnd, SW_HIDE);
		//	int tmp;
		//	std::cout << "�����������صĴ���������ǣ�����1����������0:";
		//	tmp=StringToInt(Kgetline());
		//	if (tmp == 1) {
		//		std::cout << "�����صĴ����ǣ�" << i + 1 << ". " << g_windows[i].second << std::endl;
		//		break;
		//	}
		//	else {
		//		ShowWindow(hwnd, SW_SHOW);
		//	}
		//}

		while (1) {
			string userinput1;
			if (!kswordGUIManagerCmdinput)
			{
				cout << "��ѡ������Ҫ�����Ĵ��ڱ�ţ�����exit���˳���";
				userinput1 = Kgetline();
			}
			else {
				userinput1 = "1";
			}
			if (userinput1 == "exit") {
					break;
			}
			else {
				int taskpid = StringToInt(userinput1);
				HWND hwnd = g_windows[taskpid - 1].first;
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

					int tid =
						damn(pid);
					
		
							thread hideWindowThread(ToggleWindowVisibility,hwnd,tid);
								cout << "���뵥���̣߳��߳��嵥���Ϊ" << tid << endl;
				}
				else {
					KMesErr("δ����Ĳ�����ʽ");
				}
			}
			if (!kswordGUIManagerCmdinput)
			{
				return;
			}
		}
		//if (WindowStyleChanged) {
		//	SetWindowKStyle();
		//}
		std::cout << "Window changed successfully." << std::endl << "===================================================" << std::endl;
	return;
}
#endif