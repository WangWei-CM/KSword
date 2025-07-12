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
	KMesInfo("���߳�" + KswordThread[threadNum] + "������ֹͣ�ź�");
	KswordSend1("���߳�" + KswordThread[threadNum] + "������ֹͣ�ź�");
}
void KswordStartThread(int threadNum) {
	KswordThreadStop[threadNum] = 0;
	KMesInfo("���߳�" + KswordThread[threadNum] + "�����˿�ʼ�ź�");
	KswordSend1("���߳�" + KswordThread[threadNum] + "�����˿�ʼ�ź�");
}
void KswordThreadMgr() {
	cout << "1>����ֹͣ�ź�" << endl
		<< "2>���Ϳ�ʼ�ź�" << endl
		<< "3>��ӡ�߳��б�" << endl;
	int usermethod = StringToInt(Kgetline());
	if (usermethod == 1) {
		cout << "���������TID:>"; KswordStopThread(StringToInt(Kgetline()));
	}
	else if (usermethod == 2) {
		cout << "���������TID:>"; KswordStartThread(StringToInt(Kgetline()));
	}
	else if (usermethod == 3) {
		cout << "�������߳�" << KswordThreadTopNum << "��,�������߳�ID/���ƣ�" << endl;
		for (int i = 0; i <= KswordThreadTopNum; i++) {
			cout << setw(5) << setfill('0') << i << "	" << KswordThread[i] << endl;
		}cout << "==========================" << endl;
	}
	else {
		KMesErr("δ����Ĳ�����ʽ");
	}return;
}

#endif