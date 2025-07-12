#ifdef KSWORD_WITH_COMMAND
#include "../KswordTotalHead.h"
using namespace std;

extern HANDLE Ksword_Pipe_1;
bool KswordPipeMode;

int KswordSelfProc1(int Runtime) {
	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags |= STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_SHOW; // ȷ���´��ڿɼ�
	si.dwFlags = STARTF_USECOUNTCHARS | STARTF_USESHOWWINDOW |STARTF_USEPOSITION ;
    si.dwX = RightColumnStartLocationX;       // ���ڵ� X ����
    si.dwY = RightColumnStartLocationY;       // ���ڵ� Y ����
    si.dwXCountChars = ColumnWidth;           // ���ڵĿ�ȣ��ַ�����
    si.dwYCountChars = ColumnHeight+7;          // ���ڵĸ߶ȣ��ַ�����
    si.wShowWindow = SW_SHOW;                 // ��ʾ����
	string tmp1 = GetSelfPath();
	wchar_t a[5] = L"1 ";
	wchar_t b[5] = L"999";
	if (Runtime == 999) {
		swprintf(a, sizeof(a) / sizeof(wchar_t), L"%d %d", 1, Runtime);
		// �����½���
		if (!CreateProcess(
			CharToWChar(tmp1.c_str()), // ģ��������ִ���ļ�����
			//CharToWChar("C:\\Users\\33251\\Desktop\\ksword\\test\\systemtest.exe"),
			b, // ������
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
			return 1;
		}

		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
	else {
		swprintf(a, sizeof(a) / sizeof(wchar_t), L"%d %d", 1, Runtime);
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
			return 1;
		}

		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
}

int KswordSelfPipe1(int Runtime) {
	if (Runtime == 999) {
		const std::string pipeName = "\\\\.\\pipe\\Ksword_Pipe_SOS";
		// �������ܵ�
		Ksword_Pipe_1 = CreateNamedPipe(
			CharToWChar(pipeName.c_str()),
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			1,
			256, // �����������С
			256, // ���뻺������С
			0,   // ʹ��Ĭ�ϳ�ʱ
			NULL // Ĭ�ϰ�ȫ����
		);

		if (Ksword_Pipe_1 == INVALID_HANDLE_VALUE) {
			KMesErr("�޷����ӽ���ȡ����ϵ���򿪹ܵ�ʧ��");
			return 1;
		}
		// ���ӵ��ܵ�
		ConnectNamedPipe(Ksword_Pipe_1, NULL);
		return KSWORD_SUCCESS_EXIT;

	}
	else {
		const std::string pipeName = "\\\\.\\pipe\\Ksword_Pipe_1_" + to_string(Runtime);
		// �������ܵ�
		Ksword_Pipe_1 = CreateNamedPipe(
			CharToWChar(pipeName.c_str()),
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			1,
			256, // �����������С
			256, // ���뻺������С
			0,   // ʹ��Ĭ�ϳ�ʱ
			NULL // Ĭ�ϰ�ȫ����
		);

		if (Ksword_Pipe_1 == INVALID_HANDLE_VALUE) {
			KMesErr("�޷����ӽ���ȡ����ϵ���򿪹ܵ�ʧ��");
			return 1;
		}
		// ���ӵ��ܵ�
		ConnectNamedPipe(Ksword_Pipe_1, NULL);
		return KSWORD_SUCCESS_EXIT;
	}
}


int KswordMainPipe() {
	const std::string pipeName = "\\\\.\\pipe\\Ksword_Main_Pipe";
	// �������ܵ�
	Ksword_Main_Pipe = CreateNamedPipe(
		CharToWChar(pipeName.c_str()),
		PIPE_ACCESS_DUPLEX,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		1,
		256, // �����������С
		256, // ���뻺������С
		0,   // ʹ��Ĭ�ϳ�ʱ
		NULL // Ĭ�ϰ�ȫ����
	);

	if (Ksword_Main_Pipe == INVALID_HANDLE_VALUE) {
		KMesErr("�޷����ӽ���ȡ����ϵ���򿪹ܵ�ʧ��");
		return 1;
	}
	// ���ӵ��ܵ�
	ConnectNamedPipe(Ksword_Main_Pipe, NULL);
	return KSWORD_SUCCESS_EXIT;
}
int KswordMainSockPipe() {
	const std::string pipeName = "\\\\.\\pipe\\Ksword_Main_Sock_Pipe";
	// �������ܵ�
	Ksword_Main_Pipe = CreateNamedPipe(
		CharToWChar(pipeName.c_str()),
		PIPE_ACCESS_DUPLEX,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		1,
		256, // �����������С
		256, // ���뻺������С
		0,   // ʹ��Ĭ�ϳ�ʱ
		NULL // Ĭ�ϰ�ȫ����
	);

	if (Ksword_Main_Pipe == INVALID_HANDLE_VALUE) {
		KMesErr("�޷����ӽ���ȡ����ϵ���򿪹ܵ�ʧ��");
		return 1;
	}
	// ���ӵ��ܵ�
	ConnectNamedPipe(Ksword_Main_Sock_Pipe, NULL);
	return KSWORD_SUCCESS_EXIT;
}


int KswordSend1(std::string Message) {
	Message += "`";
	DWORD bytesWritten=0;
	if (KswordPipeMode
		) {
	return WriteFile(
		Ksword_Main_Sock_Pipe,
		Message.c_str(),
		static_cast<DWORD>(Message.size()),
		&bytesWritten,
		NULL
	);
	}else
	return WriteFile(
		Ksword_Pipe_1,
		Message.c_str(),
		static_cast<DWORD>(Message.size()),
		&bytesWritten,
		NULL
	);
}


int KswordSendMain(std::string Message) {
	DWORD bytesWritten=0;
	return WriteFile(
		Ksword_Main_Pipe,
		Message.c_str(),
		static_cast<DWORD>(Message.size()),
		&bytesWritten,
		NULL
	);
}
#endif