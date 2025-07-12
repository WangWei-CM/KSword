#ifdef KSWORD_WITH_COMMAND
#include "../../KswordTotalHead.h"
using namespace std;
void GetProcessInformation(DWORD processId);

std::wstring FormatDLLPath(const std::wstring& path)
{
	const std::wstring system32 = L"C:\\Windows\\System32\\";
	if (path.substr(0, system32.size()) == system32)
	{
		return L"..\\" + path.substr(system32.size());
	}
	const std::wstring system33 = L"C:\\Windows\\SYSTEM32\\";
	if (path.substr(0, system33.size()) == system33)
	{
		return L"..\\" + path.substr(system33.size());
	}
	const std::wstring system34 = L"C:\\Windows\\system32\\";
	if (path.substr(0, system34.size()) == system34)
	{
		return L"..\\" + path.substr(system34.size());
	}
	return path;
}


void KswordProcManager() {
	KswordProcMgrStart:
	cout << "请选择你要操作的进程（PID），exit退出，tasklist列出所有任务，bat批量管理进程：";
	string userinput = Kgetline();
	if (userinput == "exit") {
		return;
	}
	else if (userinput == "bat") {
#ifndef _M_IX86
		vector<int> proc;
		cout << "批处理模式" << endl <<
			"1>add <PID>：向进程列表中添加进程PID" << endl <<
			"2>rm <PID>：从进程列表中移除PID" << endl <<
			"3>结束进程并清空进程列表" << endl <<
			"4>多线程批量挂起进程" << endl <<
			"5>多线程批量恢复进程" << endl <<
			"6>清空进程列表" << endl <<
			"7>添加区间的进程PID" << endl <<
			"8>查看已经选中的进程列表" << endl; 
		while (1) {
			cprint("Proc-Bat", 7, 2); cout << ">"; string input = Kgetline();
			if (input.substr(0, 3) == "add") {
				std::istringstream iss(input.substr(4)); // 跳过 "add" 和空格
				std::string token;
				cout << "<·添加了以下PID到列表：";
				while (iss >> token) {
					// 将字符串转换为整数并添加到vector
					if ((std::find(proc.begin(), proc.end(), StringToInt(token)) == proc.end())&&(ExistProcess(StringToInt(token)))) {
						proc.push_back(StringToInt(token));
						cout << StringToInt(token) << ";";
					}
				}
				cout << endl;
			}else if (input.substr(0, 2) == "rm") {
				std::istringstream iss(input.substr(3)); // 跳过 "rm" 和空格
				std::string token;
				cout << "<·将以下PID从列表中移除：";
				while (iss >> token) {
					int value = StringToInt(token); // 将字符串转换为整数
					// 在 vector 中查找并移除匹配的元素
					auto it = std::find(proc.begin(), proc.end(), value);
					if (it != proc.end()) {
						cout << value << ";";
						proc.erase(it); // 移除找到的元素
					}
				}cout << endl;
			}
			else if (input == "3" || input=="kill") {
				cout << "请选择你的方案：" << endl <<
					"1>taskkill结束" << endl <<
					"2>taskkill带/f结束" << endl <<
					"3>TerminateProcess结束" << endl <<
					"4>TerminateThread结束" << endl <<
					"5>NtTerminate结束" << endl <<
					"6>作业对象结束" << endl;
				int killmethod = StringToInt(Kgetline());
				std::vector<std::thread> threads;
				std::latch latch(proc.size()); // 创建一个 latch，用于同步所有线程

				// 创建线程并等待同步开始
				for (int value : proc) {
					threads.emplace_back([killmethod, value, &latch]() {
						latch.arrive_and_wait(); // 等待所有线程就绪
						KillProcess(killmethod, value);      // 调用处理函数
						});
				}

				// 等待所有线程完成
				for (auto& thread : threads) {
					if (thread.joinable()) {
						thread.join();
					}
				}
			}
			else if (input == "4"||input=="froze") {
				std::vector<std::thread> threads;
				std::latch latch(proc.size()); // 创建一个 latch，用于同步所有线程

				// 创建线程并等待同步开始
				for (int value : proc) {
					threads.emplace_back([value,&latch]() {
						latch.arrive_and_wait(); // 等待所有线程就绪
						SuspendProcess(value);      // 调用处理函数
						});
				}
				for (auto& thread : threads) {
					if (thread.joinable()) {
						thread.join();
					}
				}
			}
			else if(input=="5"||input=="unfroze") {
				std::vector<std::thread> threads;
				std::latch latch(proc.size()); // 创建一个 latch，用于同步所有线程

				// 创建线程并等待同步开始
				for (int value : proc) {
					threads.emplace_back([value, &latch]() {
						latch.arrive_and_wait(); // 等待所有线程就绪
						UnSuspendProcess(value);      // 调用处理函数
						});
				}
				for (auto& thread : threads) {
					if (thread.joinable()) {
						thread.join();
					}
				}
			}
			else if(input=="6"||input=="clr") {
				proc.clear();
			}
			else if(input=="7"){
				cout << "请输入最小范围（包含）：>";
				int minnum = StringToInt(Kgetline());
				cout << "请输入最大范围（包含）：>";
				int maxnum = StringToInt(Kgetline());
				for (int i = minnum; i <= maxnum; i++) {
					if ((std::find(proc.begin(), proc.end(), i) == proc.end()) && ExistProcess(i))
						proc.push_back(i);
				}
			}
			else if (input == "8"||input=="ls") {
				for (int pid : proc) {
					std::cout << pid << "\t";
				}
				cout << endl;
			}
			else if (input == "0" || input == "exit")break;
			else {
				cout << "<·未定义的语法" << endl;
			}
		}
		goto KswordProcMgrStart;
#endif
#ifdef _M_IX86
		KMesErr("批处理不适用于x86处理器，请更换64位程序，然后重试");
#endif

	}
	else if (userinput == "tasklist") {
		SetWindowNormal();
		Ktasklist();
		goto KswordProcMgrStart;
	}
	else if (userinput.length() == 1) {
		Ktasklist(userinput);
		goto KswordProcMgrStart;
	}
	int taskproc;
	if (!IsInt(userinput)) {
		taskproc=GetPIDByIM(userinput);
		cout << "自动锁定PID为" << taskproc << "的进程" << endl;
	}
	else
		taskproc = StringToInt(userinput);
	if (!ExistProcess(taskproc))
	{
		KMesErr("指定的进程不存在");
		return;
	}
	cout << "请选择你要进行的操作：" << endl <<
		"1>检查DLL\t\t2>结束进程" << endl <<
		"3>挂起进程\t\t4>恢复进程" << endl <<
		"5>查看进程相关信息\t6>设为关键进程" << endl <<
		"7>取消关键进程\t\t8>测试访问句柄" << endl;
	int method = StringToInt(Kgetline());
	
	if (method == 1) {
		std::vector<std::wstring> dlls = GetDLLsByPID(taskproc);
		std::cout << "PID为 "<<taskproc<<" 的进程加载的DLL模块：" << std::endl;
		if (ColumnHeight - 5 <= dlls.size())SetWindowNormal();
		for (size_t i = 0; i < dlls.size(); i += 2) {
			std::string formattedPath1 = WstringToString(FormatDLLPath(dlls[i]));
			cprint("Module:", 1, 0); std::cout << formattedPath1;

			if (i + 1 < dlls.size()) {
				std::string formattedPath2 = WstringToString(FormatDLLPath(dlls[i+1]));
				cprint("\tModule:", 1, 0); std::cout << formattedPath2;
			}

			std::cout << std::endl;
		}

	}
	else if (method == 2) {
		cout << "===================" << endl;
		cout << "请选择你的方案：" << endl <<
			"1>taskkill结束" << endl <<
			"2>taskkill带/f结束" << endl <<
			"3>TerminateProcess结束" << endl <<
			"4>TerminateThread结束" << endl <<
			"5>NtTerminate结束" << endl <<
			"6>作业对象结束" << endl;
		int killmethod = StringToInt(Kgetline());
	KillProcessStart:
		if (killmethod == 1) {
			KillProcess1(taskproc);
		}
		else if (killmethod == 2) {
			KillProcess2(taskproc);
		}
		else if (killmethod == 3) {
			HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, taskproc);

			if (hProcess == NULL)
			{
				KMesErr("打开目标进程句柄失败，错误代码：" + to_string(GetLastError()));
				return;
			}
			if (!KillProcess(3, hProcess))
				cout << "结束进程失败，是否尝试下一种方案？(y/n)(def=n):";
			if (Kgetline() == "y") {
				killmethod = 4;
				goto KillProcessStart;
			}
			else
				return;
		}
		else if (killmethod == 4) {
			HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, taskproc);

			if (hProcess == NULL)
			{
				KMesErr("打开目标进程句柄失败，错误代码：" + to_string(GetLastError()));
				return;
			}
			if (!KillProcess(4, hProcess))
				cout << "结束进程失败，是否尝试下一种方案？(y/n)(def=n):";
			if (Kgetline() == "y") {
				killmethod = 5;
				goto KillProcessStart;
			}
			else
				return;
		}
		else if (killmethod == 5) {
			HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, taskproc);

			if (hProcess == NULL)
			{
				KMesErr("打开目标进程句柄失败，错误代码：" + to_string(GetLastError()));
				return;
			}
			if (!KillProcess(5, hProcess))
				cout << "结束进程失败，是否尝试下一种方案？(y/n)(def=n):";
			if (Kgetline() == "y") {
				killmethod = 6;
				goto KillProcessStart;
			}
			else
				return;

		}
		else if (killmethod == 6) {
			HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, taskproc);
			if (hProcess == NULL)
			{
				KMesErr("打开目标进程句柄失败，错误代码：" + to_string(GetLastError()));
				return;
			}
			if (!KillProcess(6, hProcess))
				cout << "结束进程失败." << endl;
		}
		else KMesErr("未定义的操作方式");
		//TerminateProcessById(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, taskproc));
		return;
	}
	else if (method == 3) {
		SuspendProcess(taskproc);
		return;
	}
	else if (method == 4) {
		UnSuspendProcess(taskproc);
		return;
	}
	else if (method == 5) {
		HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (hSnapshot == INVALID_HANDLE_VALUE)
		{
			std::cerr << "CreateToolhelp32Snapshot failed: " << GetLastError() << std::endl;
			return;
		}

		// 准备 PROCESSENTRY32 结构
		PROCESSENTRY32 pe = { sizeof(PROCESSENTRY32) };

		// 获取第一个进程信息
		if (!Process32First(hSnapshot, &pe))
		{
			std::cerr << "Process32First failed: " << GetLastError() << std::endl;
			CloseHandle(hSnapshot);
			return;
		}

		// 遍历所有进程
		bool found = false;
		do
		{
			if (pe.th32ProcessID == taskproc)
			{
				std::string processName = WstringToString(pe.szExeFile);
				cprint("Process Name:", 1, 0);cout << "\t"<< processName << "\n";
				cprint("PID", 1, 0); cout << "\t" << pe.th32ProcessID << "\n";
				cprint("Parent PID:", 1, 0); cout << "\t" << pe.th32ParentProcessID << "\n";
				cprint("Thread Num:", 1, 0); cout << "\t" << pe.cntThreads << "\n";
				cprint("Base Priority:", 1, 0); cout << "\t"<< pe.pcPriClassBase << "\n";
				found = true;
				break;
			}
		} while (Process32Next(hSnapshot, &pe));

		// 关闭快照句柄
		CloseHandle(hSnapshot);

		if (!found)
		{
			KMesErr("找不到PID为" + to_string(taskproc) + "的进程");
		}
		GetProcessInformation(taskproc);
	}
	else if (method == 6) {
		SetKeyProcess(GetProcessHandleByPID(taskproc), 1);
	}
	else if (method == 7) {
		SetKeyProcess(GetProcessHandleByPID(taskproc), 0);
	}
	else if (method == 8) {

	// 权限标志与名称的映射表
	std::vector<std::pair<DWORD, const char*>> accessModes = {
		{PROCESS_ALL_ACCESS, "PROCESS_ALL_ACCESS"},
		{PROCESS_QUERY_INFORMATION, "PROCESS_QUERY_INFORMATION"},
		{PROCESS_QUERY_LIMITED_INFORMATION, "PROCESS_QUERY_LIMITED_INFORMATION"},
		{PROCESS_VM_READ, "PROCESS_VM_READ"},
		{PROCESS_VM_WRITE, "PROCESS_VM_WRITE"},
		{PROCESS_VM_OPERATION, "PROCESS_VM_OPERATION"},
		{PROCESS_CREATE_THREAD, "PROCESS_CREATE_THREAD"},
		{PROCESS_TERMINATE, "PROCESS_TERMINATE"},
		{PROCESS_SUSPEND_RESUME, "PROCESS_SUSPEND_RESUME"},
		{PROCESS_SET_INFORMATION, "PROCESS_SET_INFORMATION"},
		{PROCESS_SET_QUOTA, "PROCESS_SET_QUOTA"},
		{PROCESS_DUP_HANDLE, "PROCESS_DUP_HANDLE"},
		{SYNCHRONIZE, "SYNCHRONIZE"},
		{PROCESS_CREATE_PROCESS, "PROCESS_CREATE_PROCESS"},
		{0x1000, "PROCESS_SET_LIMITED_INFORMATION"}, // 部分版本定义不同
		{0x00010000, "PROCESS_DELETE"}
	};
	for (const auto& mode : accessModes) {
		HANDLE hProcess = OpenProcess(mode.first, FALSE, taskproc);
		DWORD err = GetLastError();

		std::cout << std::left << std::setw(35) << mode.second
			<< " : ";
		if (hProcess != NULL) {
			cprint("[成功]", 10, 0);
			CloseHandle(hProcess);
		}
		else {
			cprint("[失败]", 4, 0);
			if (err == 5) std::cout << " [拒绝访问]";
			else if (err == 87) std::cout << " [参数错误]";
			else cout << "错误代码" << err;
		}
		std::cout << std::endl;
	}
	}
}


