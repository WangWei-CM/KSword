#pragma once
#include "../../KswordTotalHead.h"
#include "process.h"

struct OpenProcessTestItem {
    std::string name;               // Ȩ������
    DWORD accessRight;              // Ȩ��ֵ
    std::string result;             // ���Խ��
    bool tested;                    // �Ƿ��Ѳ���
};


class ProcessOpenTest {
private:
    std::vector<OpenProcessTestItem> testItems;  // Ȩ�޲����б�
    DWORD targetPID;                             // Ŀ�����PID
    bool showTestWindow;                         // ������ʾ��־

    // ִ�е���Ȩ�޲���
    void RunTest(OpenProcessTestItem& item);

    // ������ת��Ϊ�ɶ��ַ���
    std::string GetErrorString(DWORD errorCode);

public:
    ProcessOpenTest();  // ���캯������ʼ��Ȩ���б�

    // ��ʾ���Դ���
    void ShowWindow();

    // ����Ŀ��PID
    void SetTargetPID(DWORD pid) { targetPID = pid; }

    // ��ʾ/���ش���
    void ToggleWindow() { showTestWindow = !showTestWindow; }
};


class kProcessDetail : public kProcess {
private:
    std::string processUser;       // ���������û�
    std::string processExePath;    // ��������·��
    bool isAdmin;                  // �Ƿ�Ϊ����ԱȨ��
    std::string processName;       // ��������
    bool firstShow = true;
	std::string commandLine;           // �����в���

    // ��������չ������Ϣ
    FILETIME creationTime;         // ����ʱ��
    FILETIME exitTime;             // �˳�ʱ��
    FILETIME kernelTime;           // �ں�̬ʱ��
    FILETIME userTime;             // �û�̬ʱ��
    DWORD threadCount;             // �߳���
    DWORD handleCount;             // �����
    LONG basePriority;             // �������ȼ�
    NTSTATUS exitStatus;           // �˳�״̬
    ULONG_PTR affinityMask;        // �׺�������
    DWORD sessionId;               // �ỰID
    DWORD parentPid;             // ������PID
    std::string parentProcessName; // ����������


public:
    std::string GetCommandLine();
    // ��֧��PID��ʼ��
    explicit kProcessDetail(DWORD pid);
    std::string FileTimeToString(FILETIME ft);  // FILETIMEת�ַ���
    bool GetProcessExtendedInfo();              // ��ȡ��չ��Ϣ
    bool GetParentProcessInfo();
    // ��Ⱦ������Ϣ����
    void Render();
	ProcessOpenTest openTest; // ����Ȩ�޲���ʵ��

private:
    // ��ʼ����ϸ������Ϣ
    void InitDetailInfo();
};

class ProcessDetailManager {
private:
    std::vector<std::unique_ptr<kProcessDetail>> processDetails;
public:
    // ��ӽ��̵������б�ͨ��PID��
    void add(DWORD pid);

    // �Ƴ�ָ��PID�Ľ��̣������Ƿ�ɹ��Ƴ���
    bool remove(DWORD pid);

    // ��Ⱦ���н��̵���ϸ��Ϣ����
    void renderAll();
};

