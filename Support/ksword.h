//**********Ksword Head File**********//
//Developed By WangWei_CM.,Explore & Kali-Sword Dev Team.
//רΪС��Ŀ������̨��Ŀ��windowsAPI��Ŀ���ں˶Կ������ṩ�ļ�ͷ�ļ�
//������Ⱥ�ģ�774070323������QQ��3325144899

//#pragma comment(linker, "/FILEALIGN:128")
//#pragma comment(linker, "/ALIGN:128")//������С�ڵĴ�С,��ֵԽС�������ԽС 
#pragma comment(linker, "/opt:nowin98")
//http://support.microsoft.com/kb/235956/zh-cn
#pragma optimize("gsy", on)
#pragma comment(linker, "/opt:ref")
#pragma comment (linker, "/OPT:ICF")


#include <windows.h>
//#include <winsock2.h>
//#include <winsock.h>

#pragma once
#ifndef KSWORD_HEAD_FILE
#define KSWORD_HEAD_FILE
//ͷ�ļ��汾��
#define KSWORD_HEAD_FILE_VERSION 0.0
//�Ƿ����׼������������Ϣ��1=�����0=�����
#define KSWORD_PRINT_DEBUG_INFO 1
//�Ƿ����ð�ȫģʽ�����ú󽫻��ֹһЩ�����ϵͳ����ƻ��Ĳ���
#define KSWORD_SAFE_MODE 0
//��������Ksword�����Ŷӳ�Ա�����򲻽������ô�ѡ��
#define KSWORD_DEVER_MODE 0
//�Ƿ��Ե�����ʽչʾKsword�Ĵ���;���
#define KSWORD_WINDOW_WAE 0
//�쳣�˳�����ֵ
#define KSWORD_ERROR_EXIT 1
//�����˳�����ֵ
#define KSWORD_SUCCESS_EXIT 0
//��Ϣ��ʾ�ȼ���0=��ʾ������Ϣ��1=����ʾ������Ϣ��2=����ʾ������Ϣ��3=����ʾ�κ���Ϣ
#define KSWORD_MES_LEVEL 0
//�汾��Ϣ���
#define KSWORD_VERSION_A 0
#define KSWORD_VERSION_B 0
#define KSWORD_VERSION_C 0
#define KSWORD_VERSION_D 0
#define KSWORD_VERSION_SPECIAL Dev

//ksword�л�ȡ���뷽��
#define KSWORD_KGETLINE_HOOK 0
#define KSWORD_KGETLINE_GETANSYKEYCODE 1
//#define KSWORD_KGETLINE_INPUT_MODE KSWORD_KGETLINE_GETANSYKEYCODE
#define KSWORD_KGETLINE_INPUT_MODE KSWORD_KGETLINE_HOOK


//���԰�ȫ����
#define _CRT_SECURE_NO_WARNINGS

//MD5����ؼ�ֵ
#define KSWORD_MD5_A 0x67452301
#define KSWORD_MD5_B 0xefcdab89
#define KSWORD_MD5_C 0x98badcfe
#define KSWORD_MD5_D 0x10325476
const char ksword_md5_str16[17] = "0123456789abcdef";
const unsigned int ksword_md5_T[] = {
	0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,
	0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
	0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
	0x6b901122,0xfd987193,0xa679438e,0x49b40821,
	0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
	0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
	0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,
	0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
	0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
	0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
	0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,
	0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
	0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,
	0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
	0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
	0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };

const unsigned int ksword_md5_s[] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
						   5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
						   4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
						   6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };

// C++ includes used for precompiling -*- C++ -*-

// Copyright (C) 2003-2018 Free Software Foundation, Inc.
/** @file stdc++.h
 *  This is an implementation file for a precompiled header.
 */

 // 17.4.1.2 Headers

 // C
#ifndef _GLIBCXX_NO_ASSERT
#include <cassert>
#endif
#include <cctype>
#include <cerrno>
#include <cfloat>
#include <ciso646>
#include <climits>
#include <clocale>
#include <cmath>
#include <csetjmp>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

