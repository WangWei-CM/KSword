#ifdef KSWORD_WITH_COMMAND
#include "..\..\KswordTotalHead.h"

using namespace std;
void GetTcpConnectionsByPid(DWORD pid);
void GetTcpConnectionsByPort(USHORT port);
void KswordNetManager() {
    cout << "1>����������ָ�����̵�����TCP����" << endl <<
        "2>�о�ָ�����̵�TCP����" << endl <<
        "3>���ָ���˿ڵ�ռ�����" << endl;
    
    cout << ">"; int usermethod = StringToInt(Kgetline());
    if (usermethod == 1) {
        cout << "ָ������PID:>";
        int targetPid= StringToInt(Kgetline());
        int a = KswordRegThread("����" + to_string(targetPid) + "��TCP��ɱ����");
        threads.emplace_back(KswordKeepKillTCP, targetPid, a);
    }
    else if (usermethod == 2) {
        cout << "ָ������PID:>";
        int targetPid = StringToInt(Kgetline());
        GetTcpConnectionsByPid(targetPid);
    }
    else if (usermethod == 3) {
        cout << "ָ���˿�port:>";
        int targetPort = StringToShort(Kgetline());
        GetTcpConnectionsByPort(targetPort);
    }
    else {
        KMesErr("δ����Ĳ�����ʽ");
    }
    return;
}
string ConvertDWORDToIPAddress(DWORD dwAddr) {
        // �� DWORD ת��Ϊ struct in_addr
        struct in_addr inAddr;
        inAddr.S_un.S_addr = dwAddr;

        // ʹ�� inet_ntoa ת��Ϊ���ʮ�����ַ���
        char* ipStr = inet_ntoa(inAddr);

        // ���� std::string ���͵� IP ��ַ
        return std::string(ipStr);
    }
void KswordKeepKillTCP(int targetPid, int ThreadID) {
    cout << "���뵥���̣߳��߳��嵥���Ϊ" << ThreadID << endl;
    while (!KswordThreadStop[ThreadID]) {
        //Sleep(100);
        DWORD dwSize = 0;
        DWORD dwRetVal = 0;
        // ��ȡ TCP ���С
        GetTcpTable2(NULL, &dwSize, FALSE);
        PMIB_TCPTABLE2 pTcpTable = (PMIB_TCPTABLE2)malloc(dwSize);
        if (pTcpTable == NULL) {
            KswordSend1("�߳�" + to_string(targetPid) + "��TCP�����̱߳�����һ������" + "�����ڴ�ʧ��");
            return;
        }
        // ��ȡ TCP ��
        dwRetVal = GetTcpTable2(pTcpTable, &dwSize, FALSE);
        if (dwRetVal != NO_ERROR) {
            KswordSend1("�߳�" + to_string(targetPid) + "��TCP�����̱߳�����һ������" + "��ȡ TCP ��ʧ�ܣ������룺" + to_string(dwRetVal));
            free(pTcpTable);
            return;
        }
        for (DWORD i = 0; i < pTcpTable->dwNumEntries; i++) {
            if (pTcpTable->table[i].dwOwningPid == targetPid) {

                char localAddr[16];
                char remoteAddr[16];
                if (pTcpTable->table[i].dwLocalAddr == 0||pTcpTable->table[i].dwLocalAddr == 16777343)continue;
                KswordSend1("�߳�" + to_string(targetPid) + "��TCP�����̱߳��棺" + "�ҵ����ӣ����ص�ַ "
                    + ConvertDWORDToIPAddress(pTcpTable->table[i].dwLocalAddr)
                    + ":" + to_string(pTcpTable->table[i].dwLocalPort)
                    + " Զ�̵�ַ " + ConvertDWORDToIPAddress(pTcpTable->table[i].dwRemoteAddr)
                    + ":" + to_string(pTcpTable->table[i].dwRemotePort)
                    + " ״̬��" + to_string(pTcpTable->table[i].dwState)/*+"�������"
                    + to_string(pTcpTable->table[i].dwLocalAddr)*/);
                //cout << "damnit!!" << pTcpTable->table[i].dwLocalAddr;
                MIB_TCPROW row = { 0 };
                row.dwState = pTcpTable->table[i].dwState;
                row.dwLocalAddr = pTcpTable->table[i].dwLocalAddr;
                row.dwLocalPort = pTcpTable->table[i].dwLocalPort;
                row.dwRemoteAddr = pTcpTable->table[i].dwRemoteAddr;
                row.dwRemotePort = pTcpTable->table[i].dwRemotePort;

                // ��������״̬Ϊɾ��
                row.dwState = MIB_TCP_STATE_DELETE_TCB;
                dwRetVal = SetTcpEntry(&row);
                if (dwRetVal == NO_ERROR) {
                    KswordSend1("�߳�" + to_string(targetPid) + "��TCP�����̱߳��棺" + "�����ѹر�");
                }
                else {
                    KswordSend1("�߳�" + to_string(targetPid) + "��TCP�����̱߳���:" + "�ر�����ʧ�ܣ������룺" + to_string(dwRetVal));
                }

            }
        }
        free(pTcpTable);

    }
    KswordSend1("�߳�" + to_string(targetPid) + "��TCP�����߳����˳�");
    return;
}
std::string inet_ntoa(DWORD ip) {
    in_addr addr;
    addr.S_un.S_addr = ip; // �� DWORD ת��Ϊ in_addr
    return std::string(inet_ntoa(addr)); // ���� inet_ntoa ����
}

