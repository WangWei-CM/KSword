#include "../Main/KswordTotalHead.h"

#include <wincrypt.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>

#pragma comment(lib, "crypt32.lib")
bool isR0=false;
#define CURRENT_MODULE "��������"
// ��������
bool AddPFXToTrustedStore(const std::wstring& pfxFileName, const std::wstring& password);
std::vector<BYTE> ReadCertificateFile(const std::wstring& fileName);
BOOL InstallDriver(const std::wstring& drvAbsPath);   // ���أ���װ������
BOOL StartDriver();                                   // ��������
BOOL StopDriver();                                    // ֹͣ����
BOOL UnloadDriver();                                  // ж������
static std::wstring GetDriverAbsolutePath();                 // ��������ȡ�����ļ�����·��

int  ReleaseDriverToFile()
{
    SetDllDirectoryW(L".");
    HRSRC D3DX9DLLResource = FindResource(NULL, MAKEINTRESOURCE(IDR_SYS1), _T("SYS"));
    if (D3DX9DLLResource == NULL)
    {
        DWORD err = GetLastError();
        return 0;
    }

    // ������Դ
    HGLOBAL hResourceD3DX9 = LoadResource(NULL, D3DX9DLLResource);
    if (hResourceD3DX9 == NULL)
        return 0;

    // ������Դ��ȡ����ָ��
    LPVOID pResourceData = LockResource(hResourceD3DX9);
    if (pResourceData == NULL)
        return 0;

    // ��ȡ��Դ��С
    DWORD dwResourceSize = SizeofResource(NULL, D3DX9DLLResource);
    if (dwResourceSize == 0)
        return 0;

    // ��ȡ����ǰĿ¼
    TCHAR szExePath[MAX_PATH];
    if (GetModuleFileName(NULL, szExePath, MAX_PATH) == 0)
        return 0;

    // ��ȡĿ¼·��
    TCHAR* pLastSlash = _tcsrchr(szExePath, _T('\\'));
    if (pLastSlash == NULL)
        return 0;
    *pLastSlash = _T('\0');

    // ����Ŀ��DLL·��
    std::basic_string<TCHAR> strOutputPath = std::basic_string<TCHAR>(szExePath) + _T("\\KswordDriver.sys");

    // д��DLL�ļ���������ģʽ��
    std::ofstream KswordD3DX9dllRelease(strOutputPath.c_str(), std::ios::binary);
    if (!KswordD3DX9dllRelease.is_open())
        return 0;

    KswordD3DX9dllRelease.write(static_cast<const char*>(pResourceData), dwResourceSize);
    if (!KswordD3DX9dllRelease.good())
    {
        KswordD3DX9dllRelease.close();
        DeleteFile(strOutputPath.c_str()); // д��ʧ��ʱɾ���������ļ�
        return 0;
    }

    KswordD3DX9dllRelease.close();
    return 1;

}
int  ReleasePFXToFile()
{
    SetDllDirectoryW(L".");
    HRSRC D3DX9DLLResource = FindResource(NULL, MAKEINTRESOURCE(IDR_PFX1), _T("PFX"));
    if (D3DX9DLLResource == NULL)
    {
        DWORD err = GetLastError();
        return 0;
    }

    // ������Դ
    HGLOBAL hResourceD3DX9 = LoadResource(NULL, D3DX9DLLResource);
    if (hResourceD3DX9 == NULL)
        return 0;

    // ������Դ��ȡ����ָ��
    LPVOID pResourceData = LockResource(hResourceD3DX9);
    if (pResourceData == NULL)
        return 0;

    // ��ȡ��Դ��С
    DWORD dwResourceSize = SizeofResource(NULL, D3DX9DLLResource);
    if (dwResourceSize == 0)
        return 0;

    // ��ȡ����ǰĿ¼
    TCHAR szExePath[MAX_PATH];
    if (GetModuleFileName(NULL, szExePath, MAX_PATH) == 0)
        return 0;

    // ��ȡĿ¼·��
    TCHAR* pLastSlash = _tcsrchr(szExePath, _T('\\'));
    if (pLastSlash == NULL)
        return 0;
    *pLastSlash = _T('\0');

    // ����Ŀ��DLL·��
    std::basic_string<TCHAR> strOutputPath = std::basic_string<TCHAR>(szExePath) + _T("\\KswordDriver.pfx");

    // д��DLL�ļ���������ģʽ��
    std::ofstream KswordD3DX9dllRelease(strOutputPath.c_str(), std::ios::binary);
    if (!KswordD3DX9dllRelease.is_open())
        return 0;

    KswordD3DX9dllRelease.write(static_cast<const char*>(pResourceData), dwResourceSize);
    if (!KswordD3DX9dllRelease.good())
    {
        KswordD3DX9dllRelease.close();
        DeleteFile(strOutputPath.c_str()); // д��ʧ��ʱɾ���������ļ�
        return 0;
    }

    KswordD3DX9dllRelease.close();
    return 1;

}




