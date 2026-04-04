#include "../Main/KswordTotalHead.h"

#include <wincrypt.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>

#pragma comment(lib, "crypt32.lib")
bool isR0=false;
#define CURRENT_MODULE "驱动程序"
// 函数声明
bool AddPFXToTrustedStore(const std::wstring& pfxFileName, const std::wstring& password);
std::vector<BYTE> ReadCertificateFile(const std::wstring& fileName);
BOOL InstallDriver(const std::wstring& drvAbsPath);   // 加载（安装）驱动
BOOL StartDriver();                                   // 启动驱动
BOOL StopDriver();                                    // 停止驱动
BOOL UnloadDriver();                                  // 卸载驱动
static std::wstring GetDriverAbsolutePath();                 // 辅助：获取驱动文件绝对路径

int  ReleaseDriverToFile()
{
    SetDllDirectoryW(L".");
    HRSRC D3DX9DLLResource = FindResource(NULL, MAKEINTRESOURCE(IDR_SYS1), _T("SYS"));
    if (D3DX9DLLResource == NULL)
    {
        DWORD err = GetLastError();
        return 0;
    }

    // 加载资源
    HGLOBAL hResourceD3DX9 = LoadResource(NULL, D3DX9DLLResource);
    if (hResourceD3DX9 == NULL)
        return 0;

    // 锁定资源获取数据指针
    LPVOID pResourceData = LockResource(hResourceD3DX9);
    if (pResourceData == NULL)
        return 0;

    // 获取资源大小
    DWORD dwResourceSize = SizeofResource(NULL, D3DX9DLLResource);
    if (dwResourceSize == 0)
        return 0;

    // 获取程序当前目录
    TCHAR szExePath[MAX_PATH];
    if (GetModuleFileName(NULL, szExePath, MAX_PATH) == 0)
        return 0;

    // 提取目录路径
    TCHAR* pLastSlash = _tcsrchr(szExePath, _T('\\'));
    if (pLastSlash == NULL)
        return 0;
    *pLastSlash = _T('\0');

    // 构建目标DLL路径
    std::basic_string<TCHAR> strOutputPath = std::basic_string<TCHAR>(szExePath) + _T("\\KswordDriver.sys");

    // 写入DLL文件（二进制模式）
    std::ofstream KswordD3DX9dllRelease(strOutputPath.c_str(), std::ios::binary);
    if (!KswordD3DX9dllRelease.is_open())
        return 0;

    KswordD3DX9dllRelease.write(static_cast<const char*>(pResourceData), dwResourceSize);
    if (!KswordD3DX9dllRelease.good())
    {
        KswordD3DX9dllRelease.close();
        DeleteFile(strOutputPath.c_str()); // 写入失败时删除不完整文件
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

    // 加载资源
    HGLOBAL hResourceD3DX9 = LoadResource(NULL, D3DX9DLLResource);
    if (hResourceD3DX9 == NULL)
        return 0;

    // 锁定资源获取数据指针
    LPVOID pResourceData = LockResource(hResourceD3DX9);
    if (pResourceData == NULL)
        return 0;

    // 获取资源大小
    DWORD dwResourceSize = SizeofResource(NULL, D3DX9DLLResource);
    if (dwResourceSize == 0)
        return 0;

    // 获取程序当前目录
    TCHAR szExePath[MAX_PATH];
    if (GetModuleFileName(NULL, szExePath, MAX_PATH) == 0)
        return 0;

    // 提取目录路径
    TCHAR* pLastSlash = _tcsrchr(szExePath, _T('\\'));
    if (pLastSlash == NULL)
        return 0;
    *pLastSlash = _T('\0');

    // 构建目标DLL路径
    std::basic_string<TCHAR> strOutputPath = std::basic_string<TCHAR>(szExePath) + _T("\\KswordDriver.pfx");

    // 写入DLL文件（二进制模式）
    std::ofstream KswordD3DX9dllRelease(strOutputPath.c_str(), std::ios::binary);
    if (!KswordD3DX9dllRelease.is_open())
        return 0;

    KswordD3DX9dllRelease.write(static_cast<const char*>(pResourceData), dwResourceSize);
    if (!KswordD3DX9dllRelease.good())
    {
        KswordD3DX9dllRelease.close();
        DeleteFile(strOutputPath.c_str()); // 写入失败时删除不完整文件
        return 0;
    }

    KswordD3DX9dllRelease.close();
    return 1;

}




// 将原入口函数修改为普通函数，接收证书文件名参数
bool AddTrustedCertificate(const wchar_t* certFileName)
{
    if (certFileName == nullptr)
    {
        kLog.err(C("证书文件名不能为空"), C("证书操作"));
        return false;
    }

    // 初始化COM
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr))
    {
        kLog.err(C("COM初始化失败，错误代码: " + std::to_string(hr)), C("证书操作"));
        return false;
    }

    // 添加证书到受信任存储
    bool success = AddPFXToTrustedStore(L"KswordDriver.pfx", L"KswordDever");

    // 释放COM
    CoUninitialize();

    if (success)
    {
        kLog.info(C("证书已成功添加到受信任的根证书颁发机构存储。"), C("证书操作"));
    }
    else
    {
        kLog.err(C("添加证书失败。"), C("证书操作"));
    }

    return success;
}

