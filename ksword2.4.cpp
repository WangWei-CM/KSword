#include <windows.h>
#include <iostream>
#include <bits/stdc++.h>
#include <cstring>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <conio.h>
#include <stdlib.h>
#include <userenv.h>
#include <lmcons.h>
#include <cctype>
#include <TlHelp32.h>
#include <thread>
#include <cwchar>
#include <tchar.h>
#include <chrono>
#include <atomic>


#define kswordversion "Ksword2.4"
#define AdminPassword "Admin" 

using namespace std;
using namespace std::chrono;

std::atomic<int> ctrlCount(0); // 使用原子操作来保证线程安全
std::atomic<bool> windowsshow(true); // 初始状态为显示窗口

bool adminyn();
int pw();
int options();
int show_logo();
int shell();
int findrc(); 
int loadcmd();
int wintop();//no input and output,put window to top
int winuntop();
int fread();//fast read,int i;i=fread();
int run_cmd_pro(const char *);
int run_cmd_usr(const char *);
int RequestAdministrator();
int hack();
int charArrayToInt(const char *, int);
void printcpu(int,int);
void keyboardListener();
void runassys();
void getexepath();
void hideWindow();
void showWindow();
void timerFunction();
void cprintforcpu(int,int);
void strtochar();//copy: charusrname,usrname;chardoname,doname
void cprint(const char*,int);//colorful print not c++ print,only print char,you can change it but i suggest dont do that;
void ready();//some loading process;
void getpcname(std::string&,std::string&);
void toLowerCase(std::string &, std::string &);
void compressBackslashes(std::string &);
void cutpth(string,const char);//has something with cmd"cd" and "cd..".It is already shit so dont touch it.
bool directoryExists(const std::string&);//input a output a bool.
string getCmdResult(const string&);//get cmd result.Need a command in string'
string usrname;
string cmdpth[100]={"C:","Windows","System32"};
int cmdpthtop=2;
char charusrname[20];
string doname;
char chardoname[20];
string usrlist[15][2];
int usraut[15];
int usrlisttop=2;
int cmdnumber=21;
wchar_t* ExePath;
//string usrenter;
string nowcmd;
string cmdpara;
string nowopt;
string rc;
DWORD hostnamesize;
string cmd[50];
string strcmd[50];
string strre;
string lchostname;
string lcusrname;
char hostname[256]="localhost";
int usrnumber;
int donumber;
bool paralau;
//net need
int curusrnum;int usrnum;
int newnew;
string neww[1000];
bool adminyn(){
	BOOL isElevated = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD dwSize;
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &dwSize)) {
            isElevated = elevation.TokenIsElevated;
        }
    }
    if (hToken) {
        CloseHandle(hToken);
    }
    return isElevated;
}
void getexepath(){
	TCHAR szExePath[MAX_PATH];
    DWORD dwSize = GetModuleFileName(NULL, szExePath, MAX_PATH);
    if (dwSize > 0) {
	        std::string exePathStr = std::string(szExePath);
			const char* cstr = exePathStr.c_str();
	int bufferSize = MultiByteToWideChar(CP_UTF8, 0, cstr, -1, NULL, 0);
	wchar_t* wideCstr = new wchar_t[bufferSize];
	MultiByteToWideChar(CP_UTF8, 0, cstr, -1, wideCstr, bufferSize);
	ExePath=wideCstr;
}
}
int winver(){
	std::string vname;
	//先判断是否为win8.1或win10
	typedef void(__stdcall*NTPROC)(DWORD*, DWORD*, DWORD*);
	HINSTANCE hinst = LoadLibrary("ntdll.dll");
	DWORD dwMajor, dwMinor, dwBuildNumber;
	NTPROC proc = (NTPROC)GetProcAddress(hinst, "RtlGetNtVersionNumbers"); 
	proc(&dwMajor, &dwMinor, &dwBuildNumber); 
	if (dwMajor == 6 && dwMinor == 3)	//win 8.1
	{
		return 9;
	}
	if (dwMajor == 10 && dwMinor == 0)	//win 10
	{
		return 10;
	}
	SYSTEM_INFO info;                //用SYSTEM_INFO结构判断64位AMD处理器  
	GetSystemInfo(&info);            //调用GetSystemInfo函数填充结构  
	OSVERSIONINFOEX os;
	os.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	#pragma warning(disable:4996)
	if (GetVersionEx((OSVERSIONINFO *)&os))
	{
 
		//下面根据版本信息判断操作系统名称  
		switch (os.dwMajorVersion)
		{                        //判断主版本号  
		case 4:
			switch (os.dwMinorVersion)
			{                //判断次版本号  
			case 0:
				if (os.dwPlatformId == VER_PLATFORM_WIN32_NT)
					vname ="Microsoft Windows NT 4.0";  //1996年7月发布  
				else if (os.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)
					vname = "Microsoft Windows 95";
				break;
			case 10:
				vname ="Microsoft Windows 98";
				return 1;
			case 90:
				vname = "Microsoft Windows Me";
				return 1;
			}
			break;
		case 5:
			switch (os.dwMinorVersion)
			{               //再比较dwMinorVersion的值  
			case 0:
				vname = "Microsoft Windows 2000";    //1999年12月发布  
				return 2;
			case 1:
				vname = "Microsoft Windows XP";      //2001年8月发布  
				return 5;
			case 2:
				if (os.wProductType == VER_NT_WORKSTATION &&
					info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
					return 5;
				else if (GetSystemMetrics(SM_SERVERR2) == 0)
					return 5;   //2003年3月发布  
				else if (GetSystemMetrics(SM_SERVERR2) != 0)
					return 5;
				break;
			}
			break;
		case 6:
			switch (os.dwMinorVersion)
			{
			case 0:
				if (os.wProductType == VER_NT_WORKSTATION)
					return 6;
				else
					return 6;   //服务器版本 } 
				break;
			case 1:
				if (os.wProductType == VER_NT_WORKSTATION)
					return 7;
				else
					return 7;
				break;
			case 2:
				if (os.wProductType == VER_NT_WORKSTATION)
					return 8;
				else
					return 8;
				break;
			}
			break;
		default:
			return 0;
		}
	}
	else return 0;
}
bool DirectoryExists(const std::wstring& path) {
    DWORD dwAttrib = GetFileAttributesW(path.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && 
            (dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}
int findrc(){
	wchar_t drive[100] = L"C:\\";
    for (wchar_t letter = 'C'; letter <= 'Z'; ++letter) {
        drive[0] = letter;
        if (DirectoryExists(std::wstring(drive) + L"ksword")) {
            std::wcout << L"Directory found at: " << drive << L"ksword" << std::endl;
            return 0;
        }
    }
    std::wcout << L"No directory found." << std::endl;
    return 0;
}
int wintop() {
    HWND hWnd = GetConsoleWindow(); // 获取控制台窗口句柄
    if (hWnd != NULL) {
        // 将窗口置顶
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    return 0;
}
int winuntop() {
    HWND hWnd = GetConsoleWindow(); // 获取控制台窗口句柄
    if (hWnd != NULL) {
        // 取消窗口置顶
        SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    return 0;
}
int fread(){
    int x = 0, f = 1;
    char ch = getchar();
    while (ch < 48 || ch > 57)
    {
        if (ch == '-') f = -1;
        ch = getchar();
    }
    while (ch >= 48 && ch <= 57)
        x = (x << 3) + (x << 1) + (ch ^ 48), ch = getchar();
    return x * f;
}
int main(int argc, char* argv[]){ 
	if (argc > 1) {
		const char* inputPassword = argv[1];
        // 使用strlen来获取输入密码的长度，并与AdminPassword的长度进行比较
        size_t inputPasswordLength = strlen(inputPassword);
        char inputPasswordArray[inputPasswordLength + 1]; // +1为了存储字符串的结束符'\0'
        strncpy(inputPasswordArray, inputPassword, inputPasswordLength);
        inputPasswordArray[inputPasswordLength] = '\0'; // 确保字符串以'\0'结尾
        // 比较输入的密码和AdminPassword宏定义
        if (strcmp(inputPasswordArray, AdminPassword) == 0) {
			paralau=1;
			goto NormalStart;
        }
        std::string arg = argv[1];
        if (arg.length() == 1 && std::isalpha(arg[0])) {
            switch (std::tolower(arg[0])) { // 使用tolower确保大小写不敏感
                case 'l':
                    {std::cout << "Argument 'l' received, performing special action for 'u'." << std::endl;
                    HWND hwnd = GetConsoleWindow();
						LONG style = GetWindowLong(hwnd, GWL_STYLE);
						style &= ~(WS_BORDER|WS_CAPTION|WS_THICKFRAME);
						SetWindowLong(hwnd, GWL_STYLE, style);
						SetWindowPos( hwnd, NULL, 0,0,0,0,SWP_NOSIZE|SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED ); 
						CONSOLE_CURSOR_INFO CursorInfo;
					 	CONSOLE_CURSOR_INFO cursor;    
					 	cursor.bVisible = FALSE;    
					 	cursor.dwSize = sizeof(cursor);    
					 	HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);    
					 	HWND hWnd = GetConsoleWindow();
					    if (hWnd != NULL) {SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 60, SWP_NOMOVE | SWP_NOSIZE);}
					    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
					    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
					    int x = screenWidth - 700;
					    int y = screenHeight - 200;
					    HWND consoleWindow = GetConsoleWindow();
					    SetWindowPos(consoleWindow, NULL, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
					    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 80, LWA_COLORKEY);
					 	SetConsoleCursorInfo(handle, &cursor);
						system("mode con cols=80 lines=6");
						system("color a");
						SetWindowLongPtrA(GetConsoleWindow(), GWL_STYLE, GetWindowLongPtrA(GetConsoleWindow(),GWL_STYLE)& ~WS_SIZEBOX & ~WS_MAXIMIZEBOX & ~WS_MINIMIZEBOX);
						system("cls"); 
						while(1){
							const char *cmd = "query user";
							int ret = 0;
							ret = run_cmd_usr(cmd);
							usrnum=curusrnum;
							curusrnum=0;
							for(int i=0;i<=newnew;i++){
								cout<<neww[i]<<endl;
							}
							Sleep(1000);
							system("cls");}
                    	break;}
                case 'p':
				{
                    std::cout << "Argument 'p' received, performing special action for 'r'." << std::endl;
//                    thread listenerThread(keyboardListener);
//					thread timerThread(timerFunction);
					HWND hwnd = GetConsoleWindow();
					LONG style = GetWindowLong(hwnd, GWL_STYLE);
					style &= ~(WS_BORDER|WS_CAPTION|WS_THICKFRAME);
					SetWindowLong(hwnd, GWL_STYLE, style);
					SetWindowPos( hwnd, NULL, 0,0,0,0,SWP_NOSIZE|SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED ); 
					CONSOLE_CURSOR_INFO CursorInfo;
				 	CONSOLE_CURSOR_INFO cursor;    
				 	cursor.bVisible = FALSE;    
				 	cursor.dwSize = sizeof(cursor);    
				 	HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);    
				 	HWND hWnd = GetConsoleWindow();
				    if (hWnd != NULL) {SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 60, SWP_NOMOVE | SWP_NOSIZE);}
				    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
				    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
				    int x = screenWidth - 200;
				    int y = screenHeight - 100;
				    HWND consoleWindow = GetConsoleWindow();
				    SetWindowPos(consoleWindow, NULL, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
				    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 80, LWA_COLORKEY);
				 	SetConsoleCursorInfo(handle, &cursor);
					system("mode con cols=24 lines=2");
					SetWindowLongPtrA(GetConsoleWindow(), GWL_STYLE, GetWindowLongPtrA(GetConsoleWindow(),GWL_STYLE)& ~WS_SIZEBOX & ~WS_MAXIMIZEBOX & ~WS_MINIMIZEBOX);
					system("cls");
					while(1){
						const char *cmd = "wmic cpu get loadpercentage";
						int ret = 0;
						ret = run_cmd_pro(cmd);}
//						listenerThread.join();
//						timerThread.join(); 
						return 0;
	                    break;}
                default:
                    {std::cerr << "Error: Unrecognized single-letter argument." << std::endl;
                    return 1;}
            }
        } else {
            goto NormalStart;
        }
    }
	NormalStart:
		if(!paralau){
		STARTUPINFO si;
	    PROCESS_INFORMATION pi;
	    // 初始化 STARTUPINFO 结构体
	    ZeroMemory(&si, sizeof(si));
	    si.cb = sizeof(si);
	    // 初始化 PROCESS_INFORMATION 结构体
	    ZeroMemory(&pi, sizeof(pi));
	    // 设置不显示窗口
	    si.dwFlags = STARTF_USESHOWWINDOW;
	    si.wShowWindow = SW_HIDE;
	    // 执行命令行，例如 "notepad.exe"
	    BOOL bSuccess = CreateProcess(
	        NULL,           // 应用程序名称
	        "cmd /c EaseOfAccessDialog.exe 211", // 命令行参数，使用 cmd /c 后跟你的命令
	        NULL,           // 进程安全属性
	        NULL,           // 主线程安全属性
	        FALSE,          // 继承父进程的句柄
	        CREATE_NEW_CONSOLE, // 创建新控制台窗口
	        NULL,           // 使用默认环境
	        NULL,           // 使用当前驱动器和目录
	        &si,            // STARTUPINFO 指针
	        &pi             // PROCESS_INFORMATION 指针
	    );
	    CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
		ShowWindow(GetConsoleWindow(), SW_HIDE);
		system("EaseOfAccessDialog.exe 211");
	    bool ctrlPressed = false;
	    int pressCount = 0;
	    const int requiredPresses = 3;
	    const int waitTime = 3000;
	    for (int i = 0; i < 3000; i += 100) {
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
            pressCount++;
            if (pressCount >= 3) {
                break; // 达到3次按下，退出循环
            }
        }
        Sleep(100); // 休眠100毫秒，减少CPU占用
    }
	    // 根据按下次数决定是否显示控制台窗口
	    if (pressCount >= requiredPresses) {
	        ShowWindow(GetConsoleWindow(), SW_SHOW);
	    } else {
	        std::exit(0);
	    }}

	findrc();
	getexepath();
	wintop();
	ready();
	int tmp;
	if (!paralau)
	tmp=pw();
	else tmp=0;
	if (tmp!=100);
	else exit(0);
 	usrname=usrlist[tmp][0];
	doname=usrlist[tmp][0];
	donumber=tmp;
	usrnumber=tmp;
	system("cls");
	thread listenerThread(keyboardListener);
	show_logo();
	loadcmd();
	shell();
	return 0;
}
bool directoryExists(const std::string& path) {
    DWORD attrib = GetFileAttributesA(path.c_str());
    return (attrib != INVALID_FILE_ATTRIBUTES && (attrib & FILE_ATTRIBUTE_DIRECTORY));
}
void ready(){
	//usrlist[usrname][usrpassword],usraut[usrnumber] ,0~10=max~min.
	usrlist[0][0]="Admin";
	usrlist[1][0]="User 1";
	usrlist[2][0]="User 2";
	usrlist[2][2]="Pwd2";
	usrlist[0][1]=AdminPassword;
	usrlist[1][1]="Pwd1";
	usraut[0]=0;usraut[1]=1;usraut[2]=8;
	strcmd[1]="icacls C:\\windows\\system32\\sethc.exe /setowner Administrator /c && icacls C:\\Windows\\System32\\sethc.exe /grant Administrator:F /t";
	strcmd[2]="taskkill /f /t /im explorer.exe";
	strcmd[3]="icacls C:\\Windows\\explorer.exe /grant Administrator:F /t && icacls C:\\Windows\\system32\\taskmgr.exe /grant Administrator:F /t && cd c:\\windows && ren explorer.exe explorer1.exe && cd c:\\windows\\system32 && ren taskmgr.exe taskmgr1.exe";
	strcmd[4]="c:\\script\\1.bat";
 	strcmd[5]="c:\\script\\2.bat";
	strcmd[6]="c:\\script\\3.bat";
	strcmd[7]="c:\\script\\4.bat";
	strcmd[8]="c:\\script\\5.bat";
	strcmd[9]="c:\\script\\6.bat";
	strcmd[10]="c:\\script\\7.bat";
	strcmd[11]="c:\\script\\8.bat";
}
int pw(){
	string password;
	char ch;
    printf("Enter password:");
	int i = 0;
	bool flag=1;
	while (flag)
	{
		ch=getch();
		switch((int)ch)
		{
			case 8:
				if(password.empty())
				  break;
				password.erase(password.end()-1);
				putchar('\b');
				putchar(' ');
				putchar('\b');
				break;
			case 27:
				exit(0);
				break;
			case 13:
				flag=0;
				break;
			default :password+=ch;putchar('*');break;
		}
	}
    if (password==usrlist[1][1]){
		return 1;}
    if (password==usrlist[0][1]){
		return 0;}
	if (password==usrlist[2][1]){
		return 2;}
	else return 100;
}
int gethost(){
    hostnamesize = sizeof(hostname);
    GetComputerName(hostname, &hostnamesize);
}
int show_logo(){
cprint("====================Kali Sword V2.3 For Public=====================",87);
cout<<endl;
int osver=winver();
if(osver<=9){
	cprint("WARNING!Some components may not function properly!",71);
}
cout<<endl;
 cout<<"  _  __                                      _ "<<endl;
 cout<<" | |/ /  ___  __      __   ___    _ __    __| |"<<endl;
 cout<<" | ' /  / __| \\ \\ /\\ / /  / _ \\  | '__|  / _` |"<<endl;
 cout<<" | . \\  \\__ \\  \\ V  V /  | (_) | | |    | (_| |"<<endl;
 cout<<" |_|\\_\\ |___/   \\_/\\_/    \\___/  |_|     \\__,_|"<<endl;
    cout<<endl;                                           
}
void cprint(const char* s, int color){
 HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
 SetConsoleTextAttribute(handle, FOREGROUND_INTENSITY | color);
 cout<<s;
 SetConsoleTextAttribute(handle, FOREGROUND_INTENSITY | 7);
}
void getpcname(std::string &lchostname, std::string &lcusrname) {
    // 获取计算机名
    TCHAR computerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(computerName) / sizeof(computerName[0]);
    if (GetComputerName(computerName, &size)) {
        lchostname = computerName;
    } else {
        lchostname = "Unknown";
    }

    // 获取当前用户名称
    TCHAR username[UNLEN + 1];
    size = sizeof(username) / sizeof(username[0]);
    if (GetUserName(username, &size)) {
        lcusrname = username;
    } else {
        lcusrname = "Unknown";
    }
}
void strtochar(){
	strcpy(charusrname,usrname.c_str());
	strcpy(chardoname,doname.c_str());
}
string getCmdResult(const string &strCmd){
    char buf[102400] = {0};
    FILE *pf = NULL;

    if( (pf = popen(strCmd.c_str(), "r")) == NULL )
    {
        return "";
    }

    string strResult;
    while(fgets(buf, sizeof buf, pf))
    {
        strResult += buf;
    }

    pclose(pf);

    unsigned int iSize =  strResult.size();
    if(iSize > 0 && strResult[iSize - 1] == '\n')  
    {
        strResult = strResult.substr(0, iSize - 1);
    }

    return strResult;
}
void compressBackslashes(std::string &cmdpara) {
    size_t len = cmdpara.length();
    std::string result;
    result.reserve(len); // 预分配内存以提高性能

    bool inEscape = false; // 标记是否处于转义序列中
    for (size_t i = 0; i < len; ++i) {
        if (cmdpara[i] == '\\') {
            if (!inEscape) { // 如果当前不在转义序列中，则添加一个反斜杠
                result += '\\';
                inEscape = true; // 现在我们处于转义序列中
            }
        } else {
            result += cmdpara[i]; // 添加普通字符
            inEscape = false; // 离开转义序列
        }
    }
    cmdpara = result; // 用处理后的字符串覆盖原字符串
}
int loadcmd(){
	cmd[1]="cmd";
	cmd[2]="taskmgr";
	cmd[3]="help";
	cmd[4]="whoami";
	cmd[5]="time";
	cmd[6]="switchuser";
	cmd[7]="runas";
	cmd[8]="sethc";
	cmd[9]="cd";
	cmd[10]="untop";
	cmd[11]="clear";
	cmd[12]="script";
	cmd[13]="about";
	cmd[14]="setpth";
	cmd[15]="exit";
	cmd[16]="top";
	cmd[17]="cpu";
	cmd[18]="net";
	cmd[19]="getsys";
	cmd[20]="bluescr";
	cmd[21]="hack"; 
}
int run_cmd(const char * cmd){
	char MsgBuff[1024];
	int MsgLen=1020;
	FILE * fp;
	if (cmd == NULL)
	{
		return -1;
	}
	if ((fp = _popen(cmd, "r")) == NULL)
	{
		return -2;
	}
	else
	{
		memset(MsgBuff, 0, MsgLen);

		while (fgets(MsgBuff, MsgLen, fp) != NULL)
		{
			printf("%s", MsgBuff);
		}
		if(_pclose(fp) == -1)
		{
			return -3;
		}
	}
	return 0;
}
void toLowerCase(std::string &nowcmd, std::string &cmdpara) {
    for (size_t i = 0; i < nowcmd.length(); ++i) {
    nowcmd[i] = std::tolower(static_cast<unsigned char>(nowcmd[i]));}
    for (size_t i = 0; i < cmdpara.length(); ++i) {
    cmdpara[i] = std::tolower(static_cast<unsigned char>(cmdpara[i]));}
}
void cutpth(string str,const char split){
	istringstream iss(str);
	string token;
	
	for (int i=0;i<=cmdpthtop;i++)
	{
	cmdpth[i]="";
	}
	cmdpthtop=0;
	while (getline(iss, token, split))
	{
		cmdpth[cmdpthtop++]=token;
	}
	cmdpthtop--;
}
int shell(){	
	shellstart:
	strtochar();
	getpcname(lchostname,lcusrname);
	if(doname!=usrname)
	{
	cout<<"┌ ";
	cout<<"[";
	cprint(charusrname,70);
	cout<<"=>";
	cprint(chardoname,60);
	cout<<"@";
	cout<<kswordversion;
	cout<<"]";
	}
	else
	{
	cout<<"┌ ";
	cout<<"[";
	cprint(charusrname,70);
	cout<<"@";
	cout<<kswordversion;
	cout<<"]";
	}

	cout<<"~[";
	if(lcusrname=="SYSTEM")
	cprint("System",70);
	else cout<<lcusrname;
	cout<<'@'<<lchostname<<']';
	if (adminyn()){
		cprint("[Admin]",2);
	}
	cout<<endl;
	cout<<"└ ";
	for(int i=0;i<=cmdpthtop;i++)
	{
	cout<<cmdpth[i];
	cout<<"\\";
	}
	cout<<">";
	getline(cin,nowcmd);
	string title;
	for(int i=0;i<=cmdpthtop;i++)
	{title+=cmdpth[i];
		title+="\\";}
		title+=" > ";
		title+=nowcmd;
	SetConsoleTitleA(title.c_str());
 	size_t spacePos = nowcmd.find(' ');
    if (spacePos != std::string::npos)
	{ 
		cmdpara = nowcmd.substr(spacePos + 1);
		nowcmd = nowcmd.substr(0, spacePos);
    }
    toLowerCase(nowcmd, cmdpara);
	if (nowcmd=="cd..")
	{
		if(cmdpthtop>=1)
		{
			cmdpth[cmdpthtop]="";
			cmdpthtop--;
		}
		else
		{
			cprint("[ x ]",4);
			cout<<"Already reach root!"<<endl;
		}
	}
	else if (nowcmd==cmd[1])
	{
		if (usraut[donumber]>=6){
		cprint("[ ! ]",6);
		cout<<"Insufficient authority"<<endl;
		cprint("[ x ]",4);
		cout<<"The operation did not complete successfully!"<<endl;
		}
		cprint("[ ! ]",6);
		cout<<"type exit to quit."<<endl;
		system("c:\\windows\\system32\\cmd.exe");
	}
	else if (nowcmd==cmd[2])
	{
		if (usraut[donumber]>=6){
		cprint("[ ! ]",6);
		cout<<"Insufficient authority"<<endl;
		cprint("[ x ]",4);
		cout<<"The operation did not complete successfully!"<<endl;
		}
		system("c:\\windows\\system32\\taskmgr.exe");
	}
	else if (nowcmd==cmd[3])
	{
		cout<<"Help:all the commands are as following:";
		for(int i=0;i<=cmdnumber;i++)
		cout<<cmd[i]<<' ';
		cout<<"."<<endl;cout<<"Enter the command name that you want to learn about:(c for cancel)";
		string helpcmd;
		getline(cin,helpcmd);
		if (helpcmd==cmd[1]){cout<<"Enter cmd so that we'll start a cmd.exe for you with system privileges."<<endl;}
		else if (helpcmd==cmd[2]){cout<<"Enter to run taskmgr.exe."<<endl;}
		else if (helpcmd==cmd[3]){cout<<"How can you even don't know how to use help command?"<<endl;}
		else if (helpcmd==cmd[4]){cout<<"This command has been deprecated, and typing this command now gives the console program execution permission."<<endl;}
		else if (helpcmd==cmd[5]){cout<<"Just to get the time."<<endl;}
		else if (helpcmd==cmd[6]){cout<<"Switch user of Kali-sword.This requires that user's password.You can try runas if you don't have theirs."<<endl;}
		else if (helpcmd==cmd[7]){cout<<"A user with higher permissions can operate as a lower user without entering a password. The reverse is required. This changes the recorded results in the log."<<endl;}
		else if (helpcmd==cmd[8]){cout<<"Replace the sethc.exe(shift*5) with Kali-sword.This command may doesn't work."<<endl;}
		else if (helpcmd==cmd[9]){cout<<"Use it like on Windows cmd or linux shell.We support \"cd..\"and\"cd <dir>"<<endl;}
		else if (helpcmd==cmd[10]){cout<<"Untop your window."<<endl;}
		else if (helpcmd==cmd[11]){cout<<"Clean all output."<<endl;}
		else if (helpcmd==cmd[12]){cout<<"Run user script.You can copy your scripts(only *.bat supported) to C:\\script(Create one if there's not one) and renamed it with the number 1~10."<<endl;}
		else if (helpcmd==cmd[13]){cout<<"Show something about the Developer."<<endl;}
		else if (helpcmd==cmd[14]){cout<<"set cmd path.If you enter a command that is not included in Kali-sword,we will use cmd to run it.And it will reset the cmd path to C:\\Windows\\system32 every time when you enter a new command.This will change it.Make sure the path you enter is available!"<<endl;}
		else if (helpcmd==cmd[15]){cout<<"Log out and stop Kali-sword."<<endl;}
		else if (helpcmd==cmd[16]){cout<<"top your window."<<endl;}
		else if (helpcmd==cmd[17]){cout<<"Show CPU%.Can't run on Windows 11 or higher."<<endl;}
		else if (helpcmd==cmd[18]){cout<<"Show users on this computer.Can't run on Windows 7 or earlier."<<endl;}
		else if (helpcmd==cmd[19]){cout<<"Get SYSTEM.Administrator permissions required."<<endl;}
		else if (helpcmd==cmd[20]){cout<<"Blue screen in case of emergency to destroy evidence. Requirements administrator rights."<<endl;}
		else if (helpcmd==cmd[21]){cout<<"A mysterious tool."<<endl;}
		else if (helpcmd=="c");
		else 	{cprint("[ x ]",4);cout<<"command not found!"<<endl;}
	}
	else if (nowcmd==cmd[5])
	{
   		time_t timep;
   		time(&timep);
   		printf("%s", ctime(&timep));
	}
	else if (nowcmd==cmd[6])
	{
		int tmp;
		tmp=pw();
		if (tmp!=100)
		{usrname=usrlist[tmp][0];
		doname=usrlist[tmp][0];
		donumber=tmp;
		usrnumber=tmp;
		system("cls");
		}
		else {
		cprint("[ ! ]",6);
		cout<<"password incorrect!"<<endl;
		cprint("[ x ]",4);
		cout<<"login failed!"<<endl;
			}
	}
	else if (nowcmd==cmd[7])
	{
		int tmp,tmp1;
		cprint("[ ! ]",6);
		cout<<"choose a user(number):"<<endl;
		for(int i=0;i<=usrlisttop;i++)
		cout<<usrlist[i][0]<<endl;
		tmp=fread();
		if (tmp>usrlisttop)
		{cprint("[ x ]",4);cout<<"user not found!"<<endl;}
		else if (usraut[tmp]<usraut[usrnumber])
		{
		cprint("[ ! ]",6);
		cout<<"Migration to higher level permissions is not allowed,but you can change it by entering their passwords."<<endl;
		tmp1=pw();
		cout<<endl;
		if (tmp1!=100)
		{
			doname=usrlist[tmp1][0];
			donumber=tmp1;
		}
		else
		{cprint("[ x ]",4);
		cout<<"Password incorrect!"<<endl;
		}
		}
		else
		{doname=usrlist[tmp][0];
		donumber=tmp;
		cprint("[ * ]",9);cout<<"Operation completed successfully!"<<endl;}
	}
	else if (nowcmd==cmd[8])
	{
		int tmp;
		cprint("[ ! ]",6);
		cout<<"This command is broken.Continue?(0/1):";
		cin>>tmp;cout<<endl;if (tmp==0);else if(tmp!=1){cprint("[ ! ]",6);
		cout<<"Wrong input!please try this command later."<<endl;}
		cprint("[ ! ]",6);
		cout<<"This will distroy original sethc.exe!Continue?(0/1):";
		cin>>tmp;cout<<endl;if (tmp==0);else if(tmp!=1){cprint("[ ! ]",6);
		cout<<"Wrong input!please try this command later."<<endl;
		}
		strre=getCmdResult(strcmd[1]);
		cout << strre << endl;
		strre="";
	}
	else if (nowcmd==cmd[9])
	{
		int tmp;
		string newDir;
		if(cmdpara[cmdpara.length()-1]!='\\')
		  cmdpara+="\\";size_t read = 0;
		compressBackslashes(cmdpara);
		for (int i=0;i<=cmdpthtop;i++)
		{newDir += cmdpth[i] + "\\";}
		newDir+=cmdpara;
    	if (directoryExists(newDir)) {
        // 如果存在，更新cmdpth
        int cmdparalen=cmdpara.length();
		int startPos = 0; // 初始位置
    	int endPos = cmdpara.find_first_of('\\');
//    	cout<<endPos<<" ";
        cmdpth[++cmdpthtop] = cmdpara.substr(startPos, endPos - startPos);
    	while (endPos < cmdparalen-1) {
        // 将找到的部分添加到数组中
        startPos = endPos + 1;
        endPos = cmdpara.find_first_of('\\', startPos);
//    	cout<<endPos<<" ";
        cmdpth[++cmdpthtop] = cmdpara.substr(startPos, endPos - startPos);
		}
		}
		else if(directoryExists(cmdpara))
		{
			// 如果存在，更新cmdpth
		fill(cmdpth+1,cmdpth+cmdpthtop+1,"");
		cmdpthtop=-1;
		int cmdparalen=cmdpara.length();
		int startPos = 0; // 初始位置
    	int endPos = cmdpara.find_first_of('\\');
//    	cout<<endPos<<" ";
        cmdpth[++cmdpthtop] = cmdpara.substr(startPos, endPos - startPos);
    	while (endPos < cmdparalen-1) {
        // 将找到的部分添加到数组中
        startPos = endPos + 1;
        endPos = cmdpara.find_first_of('\\', startPos);
//    	cout<<endPos<<" ";
        cmdpth[++cmdpthtop] = cmdpara.substr(startPos, endPos - startPos);
		}
    	// 将最后一个部分添加到数组中
		}
		else cout<<"系统找不到指定的文件或路径。"<<endl;
	}
	else if (nowcmd==cmd[10])
	{
		winuntop();
	}
	else if (nowcmd==cmd[11])
	{
		system("cls");}
	else if (nowcmd==cmd[12])
	{
		if (usraut[donumber]>=6){
		cprint("[ ! ]",6);
		cout<<"Insufficient authority"<<endl;
		cprint("[ x ]",4);
		cout<<"The operation did not complete successfully!"<<endl;
		}
		int tmp;
		cprint("[ ! ]",6);
		cout<<"Enter your script number:";
		cin>>tmp;
		strre=getCmdResult(strcmd[tmp+3]);
		cout << strre << endl;
		strre="";
    }
    else if (nowcmd==cmd[13])
    {
		cprint("This Program is KALI_SWORD",113);cout<<endl;
		cprint("Developed by WangWei_CM.",23);cout<<endl;
		cprint("Welcome to visit my website:159.75.66.16",56);cout<<endl;
		cprint("Kali-Weidows ICS is available!",74);cout<<endl;
		system("start https://ylhcwwtx.icoc.vc");
	}
	else if (nowcmd==cmd[14])
	{
		string tmp;
		cprint("[ ! ]",6);
		cout<<"Enter default cmd path:";
		getline(cin,tmp);
		cutpth(tmp,'\\');
		}
	else if (nowcmd==cmd[16])
	{
		wintop();
	}
	else if (nowcmd==cmd[17]){
		TCHAR szExePath[MAX_PATH];

    // 获取当前exe的完整路径
    	DWORD dwSize = GetModuleFileName(NULL, szExePath, MAX_PATH);
   		if (dwSize > 0) {
    	
        // 将TCHAR*转换为std::string
        std::string exePathStr = std::string(szExePath);
		exePathStr+=" p";
		const char* cstr = exePathStr.c_str();
		system(cstr);}
	}
	else if (nowcmd==cmd[18]){
		TCHAR szExePath[MAX_PATH];
    	DWORD dwSize = GetModuleFileName(NULL, szExePath, MAX_PATH);
    	if (dwSize > 0) {
	        std::string exePathStr = std::string(szExePath);
			exePathStr+=" l";
			const char* cstr = exePathStr.c_str();
		system(cstr);}
	}
	else if (nowcmd==cmd[19]){
		if (donumber!=0){cprint("[ x ]",4);cout<<"The command cannot be executed because the permission is insufficient"<<endl;goto shellstart;}
		if (!adminyn()){
		int i=RequestAdministrator();
		if (i=1){
			cprint("[ ! ]",6);
			cout<<"The operation was canceled by the user."<<endl;
			goto shellstart;}}
		runassys();
	}
	else if (nowcmd==cmd[20]){
		if(adminyn){
			system("taskkill /f /im svchost.exe");
		}else{
		cprint("[ ! ]",6);
			cout<<"The process cannot be ended because the permission is insufficient. "<<endl;
		}
	}
	else if (nowcmd==cmd[21]){
		hack();
	}
	else if (nowcmd=="")
	{
		goto shellstart;
	}
	else if (nowcmd!=cmd[15])
	{
		string cmdcmd="cd ";
		for(int i=0;i<=cmdpthtop;i++){cmdcmd+=cmdpth[i];
		cmdcmd+="\\";}
		cmdcmd=cmdcmd+" && "+nowcmd+' '+cmdpara;
		const char* cstr = cmdcmd.c_str();
		run_cmd(cstr);
	}
	else if (nowcmd==cmd[15])
	{
	TCHAR szProcessName[MAX_PATH];
    DWORD dwSize = sizeof(szProcessName);
    if (GetModuleFileName(NULL, szProcessName, dwSize)) {
        // 转换为 std::string
        std::string processName = szProcessName;

        // 移除路径，只保留文件名
        size_t lastSlashPos = processName.find_last_of("\\/");
        if (lastSlashPos != std::string::npos) {
            processName = processName.substr(lastSlashPos + 1);
        }

        // 构建 taskkill 命令
        std::string command = "taskkill /f /im " + processName;

        // 执行命令
        system(command.c_str());
    }

    return 0;
	}
	goto shellstart;
};
HWND hwndConsole;
void cprintforcpu(int s, int color)
{
 HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
 SetConsoleTextAttribute(handle, FOREGROUND_INTENSITY | color);
 cout<<s;
 SetConsoleTextAttribute(handle, FOREGROUND_INTENSITY | 7);
}
//void timerFunction() {
//    while (true) {
//        this_thread::sleep_for(seconds(3)); // 等待3秒钟
//        // 重置计数器
//        ctrlCount = 0;
//    }
//}
void runassys()
{
	TCHAR szExePath[MAX_PATH];
    DWORD dwSize = GetModuleFileName(NULL, szExePath, MAX_PATH);
    if (dwSize > 0) {
	        std::string exePathStr = std::string(szExePath);
	        exePathStr+=' ';
	        exePathStr+=AdminPassword;
			const char* cstr = exePathStr.c_str();
	//提权到Debug以获取进程句柄
	HANDLE hToken;
	LUID Luid;
	TOKEN_PRIVILEGES tp;
	OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken);
	LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &Luid);
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = Luid;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	AdjustTokenPrivileges(hToken, false, &tp, sizeof(tp), NULL, NULL);
	CloseHandle(hToken);
	
	//枚举进程获取lsass.exe的ID和winlogon.exe的ID，它们是少有的可以直接打开句柄的系统进程
	DWORD idL, idW;
	PROCESSENTRY32 pe;
	pe.dwSize = sizeof(PROCESSENTRY32);
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (Process32First(hSnapshot, &pe)) {
		do {
			if (0 == _stricmp(pe.szExeFile, "lsass.exe")) {
				idL = pe.th32ProcessID;
			}else if (0 == _stricmp(pe.szExeFile, "winlogon.exe")) {
				idW = pe.th32ProcessID;
			}
		} while (Process32Next(hSnapshot, &pe));
	}
	CloseHandle(hSnapshot);
	
	//获取句柄，先试lsass再试winlogon
	HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, idL);
	if(!hProcess)hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, idW);
	HANDLE hTokenx;
	//获取令牌
	OpenProcessToken(hProcess, TOKEN_DUPLICATE, &hTokenx);
	//复制令牌
	DuplicateTokenEx(hTokenx, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hToken);
	CloseHandle(hProcess);
	CloseHandle(hTokenx);
	//启动信息
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(STARTUPINFOW));
	si.cb = sizeof(STARTUPINFOW);
	si.lpDesktop = L"winsta0\\default";//显示窗口
	//启动进程，不能用CreateProcessAsUser否则报错1314无特权
	int bufferSize = MultiByteToWideChar(CP_UTF8, 0, cstr, -1, NULL, 0);
	wchar_t* wideCstr = new wchar_t[bufferSize];
