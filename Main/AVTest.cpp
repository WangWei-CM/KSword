//#include "KswordTotalHead.h"
//using namespace std;
//
//
//
//HANDLE KswordSubProcess7 = nullptr;
//HANDLE KswordSubProcess2 = nullptr;
//
//#include <thread>
////�ر��Լ�
//void MainExit() {
//	FreeConsole();
//	CloseHandle(Ksword_Pipe_1);
//	TerminateProcessById(KswordSubProcess7);
//	TerminateProcessById(KswordSubProcess2);
//	return;
//}
//// ��װ��ж�ع��ӵĺ���
//HHOOK SetInputHook(HINSTANCE hInstance) {
//	HHOOK hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProcInput, hInstance, 0);
//	return hHook;
//}
//
//void Unhook(HHOOK hHook) {
//	UnhookWindowsHookEx(hHook);
//}
//int KMainRunTime = 0;//ksword�����򱻵ڼ���������������ܵ����֡�
//
//int main(int argc, char* argv[]) {
//	// ����һ���ַ������洢���в���
//	//_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
//	//std::string params;
//
//	//// �������в�������ӵ��ַ�����
//	//for (int i = 0; i < argc; ++i) {
//	//	params += argv[i];
//	//	if (i < argc - 1) {
//	//		params += " "; // �ڲ���֮����ӿո�
//	//	}
//	//}
//
//	//// �������ַ���ת��Ϊ���ַ��ַ���
//	//std::wstring wparams(params.begin(), params.end());
//
//	//// ʹ��MessageBox��ʾ����
//	//MessageBox(NULL, wparams.c_str(), L"�������", MB_OK | MB_ICONINFORMATION);
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
//		//���������������argv�α�����
//		KMainRunTime = atoi(argv[0]);
//	}
//	//���ǳ����������ܵ�Ӧ�ò���x_1
//	//system("color 97");
//	//��ӵ����͸
//	DWORD exStyle = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);
//
//	// ��ӵ����͸��ʽ
//	exStyle |= WS_EX_TRANSPARENT;
//
//	// Ӧ���µ���չ��ʽ
//	SetWindowLong(GetConsoleWindow(), GWL_EXSTYLE, exStyle);
//
//	// ���´�����Ӧ�ø���
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
//	//			break; // �ﵽ3�ΰ��£��˳�ѭ��
//	//		}
//	//	}
//	//	Sleep(100); // ����100���룬����CPUռ��
//	//}
//	//// ���ݰ��´��������Ƿ���ʾ����̨����
//	//if (pressCount >= requiredPresses) {
//	//	ShowWindow();
//	//}
//	//else {
//	//	KMesErr("ָ���Ĵ�����ʽδ����ɣ����������˳�");
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
//	//���ӽ���===========================================================================
//	KswordSelfProc1(KMainRunTime);
//	KswordSubProcess7 = KswordSelfProcess7(KMainRunTime);
//	KswordSubProcess2 = KswordSelfProc2(KMainRunTime);
//
//	//�������ӽ��̵Ĺܵ�=================================================================
//	KswordSelfPipe1(KMainRunTime);
//	KswordSend1("������Ϣ�����ӽ���1�ɹ���������");
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
//		KMesErr("���ӽ���ͨ���쳣���յ�������Ԥ�ڵ���Ϣ");
//	}
//	else {
//		KMesInfo("���ӽ���1�ɹ���������");
//	}
//	SetForegroundWindow(GetConsoleWindow());
//	//����Ksword Shell������===================================================================================
//	shell();
//	//_CrtDumpMemoryLeaks();
//	return KSWORD_SUCCESS_EXIT;
//}
//
//int shell() {
//shellstart:
//	//��Ϣǰ׺���=====================================================
//	cout << endl;
//	KEnviProb();
//	cout << "�� ";
//	cout << "[Ksword " << KSWORD_MAIN_VER_A << "." << KSWORD_MAIN_VER_B << "]~[";
//	if (AuthName == "SYSTEM")cprint("System", 64, 6);
//	else cout << AuthName;
//	cout << '@' << HostName << ']';
//	if (IsAuthAdmin)cprint("[Admin]", 2, 0);
//	cout << endl;
//	cout << "�� ";
//	//cout << 1;
//	string result;
//	bool inConsecutive = false; // ����Ƿ���������б��������
//	for (size_t i = 0; i < path.length(); ++i) {
//		// ��鵱ǰ�ַ��Ƿ�Ϊ��б��
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
//		//������Ϊ�˴���ɵ�Ƶ�cmd�������⣬
//		// ����ͨ������cmdִ��һ���л�Ŀ¼
//		// +��ӡĿ¼�ķ�ʽ���£����������
//		// ��Ŀ¼�Ļ�������ᱨ����Ϊcmdʹ��
//		// ```d:```���л��̷����������ֱ���������·���� 
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
//	//��ȡ�û�����=================================
//	nowcmd = "";
//	cmdparanum = 0;
//	for (int i = 1; i <= MAX_PARA_NUMBER; i++) {
//		cmdpara[i - 1] = "";
//	}
//	//cin.ignore(numeric_limits<std::streamsize>::max(), '\n');
//	//getline(cin, nowcmd);
//	//�ж��Ƿ�������~����
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
//	KswordSend1("Ksword��������������" + nowcmd);
//	//����ksword.cpp������return CallNextHookEx(NULL, nCode, wParam, lParam);����ע�͵�����ֹ�������´���
//	//cout << endl;
//
//	std::string CmdDealresult = nowcmd;
//	size_t pos = 0;
//
//	// �ҵ���һ�����Ƿ����ŵ��ַ���λ��
//	while (pos < CmdDealresult.length() && CmdDealresult[pos] == '`') {
//		++pos;
//	}
//
//	// ����ַ����Է����ſ�ͷ����ȡ�ַ���
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
//	// �����ַ��������ո�ָ��
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
//	// ������һ������
//	if (wordCount < MAX_PARA_NUMBER) {
//		cmdpara[wordCount++] = nowcmd.substr(start);
//	}
//	//cout << "cmdparanum=" << cmdparanum;
//
//	// ����ո����������һ�����ʺ�Ӧ���пո�
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
//	KMesInfo("��ˢ�´��ڲ���");
//	time_t timep;
//	time(&timep);
//	printf("%s", ctime(&timep));
//	cout << path << endl;
//
//	if (cmdpara[1] == "") {//û�����������ֻ������cd����ô���д����ص���Ŀ¼ 
//		KInlineForCommandcd1();
//		goto shellstart;
//	}
//	if (cmdpara[1] == "..") {//cd ..���ص���һ��Ŀ¼ 
//		KInlineForCommandcd2();
//		goto shellstart;
//	}
//	const std::string& constpath = cmdpara[1];
//	if (cmdpara[1][cmdpara[1].length() - 1] != '\\')cmdpara[1] += '\\';
//	if (directoryExists(constpath))
//	{
//		path = cmdpara[1];
//		goto shellstart;
//	}//������һ�����߶��Ŀ¼���ȼ���Ŀ¼�Ƿ���ڣ�Ȼ���޸�ȫ�ֱ��� 
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
//	// ��ȡ���ڵ���չ��ʽ
//	LONG style = GetWindowLong(GetConsoleWindow(), GWL_EXSTYLE);
//
//	// ����Ƿ�������WS_EX_TOPMOST��־
//	if (style & WS_EX_TOPMOST) {
//		std::cout << "����̨���ڱ��ö���" << std::endl;
//	}
//	else {
//		std::cout << "����̨����û�б��ö���" << std::endl;
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
//			KMesErr("���ִ����޷�����");
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
//		KMesInfo("��ʼ����...");
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
//		KMesErr("�Ҳ���ָ������Դ�ļ���");
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