// ��ԭ��ں����޸�Ϊ��ͨ����������֤���ļ�������
bool AddTrustedCertificate(const wchar_t* certFileName)
{
    if (certFileName == nullptr)
    {
        kLog.err(C("֤���ļ�������Ϊ��"), C("֤�����"));
        return false;
    }

    // ��ʼ��COM
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr))
    {
        kLog.err(C("COM��ʼ��ʧ�ܣ��������: " + std::to_string(hr)), C("֤�����"));
        return false;
    }

    // ���֤�鵽�����δ洢
    bool success = AddPFXToTrustedStore(L"KswordDriver.pfx", L"KswordDever");

    // �ͷ�COM
    CoUninitialize();

    if (success)
    {
        kLog.info(C("֤���ѳɹ���ӵ������εĸ�֤��䷢�����洢��"), C("֤�����"));
    }
    else
    {
        kLog.err(C("���֤��ʧ�ܡ�"), C("֤�����"));
    }

    return success;
}

// ��ȡ֤���ļ�����
std::vector<BYTE> ReadCertificateFile(const std::wstring& fileName)
{
    std::vector<BYTE> certData;

    // ���ļ�
    std::ifstream file(fileName, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        std::wcout << L"�޷���֤���ļ�: " << fileName << std::endl;
        return certData;
    }

    // ��ȡ�ļ���С
    std::streamsize size = file.tellg();
    if (size <= 0)
    {
        std::wcout << L"֤���ļ�Ϊ�ջ��޷���ȡ��С��" << std::endl;
        return certData;
    }

    // ���仺��������ȡ�ļ�
    file.seekg(0, std::ios::beg);
    certData.resize(static_cast<size_t>(size));

    if (!file.read(reinterpret_cast<char*>(certData.data()), size))
    {
        std::wcout << L"��ȡ֤���ļ�ʧ�ܡ�" << std::endl;
        certData.clear();
    }

    return certData;
}

