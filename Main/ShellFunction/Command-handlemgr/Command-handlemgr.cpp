#ifdef KSWORD_WITH_COMMAND
#include "Command-handlemgr.h"
std::wstring GetProcessNameByPid(DWORD pid) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return L"";
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (!Process32FirstW(hSnapshot, &pe32)) {
        CloseHandle(hSnapshot);
        return L"";
    }

    do {
        if (pe32.th32ProcessID == pid) {
            CloseHandle(hSnapshot);
            return pe32.szExeFile; // ���ؽ�����������չ����
        }
    } while (Process32NextW(hSnapshot, &pe32));

    CloseHandle(hSnapshot);
    return L"";
}
int CurrentProcess = GetCurrentProcessId();
int GetFileOccupyInformation(std::string startString) {
    _NtQuerySystemInformation NtQuerySystemInformation =
        (_NtQuerySystemInformation)GetProcAddress(GetModuleHandle(L"ntdll.dll"), "NtQuerySystemInformation");

    ULONG bufferSize = 0x99999;
    PSYSTEM_HANDLE_INFORMATION handleInfo = (PSYSTEM_HANDLE_INFORMATION)malloc(bufferSize);
    NTSTATUS status;
    // ��̬�������������ο���ҳ5��
    while ((status = NtQuerySystemInformation(16, handleInfo, bufferSize, NULL)) == 0xC0000004L) {
        free(handleInfo);
        bufferSize *= 2;
        handleInfo = (PSYSTEM_HANDLE_INFORMATION)malloc(bufferSize);
    }
    if (status == 0) {
        std::cout << "Query succeeded with buffer size: " << bufferSize << std::endl;
        std::cout << "һ���о��������" << std::endl << handleInfo->HandleCount << std::endl;
        for (ULONG i = 0; i < handleInfo->HandleCount; i++) { // ʹ��HandleCount

            SYSTEM_HANDLE_TABLE_ENTRY_INFO sourceEntry = handleInfo->Handles[i]; // ԭʼ����
            SYSTEM_HANDLE targetHandle = {};
            if (targetHandle.ProcessId == CurrentProcess)continue;
            // ��ʽӳ���ֶ�
            std::cout << targetHandle.ProcessId << " ";
            targetHandle.ProcessId = static_cast<ULONG>(sourceEntry.UniqueProcessId);
            targetHandle.Handle = sourceEntry.HandleValue;
            targetHandle.ObjectTypeNumber = sourceEntry.ObjectTypeIndex;
            targetHandle.GrantedAccess = sourceEntry.GrantedAccess;
            // �����Ϣ
            if (sourceEntry.ObjectTypeIndex == 37) {
                // �����ļ����
                HANDLE hProcess = OpenProcess(
                    PROCESS_DUP_HANDLE,
                    FALSE,
                    sourceEntry.UniqueProcessId
                );
                if (hProcess) {
                    HANDLE hDupHandle;
                    if (DuplicateHandle(
                        hProcess,
                        (HANDLE)sourceEntry.HandleValue,
                        GetCurrentProcess(),
                        &hDupHandle,
                        0,
                        FALSE,
                        DUPLICATE_SAME_ACCESS
                    )) {
                        wchar_t typeName[64];
                        DWORD retSize;
                        if (NT_SUCCESS(NtQueryObject(
                            hDupHandle,
                            ObjectTypeInformation,
                            typeName,
                            sizeof(typeName),
                            &retSize
                        ))) {
                            //std::cout << "�ɹ���ȡ�ļ����: 0x"
                            //    << std::hex << hDupHandle << std::endl;
                            // �˴��ɴ洢�������к�������
                            //std::cout << "3";
                            wchar_t devicePath[MAX_PATH + 1] = { 0 };
                            // ʹ��GetFinalPathNameByHandleW��ȡ�豸·������ҳ3��
                            DWORD pathLen = GetFinalPathNameByHandleW(
                                hDupHandle,
                                devicePath,
                                MAX_PATH,
                                VOLUME_NAME_DOS  // ����DOS��ʽ·������ҳ8��
                            );
                            if (pathLen == 0) {
                                //DWORD err = GetLastError();
                                //std::cerr << "·����ȡʧ�� (������: 0x" << std::hex << err << ")" << std::endl;
                                continue;
                            }
                            char ansiPath[MAX_PATH * 2] = { 0 };
                            WideCharToMultiByte(
                                CP_ACP,
                                0,
                                devicePath,
                                -1,
                                ansiPath,
                                sizeof(ansiPath),
                                NULL,
                                NULL
                            );
                            //std::cout << "5";
                            // ����·��ǰ׺����ҳ3��
                            std::string finalPath(ansiPath);
                            size_t pos = finalPath.find("\\\\?\\");
                            if (pos != std::string::npos) {
                                finalPath.erase(0, 4); // �Ƴ���·��ǰ׺
                            }
                            if (finalPath.size() >= startString.size() &&
                                std::equal(startString.begin(), 
                                    startString.end(), finalPath.begin(),
                                    [](unsigned char c1, unsigned char c2) {
                                        return std::tolower(c1) == std::tolower(c2);
                                    })) {
                                std::cout << "Handle[" << i << "]: "
                                    ; cprint("PID", 1, 0); std::cout << targetHandle.ProcessId
                                    ; cprint("Value", 2, 0);std::cout << std::hex << targetHandle.Handle
                                    ; cprint("Type", 2, 0); std::cout << (int)targetHandle.ObjectTypeNumber
                                    ; cprint("ProcName", 2, 0); std::cout << WstringToString(GetProcessNameByPid(targetHandle.ProcessId))
                                    << std::dec << std::endl;

                                std::cout << "Ŀ���ļ�·��: " << finalPath << std::endl;
                            }


                        }
                        CloseHandle(hDupHandle);
                    }
                    else {
                        //std::cerr << "�������� (������: 0x" << std::hex << GetLastError() << ")" << std::endl;
                    }
                    CloseHandle(hProcess);
                }
                else {
                }
            }
        }
    }
    return 0;
}