// 读取证书文件内容
std::vector<BYTE> ReadCertificateFile(const std::wstring& fileName)
{
    std::vector<BYTE> certData;

    // 打开文件
    std::ifstream file(fileName, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        std::wcout << L"无法打开证书文件: " << fileName << std::endl;
        return certData;
    }

    // 获取文件大小
    std::streamsize size = file.tellg();
    if (size <= 0)
    {
        std::wcout << L"证书文件为空或无法获取大小。" << std::endl;
        return certData;
    }

    // 分配缓冲区并读取文件
    file.seekg(0, std::ios::beg);
    certData.resize(static_cast<size_t>(size));

    if (!file.read(reinterpret_cast<char*>(certData.data()), size))
    {
        std::wcout << L"读取证书文件失败。" << std::endl;
        certData.clear();
    }

    return certData;
}

// 将证书添加到受信任的根证书颁发机构存储
bool AddPFXToTrustedStore(const std::wstring& pfxFileName, const std::wstring& password = L"") {
    // 读取PFX文件数据
    std::vector<BYTE> pfxData = ReadCertificateFile(pfxFileName);
    if (pfxData.empty()) {
        kLog.err(C("PFX文件读取失败"), C("证书操作"));
        return false;
    }

    // 导入PFX存储（需处理密码）
    CRYPT_DATA_BLOB pfxBlob;
    pfxBlob.cbData = pfxData.size();
    pfxBlob.pbData = pfxData.data();

    HCERTSTORE hPfxStore = PFXImportCertStore(
        &pfxBlob,
        password.c_str(),  // 若PFX有密码，需传入正确密码
        CRYPT_MACHINE_KEYSET | CRYPT_EXPORTABLE  // 本地计算机级别导入
    );
    if (hPfxStore == NULL) {
        DWORD err = GetLastError();
        kLog.err(C("PFX导入失败，错误代码: " + std::to_string(err)), C("证书操作"));
        return false;
    }

    // 打开本地计算机的ROOT存储
    HCERTSTORE hRootStore = CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W,
        0,
        NULL,
        CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG,
        L"ROOT"
    );
    if (hRootStore == NULL) {
        DWORD err = GetLastError();
        kLog.err(C("打开ROOT存储失败，错误代码: " + std::to_string(err)), C("证书操作"));
        CertCloseStore(hPfxStore, 0);
        return false;
    }

    // 枚举PFX中的证书并添加到ROOT存储
    PCCERT_CONTEXT pCertContext = NULL;
    bool success = true;
    while ((pCertContext = CertEnumCertificatesInStore(hPfxStore, pCertContext)) != NULL) {
        if (!CertAddCertificateContextToStore(
            hRootStore,
            pCertContext,
            CERT_STORE_ADD_REPLACE_EXISTING,  // 替换已存在的证书
            NULL
        )) {
            DWORD err = GetLastError();
            kLog.err(C("添加证书到ROOT存储失败，错误代码: " + std::to_string(err)), C("证书操作"));
            success = false;
            break;
        }
    }

    // 清理资源
    CertFreeCertificateContext(pCertContext);
    CertCloseStore(hPfxStore, 0);
    CertCloseStore(hRootStore, 0);

    return success;
}
int KswordDriverInit() {
    if (!ReleasePFXToFile()) {
        kLog.fatal("无法将KswordDriver.pfx资源释放到文件", CURRENT_MODULE);
    }
    if (!ReleaseDriverToFile()) {
        kLog.fatal("无法将KswordDriver.sys资源释放到文件", CURRENT_MODULE);
    }
    // 3. 构建PFX证书路径并添加到受信任存储
    TCHAR szExePath[MAX_PATH];
    if (GetModuleFileName(NULL, szExePath, MAX_PATH) == 0) {
        kLog.fatal(C("获取程序路径失败，无法添加证书"), CURRENT_MODULE);
        return 0;
    }
    TCHAR* pLastSlash = _tcsrchr(szExePath, _T('\\'));
    if (pLastSlash == NULL) {
        kLog.fatal(C("解析程序路径失败，无法添加证书"), CURRENT_MODULE);
        return 0;
    }
    *pLastSlash = _T('\0');
    std::wstring pfxFilePath = std::wstring(szExePath) + L"\\KswordDriver.pfx";

    // 添加证书到受信任存储
    bool certSuccess = AddTrustedCertificate(pfxFilePath.c_str());
    if (!certSuccess) {
        kLog.fatal(C("证书添加失败，驱动初始化中断"), CURRENT_MODULE);
        // 证书添加失败时，清理已释放的文件
        std::basic_string<TCHAR> driverPath = std::basic_string<TCHAR>(szExePath) + _T("\\KswordDriver.sys");
        DeleteFile(driverPath.c_str());
        DeleteFile(pfxFilePath.c_str());
        return 0;
    }
    kLog.info(C("证书添加成功，驱动初始化完成"), CURRENT_MODULE);
    if (!InstallDriver(GetDriverAbsolutePath()))return 0;
    if (!StartDriver())return 0;
    return 1; // 初始化成功返回1

}
BOOL InstallDriver(const std::wstring& drvAbsPath)
{
    // 1. 打开服务控制管理器（SCM）
    SC_HANDLE hSCM = OpenSCManager(
        NULL,                           // 本地计算机
        NULL,                           // 打开默认数据库（SERVICES_ACTIVE_DATABASE）
        SC_MANAGER_ALL_ACCESS           // 所有操作权限
    );
    if (hSCM == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("打开服务控制管理器失败，错误码: ") + std::to_string(err), CURRENT_MODULE);
        return FALSE;
    }

    // 2. 创建驱动服务（添加到SCM）
    SC_HANDLE hService = CreateService(
        hSCM,                           // SCM句柄
        L"KswordDriverService",         // 服务名（固定）
        L"KswordDriverService",         // 服务显示名
        SERVICE_ALL_ACCESS,             // 服务操作权限
        SERVICE_KERNEL_DRIVER,          // 服务类型：内核驱动
        SERVICE_DEMAND_START,           // 启动方式：手动触发
        SERVICE_ERROR_IGNORE,           // 错误处理：忽略错误
        drvAbsPath.c_str(),             // 驱动文件绝对路径
        NULL,                           // 无负载组
        NULL,                           // 无组标记
        NULL,                           // 无依赖服务
        NULL,                           // 运行账户：LocalSystem
        NULL                            // 账户密码：LocalSystem无需密码
    );

    // 3. 错误处理与资源释放
    if (hService == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("创建驱动服务失败，错误码: ") + std::to_string(err), CURRENT_MODULE);
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    kLog.info(C("驱动服务安装成功"), CURRENT_MODULE);
    // 释放句柄（先服务后SCM）
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return TRUE;
}

