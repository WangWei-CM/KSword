#ifdef KSWORD_WITH_COMMAND
#include "../../KswordTotalHead.h"
using namespace std;

int KswordMainHelp() {

	cout << "֧�ֵ������У�===============================================" << endl <<
		"1>getsys\t\t��systemȨ������"<<endl<<
		"2>apt\t\t\t�ӷ�����IP�ϻ�ȡKsword��Դ�ļ�" << endl <<
		"3>sethc\t\t�滻ճ�ͼ���֮�������������5Shift������ط�����"<<endl<<
		"4>ai\t\t\t���ٻ�ȡAI�Ļظ�"<<endl<<
		"5>guimgr\t\t����ͼ�ν��棬��������" << endl <<
		"6>procmgr\t\t�������" << endl <<
		"7>drivermgr\t\t������������" << endl <<
		"8>threadmgr\t\t����Ksword�����߳�" << endl <<
		"9>tasklist\t\t��ȡ��ǰ�����еĽ���" << endl <<
		"10>inputmode\t\t�л�����ģʽ" << endl <<
		"11>netmgr\t\t������������" << endl <<
		"12>topmost\t\t�����ö�����ҪsystemȨ�ޣ������µĴ��ڲ����й���ԱȨ�ޣ��Ѿ����ã���ʹ��sos�����"<<endl<<
		"13>sos\t\t�ռ����ȷ�������ɱһ���ö�����ɱһ�й��ӣ���ֹ¼��"<<endl<<
		"14>asyn\t\t�첽ִ��cmd����"<<endl<<
		"15>avkill\t\t�������������ʵ���Թ��ܣ�" << endl <<
		"16>ocp\t\t\t����ļ�ռ��"<< endl <<
		"������Ӧ��Ž��в�ѯ������exit�˳���" << endl;
KswordHelpStart:
	string userinput2 = Kgetline();
	if (userinput2 == "exit")return 0;
	int userinput = StringToInt(userinput2);
	if (userinput == 0)return 0;
	else if (userinput == 1) {
		cout << "getsys����ȡϵͳȨ�ޣ�Ksword������������Ҫ����ԱȨ��" << endl <<
			"ԭ������winlogon.exe��������";
		goto KswordHelpStart;
	}
	else if (userinput == 2) {
		cout << "apt���������й������ز��Ҹ��¡���Դ·��Ĭ����<��Ŀ¼>:\\kswordrc�¡�" << endl <<
			"apt update:����������Դ�б�" << endl <<
			"apt upgrade:��������Դ�б�������" << endl <<
			"apt install <ģ����>:�ӷ��������ض�Ӧ���" << endl <<
			"setrcpath������ά������������Դ��ַ" << endl <<
			"setserver�����÷�������ַ" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 5) {
		cout << "guimgr�����͹������������ϵĴ���" << endl <<
			"1>����PID���������̣�����self�������Լ���tasklist�Ա������н��̣�exit�˳���" << endl <<
			"2>������ʾѡ�񴰿ڣ�" << endl <<
			"3>������ʾѡ�����������" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 4) {
		cout << "ai:���ٷ���kimi��" << endl <<
			"��ҪAPI KEY�Լ������ʡ���ʱ��֧�����ġ�" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 3) {
		cout << "sethc:��Ksword�滻��ϵͳճ�ͼ���" << endl <<
			"�滻������԰���5��Shift����Ksword���൱ǿ����Ϊ��```Ctrl+Alt+Del```��ȫ�����������������涼���Ե���KswordӦ�ó�����Ҳ����ΪʲôKsword����ʱ��Ҫ���������롣" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 6) {
		cout << "������̡���Ҫ�ṩ����PID��������ʾѡ��Խ��̵Ĳ�����" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 7) {
		cout << "������/��������й�����ͨ��sc�����С�������ʾѡ���Ӧ�Ĳ���" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 8) {
		cout << "�鿴������ksword�д��ڵ��̡߳������ֶ�ע��ġ�" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 9) {
		cout << "��ȡ���н��̡�����cmd�еķ�����" << endl <<
			"�����һ����ĸ���Կ��ٶ�λϵͳ���������Ǹ���ĸΪ����ĸ�Ľ���." << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 10) {
		cout << "�л����������ģʽ��������ʾѡ��ϣ���ķ�ʽ��Ĭ��Ϊ0��" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 11) {
		cout << "netmgr:�����жϻ���ҵ�ǰ���Ե��������ӡ�" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 12) {
		cout << "topmost�������ö����㼶�����������������Ļ���̡���ȡ˫���̷�������̨systemȨ�ޣ�ǰ̨��ͨ�û�Ȩ�ޡ��ܵ�ʵ�ִ������⣬Ŀǰ�Ѿ�ͣ�ã���ʹ�ø��µĹ���sos��" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 13) {
		cout << "sos�����������沢�л����ռ�������/������������������������asyn explorer���Կ������滷�����뿪ʱ�����������������г��򡣴��⣬Win11��Ҫ��getsys����������explorer�޷�������ԭ����win11����˷����������ƣ�ֻ�������εĳ������ʹ��������ܡ��������ԣ�taskmgr�������ǿ���ͨ���ģ�����ҪsystemȨ�ޡ�" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 14) {
		cout << "asyn���첽ִ��cmd������߱�ĳЩ�����飬���ܴ������⣬����ϵͳĿ¼�ǿ�������ʹ�õġ�" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 15) {
		cout << "avkill������ɱ���������Coffee Studio����������Ŀǰ��360���˾���֧���в����á�" << endl;
		goto KswordHelpStart;
	}
	else if (userinput == 16) {
		cout << "ocp������ļ�ռ�á�������������·��������·���Ŀ�ͷ�������ִ�Сд�����뵥����ĸ��ʾ��ѯ��Ӧ�̵�ռ�á�" << endl;
		goto KswordHelpStart;
	}
	else {
		cout << "δ����Ĳ�ѯ��Ĭ���˳�����" << endl;
	}
	return 0;
}
#endif