#ifdef KSWORD_WITH_COMMAND
#include "../KswordTotalHead.h"
using namespace std;
HANDLE KswordSelfProc2(int Runtime) {

	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags |= STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_SHOW; // ȷ���´��ڿɼ�
	string tmp1 = GetSelfPath();
	wchar_t a[5] = L"1 ";
	swprintf(a, sizeof(a) / sizeof(wchar_t), L"%d %d", 2, Runtime);
	// �����½���
	if (!CreateProcess(
		CharToWChar(tmp1.c_str()), // ģ��������ִ���ļ�����
		//CharToWChar("C:\\Users\\33251\\Desktop\\ksword\\test\\systemtest.exe"),
		a, // ������
		NULL, // ���̰�ȫ��
		NULL, // �̰߳�ȫ��
		FALSE, // �̳о��
		CREATE_NEW_CONSOLE, // �����¿���̨
		NULL, // ʹ�ø����̵Ļ�����
		NULL, // ʹ�ø����̵ĵ�ǰĿ¼
		&si, // ָ��STARTUPINFO�ṹ��ָ��
		&pi // ָ��PROCESS_INFORMATION�ṹ��ָ��
	)) {
		KMesErr("�޷����ӽ��̣�����������Ksword������");
		return NULL;
	}

	return pi.hProcess;
	CloseHandle(pi.hThread);
}
#endif