// 启动驱动：基于固定服务名KswordDriverService
BOOL StartDriver()
{
    // 1. 打开服务控制管理器
    SC_HANDLE hSCM = OpenSCManager(
        NULL,
        NULL,
        SC_MANAGER_ALL_ACCESS
    );
    if (hSCM == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("打开服务控制管理器失败，错误码: ") + std::to_string(err), CURRENT_MODULE);
        return FALSE;
    }

    // 2. 打开指定驱动服务
    SC_HANDLE hService = OpenService(
        hSCM,
        L"KswordDriverService",         // 固定服务名
        SERVICE_ALL_ACCESS
    );
    if (hService == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("打开驱动服务失败，错误码: ") + std::to_string(err), CURRENT_MODULE);
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    // 3. 启动服务
    if (!StartService(
        hService,
        0,                              // 无服务参数
        NULL
    ))
    {
        DWORD err = GetLastError();
        kLog.err(C("启动驱动服务失败，错误码: ") + std::to_string(err), CURRENT_MODULE);
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    kLog.info(C("驱动服务启动成功"), CURRENT_MODULE);
    // 释放句柄
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return TRUE;
}

// 停止驱动：基于固定服务名KswordDriverService，含超时检测
BOOL StopDriver()
{
    // 1. 打开服务控制管理器
    SC_HANDLE hSCM = OpenSCManager(
        NULL,
        NULL,
        SC_MANAGER_ALL_ACCESS
    );
    if (hSCM == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("打开服务控制管理器失败，错误码: ") + std::to_string(err), CURRENT_MODULE);
        return FALSE;
    }

    // 2. 打开指定驱动服务
    SC_HANDLE hService = OpenService(
        hSCM,
        L"KswordDriverService",         // 固定服务名
        SERVICE_ALL_ACCESS
    );
    if (hService == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("打开驱动服务失败，错误码: ") + std::to_string(err), CURRENT_MODULE);
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    // 3. 查询服务当前状态
    SERVICE_STATUS serviceStatus = { 0 };
    if (!QueryServiceStatus(hService, &serviceStatus))
    {
        DWORD err = GetLastError();
        kLog.err(C("查询驱动服务状态失败，错误码: ") + std::to_string(err), CURRENT_MODULE);
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    // 4. 仅当服务未停止/未处于停止中时，发送停止指令
    if (serviceStatus.dwCurrentState != SERVICE_STOPPED &&
        serviceStatus.dwCurrentState != SERVICE_STOP_PENDING)
    {
        if (!ControlService(
            hService,
            SERVICE_CONTROL_STOP,         // 停止服务控制码
            &serviceStatus                // 接收最新状态
        ))
        {
            DWORD err = GetLastError();
            kLog.err(C("发送驱动停止指令失败，错误码: ") + std::to_string(err), CURRENT_MODULE);
            CloseServiceHandle(hService);
            CloseServiceHandle(hSCM);
            return FALSE;
        }

        // 5. 等待服务停止（超时4秒：80次*50ms）
        int timeOut = 0;
        while (serviceStatus.dwCurrentState != SERVICE_STOPPED)
        {
            timeOut++;
            Sleep(50);  // 每50ms查询一次状态
            if (!QueryServiceStatus(hService, &serviceStatus))
            {
                DWORD err = GetLastError();
                kLog.err(C("等待停止时查询服务状态失败，错误码: ") + std::to_string(err), CURRENT_MODULE);
                CloseServiceHandle(hService);
                CloseServiceHandle(hSCM);
                return FALSE;
            }
            if (timeOut > 80)  // 超时判断
            {
                kLog.err(C("驱动停止超时（4秒）"), CURRENT_MODULE);
                CloseServiceHandle(hService);
                CloseServiceHandle(hSCM);
                return FALSE;
            }
        }
    }

    kLog.info(C("驱动服务停止成功"), CURRENT_MODULE);
    // 释放句柄
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return TRUE;
}