//#if __cplusplus >= 201103L
#include <ccomplex>
#include <cfenv>
#include <cinttypes>
#include <cstdalign>
#include <cstdbool>
#include <cstdint>
#include <ctgmath>
#include <cwchar>
#include <cwctype>
//#endif

// C++
#include <algorithm>
#include <bitset>
#include <complex>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <ios>
#include <iosfwd>
#include <iostream>
#include <istream>
#include <iterator>
#include <limits>
#include <list>
#include <locale>
#include <map>
#include <memory>
#include <new>
#include <numeric>
#include <ostream>
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <typeinfo>
#include <utility>
#include <valarray>
#include <vector>

//#if __cplusplus >= 201103L
#include <array>
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <mutex>
#include <random>
#include <ratio>
#include <regex>
#include <scoped_allocator>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
//#endif

//#if __cplusplus >= 201402L
#include <shared_mutex>
//#endif

//#if __cplusplus >= 201703L
#include <charconv>
#include <filesystem>
//#endif
#include <ntverp.h> // ���� RtlGetVersion �Ķ���

#pragma comment(lib, "ntdll.lib") // ���� ntdll.lib
#pragma comment(lib, "psapi.lib")

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#pragma comment(lib, "Ws2_32.lib")
//#include <wininet.h>
//#define WIN32_LEAN_AND_MEAN

//#undef WIN32_LEAN_AND_MEANp
//�����̵�˭Ҫ������ŵ�windows.h֮�����þ�����shit

//#if __has_include("WinSock2.h")
//#else
//#undef <winsock2.h>
//#endif
//
//#if __has_include("ws2tcpip.h")
//#include <ws2tcpip.h>
//#endif




#include <winternl.h>

#include <VersionHelpers.h>

#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#include <WinUser.h>
#include <TlHelp32.h>
#include <psapi.h>
#include <shellapi.h>
#include <codecvt>
#include <wintrust.h>
#include <future>
#include <fcntl.h>
#include <latch> 
#include <io.h>
#include <tchar.h>
#ifdef _M_IX86
#define _SILENCE_CXX17_C_HEADER_DEPRECATI
#endif
//��ؽṹ�嶨��

struct KVersionInfo {
	int a;
	int b;
	int c;
	int d;
	std::string e;
};//��a��d�ǰ汾����Ϣ��e������汾��ʶ��



//Ksword������Ϣ����========================================
//��ӡlogo
void KPrintLogo();
//��ʽת������==============================================
std::string WCharToString(const WCHAR* );
std::string WstringToString(std::wstring);
std::wstring StringToWString(std::string);
std::string CharToString(const char* );
const char* StringToChar(const std::string&);
const WCHAR* CharToWChar(const char*);
int StringToInt(std::string);
short StringToShort(std::string);

//Ksword�淶��Ϣ����=========================================
void Kpause();
//��Ϣ��ʾ[ * ]
void KMesInfo(const char*);
void KMesInfo(std::string);

//������ʾ[ ! ]
void KMesWarn(std::string);
void KMesWarn(const char*);
//������ʾ[ X ]
void KMesErr(std::string);
void KMesErr(const char*);

