#ifdef KSWORD_WITH_COMMAND
#include "../../KswordTotalHead.h"
using namespace std;

int KswordMainHelp() {

	cout << "支持的命令有：===============================================" << endl <<
		"1>getsys\t\t以system权限启动"<<endl<<
		"2>apt\t\t\t从服务器IP上获取Ksword资源文件" << endl <<
		"3>sethc\t\t替换粘滞键，之后可以连续按下5Shift从任意地方启动"<<endl<<
		"4>ai\t\t\t快速获取AI的回复"<<endl<<
		"5>guimgr\t\t管理图形界面，各个窗口" << endl <<
		"6>procmgr\t\t管理进程" << endl <<
		"7>drivermgr\t\t管理驱动程序" << endl <<
		"8>threadmgr\t\t管理Ksword自身线程" << endl <<
		"9>tasklist\t\t获取当前电脑中的进程" << endl <<
		"10>inputmode\t\t切换输入模式" << endl <<
		"11>netmgr\t\t管理网络连接" << endl <<
		"12>topmost\t\t超级置顶，需要system权限，但是新的窗口不具有管理员权限（已经弃用，请使用sos替代）"<<endl<<
		"13>sos\t\t终极拯救方案，秒杀一切置顶，秒杀一切钩子，防止录屏"<<endl<<
		"14>asyn\t\t异步执行cmd命令"<<endl<<
		"15>avkill\t\t反反病毒软件（实验性功能）" << endl <<
		"16>ocp\t\t\t检测文件占用"<< endl <<
		"请键入对应标号进行查询，输入exit退出：" << endl;
KswordHelpStart:
	string userinput2 = Kgetline();
	if (userinput2 == "exit")return 0;
	int userinput = StringToInt(userinput2);
	if (userinput == 0)return 0;
	else if (userinput == 1) {
		cout << "getsys：获取系统权限，Ksword将会重启。需要管理员权限" << endl <<
			"原理：复制winlogon.exe令牌启动";
		goto KswordHelpStart;
	}
	else if (userinput == 2) {
		cout << "apt：联网进行工具下载并且更新。资源路径默认在<根目录>:\\kswordrc下。" << endl <<
			"apt update:下载现有资源列表" << endl <<
			"apt upgrade:将现有资源列表读入程序" << endl <<
			"apt install <模块名>:从服务器下载对应组件" << endl <<
			"setrcpath（不再维护）：设置资源地址" << endl <<
			"setserver：设置服务器地址" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 5) {
		cout << "guimgr：监测和管理所有桌面上的窗口" << endl <<
			"1>输入PID以锁定进程，其中self以锁定自己，tasklist以遍历所有进程，exit退出；" << endl <<
			"2>按照提示选择窗口；" << endl <<
			"3>按照提示选择操作方法。" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 4) {
		cout << "ai:快速访问kimi。" << endl <<
			"需要API KEY以继续访问。暂时不支持中文。" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 3) {
		cout << "sethc:把Ksword替换掉系统粘滞键。" << endl <<
			"替换过后可以按下5次Shift运行Ksword，相当强大，因为在```Ctrl+Alt+Del```安全界面甚至是锁屏界面都可以调出Ksword应用程序。这也就是为什么Ksword启动时候要求输入密码。" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 6) {
		cout << "管理进程。需要提供进程PID。按照提示选择对进程的操作。" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 7) {
		cout << "对驱动/服务项进行管理。不通过sc命令行。按照提示选择对应的操作" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 8) {
		cout << "查看并管理ksword中存在的线程。这是手动注册的。" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 9) {
		cout << "获取所有进程。并非cmd中的方法。" << endl <<
			"后面跟一个字母可以快速定位系统中所有以那个字母为首字母的进程." << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 10) {
		cout << "切换所有输入的模式。按照提示选择希望的方式。默认为0。" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 11) {
		cout << "netmgr:管理，中断或查找当前电脑的网络连接。" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 12) {
		cout << "topmost：超级置顶，层级高于任务管理器与屏幕键盘。采取双进程方案，后台system权限，前台普通用户权限。管道实现存在问题，目前已经停用，请使用更新的功能sos。" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 13) {
		cout << "sos：开启新桌面并切换。终极反极域/反锁屏解决方案。新桌面配合asyn explorer可以开启桌面环境。离开时请销毁其他桌面所有程序。此外，Win11需要先getsys，但是这样explorer无法启动，原因是win11添加了访问令牌限制，只有受信任的程序可以使用这个功能。经过测试，taskmgr启动其是可以通过的，不需要system权限。" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 14) {
		cout << "asyn：异步执行cmd命令。不具备某些环境块，可能存在问题，但是系统目录是可以正常使用的。" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 15) {
		cout << "avkill：消灭杀毒软件，由Coffee Studio合作开发。目前对360带核晶的支持尚不良好。" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 16) {
		cout << "ocp：检查文件占用。可以输入完整路径，或者路径的开头，不区分大小写。输入单个字母表示查询对应盘的占用。" << endl;
		goto KswordHelpStart;
	}
	else {
		cout << "未定义的查询，默认退出……" << endl;
	}
	return 0;
}
#endif