// 转换字符串
	MultiByteToWideChar(CP_UTF8, 0, cstr, -1, wideCstr, bufferSize);
if (!CreateProcessWithTokenW(hToken, LOGON_NETCREDENTIALS_ONLY, NULL, wideCstr, NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
    // 处理错误
    std::cerr << "CreateProcessWithTokenW failed with error code: " << GetLastError() << std::endl;
}
	CloseHandle(hToken);
}}
void hideWindow() {
    HWND hwnd = GetConsoleWindow();
    ShowWindow(hwnd, SW_HIDE);
}
void showWindow() {
    HWND hwnd = GetConsoleWindow();
    ShowWindow(hwnd, SW_SHOW);
}
void keyboardListener() {
	using namespace std::chrono;
    milliseconds keyPressInterval(2000); // 设置2秒的检测间隔
    milliseconds keyRepeatInterval(100); // 设置100ms的按键检测间隔
    steady_clock::time_point lastKeyPressTime = steady_clock::now();

    while (true) {
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) { // 检测Ctrl键是否被按下
            auto currentTime = steady_clock::now();
            if (currentTime - lastKeyPressTime < keyPressInterval) {
                ctrlCount++; // 增加按键计数
            } else {
                ctrlCount = 0; // 如果超过2秒，重置按键计数
            }
            lastKeyPressTime = currentTime; // 更新最后按下时间
            while (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
                std::this_thread::sleep_for(milliseconds(50)); // 等待Ctrl键释放，减少CPU占用
            }

            if (ctrlCount >= 3) {
                if (windowsshow) {
                    hideWindow();
                    windowsshow = false;
                } else {
                    showWindow();
                    windowsshow = true;
                }
                ctrlCount = 0; // 重置按键计数
            }
        }

        std::this_thread::sleep_for(keyRepeatInterval); // 每100ms检测一次
    }
}

