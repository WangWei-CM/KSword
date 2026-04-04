//**********Ksword Head File**********//
//Developed By WangWei_CM.,Explore & Kali-Sword Dev Team.
//专为小项目、控制台项目、windowsAPI项目、内核对抗程序提供的简化头文件
//开发者群聊：774070323；作者QQ：3325144899

//#pragma comment(linker, "/FILEALIGN:128")
//#pragma comment(linker, "/ALIGN:128")//定义最小节的大小,数值越小程序体积越小 
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
//头文件版本号
#define KSWORD_HEAD_FILE_VERSION 0.0
//是否向标准输出输出调试信息，1=输出，0=不输出
#define KSWORD_PRINT_DEBUG_INFO 1
//是否启用安全模式，启用后将会禁止一些会对在系统造成破坏的操作
#define KSWORD_SAFE_MODE 0
//除非你是Ksword开发团队成员，否则不建议启用此选项
#define KSWORD_DEVER_MODE 0
//是否以弹窗形式展示Ksword的错误和警告
#define KSWORD_WINDOW_WAE 0
//异常退出布尔值
#define KSWORD_ERROR_EXIT 1
//正常退出布尔值
#define KSWORD_SUCCESS_EXIT 0
//消息提示等级，0=显示所有信息，1=仅显示警告信息，2=仅显示错误信息，3=不显示任何信息
#define KSWORD_MES_LEVEL 0
//版本信息相关
#define KSWORD_VERSION_A 0
#define KSWORD_VERSION_B 0
#define KSWORD_VERSION_C 0
#define KSWORD_VERSION_D 0
#define KSWORD_VERSION_SPECIAL Dev

//ksword中获取输入方案
#define KSWORD_KGETLINE_HOOK 0
#define KSWORD_KGETLINE_GETANSYKEYCODE 1
//#define KSWORD_KGETLINE_INPUT_MODE KSWORD_KGETLINE_GETANSYKEYCODE
#define KSWORD_KGETLINE_INPUT_MODE KSWORD_KGETLINE_HOOK


//忽略安全警告
#define _CRT_SECURE_NO_WARNINGS

//MD5计算关键值
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
#include <ntverp.h> // 包含 RtlGetVersion 的定义

#pragma comment(lib, "ntdll.lib") // 链接 ntdll.lib
#pragma comment(lib, "psapi.lib")

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#pragma comment(lib, "Ws2_32.lib")
//#include <wininet.h>
//#define WIN32_LEAN_AND_MEAN

//#undef WIN32_LEAN_AND_MEANp
//他奶奶的谁要把这个放到windows.h之后引用就是找shit

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
//相关结构体定义

struct KVersionInfo {
	int a;
	int b;
	int c;
	int d;
	std::string e;
};//从a到d是版本号信息，e是特殊版本标识符



//Ksword自身信息函数========================================
//打印logo
void KPrintLogo();
//格式转换函数==============================================
std::string WCharToString(const WCHAR* );
std::string WstringToString(std::wstring);
std::wstring StringToWString(std::string);
std::string CharToString(const char* );
const char* StringToChar(const std::string&);
const WCHAR* CharToWChar(const char*);
int StringToInt(std::string);
short StringToShort(std::string);

//Ksword规范消息函数=========================================
void Kpause();
//信息提示[ * ]
void KMesInfo(const char*);
void KMesInfo(std::string);

//警告提示[ ! ]
void KMesWarn(std::string);
void KMesWarn(const char*);
//错误提示[ X ]
void KMesErr(std::string);
void KMesErr(const char*);

//控制台窗口属性操控函数=====================================
//置顶与取消置顶函数返回1=成功0=失败
bool SetTopWindow();
bool UnTopWindow();
bool KTopWindow();
//隐藏与取消隐藏函数
bool HideWindow();
bool ShowWindow();
//隐藏窗口边框
bool HideSide();
//窗口全屏
bool FullScreen();
bool KFullScreen();
bool KResetWindow();
//隐藏与显示指针
bool ShowCursor();
void HideCursor();
//更改指针位置（从左到右；从上往下）
void SetCursor(int, int);
//彩色输出文本（文本内容，前景色，背景色）
//0 = 黑色 8 = 灰色
//1 = 蓝色 9 = 淡蓝色
//2 = 绿色 10 = 淡绿色
//3 = 浅绿色 11 = 淡浅绿色
//4 = 红色 12 = 淡红色
//5 = 紫色 13 = 淡紫色
//6 = 黄色 14 = 淡黄色
//7 = 白色 15 = 亮白色
void cprint(std::string, int, int ,int special=0);
//设置透明度
void SetAir(BYTE alpha);
//修改控制台窗口标题内容
//void SetTitle(const char*);
//void SetTitle(std::string);
//钩子层面获取输入
bool DirectoryExists(const std::wstring&);
bool directoryExists(const std::string& path);
bool fileExists(const std::string& path);
extern int KgetlineMode;
	//0：钩子层面获取键盘输入
	//1：硬件层面获取键盘输入
	//2：普通Getline方式获取输入