BOOL NtQueryAllProcess() {
    BOOL ret = FALSE;
    PNtQuerySystemInformation NtQuerySystemInformation = NULL;
    NtQuerySystemInformation = (PNtQuerySystemInformation)GetProcAddress(LoadLibrary(L"ntdll.dll"), "NtQuerySystemInformation");
    PSYSTEM_PROCESS_INFORMATION sysProInfo = NULL, old = NULL;
    if (NtQuerySystemInformation != NULL) {
        ULONG cbSize = sizeof(SYSTEM_PROCESS_INFORMATION);
        //��ѯ
        LONG status = 0;
        do {
            old = sysProInfo = (PSYSTEM_PROCESS_INFORMATION)malloc(cbSize);
            status = NtQuerySystemInformation(SystemProcessInformation, sysProInfo, cbSize, &cbSize);
            if (status)
                free(sysProInfo);
        } while (status);
        ret = TRUE;
        //��������
        do {
            if (sysProInfo->ImageName.Buffer != NULL)
            {
                _tprintf(L"������:\t%s \t����ID:%u \t�������:%u \t�߳�����:%u \n", sysProInfo->ImageName.Buffer, sysProInfo->UniqueProcessId,
                    sysProInfo->HandleCount, sysProInfo->NumberOfThreads);
                //��ӡ�߳���Ϣ
                PSYSTEM_THREAD_INFORMATION threadInfo = NULL;
                threadInfo = (PSYSTEM_THREAD_INFORMATION)((ULONG64)sysProInfo + sizeof(SYSTEM_PROCESS_INFORMATION));
                DWORD curThreadIndex = 1;
                do {
                    _tprintf(L"\t�߳�ID:%u\t��ʼ��ַ:%x \t�̵߳�״̬��:%u\n", threadInfo->ClientId.UniqueThread, threadInfo->StartAddress, threadInfo->ThreadState);
                    threadInfo += 1;
                } while (curThreadIndex++ < sysProInfo->NumberOfThreads);
                _tprintf(L"\n");
            }
            //ָ��ļӼ�����ĵ�λ�Ǹ�����ָ���������ʹ�С�ġ��ֽ�ָ�����1�����ԼӼ�����û���⡣�����ǽṹ��ָ�룬���Ա���ת���������������㡣
            sysProInfo = (PSYSTEM_PROCESS_INFORMATION)((ULONG64)sysProInfo + sysProInfo->NextEntryOffset);
        } while (sysProInfo->NextEntryOffset != 0);
        free(old);
    }

    return ret;
}

#endif