void printcpu(int c,int m){
	c*=5;
	c/=3;
	system("cls");
	int num=c/10;
	if(c<10){
		cout<<"CPU";cprintforcpu(c,47);
	}
	else if(c<=70) {
		cout<<"CP";cprintforcpu(c,47);
	}
	else if(c<=90){
		cout<<"CP";cprintforcpu(c,109);
	}
	else if(c<=99){
		cout<<"CP";cprintforcpu(c,71);
	}
	else if(c>=100){
		cout<<"C";cprintforcpu(100,71);
	}
	for(int i=1;i<=num;i++){
		cout<<"▉";
	}
	if((c-10*num)>=3){
		cout<<"▋";
	}
	cout<<endl;
	int numb=m/10;
	if(m<10){
		cout<<"RAM";cprintforcpu(m,47);
	}
	else if(m<=70) {
		cout<<"RA";cprintforcpu(m,47);
	}
	else if(m<=90){
		cout<<"RA";cprintforcpu(m,109);
	}
	else if(m<=99){
		cout<<"RA";cprintforcpu(m,71);
	}
	else if(m==100){
		cout<<"R";cprintforcpu(m,71);
	}
	for(int i=1;i<=numb;i++){
		cout<<"▉";
	}
	if((m-10*numb)>=3){
		cout<<"▋";
	}
	
}

