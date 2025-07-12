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
	cout << "��ѡ����Ҫ�����Ľ��̣�PID����exit�˳���tasklist�г���������bat����������̣�";
	string userinput = Kgetline();
	if (userinput == "exit") {
		return;
	}
	else if (userinput == "bat") {
#ifndef _M_IX86
		vector<int> proc;
		cout << "������ģʽ" << endl <<
			"1>add <PID>��������б�����ӽ���PID" << endl <<
			"2>rm <PID>���ӽ����б����Ƴ�PID" << endl <<
			"3>�������̲���ս����б�" << endl <<
			"4>���߳������������" << endl <<
			"5>���߳������ָ�����" << endl <<
			"6>��ս����б�" << endl <<
			"7>�������Ľ���PID" << endl <<
			"8>�鿴�Ѿ�ѡ�еĽ����б�" << endl; 
		while (1) {
			cprint("Proc-Bat", 7, 2); cout << ">"; string input = Kgetline();
			if (input.substr(0, 3) == "add") {
				std::istringstream iss(input.substr(4)); // ���� "add" �Ϳո�
				std::string token;
				cout << "<�����������PID���б�";
				while (iss >> token) {
					// ���ַ���ת��Ϊ��������ӵ�vector
					if ((std::find(proc.begin(), proc.end(), StringToInt(token)) == proc.end())&&(ExistProcess(StringToInt(token)))) {
						proc.push_back(StringToInt(token));
						cout << StringToInt(token) << ";";
					}
				}
				cout << endl;
			}else if (input.substr(0, 2) == "rm") {
				std::istringstream iss(input.substr(3)); // ���� "rm" �Ϳո�
				std::string token;
				cout << "<��������PID���б����Ƴ���";
				while (iss >> token) {
					int value = StringToInt(token); // ���ַ���ת��Ϊ����
					// �� vector �в��Ҳ��Ƴ�ƥ���Ԫ��
					auto it = std::find(proc.begin(), proc.end(), value);
					if (it != proc.end()) {
						cout << value << ";";
						proc.erase(it); // �Ƴ��ҵ���Ԫ��
					}
				}cout << endl;
			}
			else if (input == "3" || input=="kill") {
				cout << "��ѡ����ķ�����" << endl <<
					"1>taskkill����" << endl <<
					"2>taskkill��/f����" << endl <<
					"3>TerminateProcess����" << endl <<
					"4>TerminateThread����" << endl <<
					"5>NtTerminate����" << endl <<
					"6>��ҵ�������" << endl;
				int killmethod = StringToInt(Kgetline());
				std::vector<std::thread> threads;
				std::latch latch(proc.size()); // ����һ�� latch������ͬ�������߳�

				// �����̲߳��ȴ�ͬ����ʼ
				for (int value : proc) {
					threads.emplace_back([killmethod, value, &latch]() {
						latch.arrive_and_wait(); // �ȴ������߳̾���
						KillProcess(killmethod, value);      // ���ô�����
						});
				}

				// �ȴ������߳����
				for (auto& thread : threads) {
					if (thread.joinable()) {
						thread.join();
					}
				}
			}
			else if (input == "4"||input=="froze") {
				std::vector<std::thread> threads;
				std::latch latch(proc.size()); // ����һ�� latch������ͬ�������߳�

				// �����̲߳��ȴ�ͬ����ʼ
				for (int value : proc) {
					threads.emplace_back([value,&latch]() {
						latch.arrive_and_wait(); // �ȴ������߳̾���
						SuspendProcess(value);      // ���ô�����
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
				std::latch latch(proc.size()); // ����һ�� latch������ͬ�������߳�

				// �����̲߳��ȴ�ͬ����ʼ
				for (int value : proc) {
					threads.emplace_back([value, &latch]() {
						latch.arrive_and_wait(); // �ȴ������߳̾���
						UnSuspendProcess(value);      // ���ô�����
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
				cout << "��������С��Χ����������>";
				int minnum = StringToInt(Kgetline());
				cout << "���������Χ����������>";
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
				cout << "<��δ������﷨" << endl;
			}
		}
		goto KswordProcMgrStart;
#endif
#ifdef _M_IX86
		KMesErr("������������x86�������������64λ����Ȼ������");
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
		cout << "�Զ�����PIDΪ" << taskproc << "�Ľ���" << endl;
	}
	else
		taskproc = StringToInt(userinput);
	if (!ExistProcess(taskproc))
	{
		KMesErr("ָ���Ľ��̲�����");
		return;
	}
	cout << "��ѡ����Ҫ���еĲ�����" << endl <<
		"1>���DLL\t\t2>��������" << endl <<
		"3>�������\t\t4>�ָ�����" << endl <<
		"5>�鿴���������Ϣ\t6>��Ϊ�ؼ�����" << endl <<
		"7>ȡ���ؼ�����\t\t8>���Է��ʾ��" << endl;
	int method = StringToInt(Kgetline());
	
	if (method == 1) {
		std::vector<std::wstring> dlls = GetDLLsByPID(taskproc);
		std::cout << "PIDΪ "<<taskproc<<" �Ľ��̼��ص�DLLģ�飺" << std::endl;
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
		cout << "��ѡ����ķ�����" << endl <<
			"1>taskkill����" << endl <<
			"2>taskkill��/f����" << endl <<
			"3>TerminateProcess����" << endl <<
			"4>TerminateThread����" << endl <<
			"5>NtTerminate����" << endl <<
			"6>��ҵ�������" << endl;
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
				KMesErr("��Ŀ����̾��ʧ�ܣ�������룺" + to_string(GetLastError()));
				return;
			}
			if (!KillProcess(3, hProcess))
				cout << "��������ʧ�ܣ��Ƿ�����һ�ַ�����(y/n)(def=n):";
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
				KMesErr("��Ŀ����̾��ʧ�ܣ�������룺" + to_string(GetLastError()));
				return;
			}
			if (!KillProcess(4, hProcess))
				cout << "��������ʧ�ܣ��Ƿ�����һ�ַ�����(y/n)(def=n):";
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
				KMesErr("��Ŀ����̾��ʧ�ܣ�������룺" + to_string(GetLastError()));
				return;
			}
			if (!KillProcess(5, hProcess))
				cout << "��������ʧ�ܣ��Ƿ�����һ�ַ�����(y/n)(def=n):";
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
				KMesErr("��Ŀ����̾��ʧ�ܣ�������룺" + to_string(GetLastError()));
				return;
			}
			if (!KillProcess(6, hProcess))
				cout << "��������ʧ��." << endl;
		}
		else KMesErr("δ����Ĳ�����ʽ");
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

		// ׼�� PROCESSENTRY32 �ṹ
		PROCESSENTRY32 pe = { sizeof(PROCESSENTRY32) };

		// ��ȡ��һ��������Ϣ
		if (!Process32First(hSnapshot, &pe))
		{
			std::cerr << "Process32First failed: " << GetLastError() << std::endl;
			CloseHandle(hSnapshot);
			return;
		}

		// �������н���
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

		// �رտ��վ��
		CloseHandle(hSnapshot);

		if (!found)
		{
			KMesErr("�Ҳ���PIDΪ" + to_string(taskproc) + "�Ľ���");
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

	// Ȩ�ޱ�־�����Ƶ�ӳ���
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
		{0x1000, "PROCESS_SET_LIMITED_INFORMATION"}, // ���ְ汾���岻ͬ
		{0x00010000, "PROCESS_DELETE"}
	};
	for (const auto& mode : accessModes) {
		HANDLE hProcess = OpenProcess(mode.first, FALSE, taskproc);
		DWORD err = GetLastError();

		std::cout << std::left << std::setw(35) << mode.second
			<< " : ";
		if (hProcess != NULL) {
			cprint("[�ɹ�]", 10, 0);
			CloseHandle(hProcess);
		}
		else {
			cprint("[ʧ��]", 4, 0);
			if (err == 5) std::cout << " [�ܾ�����]";
			else if (err == 87) std::cout << " [��������]";
			else cout << "�������" << err;
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

// ����ṹ��
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

// ���峣��
#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif


void GetProcessInformation(DWORD processId) {
	// ��ȡ NtQuerySystemInformation ����ָ��
	_NtQuerySystemInformation NtQuerySystemInformation =
		(_NtQuerySystemInformation)GetProcAddress(GetModuleHandle(L"ntdll.dll"), "NtQuerySystemInformation");
	if (!NtQuerySystemInformation) {
		std::cerr << "Could not find NtQuerySystemInformation" << std::endl;
		return;
	}

	// ��̬�����ڴ�
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

	// �������صĽ�����Ϣ
	PSYSTEM_PROCESS_INFORMATION current = (PSYSTEM_PROCESS_INFORMATION)buffer;
	do {
		if ((DWORD)current->UniqueProcessId == processId) {
			
			cprint("��һ���ƫ������", 2, 0); cout << "\t"; std::wcout << current->NextEntryOffset << std::endl;
			cprint("�߳�������  ", 2, 0); cout << "\t"; std::wcout << current->NumberOfThreads << std::endl;
			cprint("ӳ�����ƣ�  ", 2, 0); cout << "\t"; wcout<< current->ImageName.Buffer << std::endl;
			cprint("�������ȼ���", 2, 0); cout << "\t"; wcout << current->BasePriority << std::endl;
			cprint("����Ψһ��ʶ����", 2, 0); cout << "\t"; wcout << current->UniqueProcessId << std::endl;
			cprint("���������  ", 2, 0); cout << "\t"; wcout << current->HandleCount << std::endl;
			cprint("�ỰID��   ", 2, 0); cout << "\t"; wcout << current->SessionId << std::endl;
			//cprint("�����ڴ����������", 2, 0); cout << "\t"; wcout << current->PeakVirtualSize << std::endl;
			//cprint("�����ڴ�������", 2, 0); cout << "\t"; wcout << current->VirtualSize << std::endl;
			//cprint("���������С��", 2, 0); cout << "\t"; wcout << current->PeakWorkingSetSize << std::endl;
			//cprint("��������С�� ", 2, 0); cout << "\t"; wcout << current->WorkingSetSize << std::endl;
			//cprint("��ҳ�����������", 2, 0); cout << "\t"; wcout << current->QuotaPagedPoolUsage << std::endl;
			//cprint("�Ƿ�ҳ�����������", 2, 0); cout << "\t"; wcout << current->QuotaNonPagedPoolUsage << std::endl;
			//cprint("ҳ���ļ���С��", 2, 0); cout << "\t"; wcout << current->PagefileUsage << std::endl;
			//cprint("���ҳ���С��", 2, 0); cout << "\t"; wcout << current->PeakPagefileUsage << std::endl;
			//cprint("˽��ҳ��������", 2, 0); cout << "\t"; wcout << current->PrivatePageCount << std::endl;
			cprint("�����ڴ����������", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->PeakVirtualSize) << " MB" << std::endl;
			cprint("�����ڴ�������  ", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->VirtualSize) << " MB" << std::endl;
			cprint("���������С��", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->PeakWorkingSetSize) << " MB" << std::endl;
			cprint("��������С��    ", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->WorkingSetSize) << " MB" << std::endl;
			cprint("��ҳ����������� ", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->QuotaPagedPoolUsage) << " MB" << std::endl;
			cprint("�Ƿ�ҳ�����������", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->QuotaNonPagedPoolUsage) << " MB" << std::endl;
			cprint("ҳ���ļ���С��  ", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->PagefileUsage) << " MB" << std::endl;
			cprint("���ҳ���С��  ", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->PeakPagefileUsage) << " MB" << std::endl;
			cprint("˽��ҳ��������  ", 2, 0); cout << "\t"; std::cout << std::fixed << std::setprecision(2) << BytesToMB(current->PrivatePageCount) << " MB" << std::endl;
			break;
		}
		current = (PSYSTEM_PROCESS_INFORMATION)((BYTE*)current + current->NextEntryOffset);
	} while (current->NextEntryOffset != 0);

	free(buffer);
}
#endif