#ifdef KSWORD_WITH_COMMAND
#include "../../KswordTotalHead.h"

using namespace std;
string KswordThread[50];
bool KswordThreadStop[50];
int KswordThreadTopNum;
std::vector<std::thread> threads;



int KswordRegThread(string threadName) {
	KswordThread[KswordThreadTopNum++] = threadName;
	return KswordThreadTopNum - 1;
}
void KswordStopThread(int threadNum) {
	KswordThreadStop[threadNum] = 1;
	KMesInfo("向线程" + KswordThread[threadNum] + "发送了停止信号");
	KswordSend1("向线程" + KswordThread[threadNum] + "发送了停止信号");
}
void KswordStartThread(int threadNum) {
	KswordThreadStop[threadNum] = 0;
	KMesInfo("向线程" + KswordThread[threadNum] + "发送了开始信号");
	KswordSend1("向线程" + KswordThread[threadNum] + "发送了开始信号");
}
void KswordThreadMgr() {
	cout << "1>发送停止信号" << endl
		<< "2>发送开始信号" << endl
		<< "3>打印线程列表" << endl;
	int usermethod = StringToInt(Kgetline());
	if (usermethod == 1) {
		cout << "请输入操作TID:>"; KswordStopThread(StringToInt(Kgetline()));
	}
	else if (usermethod == 2) {
		cout << "请输入操作TID:>"; KswordStartThread(StringToInt(Kgetline()));
	}
	else if (usermethod == 3) {
		cout << "共存在线程" << KswordThreadTopNum << "个,下面是线程ID/名称：" << endl;
		for (int i = 0; i <= KswordThreadTopNum; i++) {
			cout << setw(5) << setfill('0') << i << "	" << KswordThread[i] << endl;
		}cout << "==========================" << endl;
	}
	else {
		KMesErr("未定义的操作方式");
	}return;
}

#endif