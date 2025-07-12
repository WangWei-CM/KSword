#ifdef KSWORD_WITH_COMMAND
#pragma once
#ifndef KSWORD_SELFSOCKET_HEAD
#define KSWORD_SELFSOCKET_HEAD

#include "../../Support/ksword.h"

using namespace std;

extern int KswordMain1(char*);
extern int KswordSelfProc1(int);
extern int KswordSelfPipe1(int);
extern int KswordSend1(string);
extern void KMain1Refresh();

int KswordSendMain(string);
int KswordMainPipe();
int KswordMainSockPipe();
extern bool KswordPipeMode;

extern int KswordMain2();
extern HANDLE KswordSelfProc2(int Runtime);


extern int KswordMain7();
extern HANDLE KswordSelfProcess7(int Runtime);
#endif // !KSWORD_SELFSOCKET_HEAD
#endif