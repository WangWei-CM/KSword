#include "../../KswordTotalHead.h"
void KillProcessByTaskkill(int pid) {
	int Tmppid;
	Tmppid=kItem.AddProcess(
		/*C*/(("ʹ��task kill��ֹpidΪ" + std::to_string(pid) + "�Ľ���").c_str()),
		/*C*/("ִ��cmd����"),
		NULL
	);
	std::string tmp = "taskkill /pid " + std::to_string(pid);
	std::string tmp1 = GetCmdResultWithUTF8(tmp);
	kLog.Add(Info, tmp1.c_str());
	return;
}