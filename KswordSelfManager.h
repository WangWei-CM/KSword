#pragma once
#include "main\KswordTotalHead.h"
using namespace std;
inline void KswordSelfManager() {
	cout << "���棺ʹ��quit�˳�������exit��exit�ᱻ���͸��ӽ��̡�" << endl;
	int Sockettask=0;
	while (1) {
		cout << "[Ksword]";
		if (Sockettask != 0)cout << "SubProcess" << Sockettask;
		cout << ">";
		string SelfCmd = Kgetline();
		if (SelfCmd.substr(0, 3) == "set") {
			int SockettaskTemp = StringToInt(std::string(1, SelfCmd.back()));
			//cout << "���񵽵ģ�" << SockettaskTemp;
			if (SockettaskTemp == 1) {
				Sockettask = 1;
				continue;
			}
			else {
				KMesErr("ָ������Ч�Ľ���");
				continue;
			}
		}
		else if (SelfCmd == "quit")return;
		else {
			if (Sockettask == 1)KswordSend1(SelfCmd);
		}

	}
}