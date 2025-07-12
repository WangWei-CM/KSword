#ifdef KSWORD_WITH_COMMAND
#pragma once
#include "../../KswordTotalHead.h"
using namespace std;

extern std::vector<std::pair<HWND, std::string>> g_windows;
// 回调函数，用于枚举窗口
int damn(int);
extern BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam);
BOOL CALLBACK EnumWindowsProcOnScreen(HWND hwnd, LPARAM lParam);
BOOL CALLBACK EnumWindowsProcOnScreenAndReturn(HWND hwnd, LPARAM lParam);
// 查找与PID关联的所有窗口
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
		std::cout << "选择你要操作的对象（PID）（输入self以对自己操作，输入tasklist以查看当前的进程快照,输入scr以遍历屏幕上的窗口，输入exit以退出: ";
		string userinput = Kgetline();
		//cout << "userinput.length()=" << userinput.length();
		if (userinput == "self") {
				cout << "请选择对应的操作：输入对应的数字" << endl <<
					"1>普通置顶" << endl <<
					"2>超级置顶" << endl <<
					"3>恢复正常控制台样式" << endl <<
					"4>恢复Ksword样式" << endl <<
					"5>统一显示所有窗口" << endl <<
					"6>设置此窗口透明度" << endl;
				int taskmethod = StringToInt(Kgetline());
			
			if (taskmethod < 1 || taskmethod>6) {
				KMesErr("不合法的窗口操作方式请求"); return;
			}
			else {
				if (taskmethod == 6) {
						cout << "请输入目标透明度（透明0~255不透明）>" << endl;
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
			KMesWarn("已自动忽略explorer.exe所有窗口");
			KEnumWindowOnDesktopNum = 0;
			cout << "请输入目标窗口序号:>";
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
			cout << "自动锁定PID为" << pid << "的进程" << endl;
		}
		else
			pid = StringToInt(userinput);

		FindWindowsByPID(pid);

		if (g_windows.empty()) {
			std::cout << "该进程不存在或没有窗口" << std::endl; 
			return;
		}
		if (!kswordGUIManagerCmdinput)
		{
			std::cout << "找到以下窗口：" << pid << ":" << std::endl;
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
		//	std::cout << "这是你想隐藏的窗口吗？如果是，输入1，否则输入0:";
		//	tmp=StringToInt(Kgetline());
		//	if (tmp == 1) {
		//		std::cout << "你隐藏的窗口是：" << i + 1 << ". " << g_windows[i].second << std::endl;
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
				cout << "请选择你想要操作的窗口编号，输入exit以退出；";
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
				cout << endl << "请选择对应的操作：输入对应的数字" << endl <<
					"1>隐藏窗口" << endl <<
					"2>显示窗口" << endl <<
					"3>置顶窗口" << endl <<
					"4>取消置顶" << endl <<
					"5>请求关闭" << endl <<
					"6>设置透明度（透明0~256不透明）" << endl <<
					"7>结束进程" << endl <<
					"8>持续隐藏" << endl;
					int taskmethod = StringToInt(Kgetline());
					//cout << "操作符为" << taskmethod;
				
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
					cout << "请求可能不成功，如果不成功请尝试直接结束进程" << endl;
				}
				else if (taskmethod == 6) {int
					kswordSelfAir = StringToInt(Kgetline());
					SetWindowLong(hwnd, GWL_EXSTYLE,
						GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
					// 设置窗口透明度
					SetLayeredWindowAttributes(hwnd, 0, kswordSelfAir, LWA_ALPHA);
				}
				else if (taskmethod == 7) {
#ifndef _M_IX86
					if (EndTask(hwnd, TRUE, TRUE)) {
						std::cout << "窗口已成功结束。" << std::endl;
					}
					else {
						DWORD dwError = GetLastError();
						std::cerr << "EndTask 失败，错误代码：" << dwError << std::endl;
					}
#endif
#ifdef _M_IX86
					KMesErr("endtask不适用于32位版本。");
#endif
				}
				else if (taskmethod == 8) {

					int tid =
						damn(pid);
					
		
							thread hideWindowThread(ToggleWindowVisibility,hwnd,tid);
								cout << "分离单个线程，线程清单编号为" << tid << endl;
				}
				else {
					KMesErr("未定义的操作方式");
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