#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

// 定义结构体
typedef NTSTATUS(NTAPI* _NtQuerySystemInformation)(
	ULONG SystemInformationClass,
	PVOID SystemInformation,
	ULONG SystemInformationLength,
	PULONG ReturnLength
	);

//typedef struct _SYSTEM_PROCESS_INFORMATION {
//	ULONG NextEntryOffset;
//	ULONG NumberOfThreads;
//	LARGE_INTEGER SpareLi1;
//	LARGE_INTEGER SpareLi2;
//	LARGE_INTEGER SpareLi3;
//	LARGE_INTEGER CreateTime;
//	LARGE_INTEGER UserTime;
//	LARGE_INTEGER KernelTime;
//	UNICODE_STRING ImageName;
//	ULONG BasePriority;
//	ULONG UniqueProcessId;
//	ULONG InheritedFromUniqueProcessId;
//	ULONG HandleCount;
//	ULONG SessionId;
//	ULONG_PTR PageDirectoryBase;
//	SIZE_T PeakVirtualSize;
//	SIZE_T VirtualSize;
//	ULONG PageFaultCount;
//	SIZE_T PeakWorkingSetSize;
//	SIZE_T WorkingSetSize;
//	SIZE_T QuotaPeakPagedPoolUsage;
//	SIZE_T QuotaPagedPoolUsage;
//	SIZE_T QuotaPeakNonPagedPoolUsage;
//	SIZE_T QuotaNonPagedPoolUsage;
//	SIZE_T PagefileUsage;
//	SIZE_T PeakPagefileUsage;
//} 
//_SYSTEM_PROCESS_INFORMATION SYSTEM_PROCESS_INFORMATION, * PSYSTEM_PROCESS_INFORMATION;

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "Psapi.lib")

