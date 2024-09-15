# Ksword v 3.1操作说明及实现方法说明

------------
## 1. 软件用途
软件可以用于多个用途。可以安装在自己的电脑上，作为一个快捷工具，也可以稍加改造，用作近源渗透。
## 2.操作说明及功能介绍
有关详细的功能和一些其他没有介绍道的用法，可以在控制台输入help获取帮助。
###软件支持的版本
要完全使用“apt”相关功能，请确保Windows 10 1809及以上或安装了curl到环境变量；
要达到潜伏的目的，请确保Windows 8及以上的操作系统；
要使用用户监视器功能，请确保不是Windows11或更新版本。
建议的操作系统是Windows10 1809~22H2。
###如何进入软件界面
如果是Windows7及以下系统，双击即可运行弹出软件主界面；
如果是Windows8及以上系统，软件会首先调用粘滞键。启用或关闭粘滞键窗口过后5秒内连续按下3次左ctrl键可以显示控制台窗口，否则程序退出。
输入用户对应的密码进入控制台界面，否则程序退出。
### 工作目录
软件设计为命令行交互模式，使用与Windows Powershell一样的语法来切换工作目录。支持的命令：
```shell
    cd
    cd..
    cd ..
    x:\
```
与cmd的命令方式基本一样。细节方面更加接近powershell风格。
###用户身份与权限
软件运行时会要求一次密码输入。每个用户应该有对应的密码，并且互不相同。
Admin账户拥有最高的权限；软件的权限划分为垂直权限划分，即1~10级权限依次增高。部分权限较低的用户无法执行一些指令。
Admin账户可以通过命令行启动而不必输入密码。命令行应为```<程序名> AdminPassword```。
命令```runas```支持某些用户以其他人身份操作控制台。如果目标用户的权限比当前用户高，那么需要输入高权限用户的密码。

### 3Ctrl控制全局隐藏
无论在软件的哪个运行阶段，连续按下3次ctrl键都可以使应用程序隐藏或显示。
### 资源监视器
一个有些简陋的资源监视器，监视cpu和内存利用率，每秒钟刷新显示窗口。
使用命令```cpu```启动，当前进程会转变为一个资源监视器的窗口。如果仍然需要控制台窗口，需要再次启动ksword。
### 用户登录监视器
监视当前计算机中有哪些用户登录，给出新用户登陆时间或用户退出登陆时间。使用命令```net```启动。
### 查看当前时间
使用命令```time```输出当前时间。
### 状态信息条
每次唤出新的会话的时候都会显示状态信息条。其格式为
```
┌ [<软件内账户名>@<程序版本>]~[<当前进程的系统用户名>@<当前主机名>][Admin]
```
其中，[Admin]字样只会在当前进程拥有管理员权限时打印。特别的，如果软件内账户名是最高权限Admin，那么会用红色字体突出显示；如果当前进程的系统用户名是SYSTEM，也会用红色字体突出显示。
### cmd命令执行
该控制台程序在对输入命令识别之后，如果不是该程序所支持的命令，那么就会将这个命令附带当前的工作路径给cmd.exe执行。使用特殊方法连接到cmd.exe而不是使用```system("<command>")```，这样做能使cmd结果实时回显。该功能是阻塞的。cmd命令执行时拥有和当前程序相等的权限。
### 提权（需要软件内最高权限账户）
在软件中，使用```getsys```命令以提升当前权限。如果该软件当前没有管理员权限（可以通过[admin]是否显示来证实），首次执行该命令需要通过UAC防护。如果当前已经拥有管理员权限，新弹出的窗口将会是system权限的窗口（可以通过用户名处是否为标红SYSTEM来证实）。
### 置顶与取消置顶
默认启动时会拥有置顶的窗口。如果需要取消置顶或者重新置顶，应使用```top```与```untop```指令。
### 资源包管理器
沿用了著名的Linux软件包管理器的名字apt。软件启动时首先扫描所有盘下是否有```kswordrc```目录是否存在，如果没有或需要手动更改工作目录，使用```setrcpath```命令，然后输入一个以\结尾的路径。如果设置了根目录为路径，请确保拥有管理员权限，否则功能无法正常运行。
使用```apt update```到默认镜像地址服务器（http://159.75.66.16:80/kswordrc ）下下载version.txt，这里面包含了最新的软件版本号与相关的资源信息。如果使用时ksword不是最新版本，那么命令将拒绝更新。更新完成后，使用```apt upgrade```将配置文件中的内容载入进程。或者使用```apt install <资源名称>```直接安装对应的资源，ksword将会自动读取并重新加载配置文件，然后到镜像地址进行更新。
##3.实现
### 工作目录
使用了一个string类变量```path```来跟踪路径。
在每次输出的时候，校验是否有连续重复的"\"存在：
```cpp
    bool inConsecutive = false; // 标记是否处于连续反斜杠序列中
    for (size_t i = 0; i < path.length(); ++i) {
        // 检查当前字符是否为反斜杠
        if (path[i] == '\\') {
            // 如果已经处于连续反斜杠序列中，则跳过这个字符
            if (inConsecutive) {
                continue;
            }
            // 否则，添加一个反斜杠到结果字符串，并标记进入连续序列
            result += '\\';
            inConsecutive = true;
        } else {
            // 如果当前字符不是反斜杠，添加到结果字符串，并重置标记
            result += path[i];
            inConsecutive = false;
        }
    }
    path=result;
```

