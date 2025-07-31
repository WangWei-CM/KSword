#ifdef KSWORD_WITH_COMMAND
#include "..\..\KswordTotalHead.h"

using namespace std;
void GetTcpConnectionsByPid(DWORD pid);
void GetTcpConnectionsByPort(USHORT port);
void KswordNetManager() {
    cout << "1>监听并拦截指定进程的所有TCP请求" << endl <<
        "2>列举指定进程的TCP连接" << endl <<
        "3>输出指定端口的占用情况" << endl;
    
    cout << ">"; int usermethod = StringToInt(Kgetline());
    if (usermethod == 1) {
        cout << "指定进程PID:>";
        int targetPid= StringToInt(Kgetline());
        int a = KswordRegThread("进程" + to_string(targetPid) + "的TCP截杀进程");
        threads.emplace_back(KswordKeepKillTCP, targetPid, a);
    }
    else if (usermethod == 2) {
        cout << "指定进程PID:>";
        int targetPid = StringToInt(Kgetline());
        GetTcpConnectionsByPid(targetPid);
    }
    else if (usermethod == 3) {
        cout << "指定端口port:>";
        int targetPort = StringToShort(Kgetline());
        GetTcpConnectionsByPort(targetPort);
    }
    else {
        KMesErr("未定义的操作方式");
    }
    return;
}
string ConvertDWORDToIPAddress(DWORD dwAddr) {
        // 将 DWORD 转换为 struct in_addr
        struct in_addr inAddr;
        inAddr.S_un.S_addr = dwAddr;

        // 使用 inet_ntoa 转换为点分十进制字符串
        char* ipStr = inet_ntoa(inAddr);

        // 返回 std::string 类型的 IP 地址
        return std::string(ipStr);
    }
void KswordKeepKillTCP(int targetPid, int ThreadID) {
    cout << "分离单个线程，线程清单编号为" << ThreadID << endl;
    while (!KswordThreadStop[ThreadID]) {
        //Sleep(100);
        DWORD dwSize = 0;
        DWORD dwRetVal = 0;
        // 获取 TCP 表大小
        GetTcpTable2(NULL, &dwSize, FALSE);
        PMIB_TCPTABLE2 pTcpTable = (PMIB_TCPTABLE2)malloc(dwSize);
        if (pTcpTable == NULL) {
            KswordSend1("线程" + to_string(targetPid) + "的TCP监视线程报告了一个错误：" + "分配内存失败");
            return;
        }
        // 获取 TCP 表
        dwRetVal = GetTcpTable2(pTcpTable, &dwSize, FALSE);
        if (dwRetVal != NO_ERROR) {
            KswordSend1("线程" + to_string(targetPid) + "的TCP监视线程报告了一个错误：" + "获取 TCP 表失败，错误码：" + to_string(dwRetVal));
            free(pTcpTable);
            return;
        }
        for (DWORD i = 0; i < pTcpTable->dwNumEntries; i++) {
            if (pTcpTable->table[i].dwOwningPid == targetPid) {

                char localAddr[16];
                char remoteAddr[16];
                if (pTcpTable->table[i].dwLocalAddr == 0||pTcpTable->table[i].dwLocalAddr == 16777343)continue;
                KswordSend1("线程" + to_string(targetPid) + "的TCP监视线程报告：" + "找到连接：本地地址 "
                    + ConvertDWORDToIPAddress(pTcpTable->table[i].dwLocalAddr)
                    + ":" + to_string(pTcpTable->table[i].dwLocalPort)
                    + " 远程地址 " + ConvertDWORDToIPAddress(pTcpTable->table[i].dwRemoteAddr)
                    + ":" + to_string(pTcpTable->table[i].dwRemotePort)
                    + " 状态：" + to_string(pTcpTable->table[i].dwState)/*+"早密码比"
                    + to_string(pTcpTable->table[i].dwLocalAddr)*/);
                //cout << "damnit!!" << pTcpTable->table[i].dwLocalAddr;
                MIB_TCPROW row = { 0 };
                row.dwState = pTcpTable->table[i].dwState;
                row.dwLocalAddr = pTcpTable->table[i].dwLocalAddr;
                row.dwLocalPort = pTcpTable->table[i].dwLocalPort;
                row.dwRemoteAddr = pTcpTable->table[i].dwRemoteAddr;
                row.dwRemotePort = pTcpTable->table[i].dwRemotePort;

                // 设置连接状态为删除
                row.dwState = MIB_TCP_STATE_DELETE_TCB;
                dwRetVal = SetTcpEntry(&row);
                if (dwRetVal == NO_ERROR) {
                    KswordSend1("线程" + to_string(targetPid) + "的TCP监视线程报告：" + "连接已关闭");
                }
                else {
                    KswordSend1("线程" + to_string(targetPid) + "的TCP监视线程报告:" + "关闭连接失败，错误码：" + to_string(dwRetVal));
                }

            }
        }
        free(pTcpTable);

    }
    KswordSend1("线程" + to_string(targetPid) + "的TCP监视线程已退出");
    return;
}
std::string inet_ntoa(DWORD ip) {
    in_addr addr;
    addr.S_un.S_addr = ip; // 将 DWORD 转换为 in_addr
    return std::string(inet_ntoa(addr)); // 调用 inet_ntoa 函数
}

// 获取指定PID的TCP连接信息
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

    std::cout << "PID: " << pid << " 的 TCP 连接信息：" << std::endl;
    cprint("本地地址\t本地端口\t远程地址\t远程端口\t状态\tPID", 2, 0); cout << endl;
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

    std::cout << "端口 " << port << " 的 TCP 连接信息：" << std::endl;
    cprint("本地地址\t本地端口\t远程地址\t远程端口\t状态\tPID", 2, 0);

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
        KMesInfo("该端口目前没有被占用");
    }
}
#endif