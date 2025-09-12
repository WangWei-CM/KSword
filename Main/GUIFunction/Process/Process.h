#pragma once
#include "../../KswordTotalHead.h"
enum kProcKillMethod{
	kTaskkill,
	kTaskkillF,
	kTerminate,
	kTerminateThread,
	kNTTerminate,
	kJobTer
};
enum kEnumProcMethod {
	kEnumProcByCreateSnapTool
};

class kThread {
private:
	HANDLE hThread;         // �߳̾��
	DWORD TID;              // �߳�ID
	int priority;           // �߳����ȼ���-2��2��ӦWindows��THREAD_PRIORITY_*��
	int statusCode;         // �̲߳���״̬�루0��ʾ�ɹ�������Ϊ�����룩
	std::string modulePath; // �߳�����ģ��·��




public:
	// ���캯����ͨ���߳�ID��ʼ��
	kThread(DWORD tid);
	// ���캯����ͨ�����̾����ʼ������ȡ���̵����̣߳�
	kThread(HANDLE hProcess);
	// �����������ͷ���Դ
	~kThread();

	// ��ֹ�߳�
	int Terminate(UINT exitCode = 0);
	// �����߳�
	int Suspend();
	// �ָ��߳�
	int UnSuspend();

	// ��ȡ˽�г�Ա�ĺ���
	HANDLE GetHandle() const;
	DWORD GetTID() const;
	int GetPriority() const;
	int GetStatusCode() const;
	std::string GetModulePath() const;

private:
	// ��ʼ���߳���Ϣ���ڲ�ʹ�ã�
	void InitThreadInfo();
};

class kProcess {
public:

	kProcess(DWORD pid);

	kProcess(HANDLE hProc);

	bool Terminate(kProcKillMethod method = kTerminateThread, UINT uExitCode = 0);
	
	bool Suspend();
	bool Resume();
	
	bool SetKeyProc();
	bool CancelKeyProc();

	std::string GetProcessName();
	kProcess(kProcess&& other) noexcept;
	kProcess& operator=(kProcess&& other) noexcept;

	// ��ֹ������������ܱ�������
	kProcess(const kProcess&) = delete;
	kProcess& operator=(const kProcess&) = delete;
	//��������
	static HANDLE GetProcessHandleByPID(DWORD pid);
	static DWORD GetProcessIdByName(const std::wstring& procName);

	~kProcess();

	DWORD pid() const;
	std::string Name() const;
	std::string User() const;
	std::string ExePath() const;
	HANDLE Handle() const;
	bool IsAdmin() const;

	double GetCPUUsage() const { return cpuUsage; }

	void UpdateMemoryInfo();
	double UpdateCPUUsage();
	void UpdateStats();
protected:
	int PID;
	std::string AuthName;
	std::string exePath;
	bool isAdmin;
	HANDLE handle;
	std::string name;

	//���ܼ�����
	double cpuUsage;

};
struct ServiceInfo {
	std::wstring serviceName;       // �������ƣ�������
	std::wstring userName;          // ���������˻���
	std::wstring displayName;       // ������ʾ����
	std::wstring description;       // ��������
	DWORD pid;                      // �����Ӧ�Ľ���PID
};