//����̨�������Բٿغ���=====================================
//�ö���ȡ���ö���������1=�ɹ�0=ʧ��
bool SetTopWindow();
bool UnTopWindow();
bool KTopWindow();
//������ȡ�����غ���
bool HideWindow();
bool ShowWindow();
//���ش��ڱ߿�
bool HideSide();
//����ȫ��
bool FullScreen();
bool KFullScreen();
bool KResetWindow();
//��������ʾָ��
bool ShowCursor();
void HideCursor();
//����ָ��λ�ã������ң��������£�
void SetCursor(int, int);
//��ɫ����ı����ı����ݣ�ǰ��ɫ������ɫ��
//0 = ��ɫ 8 = ��ɫ
//1 = ��ɫ 9 = ����ɫ
//2 = ��ɫ 10 = ����ɫ
//3 = ǳ��ɫ 11 = ��ǳ��ɫ
//4 = ��ɫ 12 = ����ɫ
//5 = ��ɫ 13 = ����ɫ
//6 = ��ɫ 14 = ����ɫ
//7 = ��ɫ 15 = ����ɫ
void cprint(std::string, int, int ,int special=0);
//����͸����
void SetAir(BYTE alpha);
//�޸Ŀ���̨���ڱ�������
//void SetTitle(const char*);
//void SetTitle(std::string);
//���Ӳ����ȡ����
bool DirectoryExists(const std::wstring&);
bool directoryExists(const std::string& path);
bool fileExists(const std::string& path);
extern int KgetlineMode;
	//0�����Ӳ����ȡ��������
	//1��Ӳ�������ȡ��������
	//2����ͨGetline��ʽ��ȡ����
void KgetlineClean();
std::string Kgetline();
std::string Getline();
//����̨������С����ȡ����С��
void MiniWindow();
void UnMiniWindow();
//��������ΪConsola
void SetConsola();
//�޸Ŀ���̨������ʼλ��
void SetConsoleWindowPosition(int x, int y);
//�޸Ŀ���̨���ڴ�С
void SetConsoleWindowSize(int width, int height);
//����̽�⺯��===============================================
//̽��Windowsϵͳ�汾��
	// Win95:1;
	// Win98:2;
	// WinMe:3
	// Win2000:4;
	// WinXP:5;
	// Win Vista:6;
	// Win7:7;
	// Win8:8;
	// Win8.1:9;
	// Win10:10;
	// Win11:11;
int WinVer();
//���õķ�����
void
__cdecl
KWinVer();
//̽��CPU���������ʣ�����0~100������Ϊ��ǰ�ٷֱ�ֵ
__int64 CompareFileTime(FILETIME, FILETIME);
int CPUUsage();
//̽���ڴ�ٷֱȣ�����0~100������Ϊ��ǰ�ٷֱ�ֵ
int RAMUsage();
//̽���ڴ�����������������GB��������ȡ����
int RAMSize();
//̽���ڴ�����������������MB��������ȡ����
int RAMSizeMB();
//��ȡϵͳ���̷��������̷�
char SystemDrive();
//��ȡָ���̿��ÿռ䣨-1�����̲����ڣ�
int FreeSpaceOfDrive(char);
//̽���Ƿ������
	//���أ�0���������
	// 1��VMwareϵ��
	// 2��VPCϵ��
	int IsVM();
	bool IsInsideVPC();
	bool IsInsideVMWare();