// ��֤����ӵ������εĸ�֤��䷢�����洢
bool AddPFXToTrustedStore(const std::wstring& pfxFileName, const std::wstring& password = L"") {
    // ��ȡPFX�ļ�����
    std::vector<BYTE> pfxData = ReadCertificateFile(pfxFileName);
    if (pfxData.empty()) {
        kLog.err(C("PFX�ļ���ȡʧ��"), C("֤�����"));
        return false;
    }

    // ����PFX�洢���账�����룩
    CRYPT_DATA_BLOB pfxBlob;
    pfxBlob.cbData = pfxData.size();
    pfxBlob.pbData = pfxData.data();

    HCERTSTORE hPfxStore = PFXImportCertStore(
        &pfxBlob,
        password.c_str(),  // ��PFX�����룬�贫����ȷ����
        CRYPT_MACHINE_KEYSET | CRYPT_EXPORTABLE  // ���ؼ����������
    );
    if (hPfxStore == NULL) {
        DWORD err = GetLastError();
        kLog.err(C("PFX����ʧ�ܣ��������: " + std::to_string(err)), C("֤�����"));
        return false;
    }

    // �򿪱��ؼ������ROOT�洢
    HCERTSTORE hRootStore = CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W,
        0,
        NULL,
        CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG,
        L"ROOT"
    );
    if (hRootStore == NULL) {
        DWORD err = GetLastError();
        kLog.err(C("��ROOT�洢ʧ�ܣ��������: " + std::to_string(err)), C("֤�����"));
        CertCloseStore(hPfxStore, 0);
        return false;
    }

    // ö��PFX�е�֤�鲢��ӵ�ROOT�洢
    PCCERT_CONTEXT pCertContext = NULL;
    bool success = true;
    while ((pCertContext = CertEnumCertificatesInStore(hPfxStore, pCertContext)) != NULL) {
        if (!CertAddCertificateContextToStore(
            hRootStore,
            pCertContext,
            CERT_STORE_ADD_REPLACE_EXISTING,  // �滻�Ѵ��ڵ�֤��
            NULL
        )) {
            DWORD err = GetLastError();
            kLog.err(C("���֤�鵽ROOT�洢ʧ�ܣ��������: " + std::to_string(err)), C("֤�����"));
            success = false;
            break;
        }
    }

    // ������Դ
    CertFreeCertificateContext(pCertContext);
    CertCloseStore(hPfxStore, 0);
    CertCloseStore(hRootStore, 0);

    return success;
}
int KswordDriverInit() {
    if (!ReleasePFXToFile()) {
        kLog.fatal("�޷���KswordDriver.pfx��Դ�ͷŵ��ļ�", CURRENT_MODULE);
    }
    if (!ReleaseDriverToFile()) {
        kLog.fatal("�޷���KswordDriver.sys��Դ�ͷŵ��ļ�", CURRENT_MODULE);
    }
    // 3. ����PFX֤��·������ӵ������δ洢
    TCHAR szExePath[MAX_PATH];
    if (GetModuleFileName(NULL, szExePath, MAX_PATH) == 0) {
        kLog.fatal(C("��ȡ����·��ʧ�ܣ��޷����֤��"), CURRENT_MODULE);
        return 0;
    }
    TCHAR* pLastSlash = _tcsrchr(szExePath, _T('\\'));
    if (pLastSlash == NULL) {
        kLog.fatal(C("��������·��ʧ�ܣ��޷����֤��"), CURRENT_MODULE);
        return 0;
    }
    *pLastSlash = _T('\0');
    std::wstring pfxFilePath = std::wstring(szExePath) + L"\\KswordDriver.pfx";

    // ���֤�鵽�����δ洢
    bool certSuccess = AddTrustedCertificate(pfxFilePath.c_str());
    if (!certSuccess) {
        kLog.fatal(C("֤�����ʧ�ܣ�������ʼ���ж�"), CURRENT_MODULE);
        // ֤�����ʧ��ʱ���������ͷŵ��ļ�
        std::basic_string<TCHAR> driverPath = std::basic_string<TCHAR>(szExePath) + _T("\\KswordDriver.sys");
        DeleteFile(driverPath.c_str());
        DeleteFile(pfxFilePath.c_str());
        return 0;
    }
    kLog.info(C("֤����ӳɹ���������ʼ�����"), CURRENT_MODULE);
    if (!InstallDriver(GetDriverAbsolutePath()))return 0;
    if (!StartDriver())return 0;
    return 1; // ��ʼ���ɹ�����1

}
BOOL InstallDriver(const std::wstring& drvAbsPath)
{
    // 1. �򿪷�����ƹ�������SCM��
    SC_HANDLE hSCM = OpenSCManager(
        NULL,                           // ���ؼ����
        NULL,                           // ��Ĭ�����ݿ⣨SERVICES_ACTIVE_DATABASE��
        SC_MANAGER_ALL_ACCESS           // ���в���Ȩ��
    );
    if (hSCM == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("�򿪷�����ƹ�����ʧ�ܣ�������: ") + std::to_string(err), CURRENT_MODULE);
        return FALSE;
    }

    // 2. ��������������ӵ�SCM��
    SC_HANDLE hService = CreateService(
        hSCM,                           // SCM���
        L"KswordDriverService",         // ���������̶���
        L"KswordDriverService",         // ������ʾ��
        SERVICE_ALL_ACCESS,             // �������Ȩ��
        SERVICE_KERNEL_DRIVER,          // �������ͣ��ں�����
        SERVICE_DEMAND_START,           // ������ʽ���ֶ�����
        SERVICE_ERROR_IGNORE,           // ���������Դ���
        drvAbsPath.c_str(),             // �����ļ�����·��
        NULL,                           // �޸�����
        NULL,                           // ������
        NULL,                           // ����������
        NULL,                           // �����˻���LocalSystem
        NULL                            // �˻����룺LocalSystem��������
    );

    // 3. ����������Դ�ͷ�
    if (hService == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("������������ʧ�ܣ�������: ") + std::to_string(err), CURRENT_MODULE);
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    kLog.info(C("��������װ�ɹ�"), CURRENT_MODULE);
    // �ͷž�����ȷ����SCM��
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return TRUE;
}