// ��ȡָ��PID��TCP������Ϣ
void GetTcpConnectionsByPid(DWORD pid) {
    DWORD size = 0;
    DWORD ret = GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (ret != ERROR_INSUFFICIENT_BUFFER) {
        std::cerr << "GetExtendedTcpTable failed with error: " << ret << std::endl;
        return;
    }

    std::vector<char> buffer(size);
    ret = GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (ret != NO_ERROR) {
        std::cerr << "GetExtendedTcpTable failed with error: " << ret << std::endl;
        return;
    }

    PMIB_TCPTABLE_OWNER_PID tcpTable = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
    DWORD numEntries = tcpTable->dwNumEntries;

    std::cout << "PID: " << pid << " �� TCP ������Ϣ��" << std::endl;
    cprint("���ص�ַ\t���ض˿�\tԶ�̵�ַ\tԶ�̶˿�\t״̬\tPID", 2, 0); cout << endl;
    for (DWORD i = 0; i < numEntries; ++i) {
        if (tcpTable->table[i].dwOwningPid == pid) {
            std::cout << inet_ntoa(tcpTable->table[i].dwLocalAddr) << "\t"
                << ntohs(tcpTable->table[i].dwLocalPort) << "\t"
                << inet_ntoa(tcpTable->table[i].dwRemoteAddr) << "\t"
                << ntohs(tcpTable->table[i].dwRemotePort) << "\t"
                << tcpTable->table[i].dwState << "\t"
                << tcpTable->table[i].dwOwningPid << std::endl;
        }
    }
}
void GetTcpConnectionsByPort(USHORT port) {
    DWORD size = 0;
    DWORD ret = GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (ret != ERROR_INSUFFICIENT_BUFFER) {
        std::cerr << "GetExtendedTcpTable failed with error: " << ret << std::endl;
        return;
    }

    std::vector<char> buffer(size);
    ret = GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (ret != NO_ERROR) {
        std::cerr << "GetExtendedTcpTable failed with error: " << ret << std::endl;
        return;
    }

    PMIB_TCPTABLE_OWNER_PID tcpTable = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
    DWORD numEntries = tcpTable->dwNumEntries;

    std::cout << "�˿� " << port << " �� TCP ������Ϣ��" << std::endl;
    cprint("���ص�ַ\t���ض˿�\tԶ�̵�ַ\tԶ�̶˿�\t״̬\tPID", 2, 0);

    bool found = false;
    for (DWORD i = 0; i < numEntries; ++i) {
        if (ntohs(tcpTable->table[i].dwLocalPort) == port) {
            found = true;
            std::cout << inet_ntoa(tcpTable->table[i].dwLocalAddr) << "\t"
                << ntohs(tcpTable->table[i].dwLocalPort) << "\t"
                << inet_ntoa(tcpTable->table[i].dwRemoteAddr) << "\t"
                << ntohs(tcpTable->table[i].dwRemotePort) << "\t"
                << tcpTable->table[i].dwState << "\t"
                << tcpTable->table[i].dwOwningPid << std::endl;
        }
    }

    if (!found) {
        KMesInfo("�ö˿�Ŀǰû�б�ռ��");
    }
}
#endif