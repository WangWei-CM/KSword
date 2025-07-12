#ifdef KSWORD_WITH_COMMAND
#pragma once
#include "../../KswordTotalHead.h"
extern string KswordThread[50];
extern bool KswordThreadStop[50];
extern int KswordThreadTopNum;
extern std::vector<std::thread> threads;


int KswordRegThread(string threadName);
void KswordStopThread(int threadNum);
void KswordStartThread(int threadNum);
void KswordThreadMgr();

#endif