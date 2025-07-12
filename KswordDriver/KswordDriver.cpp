#ifdef KSWORD_WITH_COMMAND
#include "../Main/KswordTotalHead.h"
using namespace std;
// 总函数，接收命令，发送给驱动程序，等待返回并输出
void KswordDriverCommand(const std::string& command) {
    if (KswordDriverSend(command)) {
        std::string response = KswordDriverRecv();
        std::cout << "驱动程序返回：" << response << std::endl;
    }
    else {
        KMesErr("无法与驱动程序通信");
    }
}

bool KswordDriverSend(const std::string& sendBuffer) {
    DWORD bytesReturned;
    HANDLE hDevice = CreateFileA(
        DEVICE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hDevice == INVALID_HANDLE_VALUE) {
        std::cerr << "打开虚拟设备失败，错误代码" << GetLastError() << std::endl;
        return false;
    }
    bool success = DeviceIoControl(hDevice, IOCTL_SEND_MESSAGE, (LPVOID)sendBuffer.c_str(), sendBuffer.size() + 1, NULL, 0, &bytesReturned, NULL);
    CloseHandle(hDevice);
    return success;
}

std::string KswordDriverRecv() {
    char recvBuffer[256] = { 0 };
    DWORD bytesReturned;
    HANDLE hDevice = CreateFileA(
        DEVICE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hDevice == INVALID_HANDLE_VALUE) {
        std::cerr << "打开虚拟设备失败，错误代码" << GetLastError() << std::endl;
        return "";
    }
    if (DeviceIoControl(hDevice, IOCTL_RECEIVE_MESSAGE, NULL, 0, recvBuffer, sizeof(recvBuffer), &bytesReturned, NULL)) {
        CloseHandle(hDevice);
        return std::string(recvBuffer);
    }
    else {
        CloseHandle(hDevice);
        std::cerr << "无法从驱动程序获取数据，错误代码" << GetLastError() << std::endl;
        return "";
    }
}
#endif