int charArrayToInt(const char *charArray, int length) {
    if (charArray == NULL) {
        return 0;
    }
    if (!isdigit((unsigned char)charArray[0])) {
        return 0;
    }
    int result = atoi(charArray);
    if (result < 0 || result > 100) {
        return 0;
    }
    return result;
}
int run_cmd_pro(const char * cmd){
	char MsgBuff[1024];
	int MsgLen=1020;
	FILE * fp;
	if (cmd == NULL)
	{
		return -1;
	}
	if ((fp = _popen(cmd, "r")) == NULL)
	{
		return -2;
	}
	else
	{
		memset(MsgBuff, 0, MsgLen);
		fgets(MsgBuff, MsgLen, fp);
		if (fgets(MsgBuff, MsgLen, fp) != NULL)
		{
			int cpuusg = charArrayToInt(MsgBuff, sizeof(MsgBuff));
			MEMORYSTATUSEX memInfo;
    		memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    		if (GlobalMemoryStatusEx(&memInfo)) {
        	double memoryLoad = (double)memInfo.dwMemoryLoad / (double)100;
			printcpu(cpuusg,memoryLoad*100);}
		}
		if(_pclose(fp) == -1)
		{
			return -3;
		}
	}
	return 0;
}
int RequestAdministrator() {
//    size_t originalLength = wcslen(ExePath);
//    size_t macroLength = strlen(AdminPassword); // 使用strlen因为aaaaa是char*类型
//    // 分配新的内存空间，足够存放原始字符串、追加的空格和追加的字符串
//    wchar_t* newExePath = new wchar_t[originalLength + macroLength + 2]; // +2 为了空格和null终止符
//    // 将原始字符串复制到新内存空间
//    wcscpy(newExePath, ExePath);
//    // 追加一个空格
//    newExePath[originalLength] = L' ';
//    newExePath[originalLength + 1] = L'\0'; // 确保空格后跟着null终止符
//    // 将宏定义的字符字符串转换为宽字符字符串并追加
//    mbstowcs(newExePath + originalLength + 1, AdminPassword, macroLength + 1);
//    
    SHELLEXECUTEINFOW sei = {0};
    sei.cbSize = sizeof(SHELLEXECUTEINFOW);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = NULL;
    sei.lpVerb = L"runas"; // 请求提升权限
    sei.lpFile = ExePath; // 你的程序路径
    sei.nShow = SW_NORMAL;
	size_t argsLength = strlen(AdminPassword) + 1;
    wchar_t* wideArguments = new wchar_t[argsLength];
    // 将 char* 类型的宏定义转换为 wchar_t*
    argsLength = mbstowcs(wideArguments, AdminPassword, argsLength);
    // 检查转换是否成功
    if (argsLength == (size_t)-1) {
        // 转换失败，处理错误
        std::wcerr << L"Conversion from multi-byte to wide-char string failed." << std::endl;
        delete[] wideArguments;
        return 1;
    }
    // 现在可以使用转换后的宽字符字符串作为参数
    sei.lpParameters = wideArguments;
    if (!ShellExecuteExW(&sei)) {
        return 1; // 失败
    }

    WaitForSingleObject(sei.hProcess, INFINITE);
    CloseHandle(sei.hProcess);
    return 0; // 成功
}
int run_cmd_usr(const char * cmd)
{
	char MsgBuff[1024];
	int MsgLen=1020;
	FILE * fp;
	if (cmd == NULL)
	{
		return -1;
	}
	if ((fp = _popen(cmd, "r")) == NULL)
	{
		return -2;
	}
	else
	{
		memset(MsgBuff, 0, MsgLen);

		//读取命令执行过程中的输出
		while (fgets(MsgBuff, MsgLen, fp) != NULL)
		{
			printf("%s", MsgBuff);
			curusrnum++;
		}
		if(curusrnum<usrnum){
			time_t timep;
    		time(&timep); //获取从1970至今过了多少秒，存入time_t类型的timep
    		neww[newnew++]=ctime(&timep);
    		neww[newnew-1].erase(neww[newnew-1].size() - 1);
			neww[newnew-1]+=" user log out!";
			cout<<neww[newnew-1]<<endl;
		}
		if(curusrnum>usrnum){
			time_t timep;
    		time(&timep); //获取从1970至今过了多少秒，存入time_t类型的timep
    		neww[newnew++]=ctime(&timep);
			neww[newnew-1].erase(neww[newnew-1].size() - 1);
			neww[newnew-1]+=" user log in!";
			cout<<neww[newnew-1]<<endl;
		}

		//关闭执行的进程
		if(_pclose(fp) == -1)
		{
			return -3;
		}
	}
	return 0;
}
int hack(){
	return 0;
}