在每次输出的时候，为了获取正确大小写的地址：
```cpp
    string tmpcmd;
	//这里是为了处理傻逼的cmd兼容问题，这里通过调用cmd执行一次切换目录+打印目录的方式更新，但是如果是根目录的话该命令会报错，因为cmd使用```d:```来切换盘符。因此这里直接输出单层路径。 
    
    if(path[0]=='c'){
    	tmpcmd="cd "+path+" && cd";
	}else{
		tmpcmd+=path[0];
		tmpcmd+=": && cd "+path+" && cd";
	}
    const string& refpath=tmpcmd;
    if(path.length()>3){
	string pathtmp;
	pathtmp=run_cmd(refpath);
	if(pathtmp.length()!=0)
	pathtmp.pop_back();
	cout<<pathtmp;}
	else cout<<path;
```

cd命令相关处理：
```cpp
	else if (nowcmd==cmd[9])
	{
		if(cmdpara==""){//没有命令参数，只输入了cd，那么进行处理，回到根目录 
	    size_t firstBackslashPos = path.find('\\'); // 查找第一个反斜杠的位置
	    if (firstBackslashPos != std::string::npos) {
	        // 如果找到了反斜杠，截取从开始到反斜杠之前的内容
	        path=path.substr(0, firstBackslashPos);
	        path+="\\";//保证路径规范，以\结尾 
	    }
	    goto shellstart;
		}
		if(cmdpara==".."){//cd ..，回到上一个目录 
		size_t secondSlashPos = path.find_last_of('\\', path.length() - 2);
    	// 如果找到了第二个"\"，并且它不是最后一个字符
    	if (secondSlashPos != std::string::npos && secondSlashPos < path.length() - 1) {
        // 删除从第二个"\"之后的所有内容
        path=path.substr(0, secondSlashPos + 1);
		}goto shellstart;
		}
    const std::string& constpath = cmdpara;
    if(cmdpara[cmdpara.length()-1]!='\\')cmdpara+='\\';
	if(directoryExists(constpath)){path=cmdpara;goto shellstart;}//进入下一级或者多层目录，先检验目录是否存在，然后修改全局变量 
	else{
		string tmppath=path;
		tmppath+=cmdpara;
		const std::string& constpath = tmppath;
		if(directoryExists(constpath)){path=tmppath;goto shellstart;}
	}
	}
	else if (nowcmd=="cd.."){//兼容cd..，实际和cd ..没有任何区别。 
		size_t secondSlashPos = path.find_last_of('\\', path.length() - 2);
    	// 如果找到了第二个"\"，并且它不是最后一个字符
    	if (secondSlashPos != std::string::npos && secondSlashPos < path.length() - 1) {
        // 删除从第二个"\"之后的所有内容
        path=path.substr(0, secondSlashPos + 1);
	}
	}
	else if ((nowcmd.length()==2) && (nowcmd[1]==':')){
		path=nowcmd+"\\";
	}//比如a:,b:,c:等等，切换盘符。应该没有哪个大聪明来个1:吧？
```
### 内部权限管理
使用```pw()```函数来进行用户身份验证，返回值对应相应的用户。所有人的密码被明文储存，因为即使加密也可以被轻松修改。Admin的密码被常量定义。
此外，输入密码时使用一些小手段来防止被看到。相关代码如下
```c++
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
```
### 资源监视器
进行CPU资源的监视，使用```wmic cpu get loadpercentage```命令实时获取CPU利用率。
```cpp
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
```
再使用图形界面实时刷新。
```cpp
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
```
最后使用WindowsAPI调整窗口大小等相关内容，代码如下：(由于是早期史山，没有注释)
```cpp
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
```
### 实时显示用户
实现原理非常简单，和上面几乎没有任何区别。调用cmd的```query user```命令。
### 3ctrl全局隐藏及初始潜伏
程序运行时，首先执行以下代码以停止显示窗口：
```cpp
ShowWindow(GetConsoleWindow(), SW_HIDE);
```
然后，调用粘滞键应用程序。通常情况下我们认为sethc.exe是粘滞键应用程序，但是跟踪其行为后我们发现真正的粘滞键应用程序是```EaseOfAccessDialog.exe```，并且捕获其参数为```211```。因此使用
```cpp
system("EaseOfAccessDialog.exe 211");
```
来进行调用。调用完后，使用一段代码检测是否按下3次ctrl键并且是否显示窗口。
```cpp
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
```
对于运行时的全局隐藏，我们启用一个专门的线程来监控。
```cpp
thread listenerThread(keyboardListener);
```
其中```keyboardListener```函数实现如下：
```cpp
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
```