//̽������·��
std::wstring GetSelfPath();
//̽���Ƿ����ԱȨ��
bool IsAdmin();
//̽���û���
std::string GetUserName();
//̽��������
std::string GetHostName();
//��������==================================================
// �ֶ�ʵ�ֵ�SetWindowsHookEx
HHOOK KSetWindowsHookEx(int idHook, HOOKPROC lpfn, HINSTANCE hInstance, DWORD dwThreadId);
//��ӡ���н���
void Ktasklist();
void Ktasklist(const std::string& firstChar);
//��ӡ��ǰʱ��
void KPrintTime();
//�ͷ���Դ�ļ�
bool ReleaseResourceToFile(const char* resourceType, WORD resourceID, const char* outputFilePath);
//�첽ִ��cmd����
//����ԭ��
void ExecuteCmdAsync(const std::wstring&);
void RunCmdAsyn(const char*);
void RunCmdAsyn(std::string);
//ʵʱ����ִ��cmd����
int RunCmdNow(const char*);
int RunCmdNow(std::string);
//ƴ��ϵͳ·��
std::string ReturnCWS();
//��ȡcmd��������
std::string GetCmdResult(std::string);
std::string GetCmdResultWithUTF8(std::string);
//�������ԱȨ�޲������Լ�������·����
int RequestAdmin(std::wstring);
//���㳤MD5
std::string LMD5(const char*);
std::string LMD5(std::string);
//�����MD5
std::string SMD5(const char*);
std::string SMD5(std::string);
//�����ϣ
std::string Hash(const char*);
std::string Hash(std::string);
//�����ļ���ϣ
std::string FileHash(std::string path);
std::string FileHash(const char* path);
std::string FileHash(wchar_t path);
//�ֽ�תMB
double BytesToMB(ULONG64 bytes);
std::string GBKtoUTF8(const std::string&);
//����Powershell
//��ȡ���̾��
HWND GetHandleByTitle(const std::wstring&);
HANDLE GetHandleByProcName(const std::wstring&);
HANDLE GetProcessHandleByPID(DWORD dwProcessId);
BOOL IsInt(std::string);
DWORD GetPIDByIM(std::string ImageName);
std::string GetProcessNameByPID(DWORD processID);
//��ȡָ��PID���̵�����װ��DLL
std::vector<std::wstring>GetDLLsByPID(DWORD);
//��ȡ������
std::string GetClipBoard();
//�ں˶Կ�����==============================================
//������ָ�ָ������
int SuspendProcess(DWORD);
int UnSuspendProcess(DWORD);

//��ȡ����·��������һ��string�������ͬʱʹ�ã�������
std::string GetProgramPath();

//��ȡsystemȨ�޲������Լ�����Ҫ����ԱȨ�ޣ�
int GetSystem(const char*);
//������ö������Լ�����ҪSYSTEMȨ�ޣ�
void KGetTopMost(std::string);
//��ȡ����Ȩ�ޣ���Ҫ����ԱȨ�ޣ�
BOOL EnableDebugPrivilege(BOOL fEnable);
BOOL HasDebugPrivilege();
//��ȡ����Ȩ
int TakeOwnership(const char*);
//������غ���
	//װ����������������·�����������ƣ�
	bool LoadWinDrive(const WCHAR*,const WCHAR*);
	bool LoadWinDrive(const char*,const char*);
	bool LoadWinDrive(std::string,std::string);
	//�������񣨷������ƣ�
	bool StartDriverService(const WCHAR*);
	bool StartDriverService(const char*);
	bool StartDriverService(std::string);
	//ֹͣ���񣨷������ƣ�
	bool StopDriverService(const WCHAR*);
	bool StopDriverService(std::string);
	bool StopDriverService(const char*);
	//ж���������������ƣ�
	bool UnLoadWinDrive(const WCHAR*);
	bool UnLoadWinDrive(std::string);
	bool UnLoadWinDrive(const char*);
//��Ϊ�ؼ�����
	bool SetKeyProcess(HANDLE hProcess,bool IsKeyOrNot);
//��������
	//����������
	//0���������з���ֱ�������˳����൱���ˣ�
	//1��ʹ��taskkill�����ǿ�Ʊ�ʶ����������
	//2��ʹ��taskkill�����ǿ�Ʊ�ʶ����������
	//3������TerminateProcess()������ֹ����
	//4������TerminateThread()�����ݻ������߳�
	//5������nt terminate��������
	//6: ʹ����ҵ�����������
	//7�����ں�Ȩ�ޣ�ʹ��ZwTerminateProcess()������������
	//8�����ں�Ȩ�ޣ���̬����PspTerminateThreadByPointer�������н���
	//9�����ں�Ȩ�ޣ������ڴ�
	int KillProcess(int, HANDLE);
	int KillProcess(int, DWORD);
	bool KillProcess1(DWORD PID);
	bool KillProcess2(DWORD PID);
	BOOL TerminateProcessById(HANDLE hProcess, UINT uExitCode=0);

	bool ExistProcess(int pid);
	bool ExistProcess(HANDLE hProcess);

	bool SetProcessAndThreadsToRealtimePriority();
#endif#endif