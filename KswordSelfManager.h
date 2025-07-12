#pragma once
#include "main\KswordTotalHead.h"
using namespace std;
inline void KswordSelfManager() {
	cout << "警告：使用quit退出而不是exit。exit会被发送给子进程。" << endl;
	int Sockettask=0;
	while (1) {
		cout << "[Ksword]";
		if (Sockettask != 0)cout << "SubProcess" << Sockettask;
		cout << ">";
		string SelfCmd = Kgetline();
		if (SelfCmd.substr(0, 3) == "set") {
			int SockettaskTemp = StringToInt(std::string(1, SelfCmd.back()));
			//cout << "捕获到的：" << SockettaskTemp;
			if (SockettaskTemp == 1) {
				Sockettask = 1;
				continue;
			}
			else {
				KMesErr("指定了无效的进程");
				continue;
			}
		}
		else if (SelfCmd == "quit")return;
		else {
			if (Sockettask == 1)KswordSend1(SelfCmd);
		}

	}
}