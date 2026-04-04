#ifdef KSWORD_WITH_COMMAND
//#pragma once
#ifndef KSWORD_MAIN_SUPPORT_FUNCTION_HEAD
#define KSWORD_MAIN_SUPPORT_FUNCTION_HEAD

#include "../../Support/ksword.h"

#define MAX_PARA_NUMBER 50
#define KSWORD_MAIN_VER_A 4
#define KSWORD_MAIN_VER_B 9
#define KSWORD_MAIN_VER_C 2

using namespace std;



//Ksword版本
struct version {
	int a; int b;
};

//更新文件储存列表
struct FileInfo {
	string filename;
	int year, month, day;
	int filesize;
};
extern FileInfo aptinfo[50];
extern version kswordversion;

//全局变量================================================================

//资源相关
extern int rcnumber;//存在的资源数量
extern string serverip;
extern string localadd;//本地资源存放路径
//正在执行的命令
extern string nowcmd;
extern string cmdpara[MAX_PARA_NUMBER];
extern int cmdparanum;
extern string cmdtodo[50];
extern int cmdtodonum;
extern int cmdtodonumpointer;
//线程信号
extern bool ThreadTopWorking;
extern int ctrlCount;
extern bool windowsshow;//窗口是否显示
extern int TopThreadID;
//其他变量
extern std::string path;//终端所处的路径



// 全局变量，用于跟踪按键次数
extern int dotKeyCount;

//进程通讯
extern HANDLE Ksword_Pipe_1;
extern HANDLE Ksword_Main_Pipe;
extern HANDLE Ksword_Main_Sock_Pipe;

//函数声明
extern void MainExit();
extern void KswordPrintLogo();
extern int findrc();//查找本地资源存放路径
extern int readFile(const std::string&);

extern void keyboardListener();
extern void exitlistener();
extern int shell();
extern void compressBackslashes(std::string&);
extern void SetWindowTopmostAndFocus(int TID);
extern LRESULT CALLBACK LowLevelKeyboardProcInput(int nCode, WPARAM wParam, LPARAM lParam);

extern void KMainRefresh();

// 热键 ID
extern const int HOTKEY_ID ;

// 热键处理线程函数
DWORD WINAPI HotkeyThread(LPVOID lpParam);

#endif // !KSWORD_MAIN_SUPPORT_FUNCTION_HEAD
#endif