// �������������ڹ̶�������KswordDriverService
BOOL StartDriver()
{
    // 1. �򿪷�����ƹ�����
    SC_HANDLE hSCM = OpenSCManager(
        NULL,
        NULL,
        SC_MANAGER_ALL_ACCESS
    );
    if (hSCM == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("�򿪷�����ƹ�����ʧ�ܣ�������: ") + std::to_string(err), CURRENT_MODULE);
        return FALSE;
    }

    // 2. ��ָ����������
    SC_HANDLE hService = OpenService(
        hSCM,
        L"KswordDriverService",         // �̶�������
        SERVICE_ALL_ACCESS
    );
    if (hService == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("����������ʧ�ܣ�������: ") + std::to_string(err), CURRENT_MODULE);
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    // 3. ��������
    if (!StartService(
        hService,
        0,                              // �޷������
        NULL
    ))
    {
        DWORD err = GetLastError();
        kLog.err(C("������������ʧ�ܣ�������: ") + std::to_string(err), CURRENT_MODULE);
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    kLog.info(C("�������������ɹ�"), CURRENT_MODULE);
    // �ͷž��
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return TRUE;
}

// ֹͣ���������ڹ̶�������KswordDriverService������ʱ���
BOOL StopDriver()
{
    // 1. �򿪷�����ƹ�����
    SC_HANDLE hSCM = OpenSCManager(
        NULL,
        NULL,
        SC_MANAGER_ALL_ACCESS
    );
    if (hSCM == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("�򿪷�����ƹ�����ʧ�ܣ�������: ") + std::to_string(err), CURRENT_MODULE);
        return FALSE;
    }

    // 2. ��ָ����������
    SC_HANDLE hService = OpenService(
        hSCM,
        L"KswordDriverService",         // �̶�������
        SERVICE_ALL_ACCESS
    );
    if (hService == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("����������ʧ�ܣ�������: ") + std::to_string(err), CURRENT_MODULE);
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    // 3. ��ѯ����ǰ״̬
    SERVICE_STATUS serviceStatus = { 0 };
    if (!QueryServiceStatus(hService, &serviceStatus))
    {
        DWORD err = GetLastError();
        kLog.err(C("��ѯ��������״̬ʧ�ܣ�������: ") + std::to_string(err), CURRENT_MODULE);
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    // 4. ��������δֹͣ/δ����ֹͣ��ʱ������ָֹͣ��
    if (serviceStatus.dwCurrentState != SERVICE_STOPPED &&
        serviceStatus.dwCurrentState != SERVICE_STOP_PENDING)
    {
        if (!ControlService(
            hService,
            SERVICE_CONTROL_STOP,         // ֹͣ���������
            &serviceStatus                // ��������״̬
        ))
        {
            DWORD err = GetLastError();
            kLog.err(C("��������ָֹͣ��ʧ�ܣ�������: ") + std::to_string(err), CURRENT_MODULE);
            CloseServiceHandle(hService);
            CloseServiceHandle(hSCM);
            return FALSE;
        }

        // 5. �ȴ�����ֹͣ����ʱ4�룺80��*50ms��
        int timeOut = 0;
        while (serviceStatus.dwCurrentState != SERVICE_STOPPED)
        {
            timeOut++;
            Sleep(50);  // ÿ50ms��ѯһ��״̬
            if (!QueryServiceStatus(hService, &serviceStatus))
            {
                DWORD err = GetLastError();
                kLog.err(C("�ȴ�ֹͣʱ��ѯ����״̬ʧ�ܣ�������: ") + std::to_string(err), CURRENT_MODULE);
                CloseServiceHandle(hService);
                CloseServiceHandle(hSCM);
                return FALSE;
            }
            if (timeOut > 80)  // ��ʱ�ж�
            {
                kLog.err(C("����ֹͣ��ʱ��4�룩"), CURRENT_MODULE);
                CloseServiceHandle(hService);
                CloseServiceHandle(hSCM);
                return FALSE;
            }
        }
    }

    kLog.info(C("��������ֹͣ�ɹ�"), CURRENT_MODULE);
    // �ͷž��
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return TRUE;
}