void KgetlineClean();
std::string Kgetline();
std::string Getline();
//控制台窗口最小化与取消最小化
void MiniWindow();
void UnMiniWindow();
//更改字体为Consola
void SetConsola();
//修改控制台窗口起始位置
void SetConsoleWindowPosition(int x, int y);
//修改控制台窗口大小
void SetConsoleWindowSize(int width, int height);
//环境探测函数===============================================
//探测Windows系统版本：
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
//更好的方法：
void
__cdecl
KWinVer();
//探测CPU整体利用率，返回0~100整数作为当前百分比值
__int64 CompareFileTime(FILETIME, FILETIME);
int CPUUsage();
//探测内存百分比，返回0~100整数作为当前百分比值
int RAMUsage();
//探测内存容量，返回容量（GB）（向上取整）
int RAMSize();
//探测内存容量，返回容量（MB）（向下取整）
int RAMSizeMB();
//获取系统盘盘符：返回盘符
char SystemDrive();
//获取指定盘可用空间（-1：该盘不存在）
int FreeSpaceOfDrive(char);
//探测是否虚拟机
	//返回：0：非虚拟机
	// 1：VMware系列
	// 2：VPC系列
	int IsVM();
	bool IsInsideVPC();
	bool IsInsideVMWare();
//探测自身路径
std::wstring GetSelfPath();
//探测是否管理员权限
bool IsAdmin();
//探测用户名
std::string GetUserName();
//探测主机名
std::string GetHostName();
//操作函数==================================================
// 手动实现的SetWindowsHookEx
HHOOK KSetWindowsHookEx(int idHook, HOOKPROC lpfn, HINSTANCE hInstance, DWORD dwThreadId);
//打印所有进程
void Ktasklist();
void Ktasklist(const std::string& firstChar);
//打印当前时间
void KPrintTime();
//释放资源文件
bool ReleaseResourceToFile(const char* resourceType, WORD resourceID, const char* outputFilePath);
//异步执行cmd命令
//函数原型
void ExecuteCmdAsync(const std::wstring&);
void RunCmdAsyn(const char*);
void RunCmdAsyn(std::string);
//实时回显执行cmd命令
int RunCmdNow(const char*);
int RunCmdNow(std::string);
//拼接系统路径
std::string ReturnCWS();
//获取cmd返回内容
std::string GetCmdResult(std::string);
std::string GetCmdResultWithUTF8(std::string);
//请求管理员权限并启动自己（程序路径）
int RequestAdmin(std::wstring);
//计算长MD5
std::string LMD5(const char*);
std::string LMD5(std::string);
//计算短MD5
std::string SMD5(const char*);
std::string SMD5(std::string);
//计算哈希
std::string Hash(const char*);
std::string Hash(std::string);
//计算文件哈希
std::string FileHash(std::string path);
std::string FileHash(const char* path);
std::string FileHash(wchar_t path);
//字节转MB
double BytesToMB(ULONG64 bytes);
std::string GBKtoUTF8(const std::string&);
//调用Powershell
//获取进程句柄
HWND GetHandleByTitle(const std::wstring&);
HANDLE GetHandleByProcName(const std::wstring&);
HANDLE GetProcessHandleByPID(DWORD dwProcessId);
BOOL IsInt(std::string);
DWORD GetPIDByIM(std::string ImageName);
std::string GetProcessNameByPID(DWORD processID);
//获取指定PID进程的所有装载DLL
std::vector<std::wstring>GetDLLsByPID(DWORD);
//获取剪贴板
std::string GetClipBoard();
//内核对抗函数==============================================
//挂起与恢复指定进程
int SuspendProcess(DWORD);
int UnSuspendProcess(DWORD);

//获取自身路径，返回一个string，请务必同时使用！！！！
std::string GetProgramPath();

//获取system权限并启动自己（需要管理员权限）
int GetSystem(const char*);
//以最高置顶启动自己（需要SYSTEM权限）
void KGetTopMost(std::string);
//获取调试权限（需要管理员权限）
BOOL EnableDebugPrivilege(BOOL fEnable);
BOOL HasDebugPrivilege();
//获取所有权
int TakeOwnership(const char*);
//驱动相关函数
	//装载驱动（驱动绝对路径，服务名称）
	bool LoadWinDrive(const WCHAR*,const WCHAR*);
	bool LoadWinDrive(const char*,const char*);
	bool LoadWinDrive(std::string,std::string);
	//启动服务（服务名称）
	bool StartDriverService(const WCHAR*);
	bool StartDriverService(const char*);
	bool StartDriverService(std::string);
	//停止服务（服务名称）
	bool StopDriverService(const WCHAR*);
	bool StopDriverService(std::string);
	bool StopDriverService(const char*);
	//卸载驱动（服务名称）
	bool UnLoadWinDrive(const WCHAR*);
	bool UnLoadWinDrive(std::string);
	bool UnLoadWinDrive(const char*);
//设为关键进程
	bool SetKeyProcess(HANDLE hProcess,bool IsKeyOrNot);
//结束进程
	//结束方法：
	//0：尝试所有方法直到进程退出（相当极端）
	//1：使用taskkill命令不带强制标识符结束进程
	//2：使用taskkill命令带强制标识符结束进程
	//3：调用TerminateProcess()函数终止进程
	//4：调用TerminateThread()函数摧毁所有线程
	//5：调用nt terminate结束进程
	//6: 使用作业对象结束进程
	//7：（内核权限）使用ZwTerminateProcess()函数结束进程
	//8：（内核权限）动态调用PspTerminateThreadByPointer结束所有进程
	//9：（内核权限）清零内存
	int KillProcess(int, HANDLE);
	int KillProcess(int, DWORD);
	bool KillProcess1(DWORD PID);
	bool KillProcess2(DWORD PID);
	BOOL TerminateProcessById(HANDLE hProcess, UINT uExitCode=0);

	bool ExistProcess(int pid);
	bool ExistProcess(HANDLE hProcess);

	bool SetProcessAndThreadsToRealtimePriority();
#endif#endif