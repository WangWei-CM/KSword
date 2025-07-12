//#include "KswordTotalHead.h"
//using namespace std;
//
//
//
//HANDLE KswordSubProcess7 = nullptr;
//HANDLE KswordSubProcess2 = nullptr;
//
//#include <thread>
////关闭自己
//void MainExit() {
//	FreeConsole();
//	CloseHandle(Ksword_Pipe_1);
//	TerminateProcessById(KswordSubProcess7);
//	TerminateProcessById(KswordSubProcess2);
//	return;
//}
//// 安装和卸载钩子的函数
//HHOOK SetInputHook(HINSTANCE hInstance) {
//	HHOOK hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProcInput, hInstance, 0);
//	return hHook;
//}
//
//void Unhook(HHOOK hHook) {
//	UnhookWindowsHookEx(hHook);
//}
//int KMainRunTime = 0;//ksword主程序被第几次启动。这决定管道名字。
//
//int main(int argc, char* argv[]) {
//	// 构建一个字符串来存储所有参数
//	//_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
//	//std::string params;
//
//	//// 遍历所有参数并添加到字符串中
//	//for (int i = 0; i < argc; ++i) {
//	//	params += argv[i];
//	//	if (i < argc - 1) {
//	//		params += " "; // 在参数之间添加空格
//	//	}
//	//}
//
//	//// 将参数字符串转换为宽字符字符串
//	//std::wstring wparams(params.begin(), params.end());
//
//	//// 使用MessageBox显示参数
//	//MessageBox(NULL, wparams.c_str(), L"程序参数", MB_OK | MB_ICONINFORMATION);
//
//	CalcWindowStyle();
//	//for (int i = 1; i <= argc - 1; i++)cout << argv[i] << ' ';
//	if (argc == 2)
//	{
//		if (strcmp(argv[0], "1") == 0) {
//			return KswordMain1(argv[1]);
//		}
//		else if (strcmp(argv[0], "7") == 0) {
//			return KswordMain7();
//		}
//		else if (strcmp(argv[0], "2") == 0) {
//			return KswordMain2();
//		}
//	}
//	if (argc == 1) {
//		//表明这是主程序第argv次被启动
//		KMainRunTime = atoi(argv[0]);
//	}
//	//这是初次启动，管道应该采用x_1
//	//system("color 97");
//	//添加点击穿透
//	DWORD exStyle = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);
//
//	// 添加点击穿透样式
//	exStyle |= WS_EX_TRANSPARENT;
//
//	// 应用新的扩展样式
//	SetWindowLong(GetConsoleWindow(), GWL_EXSTYLE, exStyle);
//
//	// 更新窗口以应用更改
//	SetWindowPos(GetConsoleWindow(), nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
//	SetTopWindow();
//	SetConsola();
//	SetAir(200);
//	HideSide();
//	SetConsoleWindowPosition(LeftColumnStartLocationX, LeftColumnStartLocationY);
//	SetConsoleWindowSize(ColumnWidth, ColumnHeight);
//
//	//bool ctrlPressed = false;
//	//int pressCount = 0;
//	//const int requiredPresses = 3;
//	//const int waitTime = 3000;
//	//for (int i = 0; i < 3000; i += 100) {
//	//	if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
//	//		pressCount++;
//	//		if (pressCount >= 3) {
//	//			break; // 达到3次按下，退出循环
//	//		}
//	//	}
//	//	Sleep(100); // 休眠100毫秒，减少CPU占用
//	//}
//	//// 根据按下次数决定是否显示控制台窗口
//	//if (pressCount >= requiredPresses) {
//	//	ShowWindow();
//	//}
//	//else {
//	//	KMesErr("指定的触发方式未能完成，程序正在退出");
//	//}
//
//
//	findrc();
//	//
// se");
//	system("cls");
//	thread listenerThread(keyboardListener);
//	//thread Top(SetWindowTopmostAndFocus);
//	KswordPrintLogo();
//
//	//打开子进程===========================================================================
//	KswordSelfProc1(KMainRunTime);
//	KswordSubProcess7 = KswordSelfProcess7(KMainRunTime);
//	KswordSubProcess2 = KswordSelfProc2(KMainRunTime);
//
//	//建立与子进程的管道=================================================================
//	KswordSelfPipe1(KMainRunTime);
//	KswordSend1("测试信息：与子进程1成功建立连接");
//
//	const std::string pipeName = "\\\\.\\pipe\\Ksword_Pipe_1_" + to_string(KMainRunTime);
//	char buffer[256];
//	DWORD bytesRead;
//	HANDLE hPipe = nullptr;
//
//	ReadFile(
//		hPipe,
//		buffer,
//		sizeof(buffer) - 1,
//		&bytesRead,
//		NULL
//	);
//	if (!strcmp(buffer, "sub1")) {
//		KMesErr("与子进程通信异常：收到不符合预期的信息");
//	}
//	else {
//		KMesInfo("与子进程1成功建立连接");
//	}
//	SetForegroundWindow(GetConsoleWindow());
//	//进入Ksword Shell主函数===================================================================================
//	shell();
//	//_CrtDumpMemoryLeaks();
//	return KSWORD_SUCCESS_EXIT;
//}
//
//int shell() {
//shellstart:
//	//信息前缀输出=====================================================
//	cout << endl;
//	KEnviProb();
//	cout << "┌ ";
//	cout << "[Ksword " << KSWORD_MAIN_VER_A << "." << KSWORD_MAIN_VER_B << "]~[";
//	if (AuthName == "SYSTEM")cprint("System", 64, 6);
//	else cout << AuthName;
//	cout << '@' << HostName << ']';
//	if (IsAuthAdmin)cprint("[Admin]", 2, 0);
//	cout << endl;
//	cout << "└ ";
//	//cout << 1;
//	string result;
//	bool inConsecutive = false; // 标记是否处于连续反斜杠序列中
//	for (size_t i = 0; i < path.length(); ++i) {
//		// 检查当前字符是否为反斜杠
//		if (path[i] == '\\') {
//			if (inConsecutive)continue;
//			result += '\\';
//			inConsecutive = true;
//		}
//		else {
//			result += path[i];
//			inConsecutive = false;
//		}
//	}
//	//cout << 2;
//	path = result;
//	string tmpcmd;
//	if (path[0] == 'c') {
//		//这里是为了处理傻逼的cmd兼容问题，
//		// 这里通过调用cmd执行一次切换目录
//		// +打印目录的方式更新，但是如果是
//		// 根目录的话该命令会报错，因为cmd使用
//		// ```d:```来切换盘符。因此这里直接输出单层路径。 
//		tmpcmd = "cd " + path + " && cd";
//	}
//	else {
//		tmpcmd += path[0];
//		tmpcmd += ": && cd " + path + " && cd";
//	}
//	//cout << 3;
//	const string& refpath = tmpcmd;
//	if (path.length() > 3) {
//		string pathtmp = GetCmdResult(tmpcmd);
//		if (!pathtmp.empty() && pathtmp.back() == '\n')
//			pathtmp.pop_back();
//		cout << pathtmp;
//	}
//
//	else cout << path;
//	cout << ">";
//	//获取用户输入=================================
//	nowcmd = "";
//	cmdparanum = 0;
//	for (int i = 1; i <= MAX_PARA_NUMBER; i++) {
//		cmdpara[i - 1] = "";
//	}
//	//cin.ignore(numeric_limits<std::streamsize>::max(), '\n');
//	//getline(cin, nowcmd);
//	//判断是否按下三次~按键
//	HINSTANCE hInstanceInput = GetModuleHandle(NULL);
//	HHOOK hHook = SetInputHook(hInstanceInput);
//
//	MSG msg;
//	while ((GetMessage(&msg, NULL, 0, 0))) {
//		TranslateMessage(&msg);
//		DispatchMessage(&msg);
//	}
//	Unhook(hHook);
//	nowcmd = Kgetline();
//	//getline(cin, nowcmd);
//	KswordSend1("Ksword主程序：命令输入" + nowcmd);
//	//请在ksword.cpp中搜索return CallNextHookEx(NULL, nCode, wParam, lParam);并且注释掉来防止钩子向下传递
//	//cout << endl;
//
//	std::string CmdDealresult = nowcmd;
//	size_t pos = 0;
//
//	// 找到第一个不是反引号的字符的位置
//	while (pos < CmdDealresult.length() && CmdDealresult[pos] == '`') {
//		++pos;
//	}
//
//	// 如果字符串以反引号开头，截取字符串
//	if (pos > 0) {
//		CmdDealresult = CmdDealresult.substr(pos);
//	}
//	nowcmd = CmdDealresult;
//
//	int wordCount = 0;
//	int spaceCount = 0;
//
//	std::string word;
//	size_t start = 0;
//	size_t end = 0;
//
//	// 遍历字符串，按空格分割单词
//	while ((end = nowcmd.find(' ', start)) != std::string::npos && wordCount < MAX_PARA_NUMBER - 1) {
//		//cout << "cmdparanum=" << cmdparanum;
//		word = nowcmd.substr(start, end - start);
//		if (!word.empty()) {
//			cmdpara[wordCount++] = word;
//		}
//		start = end + 1;
//		spaceCount++;
//	}
//
//	// 添加最后一个单词
//	if (wordCount < MAX_PARA_NUMBER) {
//		cmdpara[wordCount++] = nowcmd.substr(start);
//	}
//	//cout << "cmdparanum=" << cmdparanum;
//
//	// 计算空格数量，最后一个单词后不应该有空格
//	spaceCount = nowcmd.length() > 0 && nowcmd.back() != ' ' ? spaceCount : spaceCount - 1;
//
//	nowcmd = cmdpara[0];
//	cmdparanum = spaceCount;
//
//	//for (int i = 1; i <= 10; i++) {
//	//	cout << i << "	" << cmdpara[i];
//	//}
//	//cout << "cmdparanum=" << cmdparanum;
//
//	string title;
//	title += path;
//	title += " > ";
//	title += nowcmd;
//	SetConsoleTitleA(title.c_str());
//	size_t spacePos = nowcmd.find(' ');
//
//
//	KswordMainHelp();
//
//	KMainRefresh();
//	KswordSend1("refresh");
//	system("cls");
//	KswordPrintLogo();
//	KMesInfo("已刷新窗口布局");
//	time_t timep;
//	time(&timep);
//	printf("%s", ctime(&timep));
//	cout << path << endl;
//
//	if (cmdpara[1] == "") {//没有命令参数，只输入了cd，那么进行处理，回到根目录 
//		KInlineForCommandcd1();
//		goto shellstart;
//	}
//	if (cmdpara[1] == "..") {//cd ..，回到上一个目录 
//		KInlineForCommandcd2();
//		goto shellstart;
//	}
//	const std::string& constpath = cmdpara[1];
//	if (cmdpara[1][cmdpara[1].length() - 1] != '\\')cmdpara[1] += '\\';
//	if (directoryExists(constpath))
//	{
//		path = cmdpara[1];
//		goto shellstart;
//	}//进入下一级或者多层目录，先检验目录是否存在，然后修改全局变量 
//	else {
//		string tmppath = path;
//		tmppath += cmdpara[1];
//		const std::string& constpath = tmppath;
//		if (directoryExists(constpath)) { path = tmppath; goto shellstart; }
//	}
//
//	KInlineForCommandcd3();
//	path = nowcmd + "\\";
//	ThreadTopWorking = 0;
//	Sleep(100);
//	UnTopWindow();
//
//	// 获取窗口的扩展样式
//	LONG style = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);
//
//	// 检查是否设置了WS_EX_TOPMOST标志
//	if (style & WS_EX_TOPMOST) {
//		std::cout << "控制台窗口被置顶。" << std::endl;
//	}
//	else {
//		std::cout << "控制台窗口没有被置顶。" << std::endl;
//	}
//
//	;
//	system("cls");
//
//
//	SetTopWindow();
//	ThreadTopWorking = 1;
//
//	if (!IsAdmin()) {
//		if (RequestAdmin(StringToWString(GetSelfPath())) == KSWORD_ERROR_EXIT) {
//			KMesErr("出现错误，无法启动");
//			goto shellstart;
//		}
//		else {
//			MainExit();
//			return KSWORD_SUCCESS_EXIT;
//		}
//	}
//	GetProgramPath();
//	if (GetSystem(to_string(KMainRunTime + 1).c_str()) == KSWORD_SUCCESS_EXIT) {
//		MainExit();
//		return KSWORD_SUCCESS_EXIT;
//	}
//
//	if (cmdpara[1] == "update") {
//		KInlineForCommandapt1();
//	}
//	else if (cmdpara[1] == "upgrade") {
//		KInlineForCommandapt2();
//	}
//	else if (cmdpara[1] == "install") {
//		string path; path = localadd + "version.txt";
//		KMesInfo("开始更新...");
//		readFile(path);
//		string pkgname;
//		pkgname = cmdpara[2];
//		for (int i = 1; i <= rcnumber; i++) {
//			//cprint("[ * ]", 9); cout << aptinfo[i].filename << aptinfo[i].year << aptinfo[i].month << aptinfo[i].day << aptinfo[i].filesize << endl;
//			if (pkgname == aptinfo[i].filename) {
//				string installcmd;
//				installcmd += "curl -o " + localadd + aptinfo[i].filename + " " + "http://" + serverip + ":80/kswordrc/" + aptinfo[i].filename;
//				cout << installcmd;
//				//Sleep(500);
//				cout << "Recources found." << to_string(aptinfo[i].filesize) << "MB disk space will be used.Continue ? (y / n)";
//				char yon;
//				cin >> yon;
//				if ((yon == 'N') or (yon == 'n'))break;
//				if ((yon == 'Y') or (yon == 'y')) {
//					RunCmdNow(installcmd.c_str());
//					goto shellstart;
//				}
//			}
//		}
//		KMesErr("找不到指定的资源文件。");
//
//		KInlineForCommandapt3();
//
//		string scriptcmd = "";
//		if (localadd[0] != 'c') {
//			scriptcmd += localadd[0];
//			scriptcmd += ": && ";
//		}
//		scriptcmd += "cd ";
//		scriptcmd += localadd;
//		scriptcmd += " && ";
//		for (int i = 1; i <= cmdparanum; i++) {
//			scriptcmd += cmdpara[i];
//			scriptcmd += " ";
//		}
//		cout << scriptcmd << endl;
//		RunCmdNow(scriptcmd);
//		MainExit();
//
//		string cmdcmd;
//		if (path != "") {
//			if (path.length() <= 3) {
//				if (path[0] == 'c') {
//					cmdcmd = "cd\\ && " + nowcmd + ' ';
//					for (int i = 1; i <= cmdparanum; i++) {
//						cmdcmd += cmdpara[i];
//					}
//				}
//				else {
//					cmdcmd = path[0];
//					cmdcmd += ": && ";
//					cmdcmd += nowcmd + ' ';
//					for (int i = 1; i <= cmdparanum; i++) {
//						cmdcmd += cmdpara[i];
//					}
//				}
//				cout << "Running command:" << cmdcmd << endl;
//			}
//			else {
//				if (path[0] == 'c') {
//					cmdcmd = "cd " + path + " && " + nowcmd + ' ';
//					for (int i = 1; i <= cmdparanum; i++) {
//						cmdcmd += cmdpara[i];
//					}
//				}
//				else {
//					cmdcmd += path[0];
//					cmdcmd += ": && cd \"" + path;
//					cmdcmd += "\" && ";
//					cmdcmd += nowcmd + ' ';
//					for (int i = 1; i <= cmdparanum; i++) {
//						cmdcmd += cmdpara[i];
//					}
//				}
//				cout << "Running command:" << cmdcmd << endl;
//			}
//			const char* cstr = cmdcmd.c_str();
//			RunCmdNow(cstr);
//		}
//
//	}
//}