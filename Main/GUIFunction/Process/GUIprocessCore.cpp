#include "../../KswordTotalHead.h"
void KillProcessByTaskkill(int pid) {
	int Tmppid;
	Tmppid=kItem.AddProcess(
		/*C*/(("使用task kill终止pid为" + std::to_string(pid) + "的进程").c_str()),
		/*C*/("执行cmd命令"),
		NULL
	);
	std::string tmp = "taskkill /pid " + std::to_string(pid);
	std::string tmp1 = GetCmdResultWithUTF8(tmp);
	kLog.Add(Info, tmp1.c_str());
	return;
}