// 定义常量
#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif


void GetProcessInformation(DWORD processId) {
	// 获取 NtQuerySystemInformation 函数指针
	_NtQuerySystemInformation NtQuerySystemInformation =
		(_NtQuerySystemInformation)GetProcAddress(GetModuleHandle(L"ntdll.dll"), "NtQuerySystemInformation");
	if (!NtQuerySystemInformation) {
		std::cerr << "Could not find NtQuerySystemInformation" << std::endl;
		return;
	}

	// 动态分配内存
	PVOID buffer = nullptr;
	ULONG bufferSize = 0x1000;
	NTSTATUS status;

	do {
		buffer = malloc(bufferSize);
		if (!buffer) {
			std::cerr << "Memory allocation failed" << std::endl;
			return;
		}

		status = NtQuerySystemInformation((ULONG)5, buffer, bufferSize, &bufferSize);
		if (status == STATUS_INFO_LENGTH_MISMATCH) {
			free(buffer);
			buffer = nullptr;
		}
	} while (status == STATUS_INFO_LENGTH_MISMATCH);

	if (!NT_SUCCESS(status)) {
		std::cerr << "NtQuerySystemInformation failed with status: " << status << std::endl;
		free(buffer);
		return;
	}

	// 遍历返回的进程信息
	PSYSTEM_PROCESS_INFORMATION current = (PSYSTEM_PROCESS_INFORMATION)buffer;
	do {
		if ((DWORD)current->UniqueProcessId == processId) {
			
			cprint("下一入口偏移量：", 2, 0); cout << "\t"; std::wcout << current->NextEntryOffset << std::endl;
			cprint("线程数量：  ", 2, 0); cout << "\t"; std::wcout << current->NumberOfThreads << std::endl;
			cprint("映像名称：  ", 2, 0); cout << "\t"; wcout<< current->ImageName.Buffer << std::endl;
			cprint("基线优先级：", 2, 0); cout << "\t"; wcout << current->BasePriority << std::endl;
			cprint("进程唯一标识符：", 2, 0); cout << "\t"; wcout << current->UniqueProcessId << std::endl;
			cprint("句柄数量：  ", 2, 0); cout << "\t"; wcout << current->HandleCount << std::endl;
			cprint("会话ID：   ", 2, 0); cout << "\t"; wcout << current->SessionId << std::endl;
			//cprint("虚拟内存最大用量：", 2, 0); cout << "\t"; wcout << current->PeakVirtualSize << std::endl;
			//cprint("虚拟内存用量：", 2, 0); cout << "\t"; wcout << current->VirtualSize << std::endl;
			//cprint("最大工作集大小：", 2, 0); cout << "\t"; wcout << current->PeakWorkingSetSize << std::endl;
			//cprint("工作集大小： ", 2, 0); cout << "\t"; wcout << current->WorkingSetSize << std::endl;
			//cprint("分页缓冲池用量：", 2, 0); cout << "\t"; wcout << current->QuotaPagedPoolUsage << std::endl;
			//cprint("非分页缓冲池用量：", 2, 0); cout << "\t"; wcout << current->QuotaNonPagedPoolUsage << std::endl;
			//cprint("页面文件大小：", 2, 0); cout << "\t"; wcout << current->PagefileUsage << std::endl;
			//cprint("最大页面大小：", 2, 0); cout << "\t"; wcout << current->PeakPagefileUsage << std::endl;
			//cprint("私有页面数量：", 2, 0); cout << "\t"; wcout << current->PrivatePageCount << std::endl;
			cprint("虚拟内存最大用量：", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->PeakVirtualSize) << " MB" << std::endl;
			cprint("虚拟内存用量：  ", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->VirtualSize) << " MB" << std::endl;
			cprint("最大工作集大小：", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->PeakWorkingSetSize) << " MB" << std::endl;
			cprint("工作集大小：    ", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->WorkingSetSize) << " MB" << std::endl;
			cprint("分页缓冲池用量： ", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->QuotaPagedPoolUsage) << " MB" << std::endl;
			cprint("非分页缓冲池用量：", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->QuotaNonPagedPoolUsage) << " MB" << std::endl;
			cprint("页面文件大小：  ", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->PagefileUsage) << " MB" << std::endl;
			cprint("最大页面大小：  ", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->PeakPagefileUsage) << " MB" << std::endl;
			cprint("私有页面数量：  ", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->PrivatePageCount) << " MB" << std::endl;
			break;
		}
		current = (PSYSTEM_PROCESS_INFORMATION)((BYTE*)current + current->NextEntryOffset);
	} while (current->NextEntryOffset != 0);

	free(buffer);
}
#endif