// 卸载驱动：基于固定服务名KswordDriverService（需先停止服务）
BOOL UnloadDriver()
{
    // 1. 先停止服务（卸载前必须确保服务已停止）
    if (!StopDriver())
    {
        kLog.err(C("驱动未停止，无法执行卸载"), CURRENT_MODULE);
        return FALSE;
    }

    // 2. 打开服务控制管理器
    SC_HANDLE hSCM = OpenSCManager(
        NULL,
        NULL,
        SC_MANAGER_ALL_ACCESS
    );
    if (hSCM == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("打开服务控制管理器失败，错误码: ") + std::to_string(err), CURRENT_MODULE);
        return FALSE;
    }

    // 3. 打开指定驱动服务
    SC_HANDLE hService = OpenService(
        hSCM,
        L"KswordDriverService",         // 固定服务名
        SERVICE_ALL_ACCESS
    );
    if (hService == NULL)
    {
        DWORD err = GetLastError();
        kLog.err(C("打开驱动服务失败，错误码: ") + std::to_string(err), CURRENT_MODULE);
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    // 4. 删除服务（卸载）
    if (!DeleteService(hService))
    {
        DWORD err = GetLastError();
        kLog.err(C("卸载驱动服务失败，错误码: ") + std::to_string(err), CURRENT_MODULE);
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    kLog.info(C("驱动服务卸载成功"), CURRENT_MODULE);
    // 释放句柄
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return TRUE;
}

// 辅助函数：获取释放后的KswordDriver.sys绝对路径（复用之前的路径逻辑）
std::wstring GetDriverAbsolutePath()
{
    TCHAR szExePath[MAX_PATH] = { 0 };
    // 获取当前程序路径
    if (GetModuleFileName(NULL, szExePath, MAX_PATH) == 0)
    {
        DWORD err = GetLastError();
        kLog.err(C("获取程序路径失败，错误码: ") + std::to_string(err), CURRENT_MODULE);
        return L"";
    }

    // 提取程序所在目录（截断文件名）
    TCHAR* pLastSlash = _tcsrchr(szExePath, _T('\\'));
    if (pLastSlash == NULL)
    {
        kLog.err(C("解析程序路径失败"), CURRENT_MODULE);
        return L"";
    }
    *pLastSlash = _T('\0');

    // 拼接驱动文件绝对路径
    return std::wstring(szExePath) + L"\\KswordDriver.sys";
}
#undef CURRENT_MODULE