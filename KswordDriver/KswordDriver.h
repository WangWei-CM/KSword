#ifdef KSWORD_WITH_COMMAND
#pragma once
#define DEVICE_NAME "\\\\.\\MessageLink"
#include "../Main/KswordTotalHead.h"

// ����IOCTL���룬������������һ��
#define IOCTL_SEND_MESSAGE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RECEIVE_MESSAGE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

extern bool KswordDriverSend(const std::string& sendBuffer);
extern std::string KswordDriverRecv();
extern void KswordDriverCommand(const std::string& command);
#endif