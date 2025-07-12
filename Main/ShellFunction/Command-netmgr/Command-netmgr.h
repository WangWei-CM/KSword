#ifdef KSWORD_WITH_COMMAND
#pragma once
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#include "KswordTotalHead.h"
void KswordNetManager();
void KswordKeepKillTCP(int,int);
#endif