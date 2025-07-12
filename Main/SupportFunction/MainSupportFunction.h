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



//Ksword�汾
struct version {
	int a; int b;
};

//�����ļ������б�
struct FileInfo {
	string filename;
	int year, month, day;
	int filesize;
};
extern FileInfo aptinfo[50];
extern version kswordversion;

//ȫ�ֱ���================================================================

//��Դ���
extern int rcnumber;//���ڵ���Դ����
extern string serverip;
extern string localadd;//������Դ���·��
//����ִ�е�����
extern string nowcmd;
extern string cmdpara[MAX_PARA_NUMBER];
extern int cmdparanum;
extern string cmdtodo[50];
extern int cmdtodonum;
extern int cmdtodonumpointer;
//�߳��ź�
extern bool ThreadTopWorking;
extern int ctrlCount;
extern bool windowsshow;//�����Ƿ���ʾ
extern int TopThreadID;
//��������
extern std::string path;//�ն�������·��



// ȫ�ֱ��������ڸ��ٰ�������
extern int dotKeyCount;

//����ͨѶ
extern HANDLE Ksword_Pipe_1;
extern HANDLE Ksword_Main_Pipe;
extern HANDLE Ksword_Main_Sock_Pipe;

//��������
extern void MainExit();
extern void KswordPrintLogo();
extern int findrc();//���ұ�����Դ���·��
extern int readFile(const std::string&);

extern void keyboardListener();
extern void exitlistener();
extern int shell();
extern void compressBackslashes(std::string&);
extern void SetWindowTopmostAndFocus(int TID);
extern LRESULT CALLBACK LowLevelKeyboardProcInput(int nCode, WPARAM wParam, LPARAM lParam);

extern void KMainRefresh();

// �ȼ� ID
extern const int HOTKEY_ID ;

// �ȼ������̺߳���
DWORD WINAPI HotkeyThread(LPVOID lpParam);

#endif // !KSWORD_MAIN_SUPPORT_FUNCTION_HEAD
#endif