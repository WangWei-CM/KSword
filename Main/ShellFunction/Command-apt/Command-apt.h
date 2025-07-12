#ifdef KSWORD_WITH_COMMAND
#pragma once
#include "../../KswordTotalHead.h"

inline void KInlineForCommandapt1() {

	string updatecmd;
	updatecmd += "curl -o " + localadd + "version.txt " + "http://" + serverip + ":80/kswordrc/" + "version.txt";
	KMesInfo(updatecmd);
	RunCmdNow(updatecmd.c_str());
	cout << "Update finished." << endl;
}
inline void KInlineForCommandapt2() {

	string path;
	path = localadd + "version.txt";
	KMesInfo("开始更新...");
	readFile(path);
}
inline void KInlineForCommandapt3() {

	string userinput;
	cin >> userinput;
	const string& userinputref = userinput;
	if (directoryExists(userinputref))localadd = userinput;
	else KMesErr("指定的目录不存在");
}
#endif