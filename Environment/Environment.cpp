#include "..\Support\ksword.h"
#include "Environment.h"
std::string AuthName;
std::string HostName;
std::wstring ExePath;
int ScreenX;
int ScreenY;
bool IsAuthAdmin;
void KEnviProb() {
	AuthName = GetUserName();
	HostName = GetHostName();
	IsAuthAdmin = IsAdmin();
	ExePath = GetSelfPath();
	ScreenX= GetSystemMetrics(SM_CXSCREEN);
	ScreenY = GetSystemMetrics(SM_CYSCREEN);
}