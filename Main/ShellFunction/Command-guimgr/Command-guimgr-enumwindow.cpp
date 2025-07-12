#ifdef KSWORD_WITH_COMMAND
#include "../../KswordTotalHead.h"
using namespace std;
std::vector<std::pair<HWND, std::string>> g_windows;
int KEnumWindowOnDesktopNum;
int KEnumWindowOnDesktopTask;
int damn(int pid) {
	return KswordRegThread("进程" + to_string(pid) + "的重复隐藏线程");
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
    // 获取窗口标题
    KEnumWindowOnDesktopNum++;
    const int titleSize = 256;
    WCHAR title[titleSize];
    GetWindowText(hwnd, title, titleSize);
    string a=WCharToString(title);
    // 检查窗口是否可见
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
    return TRUE; // 返回TRUE以继续遍历
}
void ToggleWindowVisibility(HWND hwnd, int TID);

BOOL CALLBACK EnumWindowsProcOnScreenAndReturn(HWND hwnd, LPARAM lParam) {
    // 获取窗口标题
    KEnumWindowOnDesktopNum++;
        DWORD pid;
        GetWindowThreadProcessId(hwnd,&pid);
        if (KEnumWindowOnDesktopNum == KEnumWindowOnDesktopTask) {
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
							
							int tid = KswordRegThread("进程" + to_string(pid) + "的重复隐藏线程");

							thread hideWindowThread(ToggleWindowVisibility,hwnd,tid);
								cout << "分离单个线程，线程清单编号为" << tid << endl;
				}
				else {
					KMesErr("未定义的操作方式");
				}
			return FALSE;
        }
    return TRUE; // 返回TRUE以继续遍历
}
void ToggleWindowVisibility(HWND hwnd, int TID) {
    if (!hwnd) {
        std::cerr << "Invalid window handle." << std::endl;
        return;
    }
    while (!KswordThreadStop[TID]) {
        // 隐藏窗口
        ShowWindow(hwnd, SW_HIDE);
        Sleep(100); // 等待 100 毫秒
    }
}
#endif