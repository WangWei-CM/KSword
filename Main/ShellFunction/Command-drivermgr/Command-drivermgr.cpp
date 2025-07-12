#ifdef KSWORD_WITH_COMMAND
#pragma once
#include "../../KswordTotalHead.h"
using namespace std;
int usermethod;
string servicename;
string driverpath;
bool cmdinput = 0;
int drivermgr() {
	cmdinput = 0;
	if (cmdparanum != 0) {
		cmdinput = 1;
		for (int i = 1; i <= cmdparanum; i++) {
			if (cmdpara[i] == "-load" || cmdpara[i] == "/load") {
				usermethod = 1;
				if (i + 2 <= cmdparanum) {
					servicename = cmdpara[i + 2];
					driverpath = cmdpara[i + 1];
				}
			}
			else if (cmdpara[i] == "-start" || cmdpara[i] == "/start") {
				usermethod = 2;
				if (i + 1 <= cmdparanum) {
					servicename = cmdpara[i + 2];
				}
			}
			else if (cmdpara[i] == "-stop" || cmdpara[i] == "/stop") {
				usermethod = 3;
				if (i + 1 <= cmdparanum) {
					servicename = cmdpara[i + 2];
				}
			}
			else if (cmdpara[i] == "-unload" || cmdpara[i] == "/unload") {
				usermethod = 4;
				if (i + 1 <= cmdparanum) {
					servicename = cmdpara[i + 2];
				}
			}
			else if (cmdpara[i] == "-install" || cmdpara[i] == "/install") {
				usermethod = 5;
			}
		}
	}
	if (cmdparanum == 0) {
		cout << "1>��������" << endl
			<< "2>ж������" << endl
			<< "3>��������" << endl
			<< "4>ֹͣ����" << endl
			<< "5>��װ֤��" << endl
			<< ">";
		usermethod = StringToInt(Kgetline());
	}
		if (usermethod == 1) {
			if (!cmdinput) {
				cout << "������������·��:(ʹ��.\\��ͷ��ʹ����Դ·����>";
				driverpath = Kgetline();
				if (driverpath.substr(0, 2) == ".\\") {
					driverpath = localadd + driverpath.substr(2);
					cout << "�Զ���ȫ����·����" << driverpath << endl;
				}
				cout << "����Ŀ���������:>";	servicename = Kgetline();
			}
			LoadWinDrive(driverpath, servicename);
			StartDriverService(servicename);
		}
		else if (usermethod == 2) {
			if (!cmdinput)
			{
				cout << "����Ŀ���������:>"; servicename = Kgetline();
			}
			UnLoadWinDrive(Kgetline());
		}
		else if (usermethod == 3) {
			if (!cmdinput) {
				cout << "����Ŀ���������:>"; servicename = Kgetline();
			}
			StartDriverService(servicename);
		}
		else if (usermethod == 4) {
			if (!cmdinput) {
				cout << "����Ŀ���������:>"; servicename = Kgetline();
			}
			StopDriverService(servicename);
		}
		else if (usermethod == 5) {
			cout << "�����κζ��������ò���ǩ��ģʽ,����������" << endl;
			if (Kgetline() == "") {
				HCERTSTORE hCertStore = CertOpenStore(
					CERT_STORE_PROV_SYSTEM, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"My");
				if (!hCertStore) {
					std::cerr << "CertOpenStore failed." << std::endl;
					return false;
				}

				// ����֤��������
				CERT_NAME_BLOB subjectName = { 0 };
				subjectName.cbData = 13;
				subjectName.pbData = (BYTE*)"CN=KswordTestCert";

				CERT_NAME_BLOB issuerName = subjectName;

				CERT_INFO certInfo = { 0 };
				certInfo.dwVersion = CERT_V3;
				certInfo.Subject = subjectName;
				certInfo.Issuer = issuerName;
				certInfo.NotBefore = { 0 };
				certInfo.NotBefore.dwLowDateTime = 0;
				certInfo.NotBefore.dwHighDateTime = 0;
				certInfo.NotAfter = { 0 };
				certInfo.NotAfter.dwLowDateTime = 0x7FFFFFFFFFFFFFFF;
				certInfo.NotAfter.dwHighDateTime = 0x7FFFFFFFFFFFFFFF;

				// ������ǩ��֤��
				PCCERT_CONTEXT pCertContext = CertCreateSelfSignCertificate(
					NULL, &subjectName, 0, NULL, NULL, NULL, NULL, NULL);
				if (!pCertContext) {
					KMesErr("������ǩ��֤��ʱ��������");
					CertCloseStore(hCertStore, 0);
					return false;
				}

				// ��֤����ӵ�ϵͳ֤��洢��
				if (!CertAddCertificateContextToStore(
					hCertStore, pCertContext, CERT_STORE_ADD_REPLACE_EXISTING, NULL)) {
					KMesErr("��֤����ӵ�ϵͳ֤��洢��ʧ��");
					CertFreeCertificateContext(pCertContext);
					CertCloseStore(hCertStore, 0);
					return false;
				}

				CertFreeCertificateContext(pCertContext);
				CertCloseStore(hCertStore, 0);

				return true;
			}
			else {
				RunCmdNow("bcdedit /set testsigning on && shutdown -r -t 0");
			}
		}
		else {
			KMesErr("δ����Ĳ�����ʽ");
		}
		cout << "======= ������ɡ�=======" << endl;
		return 0;
	
}
#endif