// ж�����������ڹ̶�������KswordDriverService������ֹͣ����
BOOL UnloadDriver()
{
    // 1. ��ֹͣ����ж��ǰ����ȷ��������ֹͣ��
    if (!StopDriver())
    {
        kLog.err(C("����δֹͣ���޷�ִ��ж��"), CURRENT_MODULE);
        return FALSE;
    }

    // 2. �򿪷�����ƹ�����
    SC_HANDLE hSCM = OpenSCManager(
        NULL,
        NULL,
        SC_MANAGER_ALL_ACCESS
    );
    if (hSCM == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("�򿪷�����ƹ�����ʧ�ܣ�������: ") + std::to_string(err), CURRENT_MODULE);
        return FALSE;
    }

    // 3. ��ָ����������
    SC_HANDLE hService = OpenService(
        hSCM,
        L"KswordDriverService",         // �̶�������
        SERVICE_ALL_ACCESS
    );
    if (hService == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("����������ʧ�ܣ�������: ") + std::to_string(err), CURRENT_MODULE);
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    // 4. ɾ������ж�أ�
    if (!DeleteService(hService))
    {
        DWORD err = GetLastError();
        kLog.err(C("ж����������ʧ�ܣ�������: ") + std::to_string(err), CURRENT_MODULE);
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    kLog.info(C("��������ж�سɹ�"), CURRENT_MODULE);
    // �ͷž��
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return TRUE;
}

// ������������ȡ�ͷź��KswordDriver.sys����·��������֮ǰ��·���߼���
std::wstring GetDriverAbsolutePath()
{
    TCHAR szExePath[MAX_PATH] = { 0 };
    // ��ȡ��ǰ����·��
    if (GetModuleFileName(NULL, szExePath, MAX_PATH) == 0)
    {
        DWORD err = GetLastError();
        kLog.err(C("��ȡ����·��ʧ�ܣ�������: ") + std::to_string(err), CURRENT_MODULE);
        return L"";
    }

    // ��ȡ��������Ŀ¼���ض��ļ�����
    TCHAR* pLastSlash = _tcsrchr(szExePath, _T('\\'));
    if (pLastSlash == NULL)
    {
        kLog.err(C("��������·��ʧ��"), CURRENT_MODULE);
        return L"";
    }
    *pLastSlash = _T('\0');

    // ƴ�������ļ�����·��
    return std::wstring(szExePath) + L"\\KswordDriver.sys";
}
#undef CURRENT_MODULE