### 状态信息条
首先，输出部分实现如下：
```cpp
	getpcname(lchostname,lcusrname);
	if(doname!=usrname)
	{
	cout<<"┌ ";
	cout<<"[";
	cprint(charusrname,70);
	cout<<"=>";
	cprint(chardoname,60);
	cout<<"@";
	cout<<Kswordversion;
	cout<<"]";
	}
	else
	{
	cout<<"┌ ";
	cout<<"[";
	cprint(charusrname,70);
	cout<<"@";
	cout<<Kswordversion;
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
```
然后，	我们调用WindowsAPI实现获取计算机名的操作，```getpcname```函数实现如下：
```cpp
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
```
另外，我们使用函数```adminyn```判断当前程序是否拥有跟管理员权限。调用相关的WindowsAPI：
```cpp
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
```
### cmd执行命令并实时回显结果：
```cpp
int out_run_cmd(const char * cmd){
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
```
### 提权
首先，调用adminyn函数确认是否管理员权限。相关函数不再赘述。如果没有管理员权限，那么使用下面的代码调用WindowsAPI请求管理员权限，即UAC：
```cpp
int RequestAdministrator() {
    SHELLEXECUTEINFOW sei = {0};
    sei.cbSize = sizeof(SHELLEXECUTEINFOW);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = NULL;
    sei.lpVerb = L"runas"; // 请求提升权限
    sei.lpFile = ExePath; // 程序路径
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
```
值得注意的是程序如何获取自己的路径。相关代码如下，储存在全局变量。
```cpp
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
```
然后，调用```runassys```函数获取system权限。相关代码如下：
```cpp
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
```
### 资源管理命令apt
首先，程序运行时查找资源路径。实现非常简单，依次查找```<盘符>:\kswordrc```目录是否存在。检验是否存在，调用了WindowsAPI函数，相关代码如下：
```cpp
bool DirectoryExists(const std::wstring& path) {
    DWORD dwAttrib = GetFileAttributesW(path.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && 
            (dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}
```
然后，程序拼接命令并使用curl工具从镜像站点获取资源。
```cpp
else if(nowcmd=="apt"){
			if(cmdpara=="update"){
				string updatecmd;
				updatecmd+="curl -o "+localadd+"version.txt "+"http://"+serverip+":80/kswordrc/"+"version.txt";
				cprint("[ * ]",9);cout<<updatecmd<<endl;
				out_run_cmd(updatecmd.c_str());
				cout<<"Update finished."<<endl;
			}
			else if(cmdpara=="upgrade"){
				string path;path=localadd+"version.txt";
				cprint("[ * ]",9);cout<<"start to update..."<<endl;
				readFile(path);
    		}
    		else if(cmdpara.find("install")==0){
    			cmdpara.erase(0, 8);
    			string path;path=localadd+"version.txt";
				cprint("[ * ]",9);cout<<"start to update..."<<endl;
				readFile(path);
				string pkgname;
				pkgname=cmdpara;
				for(int i=1;i<=rcnumber;i++){
					cprint("[ * ]",9);cout<<aptinfo[i].filename<<aptinfo[i].year<<aptinfo[i].month<<aptinfo[i].day<<aptinfo[i].filesize<<endl;
					if(pkgname==aptinfo[i].filename){
						string installcmd;
						installcmd+="curl -o "+localadd+aptinfo[i].filename+" "+"http://"+serverip+":80/kswordrc/"+aptinfo[i].filename;
						cprint("[ * ]",9);cout<<installcmd<<endl;
						cprint("[ * ]",9);cout<<"Recources found."<<aptinfo[i].filesize<<"MB disk space will be used.Continue?(y/n)";
						char yon;
						cin>>yon;
						if((yon=='N')or(yon=='n'))break;
						if((yon=='Y')or(yon=='y')){
							out_run_cmd(installcmd.c_str());
							goto shellstart;
						}
					}}
			cprint("[ x ]",4);cout<<"Can't find such recuorse."<<endl;
    		}
		}
	else if(nowcmd=="setrcpath"){
			string userinput;
			cin>>userinput;
			const string& userinputref = userinput;
			if (directoryExists(userinputref))localadd=userinput;
			else {cprint("[ x ]",4);cout<<"Dir not exist!"<<endl;}	
			goto shellstart;
	} 
	else if(nowcmd=="check"){
		string checkcmd;
		checkcmd+="ping "+serverip;
		out_run_cmd(checkcmd.c_str());
	}
```
其中，aptinfo是定义的结构体，它用于装载文件中的配置信息。定义如下：
```cpp
struct FileInfo {
    string filename;
    int year, month, day;
    int filesize;
};
```
有关镜像站点的配置，只需要非常简单地使用有公网IP的服务器创建IIS站点，然后将文件资源放到80端口下```/kswordrc```目录中。更新也非常简单，只需要添加文件并且向配置文件中添加相关信息即可。
这是不调用网站API和使用websocket进行交互的最简单的方式，有效减小了维护负担和服务器压力。此外，没有提供命令来切换镜像地址，有自定义需求的用户应该有能力修改源代码并且自行编译。
### 置顶
相关函数非常简单，使用了WindowsAPI。
```cpp
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
```

##有关作者
此程序由作品提交人王伟懿独立编写完成，参考了部分CSDN上的实现源码。所有历史版本均开源分享至Github。
[Github相关链接](https://github.com/WangWei-CM/KSword "Github相关链接")

