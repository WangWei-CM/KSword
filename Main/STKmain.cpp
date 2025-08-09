//#include "KswordTotalHead.h"
//#include <stdio.h>
//#include <Windows.h>
//#include <iostream>
//#include <fstream>
//#include <sstream>
//#include <vector>
//#include <string>
//#include <tlhelp32.h>
//#include <comdef.h>
//#include <Wbemidl.h>
//#include <winternl.h>
//#include <filesystem>
//#include <atlconv.h>
//#include <fwpmu.h>
//#include "Driver.h"
////#include "resource.h"
//#include <capstone/capstone.h>
//
//
//ULONG DebugMode = FALSE;
//
//ULONG KDebugMode = FALSE;
//
//ULONG ISUnload = NULL;
//
//
////#pragma comment(lib, "wbemuuid.lib")
////#define GL_SILENCE_DEPRECATION
////#if defined(IMGUI_IMPL_OPENGL_ES2)
////#include <GLES2/gl2.h>
////#endif
////#include <GLFW/glfw3.h> 
////#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
////#pragma comment(lib, "legacy_stdio_definitions")
////#endif
////#ifdef __EMSCRIPTEN__
////#include "../libs/emscripten/emscripten_mainloop_stub.h"
////#endif
//
//static void glfw_error_callback(int error, const char* description)
//{
//    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
//}
//
//namespace fs = std::filesystem;
//
//
//static bool EnableShowhideproc = false;
//static bool System_Monitor_ShowFileRW = false;
//
//static bool sandboxinited = false;
//
//ULONG disamSizes = NULL;
//
//HANDLE hThread = NULL;
//
//HANDLE hDevice = NULL;
//
//ULONG NoExit = NULL;
//ULONG NoEnableHvm = NULL;
//ULONG HvmEnabled = NULL;
//ULONG EnableAAV = FALSE;
//
//
//ULONG NoEnableMonitor = NULL;
//ULONG MonitorEnabled = NULL;
//ULONG isprocinfowindowcloseed = FALSE;
//
//ULONG isprohibitcreatefileenableed = FALSE;
//
//ULONG isShadowSSDTRefeshed = FALSE;
//ULONG isHalRefeshed = FALSE;
//
//
//
//HANDLE hProcessEvent = NULL;
//
//int CheckProcessExists(const char* processName) {
//    PROCESSENTRY32 pe32;
//    pe32.dwSize = sizeof(PROCESSENTRY32);
//
//    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
//    if (hSnapshot == INVALID_HANDLE_VALUE) {
//        return 2;
//    }
//
//    BOOL bMore = Process32First(hSnapshot, &pe32);
//    while (bMore) {
//        if (strcmp(pe32.szExeFile, processName) == 0) {
//            CloseHandle(hSnapshot);
//            return 1;
//        }
//        bMore = Process32Next(hSnapshot, &pe32);
//    }
//
//    CloseHandle(hSnapshot);
//    return 2;
//}
//
//struct Process_ListItem {
//    std::string name;
//    int id;
//    PVOID eprocess;
//    std::string Path;
//    std::string Status;
//    bool marked_for_deletion = false;
//    bool selected = false;
//};
//
//static std::vector<BYTE> g_mem_data;
//
//
//static std::vector<Process_ListItem> Process_items;
//static int Process_context_menu_id = -1; 
//
//static std::vector<std::string> Monitor_listItems;
//static int Monitor_selectedIndex = -1;
//
//
//void Process_AddListItem(const char* name, int id, PVOID eprocess, const char* Path, const char* Status) {
//    Process_items.push_back({ name, id, eprocess, Path, Status });
//}
//
//struct Driver_ListItem {
//    std::string name;
//    std::string path;
//    PVOID Base;
//    PVOID DriverObject;
//    ULONG Size;
//    bool selected = false;
//};
//
//struct Hook_MSR_ListItem {
//    std::string name;
//    std::string Module;
//    std::string Hook;
//    PVOID CurrAddress;
//    PVOID OriginalAddress;
//    bool selected = false;
//};
//
//struct File_ListItem {
//    std::string name;
//    std::string Type;
//    std::string InFolder;
//    ULONG isFolder;
//    bool selected = false;
//};
//
//struct MiniFilter_ListItem 
//{
//    std::string Module;
//    std::string IRP;
//    PVOID Filter;
//    PVOID PostFunc;
//    PVOID PreFunc;
//    bool selected = false;
//};
//
//struct ExCallback_ListItem
//{
//    std::string Module;
//    std::string Name;
//    PVOID Entry;
//    PVOID Object;
//    PVOID Handle;
//    bool selected = false;
//};
//
//struct SystemThread_ListItem
//{
//    std::string Module;
//    PVOID ethread;
//    PVOID Address;
//    ULONG TID;
//    bool selected = false;
//};
//
//struct IrpHook_ListItem
//{
//    std::string Module;
//    std::string Driver;
//    std::string IrpFunc;
//    PVOID Address;
//    PVOID OriginalAddress;
//    std::string Hook;
//    ULONG irpfunccode;
//    PVOID DriverObject;
//    bool selected = false;
//};
//
//struct Notify_ListItem
//{
//    std::string Module;
//    std::string Type;
//    PVOID Address;
//    PVOID Handle;
//    ULONG NotifyTypes;
//    bool selected = false;
//};
//
//struct ObjectTypeTable_ListItem
//{
//    std::string Module;
//    std::string Type;
//    PVOID Address;
//    bool selected = false;
//};
//
//struct IoTimer_ListItem
//{
//    std::string Module;
//    PVOID Address;
//    PVOID Object;
//    bool selected = false;
//};
//
//struct IDT_ListItem
//{
//    std::string Module;
//    ULONG ID;
//    ULONG Cpu;
//    PVOID Address;
//    bool selected = false;
//};
//
//struct SSDT_ListItem
//{
//    std::string Module;
//    std::string Name;
//    std::string Hook;
//    PVOID Address;
//    PVOID OriginalAddress;
//    ULONG Index;
//    ULONG isInlineHook;
//    bool selected = false;
//};
//
//
//struct Memory_ListItem
//{
//    PVOID Address;
//    BYTE data;
//    bool selected = false;
//};
//
//struct WFPFunction_ListItem
//{
//    std::string Name;
//    UINT id;
//    GUID guid;
//    bool selected = false;
//};
//
//struct WFPCallout_ListItem
//{
//    std::string Module;
//    PVOID Entry;
//    PVOID ClassFunc;
//    PVOID Notify_Func;
//    PVOID deletefunc;
//    ULONG Id;
//    bool selected = false;
//};
//
//struct ProcessThread_ListItem
//{
//    std::string Status;
//    ULONG Prioriry;
//    PVOID ethread;
//    PVOID Address;
//    ULONG TID;
//    bool selected = false;
//};
//
//struct GDT_ListItem
//{
//    std::string Name;
//    ULONG ID;
//    PVOID Base;
//    PVOID limit;
//    ULONG dpi;
//    ULONG flags;
//    bool selected = false;
//};
//
//struct unloadeddriver_ListItem
//{
//    std::string Name;
//    ULONG ID;
//    PVOID Base;
//    bool selected = false;
//};
//
//struct DriverMajorInfo_ListItem
//{
//    std::string Name;
//    std::string Module;
//    PVOID Address;
//    bool selected = false;
//};
//
//struct Plugins_ListItem
//{
//    std::string Name;
//    std::string Entry;
//    std::string author;
//    std::string version;
//    bool selected = false;
//};
//
//struct ShadowSSDT_ListItem
//{
//    std::string Name;
//    std::string Module;
//    ULONG Index;
//    PVOID Address;
//    std::string Hook;
//    PVOID OriginalAddress;
//    ULONG IsInlineHook;
//    bool selected = false;
//};
//
//struct Disassembly_ListItem
//{
//    std::string DATA;
//    std::string mnemonic;
//    PVOID Address;
//    bool selected = false;
//};
//
//struct HalDispathTable_ListItem
//{
//    std::string Name;
//    std::string Module;
//    PVOID Address;
//    bool selected = false;
//};
//
//struct PiDDB_ListItem
//{
//    std::string Name;
//    ULONG status;
//    ULONG Time;
//    bool selected = false;
//};
//
//static std::vector<Driver_ListItem> Driver_items;
//static int Driver_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<Process_ListItem>* ARK_GetProcessList() {
//    return &Process_items;
//}
//
//static std::vector<Hook_MSR_ListItem> Hook_MSR_items;
//static int Hook_MSR_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<Hook_MSR_ListItem>* ARK_GetMSRHookList() {
//    return &Hook_MSR_items;
//}
//
//static std::vector<File_ListItem> File_items;
//static int File_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<File_ListItem>* ARK_GetFileList() {
//    return &File_items;
//}
//
//static std::vector<MiniFilter_ListItem> MiniFilter_items;
//static int MiniFilter_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<MiniFilter_ListItem>* ARK_GetMiniFilterList() {
//    return &MiniFilter_items;
//}
//
//static std::vector<ExCallback_ListItem> ExCallback_items;
//static int ExCallback_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<ExCallback_ListItem>* ARK_GetExCallbackList() {
//    return &ExCallback_items;
//}
//
//static std::vector<SystemThread_ListItem> SystemThread_items;
//static int SystemThread_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<SystemThread_ListItem>* ARK_GetSystemThreadList() {
//    return &SystemThread_items;
//}
//
//static std::vector<IrpHook_ListItem> IrpHook_items;
//static int IrpHook_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<IrpHook_ListItem>* ARK_GetIrpHookList() {
//    return &IrpHook_items;
//}
//
//static std::vector<Notify_ListItem> Notify_items;
//static int Notify_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<Notify_ListItem>* ARK_GetNotifyList() {
//    return &Notify_items;
//}
//
//static std::vector<ObjectTypeTable_ListItem> ObjectTypeTable_items;
//static int ObjectTypeTable_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<ObjectTypeTable_ListItem>* ARK_GetObjectTypeTableList() {
//    return &ObjectTypeTable_items;
//}
//
//static std::vector<IoTimer_ListItem> IoTimer_items;
//static int IoTimer_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<IoTimer_ListItem>* ARK_GetIoTimerList() {
//    return &IoTimer_items;
//}
//
//static std::vector<IDT_ListItem> IDT_items;
//static int IDT_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<IDT_ListItem>* ARK_GetIDTList() {
//    return &IDT_items;
//}
//
//static std::vector<SSDT_ListItem> SSDT_items;
//static int SSDT_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<SSDT_ListItem>* ARK_GetSSDTList() {
//    return &SSDT_items;
//}
//
//static std::vector<Memory_ListItem> Memory_items;
//static int Memory_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<Memory_ListItem>* ARK_GetMemoryList() {
//    return &Memory_items;
//}
//
//static std::vector<WFPFunction_ListItem> WFPFunction_items;
//static int WFPFunction_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<WFPFunction_ListItem>* ARK_GetWFPFunctionList() {
//    return &WFPFunction_items;
//}
//
//static std::vector<WFPCallout_ListItem> WFPCallout_items;
//static int WFPCallout_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<WFPCallout_ListItem>* ARK_GetWFPCalloutList() {
//    return &WFPCallout_items;
//}
//
//static std::vector<ProcessThread_ListItem> ProcessThread_items;
//static int PrcessThread_context_menu_id = -1;
//
//static std::vector<GDT_ListItem> GDT_items;
//static int GDT_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<GDT_ListItem>* ARK_GetGDTList() {
//    return &GDT_items;
//}
//
//static std::vector<unloadeddriver_ListItem> unloadeddriver_items;
//static int unloadeddriver_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<unloadeddriver_ListItem>* ARK_GetUnloadedDriverList() {
//    return &unloadeddriver_items;
//}
//
//static std::vector<DriverMajorInfo_ListItem> DriverMajorInfo_items;
//static int DriverMajorInfo_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<DriverMajorInfo_ListItem>* ARK_GetDriverMajorInfoList() {
//    return &DriverMajorInfo_items;
//}
//
//static std::vector<Plugins_ListItem> Plugins_items;
//static int Plugins_context_menu_id = -1;
//
//static std::vector<ShadowSSDT_ListItem> ShadowSSDT_items;
//static int ShadowSSDT_context_menu_id = -1;
//
//extern "C" __declspec(dllexport) std::vector<ShadowSSDT_ListItem>* ARK_GetShadowSSDTList() {
//    return &ShadowSSDT_items;
//}
//
//static std::vector<Disassembly_ListItem> Disassembly_items;
//static int Disassembly_context_menu_id = -1;
//
//static std::vector<HalDispathTable_ListItem> HalDispathTable_items;
//static int HalDispathTable_context_menu_id = -1;
//
//static std::vector<HalDispathTable_ListItem> HalPrivateDispathTable_items;
//static int HalPrivateDispathTable_context_menu_id = -1;
//
//static std::vector<PiDDB_ListItem> PiDDB_items;
//static int PiDDB_context_menu_id = -1;
//
//void Driver_AddListItem(const char* name, const char* path, PVOID Base, PVOID DriverObject, ULONG Size) {
//    Driver_items.push_back({ name, path, Base, DriverObject, Size });
//}
//
//void Hook_MSR_AddListItem(const char* name, const char* Module, const char* Hook, PVOID CurrentAddress, PVOID OriginalAddress) {
//    Hook_MSR_items.push_back({ name, Module, Hook, CurrentAddress,OriginalAddress });
//}
//
//void File_AddListItem(const char* name, const char* Type, const char* InFolder, ULONG IsFolder) {
//    File_items.push_back({ name, Type, InFolder, IsFolder });
//}
//
//void MiniFilter_AddListItem(const char* Module, const char* IRP, PVOID Filter, PVOID PostFunc, PVOID PreFunc) {
//    MiniFilter_items.push_back({ Module, IRP, Filter, PostFunc, PreFunc });
//}
//
//void ExCallback_AddListItem(const char* Module, const char* Name, PVOID Entry, PVOID Object, PVOID Handle)
//{
//    ExCallback_items.push_back({ Module, Name, Entry, Object, Handle });
//}
//
//void SystemThread_AddListItem(const char* Module, PVOID ethread, PVOID address, ULONG TID)
//{
//    SystemThread_items.push_back({ Module, ethread, address, TID });
//}
//
//void IrpHook_AddListItem(const char* Module, const char* Name, const char* IrpFunc, PVOID Address, PVOID OriginalAddress, const char* Hook, ULONG irpfunccode, PVOID DriverObject)
//{
//    IrpHook_items.push_back({ Module, Name, IrpFunc, Address, OriginalAddress, Hook, irpfunccode, DriverObject });
//}
//
//void Notify_AddListItem(const char* Module, const char* Type, PVOID Address, PVOID Handle, ULONG NotifyTypes)
//{
//    Notify_items.push_back({ Module, Type, Address, Handle, NotifyTypes });
//}
//
//void ObjectTypeTable_AddListItem(const char* Module, const char* Type, PVOID Address)
//{
//    ObjectTypeTable_items.push_back({ Module, Type, Address });
//}
//
//void IoTimer_AddListItem(const char* Module, PVOID Address, PVOID Object)
//{
//    IoTimer_items.push_back({ Module, Address, Object });
//}
//
//void IDT_AddListItem(const char* Module, ULONG ID, ULONG Cpu, PVOID Address)
//{
//    IDT_items.push_back({ Module, ID, Cpu, Address });
//}
//
//void SSDT_AddListItem(const char* Module, const char* Name, PVOID Address, PVOID OriginalAddress, const char* Hook, ULONG Index, ULONG IsInlineHook)
//{
//    SSDT_items.push_back({ Module, Name, Hook, Address, OriginalAddress, Index, IsInlineHook });
//}
//
//void Memory_AddListItem(PVOID Address, BYTE data)
//{
//    Memory_items.push_back({ Address, data });
//}
//
//void WFPFunction_AddListItem(const char* Name, UINT id, GUID guid)
//{
//    WFPFunction_items.push_back({ Name, id, guid });
//}
//
//void WFPCallout_AddListItem(const char* Module, PVOID entry, PVOID classfunc, PVOID notifyfunc, PVOID deletefunc, ULONG id)
//{
//    WFPCallout_items.push_back({ Module, entry, classfunc, notifyfunc, deletefunc, id });
//}
//
//void ProcessThread_AddListItem(const char* Status, ULONG Prioriry, PVOID ethread, PVOID address, ULONG TID)
//{
//    ProcessThread_items.push_back({ Status,Prioriry, ethread, address, TID });
//}
//
//void GDT_AddListItem(const char* name, ULONG ID, PVOID Base, PVOID limit, ULONG dpi, ULONG flags)
//{
//    GDT_items.push_back({ name, ID, Base, limit, dpi, flags });
//}
//
//void unloadeddriver_AddListItem(const char* Module, ULONG ID, PVOID Address)
//{
//    unloadeddriver_items.push_back({ Module, ID, Address });
//}
//
//void DriverMajorInfo_AddListItem(const char* Name, const char* Module, PVOID Address)
//{
//    DriverMajorInfo_items.push_back({ Name, Module, Address });
//}
//
//void Plugins_AddListItem(std::string Name, std::string Entry, std::string author, std::string version)
//{
//    Plugins_items.push_back({ Name, Entry, author,version });
//}
//
//void ShadowSSDT_AddListItem(std::string Name, std::string Module, ULONG Index, PVOID Address, std::string Hook, PVOID OriginalAddress, ULONG IsInlineHook)
//{
//    ShadowSSDT_items.push_back({ Name, Module, Index, Address, Hook, OriginalAddress, IsInlineHook });
//}
//
//void Disassembly_AddListItem(const char* data, const char* mnemonic, PVOID Address)
//{
//    Disassembly_items.push_back({ data, mnemonic, Address });
//}
//
//void HalDispathTable_AddListItem(const char* Name, const char* Module, PVOID Address)
//{
//    HalDispathTable_items.push_back({ Name, Module, Address });
//}
//
//void HalPrivateDispathTable_AddListItem(const char* Name, const char* Module, PVOID Address)
//{
//    HalPrivateDispathTable_items.push_back({ Name, Module, Address });
//}
//
//void PIDDB_AddListItem(const char* Name, ULONG Status, ULONG Time)
//{
//    PiDDB_items.push_back({ Name, Status, Time });
//}
//
//void DriverUnload(LPCSTR serviceName)
//{
//    SC_HANDLE serviceControlManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
//    if (!serviceControlManager) {
//        std::cerr << "Failed Open SCManager: " << GetLastError() << std::endl;
//
//        return;
//    }
//
//    SC_HANDLE serviceHandle = OpenService(serviceControlManager, serviceName, SERVICE_STOP | DELETE);
//    if (!serviceHandle) {
//        std::cerr << "Failed Open Service: " << GetLastError() << std::endl;
//        CloseServiceHandle(serviceControlManager);
//        return;
//    }
//
//    SERVICE_STATUS serviceStatus;
//    if (ControlService(serviceHandle, SERVICE_CONTROL_STOP, &serviceStatus)) {
//        std::cout << "Stop Serviceing..." << std::endl;
//        Sleep(1000);
//    }
//    else {
//        std::cerr << "Failed Stop Service: " << GetLastError() << std::endl;
//
//    }
//
//    if (DeleteService(serviceHandle)) {
//        std::cout << "Driver Unload Success" << std::endl;
//    }
//    else {
//        std::cerr << "Failed Delete Service: " << GetLastError() << std::endl;
//    }
//
//    CloseServiceHandle(serviceHandle);
//    CloseServiceHandle(serviceControlManager);
//}
//
//void DriverUnload_INIT(LPCSTR serviceName)
//{
//    SC_HANDLE serviceControlManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
//    if (!serviceControlManager) {
//        return;
//    }
//
//    SC_HANDLE serviceHandle = OpenService(serviceControlManager, serviceName, SERVICE_STOP | DELETE);
//    if (!serviceHandle) {
//        CloseServiceHandle(serviceControlManager);
//        return;
//    }
//    SERVICE_STATUS serviceStatus;
//    if (ControlService(serviceHandle, SERVICE_CONTROL_STOP, &serviceStatus)) 
//    {
//        Sleep(1000);
//    }
//    DeleteService(serviceHandle);
//    CloseServiceHandle(serviceHandle);
//    CloseServiceHandle(serviceControlManager);
//}
//
//bool LoadDriver_SELF(LPCWSTR driverPath, LPCWSTR serviceName) 
//{
//    if (hDevice != NULL)
//    {
//        printf("Driver Loaded\n");
//        return 0;
//    }
//    DriverUnload_INIT("SKT64-Kernel-Driver");
//    SC_HANDLE hSCManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
//    if (hSCManager == nullptr) {
//        std::wcerr << L"Failed to open service manager: " << GetLastError() << std::endl;
//        return false;
//    }
//
//    SC_HANDLE hService = CreateServiceW(
//        hSCManager,
//        serviceName,
//        serviceName,
//        SERVICE_ALL_ACCESS,
//        SERVICE_KERNEL_DRIVER,
//        SERVICE_DEMAND_START,
//        SERVICE_ERROR_NORMAL,
//        driverPath,
//        NULL,
//        NULL,
//        NULL,
//        NULL,
//        NULL
//    );
//
//    if (hService == nullptr) {
//        DWORD error = GetLastError();
//        if (error == ERROR_SERVICE_EXISTS) {
//            hService = OpenService(hSCManager, (LPCSTR)serviceName, SERVICE_START);
//            if (hService == nullptr) {
//                std::wcerr << L"[-]Failed to open existing service: " << GetLastError() << std::endl;
//                CloseServiceHandle(hSCManager);
//                return false;
//            }
//        }
//        else {
//            std::wcerr << L"[-]Failed to create service: " << error << std::endl;
//            CloseServiceHandle(hSCManager);
//            return false;
//        }
//    }
//
//    if (!StartService(hService, 0, nullptr)) {
//        std::wcerr << L"[-]Failed to start service: " << GetLastError() << std::endl;
//
//        CloseServiceHandle(hService);
//        CloseServiceHandle(hSCManager);
//        return false;
//    }
//
//    std::wcout << L"[+]Load Driver successfully." << std::endl;
//
//    CloseServiceHandle(hService);
//    CloseServiceHandle(hSCManager);
//    return true;
//}
//
//bool LoadDriver(LPCWSTR driverPath, LPCWSTR serviceName)
//{
//    SC_HANDLE hSCManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
//    if (hSCManager == nullptr) {
//        std::wcerr << L"Failed to open service manager: " << GetLastError() << std::endl;
//        return false;
//    }
//
//    SC_HANDLE hService = CreateServiceW(
//        hSCManager,
//        serviceName,
//        serviceName,
//        SERVICE_ALL_ACCESS,
//        SERVICE_KERNEL_DRIVER,
//        SERVICE_DEMAND_START,
//        SERVICE_ERROR_NORMAL,
//        driverPath,
//        NULL,
//        NULL,
//        NULL,
//        NULL,
//        NULL
//    );
//
//    if (hService == nullptr) {
//        DWORD error = GetLastError();
//        if (error == ERROR_SERVICE_EXISTS) {
//            hService = OpenService(hSCManager, (LPCSTR)serviceName, SERVICE_START);
//            if (hService == nullptr) {
//                std::wcerr << L"[-]Failed to open existing service: " << GetLastError() << std::endl;
//                CloseServiceHandle(hSCManager);
//                return false;
//            }
//        }
//        else {
//            std::wcerr << L"[-]Failed to create service: " << error << std::endl;
//            CloseServiceHandle(hSCManager);
//            return false;
//        }
//    }
//
//    if (!StartService(hService, 0, nullptr)) {
//        std::wcerr << L"[-]Failed to start service: " << GetLastError() << std::endl;
//
//        CloseServiceHandle(hService);
//        CloseServiceHandle(hSCManager);
//        return false;
//    }
//
//    std::wcout << L"[+]Load Driver successfully." << std::endl;
//
//    CloseServiceHandle(hService);
//    CloseServiceHandle(hSCManager);
//    return true;
//}
//
//BOOL ReleaseCoreDriver()
//{
//    HMODULE hModule = GetModuleHandle(NULL);
//    if (hModule == NULL)
//    {
//        std::cerr << "Failed To Create Driver File" << std::endl;
//        return FALSE;
//    }
//
//    HRSRC hRsrc = FindResource(hModule, MAKEINTRESOURCE(IDR_COREDRV1), TEXT("CoreDrv"));
//    if (hRsrc == NULL)
//    {
//        std::cerr << "Failed To Create Driver File" << std::endl;
//        return FALSE;
//    }
//
//    DWORD dwSize = SizeofResource(hModule, hRsrc);
//    if (dwSize == 0)
//    {
//        std::cerr << "Failed To Create Driver File" << std::endl;
//        return FALSE;
//    }
//
//    HGLOBAL hGlobal = LoadResource(hModule, hRsrc);
//    if (hGlobal == NULL)
//    {
//        std::cerr << "Failed To Create Driver File" << std::endl;
//        return FALSE;
//    }
//
//    LPVOID lpVoid = LockResource(hGlobal);
//    if (lpVoid == NULL)
//    {
//        std::cerr << "Failed To Create Driver File" << std::endl;
//        FreeResource(hGlobal);
//        return FALSE;
//    }
//
//
//    FILE* fp = fopen("C:\\Windows\\System32\\drivers\\ArkDrv64.sys", "wb+");
//    if (fp == NULL)
//    {
//        std::cerr << "Failed To Create Driver File" << std::endl;
//        FreeResource(hGlobal);
//        return FALSE;
//    }
//
//    fwrite(lpVoid, sizeof(char), dwSize, fp);
//    fclose(fp);
//
//    FreeResource(hGlobal);
//
//    return TRUE;
//}
//
//
//BOOL ExitArk()
//{
//    int result = MessageBoxW(NULL, L"Are you sure you want to exit", L"EXIT", MB_YESNO | MB_ICONQUESTION);
//
//    if (result == IDYES) {
//        return 0;
//    }
//    else if (result == IDNO) {
//        return 1;
//    }
//}
///*
//BOOL EnumProcess()
//{
//    typedef struct _ALL_PROCESSES_
//    {
//        ULONG_PTR nSize;
//        PVOID ProcessInfo;
//    }ALL_PROCESSES, * PALL_PROCESSES;
//
//    BOOL bRet = FALSE;
//    ALL_PROCESSES pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.ProcessInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//    ULONG Failedcount = NULL;
//    startenumprocess:
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_PROCESS, &pInput, sizeof(ALL_PROCESSES), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) 
//    {
//        Failedcount = NULL;
//        pProcessInfo = (PDATA_INFO)pInput.ProcessInfo;
//        for (ULONG i = 0; i < nRet; i++)
//        {
//            if (pProcessInfo[i].ulongdata1 == 0)
//            {
//                Process_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1);
//            }
//        }
//        for (ULONG i = 0; i < nRet; i++)
//        {
//            if (strstr(pProcessInfo[i].Module, "a") || strstr(pProcessInfo[i].Module, "b")
//                || strstr(pProcessInfo[i].Module, "c") || strstr(pProcessInfo[i].Module, "d") ||
//                strstr(pProcessInfo[i].Module, "e") || strstr(pProcessInfo[i].Module, "f") ||
//                strstr(pProcessInfo[i].Module, "g") || strstr(pProcessInfo[i].Module, "h") ||
//                strstr(pProcessInfo[i].Module, "i") || strstr(pProcessInfo[i].Module, "j") ||
//                strstr(pProcessInfo[i].Module, "k") || strstr(pProcessInfo[i].Module, "l") ||
//                strstr(pProcessInfo[i].Module, "m") || strstr(pProcessInfo[i].Module, "n") ||
//                strstr(pProcessInfo[i].Module, "o") || strstr(pProcessInfo[i].Module, "p") ||
//                strstr(pProcessInfo[i].Module, "q") || strstr(pProcessInfo[i].Module, "r") ||
//                strstr(pProcessInfo[i].Module, "s") || strstr(pProcessInfo[i].Module, "t") ||
//                strstr(pProcessInfo[i].Module, "u") || strstr(pProcessInfo[i].Module, "v") ||
//                strstr(pProcessInfo[i].Module, "w") || strstr(pProcessInfo[i].Module, "x") ||
//                strstr(pProcessInfo[i].Module, "y") || strstr(pProcessInfo[i].Module, "z") ||
//                strstr(pProcessInfo[i].Module, "A") || strstr(pProcessInfo[i].Module, "B") ||
//                strstr(pProcessInfo[i].Module, "C") || strstr(pProcessInfo[i].Module, "D") ||
//                strstr(pProcessInfo[i].Module, "E") || strstr(pProcessInfo[i].Module, "F") ||
//                strstr(pProcessInfo[i].Module, "G") || strstr(pProcessInfo[i].Module, "H") ||
//                strstr(pProcessInfo[i].Module, "I") || strstr(pProcessInfo[i].Module, "J") ||
//                strstr(pProcessInfo[i].Module, "K") || strstr(pProcessInfo[i].Module, "L") ||
//                strstr(pProcessInfo[i].Module, "M") || strstr(pProcessInfo[i].Module, "N") ||
//                strstr(pProcessInfo[i].Module, "O") || strstr(pProcessInfo[i].Module, "P") ||
//                strstr(pProcessInfo[i].Module, "Q") || strstr(pProcessInfo[i].Module, "R") ||
//                strstr(pProcessInfo[i].Module, "S") || strstr(pProcessInfo[i].Module, "T") ||
//                strstr(pProcessInfo[i].Module, "U") || strstr(pProcessInfo[i].Module, "V") ||
//                strstr(pProcessInfo[i].Module, "W") || strstr(pProcessInfo[i].Module, "X") ||
//                strstr(pProcessInfo[i].Module, "Y") || strstr(pProcessInfo[i].Module, "Z") ||
//                strstr(pProcessInfo[i].Module, ".") || strstr(pProcessInfo[i].Module, "_") ||
//                strstr(pProcessInfo[i].Module, "-") || strstr(pProcessInfo[i].Module, " ")
//                )
//            {
//                if (pProcessInfo[i].ulongdata1 != 0)
//                {
//                    Process_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1);
//                }
//            }
//            else
//            {
//                if (EnableShowhideproc)
//                {
//                    Process_AddListItem("Hide/EXIT Process", pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1);
//                }
//            }
//        }
//    }
//    else 
//    {
//        if (GetLastError() == 998)
//        {
//            if (Failedcount < 5)
//            {
//                Failedcount++;
//                goto startenumprocess;
//            }
//        }
//        printf("Failed Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.ProcessInfo);
//    printf("Enum Process successfully\n");
//    return 0;
//}
//*/
//extern "C"  __declspec(dllexport)
//BOOL EnumDrivers()
//{
//    BOOL bRet = FALSE;
//    ULONG nCnt = 1000;
//    PALL_DRIVERSS pDriverInfo = NULL;
//    ULONG nCount = NULL;
//
//    struct input {
//        ULONG nSize;
//        PALL_DRIVERSS pBuffer;
//    };
//
//    input inputs = { 0 };
//
//    inputs.nSize = nCnt * sizeof(ALL_DRIVERSS);
//    inputs.pBuffer = (PALL_DRIVERSS)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, nCnt * sizeof(ALL_DRIVERSS));
//    if (inputs.pBuffer)
//    {
//        pDriverInfo = inputs.pBuffer;
//        DeviceIoControl(hDevice, IOCTL_ENUM_DRIVERS, &inputs, sizeof(input), 0, 0, 0, NULL);
//        if (pDriverInfo->nCnt > 0)
//        {
//            for (ULONG i = 0; i < pDriverInfo->nCnt; i++)
//            {
//                if (strstr(pDriverInfo->Drivers[i].szDriverName, "sysdiag"))
//                {
//                    char DriverNames[MAX_PATH];
//                    sprintf_s(DriverNames, "%s (WARNING:ROOTKIT)", pDriverInfo->Drivers[i].szDriverName);
//                    Driver_AddListItem(DriverNames, pDriverInfo->Drivers[i].szDriverPath, (PVOID)pDriverInfo->Drivers[i].nBase, (PVOID)pDriverInfo->Drivers[i].nDriverObject, pDriverInfo->Drivers[i].nSize);
//                }
//                else
//                {
//                    Driver_AddListItem(pDriverInfo->Drivers[i].szDriverName, pDriverInfo->Drivers[i].szDriverPath, (PVOID)pDriverInfo->Drivers[i].nBase, (PVOID)pDriverInfo->Drivers[i].nDriverObject, pDriverInfo->Drivers[i].nSize);
//                }
//                nCount++;
//            }
//        }
//        bRet = HeapFree(GetProcessHeap(), 0, inputs.pBuffer);
//    }
//    printf("Enum Kernel-Module successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL KernelTerminateProcess(ULONG PID)
//{
//    struct input {
//        ULONG PID;
//    };
//    input inputs = { 0 };
//    inputs.PID = PID;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_KILL_PROCESS, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (status) {
//        printf("Terminate Process: %d Success.\n", inputs.PID);
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//extern "C"  __declspec(dllexport)
//BOOL KernelForceTerminateProcess(ULONG PID)
//{
//    struct input {
//        ULONG PID;
//    };
//    input inputs = { 0 };
//    inputs.PID = PID;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_Force_KILL_PROCESS, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (status) {
//        printf("Terminate Process: %d Success.\n", inputs.PID);
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//extern "C"  __declspec(dllexport)
//BOOL Suspend(ULONG PID)
//{
//    struct input {
//        ULONG PID;
//    };
//    input inputs = { 0 };
//    inputs.PID = PID;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_Suspend_PROCESS, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (status) {
//        printf("Suspend Process: %d Success.\n", inputs.PID);
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//extern "C"  __declspec(dllexport)
//BOOL Resume(ULONG PID)
//{
//    struct input {
//        ULONG PID;
//    };
//    input inputs = { 0 };
//    inputs.PID = PID;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_Resume_PROCESS, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (status) {
//        printf("Resume Process: %d Success.\n", inputs.PID);
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//extern "C"  __declspec(dllexport)
//BOOL SetCriticalProcess(ULONG PID)
//{
//    struct input {
//        ULONG PID;
//    };
//    input inputs = { 0 };
//    inputs.PID = PID;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_SET_CRITICAL_PROCESS, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (status) {
//        printf("Set Critical Process: %d Success.\n", inputs.PID);
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//extern "C"  __declspec(dllexport)
//BOOL KillProcessAndDeleteFile(ULONG PID)
//{
//    struct input {
//        ULONG PID;
//    };
//    input inputs = { 0 };
//    inputs.PID = PID;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_KILL_PROCESS_DELETE, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (status) {
//        printf("Kill Process: %d Success.\n", inputs.PID);
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//extern "C"  __declspec(dllexport)
//BOOL SetPPL(ULONG PID,int PPLLevel)
//{
//    struct input {
//        ULONG PID;
//        int PPLLevel;
//    };
//    input inputs = { 0 };
//    inputs.PID = PID;
//    inputs.PPLLevel = PPLLevel;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_Set_PPL, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (status) {
//        printf("Set Process: %d Success.\n", inputs.PID);
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//extern "C"  __declspec(dllexport)
//BOOL HiddenProcess(ULONG PID, int Mode)
//{
//    struct input {
//        ULONG PID;
//    };
//    input inputs = { 0 };
//    inputs.PID = PID;
//    BOOL status = NULL;
//    if (Mode == FALSE)
//    {
//        status = DeviceIoControl(hDevice, IOCTL_Hidden_PROCESS, &inputs, sizeof(input), 0, 0, 0, NULL);
//        if (status) {
//            printf("Hidden Process: %d Success.\n", inputs.PID);
//        }
//        else {
//            printf("Failed. Error %ld\n", GetLastError());
//        }
//    }
//    else
//    {
//        status = DeviceIoControl(hDevice, IOCTL_Force_Hidden_PROCESS, &inputs, sizeof(input), 0, 0, 0, NULL);
//        if (status) {
//            printf("Hidden Process: %d Success.\n", inputs.PID);
//        }
//        else {
//            printf("Failed. Error %ld\n", GetLastError());
//        }
//    }
//
//    return status;
//}
//extern "C"  __declspec(dllexport)
//BOOL UnloadDriver(PVOID DriverObject)
//{
//    struct input {
//        PVOID Address;
//    };
//    input inputs = { 0 };
//    inputs.Address = DriverObject;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_Unload_Driver, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (status) {
//        printf("Unload = %p Success.\n", inputs.Address);
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//extern "C"  __declspec(dllexport)
//BOOL HiddenDriver(PVOID DriverObject)
//{
//    struct input {
//        PVOID Address;
//    };
//    input inputs = { 0 };
//    inputs.Address = DriverObject;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_Hidden_Driver, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (status) {
//        printf("Hidden = %p Success.\n", inputs.Address);
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//extern "C"  __declspec(dllexport)
//BOOL ForceHiddenDriver(PVOID DriverObject)
//{
//    struct input {
//        PVOID Address;
//    };
//    input inputs = { 0 };
//    inputs.Address = DriverObject;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_FORCE_HIDDEN_DRIVER, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (status) {
//        printf("Hidden = %p Success.\n", inputs.Address);
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//
//DWORD WINAPI Monitor_Thread(LPVOID lpParam) 
//{
//    PROCESS_PTR Master = { 0 };
//    DWORD dwRet = 0;
//    hProcessEvent = OpenEventW(SYNCHRONIZE, FALSE, L"Global\\System-Monitor-Event");
//
//    while (TRUE)
//    {
//        WaitForSingleObject(hProcessEvent, INFINITE);
//        BOOL bRet = DeviceIoControl(hDevice, IOCTL_GET_EVENT, NULL, 0, &Master, sizeof(Master), &dwRet, NULL);
//        if (!bRet)
//        {
//        }
//        
//        if (Master.Type == Event_CreateProcess)
//        {
//            PROCESS_PTR Slave = { 0 };
//            if (Master.hpParProcessId != Slave.hpParProcessId || Master.hProcessId != Slave.hProcessId || Master.bIsCreateMark != Slave.bIsCreateMark)
//            {
//                //printf("[INFO] Process Create PID = %d \n", Master.hProcessId);
//                char buffer[3000];
//                snprintf(buffer, sizeof(buffer), "[INFO] Create Process PID = %d", Master.hProcessId);
//                Monitor_listItems.emplace_back(buffer);
//                Slave = Master;
//            }
//        }
//        if (Master.Type == Event_LoadDriver)
//        {
//            PROCESS_PTR Slave = { 0 };
//            if (Master.DriverEntryPoint != Slave.DriverEntryPoint)
//            {
//                char buffer[3000];
//                snprintf(buffer, sizeof(buffer), "[INFO] Load Driver = %s", Master.Loaded_DriverPath);
//                Monitor_listItems.emplace_back(buffer);
//                Slave = Master;
//            }
//        }
//        if (Master.Type == Event_Registry)
//        {
//            PROCESS_PTR Slave = { 0 };
//            if (Master.hProcessId != Slave.hProcessId)
//            {
//                if (Master.RegOp == 0)
//                {
//                    char buffer[3000];
//                    snprintf(buffer, sizeof(buffer), "[INFO] Registry Edit PID = %d | operate = CREATE | Path = %s \n", Master.hProcessId, Master.Loaded_DriverPath);
//                    Monitor_listItems.emplace_back(buffer);
//                }
//                if (Master.RegOp == 1)
//                {
//                    char buffer[3000];
//                    snprintf(buffer, sizeof(buffer), "[INFO] Registry Edit PID = %d | operate = DELETE | Path = %s \n", Master.hProcessId, Master.Loaded_DriverPath);
//                    Monitor_listItems.emplace_back(buffer);
//                }
//                if (Master.RegOp == 2)
//                {
//                    char buffer[3000];
//                    snprintf(buffer, sizeof(buffer), "[INFO] Registry Edit PID = %d | operate = SETINFO | Path = %s \n", Master.hProcessId, Master.Loaded_DriverPath);
//                    Monitor_listItems.emplace_back(buffer);
//                }
//                Slave = Master;
//            }
//        }
//        if (Master.Type == Event_File)
//        {
//            PROCESS_PTR Slave = { 0 };
//            if (System_Monitor_ShowFileRW == true)
//            {
//                if (Master.hProcessId != Slave.hProcessId)
//                {
//                    if (Master.RegOp == 0)
//                    {
//                        char buffer[3000];
//                        snprintf(buffer, sizeof(buffer), "[INFO] File PID = %d | operate = CREATE | Path = %s \n", Master.hProcessId, Master.Loaded_DriverPath);
//                        Monitor_listItems.emplace_back(buffer);
//                    }
//                    if (Master.RegOp == 1)
//                    {
//                        char buffer[3000];
//                        snprintf(buffer, sizeof(buffer), "[INFO] File PID = %d | operate = READ | Path = %s \n", Master.hProcessId, Master.Loaded_DriverPath);
//                        Monitor_listItems.emplace_back(buffer);
//                    }
//                    if (Master.RegOp == 2)
//                    {
//                        char buffer[3000];
//                        snprintf(buffer, sizeof(buffer), "[INFO] File PID = %d | operate = WRITE | Path = %s \n", Master.hProcessId, Master.Loaded_DriverPath);
//                        Monitor_listItems.emplace_back(buffer);
//                    }
//                    if (Master.RegOp == 3)
//                    {
//                        char buffer[3000];
//                        snprintf(buffer, sizeof(buffer), "[INFO] File PID = %d | operate = SETINFO | Path = %s \n", Master.hProcessId, Master.Loaded_DriverPath);
//                        Monitor_listItems.emplace_back(buffer);
//                    }
//                    Slave = Master;
//                }
//            }
//        }
//    }
//    return 0;
//}
//extern "C"  __declspec(dllexport)
//BOOL ScanSysenterHook()
//{
//    typedef struct _ALL_CALLBACKS_
//    {
//        ULONG_PTR nSize;
//        PVOID CallBackInfo;
//    }ALL_CALLBACKS, * PALL_CALLBACKS;
//    BOOL bRet = FALSE;
//    ALL_CALLBACKS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.CallBackInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_Scan_MSRHook, &pInput, sizeof(ALL_CALLBACKS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.CallBackInfo;
//        if (nRet < 2000)
//        {
//            if (pProcessInfo[1].ulongdata1 == 10)
//            {
//                Hook_MSR_AddListItem("KiSystemCall64", pProcessInfo[1].Module, "VTHook", pProcessInfo[1].pvoidaddressdata2, pProcessInfo[1].pvoidaddressdata1);
//            }
//            else
//            {
//                if (pProcessInfo[1].pvoidaddressdata1 == NULL)
//                {
//                    Hook_MSR_AddListItem("KiSystemCall64", pProcessInfo[1].Module, "Inline Hook", pProcessInfo[1].pvoidaddressdata2, pProcessInfo[1].pvoidaddressdata1);
//                }
//                else
//                {
//                    if (pProcessInfo[1].pvoidaddressdata1 == pProcessInfo[1].pvoidaddressdata2)
//                    {
//                        Hook_MSR_AddListItem("KiSystemCall64", pProcessInfo[1].Module, "-", pProcessInfo[1].pvoidaddressdata2, pProcessInfo[1].pvoidaddressdata1);
//                    }
//                    else
//                    {
//                        Hook_MSR_AddListItem("KiSystemCall64", pProcessInfo[1].Module, "MSR Hook", pProcessInfo[1].pvoidaddressdata2, pProcessInfo[1].pvoidaddressdata1);
//                    }
//                }
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.CallBackInfo);
//    return 0;
//}
//extern "C"  __declspec(dllexport)
//BOOL Kernel_DeleteFile(WCHAR* PATH)
//{
//    UNICODE_STRING Path[MAX_PATH];
//    RtlInitUnicodeString(Path, PATH);
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_DELETE_FILE_UNICODE, Path, sizeof(Path), NULL, 0, &bytesReturned, NULL);
//    if (!status)
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return 0;
//}
//extern "C"  __declspec(dllexport)
//BOOL Kernel_DeleteFile_Auto(WCHAR* PATH)
//{
//    WCHAR TargetPath[MAX_PATH];
//    wcscpy(TargetPath, L"\\??\\");
//    wcscat(TargetPath, PATH);
//    UNICODE_STRING Path[MAX_PATH];
//    RtlInitUnicodeString(Path, TargetPath);
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_DELETE_FILE_UNICODE, Path, sizeof(Path), NULL, 0, &bytesReturned, NULL);
//    if (!status)
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return 0;
//}
//extern "C"  __declspec(dllexport)
//BOOL Kernel_Force_DeleteFile(WCHAR* PATH)
//{
//    UNICODE_STRING Path[MAX_PATH];
//    RtlInitUnicodeString(Path, PATH);
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_FORCE_DELETE_UNICODE, Path, sizeof(Path), NULL, 0, &bytesReturned, NULL);
//    if (status)
//    {
//        DeleteFileW(PATH);
//    }
//    else
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return 0;
//}
//extern "C"  __declspec(dllexport)
//BOOL Kernel_Force_DeleteFile_Auto(WCHAR* PATH)
//{
//    WCHAR TargetPath[MAX_PATH];
//    wcscpy(TargetPath, L"\\??\\");
//    wcscat(TargetPath, PATH);
//    UNICODE_STRING Path[MAX_PATH];
//    RtlInitUnicodeString(Path, TargetPath);
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_FORCE_DELETE_UNICODE, Path, sizeof(Path), NULL, 0, &bytesReturned, NULL);
//    if (status)
//    {
//        DeleteFileW(PATH);
//    }
//    else
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return 0;
//}
//extern "C"  __declspec(dllexport)
//BOOL Kernel_LockFile(WCHAR* PATH)
//{
//    UNICODE_STRING Path[MAX_PATH];
//    RtlInitUnicodeString(Path, PATH);
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_LOCK_FILE_UNICODE, Path, sizeof(Path), NULL, 0, &bytesReturned, NULL);
//    if (!status)
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return 0;
//}
//extern "C"  __declspec(dllexport)
//BOOL Kernel_LockFile_Auto(WCHAR* PATH)
//{
//    WCHAR TargetPath[MAX_PATH];
//    wcscpy(TargetPath, L"\\??\\");
//    wcscat(TargetPath, PATH);
//    UNICODE_STRING Path[MAX_PATH];
//    RtlInitUnicodeString(Path, TargetPath);
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_LOCK_FILE_UNICODE, Path, sizeof(Path), NULL, 0, &bytesReturned, NULL);
//    if (!status)
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return 0;
//}
//
//
//int windowFlags = 
// ImGuiWindowFlags_NoDocking
//| ImGuiWindowFlags_NoBringToFrontOnFocus
//| ImGuiWindowFlags_NoNavFocus
//;
//
//void charToWCHAR(const char* input, WCHAR* output) {
//    int wideCharLen = MultiByteToWideChar(CP_ACP, 0, input, -1, nullptr, 0);
//    if (wideCharLen <= 0) {
//        throw std::runtime_error("MultiByteToWideChar failed to get length");
//    }
//
//    int result = MultiByteToWideChar(CP_ACP, 0, input, -1, output, wideCharLen);
//    if (result == 0) {
//        throw std::runtime_error("MultiByteToWideChar failed to convert");
//    }
//}
//
//void enumerateFilestodelete(const std::string& folderPath) {
//    try {
//        if (!fs::exists(folderPath) || !fs::is_directory(folderPath)) {
//            return;
//        }
//
//        for (const auto& entry : fs::directory_iterator(folderPath)) {
//            if (fs::is_regular_file(entry)) {
//                //std::cout << "Path: " << entry.path().string().c_str() << std::endl;
//                WCHAR widePath[MAX_PATH];
//                WCHAR TargetPath[MAX_PATH];
//                charToWCHAR(entry.path().string().c_str(), widePath);
//                wcscpy(TargetPath, L"\\??\\");
//                wcscat(TargetPath, widePath);
//                printf("%ws\n", TargetPath);
//                Kernel_DeleteFile(TargetPath);
//            }
//        }
//    }
//    catch (const fs::filesystem_error& e) {
//    }
//}
//
//void enumerateFilestodelete_Force(const std::string& folderPath) {
//    try {
//        if (!fs::exists(folderPath) || !fs::is_directory(folderPath)) {
//            return;
//        }
//
//        for (const auto& entry : fs::directory_iterator(folderPath)) {
//            if (fs::is_regular_file(entry)) {
//                //std::cout << "Path: " << entry.path().string().c_str() << std::endl;
//                WCHAR widePath[MAX_PATH];
//                WCHAR TargetPath[MAX_PATH];
//                charToWCHAR(entry.path().string().c_str(), widePath);
//                wcscpy(TargetPath, L"\\??\\");
//                wcscat(TargetPath, widePath);
//                printf("%ws\n", TargetPath);
//                Kernel_Force_DeleteFile(TargetPath);
//            }
//        }
//    }
//    catch (const fs::filesystem_error& e) {
//    }
//}
//
//BOOL Kernel_QueryFile(WCHAR* PATH, const char* InFolder)
//{
//    BOOL bRet = FALSE;
//    ULONG nRetLength = 0;
//    WCHAR TargetPath[MAX_PATH];
//    wcscpy(TargetPath, L"\\??\\");
//    wcscat(TargetPath, PATH);
//    UNICODE_STRING Path[MAX_PATH];
//    RtlInitUnicodeString(Path, TargetPath);
//    typedef struct _INPUT
//    {
//        ULONG_PTR nSize;
//        PDATA_INFO pBuffer;
//        PUNICODE_STRING Path;
//    }Input;
//
//    Input inputs = { 0 };
//    inputs.Path = Path;
//    inputs.nSize = sizeof(DATA_INFO) * 200000;
//    inputs.pBuffer = (DATA_INFO*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, inputs.nSize);
//    if (inputs.pBuffer)
//    {
//        bRet = DeviceIoControl(hDevice, IOCTL_QUERY_FILE, &inputs, sizeof(Input), &nRetLength, sizeof(ULONG), 0, 0);
//        if (bRet && nRetLength > 0)
//        {
//            for (ULONG i = 0; i < nRetLength; i++)
//            {
//                if (inputs.pBuffer[i].ulongdata4 == FALSE)
//                {
//                    File_AddListItem(inputs.pBuffer[i].Module, "File", InFolder, FALSE);
//                }
//                else
//                {
//                    File_AddListItem(inputs.pBuffer[i].Module, "Folder", InFolder, TRUE);
//                }
//            }
//        }
//        bRet = HeapFree(GetProcessHeap(), 0, inputs.pBuffer);
//    }
//    printf("Enum File successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumMiniFilter()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_MINIFILTER, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 1 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                MiniFilter_AddListItem(pProcessInfo[i].Module, IPR_FUNC_NAME[pProcessInfo[i].ulongdata1], pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata3, pProcessInfo[i].pvoidaddressdata2);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum MiniFilter successfully\n");
//    return bRet;
//}
//
//extern "C"  __declspec(dllexport)
//BOOL RemoveMiniFilter(PVOID NotifyAddress)
//{
//    struct input {
//        PVOID Address;
//    };
//    input inputs = { 0 };
//
//    inputs.Address = NotifyAddress;
//    //BOOL status = DeviceIoControl(hDevice, IOCTL_REMOVE_MINIFILTER, &inputs, sizeof(input), 0, 0, 0, NULL);
//    BOOL status = DeviceIoControl(hDevice, IOCTL_REMOVE_MINIFILTER2, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (!status) 
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    Sleep(1500);
//    //ISUnload++;
//    return 0;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumExCallback_Core()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_EXCALLBACK, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 1 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                ExCallback_AddListItem(pProcessInfo[i].Module1, pProcessInfo[i].Module, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, pProcessInfo[i].pvoidaddressdata3);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum ExCallback successfully\n");
//    return bRet;
//}
//
//DWORD WINAPI EnumExCallbackThread(LPVOID lpParam)
//{
//    EnumExCallback_Core();
//    ExitThread(ERROR_SUCCESS);
//}
//
//BOOL EnumExCallback()
//{
//    CreateThread(NULL, 0, EnumExCallbackThread, NULL, 0, NULL);
//    return 0;
//}
//
//
//extern "C"  __declspec(dllexport)
//BOOL RemoveExCallback(PVOID NotifyAddress)
//{
//    struct input {
//        PVOID Address;
//    };
//    input inputs = { 0 };
//
//    inputs.Address = NotifyAddress;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_REMOVE_EXCALLBACK, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (!status)
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return 0;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumSystemThread()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_SYTSTEMTHREAD, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 1 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                SystemThread_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, pProcessInfo[i].ulongdata1);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum SystemThread successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL Kernel_TerminateThread(ULONG TID)
//{
//    struct input {
//        ULONG Tid;
//    };
//    input inputs = { 0 };
//
//    inputs.Tid = TID;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_TERMINATE_THREAD, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (!status)
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return 0;
//}
//extern "C"  __declspec(dllexport)
//BOOL Kernel_Force_TerminateThread(ULONG TID)
//{
//    struct input {
//        ULONG Tid;
//    };
//    input inputs = { 0 };
//
//    inputs.Tid = TID;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_FORCE_TERMINATE_THREAD, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (!status)
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return 0;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumIrpHook()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_IRP_HOOK, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 1 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                if (pProcessInfo[i].ulongdata1 == IrpHook_NTOSKRNL)
//                {
//                    char IrpFuncName[MAX_PATH];
//                    sprintf_s(IrpFuncName, "%s", pProcessInfo[i].wcstr);
//                    if (pProcessInfo[i].pvoidaddressdata1 != pProcessInfo[i].pvoidaddressdata2)
//                    {
//                        IrpHook_AddListItem(pProcessInfo[i].Module, "ntoskrnl.exe", IrpFuncName, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "NTOSKRNL-Hook", pProcessInfo[i].ulongdata2, pProcessInfo[i].pvoidaddressdata3);
//                    }
//                    else
//                    {
//                        IrpHook_AddListItem(pProcessInfo[i].Module, "ntoskrnl.exe", IrpFuncName, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "-", pProcessInfo[i].ulongdata2, pProcessInfo[i].pvoidaddressdata3);
//                    }
//                }
//                if (pProcessInfo[i].ulongdata1 == IrpHook_DISK)
//                {
//                    char IrpFuncName[MAX_PATH];
//                    sprintf_s(IrpFuncName, "%s", pProcessInfo[i].wcstr);
//                    if (pProcessInfo[i].pvoidaddressdata1 != pProcessInfo[i].pvoidaddressdata2)
//                    {
//                        IrpHook_AddListItem(pProcessInfo[i].Module, "DISK.SYS", IrpFuncName, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "DISK-Hook", pProcessInfo[i].ulongdata2, pProcessInfo[i].pvoidaddressdata3);
//                    }
//                    else
//                    {
//                        IrpHook_AddListItem(pProcessInfo[i].Module, "DISK.SYS", IrpFuncName, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "-", pProcessInfo[i].ulongdata2, pProcessInfo[i].pvoidaddressdata3);
//                    }
//                }
//                if (pProcessInfo[i].ulongdata1 == IrpHook_FSD)
//                {
//                    char IrpFuncName[MAX_PATH];
//                    sprintf_s(IrpFuncName, "%s", pProcessInfo[i].wcstr);
//                    if (pProcessInfo[i].pvoidaddressdata1 != pProcessInfo[i].pvoidaddressdata2)
//                    {
//                        IrpHook_AddListItem(pProcessInfo[i].Module, "FSD", IrpFuncName, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "FSD-Hook", pProcessInfo[i].ulongdata2, pProcessInfo[i].pvoidaddressdata3);
//                    }
//                    else
//                    {
//                        IrpHook_AddListItem(pProcessInfo[i].Module, "FSD", IrpFuncName, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "-", pProcessInfo[i].ulongdata2, pProcessInfo[i].pvoidaddressdata3);
//                    }
//                }
//                if (pProcessInfo[i].ulongdata1 == IrpHook_BEEP)
//                {
//                    char IrpFuncName[MAX_PATH];
//                    sprintf_s(IrpFuncName, "%s", pProcessInfo[i].wcstr);
//                    if (pProcessInfo[i].pvoidaddressdata1 != pProcessInfo[i].pvoidaddressdata2)
//                    {
//                        IrpHook_AddListItem(pProcessInfo[i].Module, "BEEP.SYS", IrpFuncName, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "BEEP-Hook", pProcessInfo[i].ulongdata2, pProcessInfo[i].pvoidaddressdata3);
//                    }
//                    else
//                    {
//                        IrpHook_AddListItem(pProcessInfo[i].Module, "BEEP.SYS", IrpFuncName, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "-", pProcessInfo[i].ulongdata2, pProcessInfo[i].pvoidaddressdata3);
//                    }
//                }
//                if (pProcessInfo[i].ulongdata1 == IrpHook_NULL)
//                {
//                    char IrpFuncName[MAX_PATH];
//                    sprintf_s(IrpFuncName, "%s", pProcessInfo[i].wcstr);
//                    if (pProcessInfo[i].pvoidaddressdata1 != pProcessInfo[i].pvoidaddressdata2)
//                    {
//                        IrpHook_AddListItem(pProcessInfo[i].Module, "NULL.SYS", IrpFuncName, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "NULL-Hook", pProcessInfo[i].ulongdata2, pProcessInfo[i].pvoidaddressdata3);
//                    }
//                    else
//                    {
//                        IrpHook_AddListItem(pProcessInfo[i].Module, "NULL.SYS", IrpFuncName, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "-", pProcessInfo[i].ulongdata2, pProcessInfo[i].pvoidaddressdata3);
//                    }
//                }
//                if (pProcessInfo[i].ulongdata1 == IrpHook_TCPIP)
//                {
//                    char IrpFuncName[MAX_PATH];
//                    sprintf_s(IrpFuncName, "%s", pProcessInfo[i].wcstr);
//                    if (pProcessInfo[i].pvoidaddressdata1 != pProcessInfo[i].pvoidaddressdata2)
//                    {
//                        IrpHook_AddListItem(pProcessInfo[i].Module, "TCPIP.SYS", IrpFuncName, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "TCPIP-Hook", pProcessInfo[i].ulongdata2, pProcessInfo[i].pvoidaddressdata3);
//                    }
//                    else
//                    {
//                        IrpHook_AddListItem(pProcessInfo[i].Module, "TCPIP.SYS", IrpFuncName, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "-", pProcessInfo[i].ulongdata2, pProcessInfo[i].pvoidaddressdata3);
//                    }
//                }
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum Irp Hook successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumCreateProcessNotify()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_CREATE_PROCESS_NOTIFY, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "CreateProcess", pProcessInfo[i].pvoidaddressdata1, NULL, Notify_CreateProcess);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum CreateProcess Notify successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumCreateThreadNotify()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_CREATE_THREAD_NOTIFY, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "CreateThread", pProcessInfo[i].pvoidaddressdata1, NULL, Notify_CreateThread);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum Create Thread Notify successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumObCallback()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_OB_PROCESS_CALLBACK, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                if (pProcessInfo[i].pvoidaddressdata1 != NULL)
//                {
//                    Notify_AddListItem(pProcessInfo[i].Module, "ObCallback-PsProcessType", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].handledata1, Notify_ObCllback);
//                }
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum ObProcess Callback successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumObCallback_Thread()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_OB_THREAD_CALLBACK, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                if (pProcessInfo[i].pvoidaddressdata1 != NULL)
//                {
//                    Notify_AddListItem(pProcessInfo[i].Module, "ObCallback-PsThreadType", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].handledata1, Notify_ObCllback);
//                }
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum ObThread Callback successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumObCallback_Desktop()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_OB_THREAD_CALLBACK, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                if (pProcessInfo[i].pvoidaddressdata1 != NULL)
//                {
//                    Notify_AddListItem(pProcessInfo[i].Module, "ObCallback-Desktop", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].handledata1, Notify_ObCllback);
//                }
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum ObDesktop Callback successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumLoadImage()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_LOADIMAGE_NOTIFY, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "Loadimage", pProcessInfo[i].pvoidaddressdata1, NULL, Notify_Loadimage);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum LoadImage Notify successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumRegistry()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_REGISTRY_CALLBACK, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "Registry", pProcessInfo[i].pvoidaddressdata1, NULL, Notify_Registry);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum Registry Callback successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumBugCheckCallback()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_BUGCHECK_CALLBACK, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "BugCheck", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, Notify_BugCheck);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum BugCheck Callback successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumBugCheckReasonCallback()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_BUGCHECKREASON_CALLBACK, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "BugCheckReason", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, Notify_BugCheckReason);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum BugCheckReason Callback successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumShutdownNotify()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_SHUTDOWN_NOTIFY, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "Shutdown", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, Notify_Shutdown);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum Shutdown Notify successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumLastShutdownNotify()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_LASTSHUTDOWN_NOTIFY, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "LastChanceShutdown", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, Notify_LoastShutdown);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum Last Shutdown Notify successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumFsNotify()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_FS_NOTIFY, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "FileSystemNotify", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, Notify_FsNotofy);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumPowerSettingNotify()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_LASTSHUTDOWN_NOTIFY, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "PowerSetting", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, Notify_PowerSetting);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum PowerSetting Notify successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumCoalescingNotify()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_COALESCING_NOTIFY, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "CoalescingCallback", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, Notify_Coalescing);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum Coalsecing Callback successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumPrioriryNotify()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_PRIORIRY_NOTIFY, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "PrioriryCallback", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, Notify_Priority);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum Prioriry Callback successfully\n");
//    return bRet;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumDbgPrintCallback()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_DBGPRINT_CALLBACK, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "DbgPrint", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, Notify_DbgPrint);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum DbgPrint Callback successfully\n");
//    return bRet;
//}
//
//BOOL RemoveKernelNotify(PVOID Address, PVOID Handle, ULONG Type)
//{
//    struct input {
//        PVOID Address;
//        PVOID Handle;
//        ULONG Type;
//    };
//    input inputs = { 0 };
//
//    inputs.Address = Address;
//    inputs.Handle = Handle;
//    inputs.Type = Type;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_REMOVE_NOTIFY, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (!status)
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return 0;
//}
//extern "C"  __declspec(dllexport)
//BOOL EnumNmiCallbacks()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_NMI_CALLBACK, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "NmiCallback", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, Notify_NmiCallback);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum Nmi Callback successfully\n");
//    return bRet;
//}
//
//extern "C"  __declspec(dllexport)
//BOOL EnumPlugPlayNotify()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_PLUGPLAY_NOTIFY, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "PlugPlay", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, Notify_PlugPlay);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum PlugPlay Notify successfully\n");
//    return bRet;
//}
//
//extern "C"  __declspec(dllexport)
//BOOL EnumEmpCallback()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_EMP_CALLBACK, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                Notify_AddListItem(pProcessInfo[i].Module, "EmpCallback", pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, Notify_EmpCallback);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum EmpCallback successfully\n");
//    return bRet;
//}
//
//BOOL EnumObjectTypeTable()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_OBJECT_TYPE_TABLE, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                ObjectTypeTable_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].pvoidaddressdata1);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum ObjectType Table successfully\n");
//    return bRet;
//}
//
//BOOL EnumIoTimer()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_IO_TIMER, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                IoTimer_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum IoTimer successfully\n");
//    return bRet;
//}
//
//BOOL EnumIDT()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_IDT, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                IDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].ulongdata1, pProcessInfo[i].ulongdata2, pProcessInfo[i].pvoidaddressdata1);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum IDT successfully\n");
//    return bRet;
//}
//
//extern "C"  __declspec(dllexport)
//BOOL EnumSSDT()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_SSDT, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                if (pProcessInfo[i].pvoidaddressdata1 != NULL)
//                {
//                    if (pProcessInfo[i].pvoidaddressdata2 != NULL)
//                    {
//                        if (pProcessInfo[i].pvoidaddressdata1 != pProcessInfo[i].pvoidaddressdata2)
//                        {
//                            SSDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "SSDT-Hook", pProcessInfo[i].ulongdata1, FALSE);
//                        }
//                        else
//                        {
//                            if (pProcessInfo[i].ulongdata2 == TRUE)
//                            {
//                                SSDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "Inline-Hook", pProcessInfo[i].ulongdata1, TRUE);
//                            }
//                            else
//                            {
//                                SSDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "-", pProcessInfo[i].ulongdata1, FALSE);
//                            }
//                        }
//                    }
//                    else
//                    {
//                        SSDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "UNSUPPORT", pProcessInfo[i].ulongdata1, FALSE);
//                    }
//                }
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum SSDT successfully\n");
//    return 0;
//}
//
//
//BOOL EnumWfpCallout(ULONG Id)
//{
//    BOOL bRet = FALSE;
//    ULONG nRetLength = 0;
//    typedef struct _INPUT
//    {
//        ULONG_PTR nSize;
//        PDATA_INFO pBuffer;
//        ULONG id;
//    }Input;
//
//    Input inputs = { 0 };
//    inputs.id = Id;
//    inputs.nSize = sizeof(DATA_INFO) * 200000;
//    inputs.pBuffer = (DATA_INFO*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, inputs.nSize);
//    if (inputs.pBuffer)
//    {
//        bRet = DeviceIoControl(hDevice, IOCTL_ENUM_WFPCALLOUT, &inputs, sizeof(Input), &nRetLength, sizeof(ULONG), 0, 0);
//        if (bRet && nRetLength > 0)
//        {
//            for (ULONG i = 0; i < nRetLength; i++)
//            {
//                WFPCallout_AddListItem(inputs.pBuffer->Module, inputs.pBuffer->pvoidaddressdata1, inputs.pBuffer->pvoidaddressdata2, inputs.pBuffer->pvoidaddressdata3, inputs.pBuffer->pvoidaddressdata4, inputs.pBuffer->ulongdata1);
//            }
//        }
//        bRet = HeapFree(GetProcessHeap(), 0, inputs.pBuffer);
//    }
//    return bRet;
//}
//
//VOID GetWFPObject()
//{
//    HANDLE EngineHandle = NULL;
//    DWORD Status = FwpmEngineOpen(NULL, RPC_C_AUTHN_WINNT, NULL, NULL, &EngineHandle);
//    if (Status != ERROR_SUCCESS)
//    {
//        return;
//    }
//    HANDLE EnumHandle = NULL;
//    Status = FwpmCalloutCreateEnumHandle(EngineHandle, NULL, &EnumHandle);
//    if (Status != ERROR_SUCCESS)
//    {
//        FwpmEngineClose(EngineHandle);
//        return;
//    }
//    FWPM_CALLOUT0** CalloutEntries = NULL;
//    UINT32 Count = 0;
//    Status = FwpmCalloutEnum(EngineHandle, EnumHandle, 0xFFFFFFFF, &CalloutEntries, &Count);
//    if (Status != ERROR_SUCCESS)
//    {
//        FwpmCalloutDestroyEnumHandle(EngineHandle, EnumHandle);
//        FwpmEngineClose(EngineHandle);
//        return;
//    }
//    for (ULONG i = 0; i < Count; ++i)
//    {
//        std::wstring CalloutName = CalloutEntries[i]->displayData.name;
//        UINT CalloutId = CalloutEntries[i]->calloutId;
//        GUID LayerGuid = CalloutEntries[i]->applicableLayer;
//        WCHAR wLayerGuid[100];
//        if (!StringFromGUID2(LayerGuid, wLayerGuid, sizeof(wLayerGuid) / sizeof(WCHAR)))
//        {
//            FwpmFreeMemory((void**)&CalloutEntries);
//            FwpmCalloutDestroyEnumHandle(EngineHandle, EnumHandle);
//            FwpmEngineClose(EngineHandle);
//        }
//        char cname[MAX_PATH];
//        sprintf_s(cname, "%s", CalloutName);
//        EnumWfpCallout(CalloutId);
//        WFPFunction_AddListItem("FAILED", CalloutId, LayerGuid);
//    }
//    FwpmFreeMemory((void**)&CalloutEntries);
//    FwpmCalloutDestroyEnumHandle(EngineHandle, EnumHandle);
//    FwpmEngineClose(EngineHandle);
//}
//
//BOOL SSDT_UnHook(ULONG Index, PVOID OriginalAddress, ULONG IsInlineHook)
//{
//    struct input {
//        PVOID OriginalAddress;
//        ULONG id;
//    };
//    input inputs = { 0 };
//
//    struct input2 {
//        ULONG id;
//    };
//    input2 inputs2 = { 0 };
//    if (IsInlineHook == TRUE)
//    {
//        inputs2.id = Index;
//        BOOL status = DeviceIoControl(hDevice, IOCTL_UNHOOK_SSDT_INLINEHOOK, &inputs2, sizeof(input2), 0, 0, 0, NULL);
//        if (!status)
//        {
//            printf("Failed. Error %ld\n", GetLastError());
//        }
//    }
//    else
//    {
//        inputs.id = Index;
//        inputs.OriginalAddress = OriginalAddress;
//        BOOL status = DeviceIoControl(hDevice, IOCTL_REMOVE_SSDTHOOK, &inputs, sizeof(input), 0, 0, 0, NULL);
//        if (!status)
//        {
//            printf("Failed. Error %ld\n", GetLastError());
//        }
//    }
//    return 0;
//}
//
//extern "C"  __declspec(dllexport)
//BOOL K_SuspendThread(ULONG TID)
//{
//    struct input {
//        ULONG TID;
//    };
//    input inputs = { 0 };
//    inputs.TID = TID;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_Suspend_Thread, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (status) {
//        printf("Suspend Thread: %d Success.\n", inputs.TID);
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//
//extern "C"  __declspec(dllexport)
//BOOL K_ResumeThread(ULONG TID)
//{
//    struct input {
//        ULONG TID;
//    };
//    input inputs = { 0 };
//    inputs.TID = TID;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_Resume_Thread, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (status) {
//        printf("Resume Thread: %d Success.\n", inputs.TID);
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//
//BOOL SetProtectPid(ULONG Pid)
//{
//    struct input {
//        ULONG ProcessID;
//    };
//    input inputs = { 0 };
//    inputs.ProcessID = Pid;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_GET_PROTECT_PID, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (!status) {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//
//BOOL SetWindowProtect(HWND window)
//{
//    struct input {
//        HWND windowhandle;
//    };
//    input inputs = { 0 };
//    inputs.windowhandle = window;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_PROTECT_WINDOW, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (!status) {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//
//BOOL EnumGDT()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_GDT, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                GDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, pProcessInfo[i].ulongdata2, pProcessInfo[i].ulongdata3);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum GDT successfully\n");
//    return bRet;
//}
//
//BOOL EnumUnloadedDrivers()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_UNLOADED_DRIVERS, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                unloadeddriver_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum UnloadedDrivers successfully\n");
//    return bRet;
//}
//
//std::string ConversionPath(std::string path)
//{
//    size_t colon = path.find(':');
//    std::string trimmed = (colon != std::string::npos) ? path.substr(colon + 1) : path;
//    printf("%s\n", trimmed);
//    return trimmed;
//}
//
//BOOL NTFS_Search_Object_Delete_File(WCHAR* PATH)
//{
//    UNICODE_STRING Path[MAX_PATH];
//    RtlInitUnicodeString(Path, PATH);
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_SEARCH_DELETE_FILE_THREAD, Path, sizeof(Path), NULL, 0, &bytesReturned, NULL);
//    if (!status)
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return 0;
//}
//
//BOOL Kernel_InjectDll(ULONG PID, WCHAR* PATH)
//{
//    struct input {
//        ULONG PID;
//        UNICODE_STRING DllPath[MAX_PATH];
//    };
//    input inputs = { 0 };
//
//    inputs.PID = PID;
//    RtlInitUnicodeString(inputs.DllPath, PATH);
//    BOOL status = DeviceIoControl(hDevice, IOCTL_SHELLCODE_INJECT_DLL, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (!status)
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return 0;
//}
//
//BOOL Kernel_InjectDll_Thread(ULONG PID, WCHAR* PATH)
//{
//    struct input {
//        ULONG PID;
//        UNICODE_STRING DllPath[MAX_PATH];
//    };
//    input inputs = { 0 };
//
//    inputs.PID = PID;
//    RtlInitUnicodeString(inputs.DllPath, PATH);
//    BOOL status = DeviceIoControl(hDevice, IOCTL_THREAD_INJECT_DLL, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (!status)
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return 0;
//}
//
///*
//BOOL Kernel_ReadKernelMemory(PVOID Address, ULONG Size)
//{
//    struct input {
//        ULONG Size;
//        PVOID Address;
//    };
//    input inputs = { 0 };
//    inputs.Size = Size;
//    inputs.Address = Address;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_READ_MEMORY_GET_ADDRESS, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (status)
//    {
//        KMemReadsData data;
//        DWORD dwSize = 0;
//        data.size = Size;
//        data.data = new BYTE[data.size];
//
//        DeviceIoControl(hDevice, IOCTL_READ_MEMORY, &data, sizeof(data), &data, sizeof(data), &dwSize, NULL);
//        for (int i = 0; i < data.size; i++)
//        {
//            printf("0x%02X ", data.data[i]);
//        }
//        printf("\n");
//    }
//    return 0;
//}
//*/
//
//BOOL Kernel_ReadKernelMemory(PVOID Address, ULONG Size)
//{
//    static struct input {
//        ULONG Size;
//        PVOID Address;
//    } inputs;
//
//    inputs.Size = Size;
//    inputs.Address = Address;
//
//    if (!DeviceIoControl(hDevice, IOCTL_READ_MEMORY_GET_ADDRESS, &inputs, sizeof(input), 0, 0, 0, NULL))
//        return FALSE;
//
//    KMemReadsData data;
//    DWORD dwSize = 0;
//    data.size = Size;
//    data.data = new BYTE[data.size];
//
//    if (DeviceIoControl(hDevice, IOCTL_READ_MEMORY, &data, sizeof(data), &data, sizeof(data), &dwSize, NULL))
//    {
//        g_mem_data.assign(data.data, data.data + data.size);
//    }
//
//    delete[] data.data;
//    return !g_mem_data.empty();
//}
//
//BOOL ModifyDriverBase(PVOID OriginalBase, PVOID TargetBase)
//{
//    struct input {
//        PVOID OriginalAddr;
//        PVOID TargetAddr;
//    };
//    input inputs = { 0 };
//    inputs.OriginalAddr = OriginalBase;
//    inputs.TargetAddr = TargetBase;
//
//    BOOL status = DeviceIoControl(hDevice, IOCTL_MODIFY_DRIVER_BASE, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (!status) {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//
//BOOL EnumDriverMajorFunctions(PVOID DriverObject)
//{
//    BOOL bRet = FALSE;
//    ULONG nRetLength = 0;
//    typedef struct _INPUT
//    {
//        ULONG_PTR nSize;
//        PDATA_INFO pBuffer;
//        PVOID DriverObject;
//    }Input;
//
//    Input inputs = { 0 };
//    inputs.DriverObject = DriverObject;
//    inputs.nSize = sizeof(DATA_INFO) * 200000;
//    inputs.pBuffer = (DATA_INFO*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, inputs.nSize);
//    if (inputs.pBuffer)
//    {
//        bRet = DeviceIoControl(hDevice, IOCTL_ENUM_DRIVER_MAJORFUNCTIONS, &inputs, sizeof(Input), &nRetLength, sizeof(ULONG), 0, 0);
//        if (bRet && nRetLength > 0)
//        {
//            for (ULONG i = 0; i < nRetLength; i++)
//            {
//                char IrpName[MAX_PATH];
//                sprintf_s(IrpName, "%s", inputs.pBuffer[i].wcstr);
//                //printf("%p %s %s\n", inputs.pBuffer[i].pvoidaddressdata1, inputs.pBuffer[i].Module, inputs.pBuffer[i].wcstr);
//                DriverMajorInfo_AddListItem(IrpName, inputs.pBuffer[i].Module, inputs.pBuffer[i].pvoidaddressdata1);
//            }
//        }
//        bRet = HeapFree(GetProcessHeap(), 0, inputs.pBuffer);
//    }
//    return bRet;
//}
//
//BOOL EnumProcThread(ULONG Pid)
//{
//    BOOL bRet = FALSE;
//
//    typedef struct _INPUT
//    {
//        ULONG nPid;
//        ULONG nSize;
//        ULONG_PTR pEprocess;
//        PDATA_INFO pBuffer;
//    }Input;
//
//    Input inputs = { 0 };
//
//    ULONG nRetLength = 0;
//    inputs.nPid = Pid;
//    inputs.nSize = sizeof(DATA_INFO) * 1000;
//    inputs.pBuffer = (DATA_INFO*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, inputs.nSize);
//
//    if (inputs.pBuffer)
//    {
//        bRet = DeviceIoControl(hDevice, IOCTL_ENUM_PROCESS_THREAD, &inputs, sizeof(Input), &nRetLength, sizeof(ULONG), 0, 0);
//        if (bRet && nRetLength > 0)
//        {
//            for (ULONG i = 0; i < nRetLength; i++)
//            {
//                switch (inputs.pBuffer[i].ulongdata3)
//                {
//                case ThreadState_Initialized:
//                    ProcessThread_AddListItem("Initialized", inputs.pBuffer[i].ulongdata2, inputs.pBuffer[i].pvoidaddressdata2, (PVOID)inputs.pBuffer[i].ulong64data1, inputs.pBuffer[i].ulongdata1);
//                    break;
//
//                case ThreadState_Ready:
//                    ProcessThread_AddListItem("Ready", inputs.pBuffer[i].ulongdata2, inputs.pBuffer[i].pvoidaddressdata2, (PVOID)inputs.pBuffer[i].ulong64data1, inputs.pBuffer[i].ulongdata1);
//                    break;
//
//                case ThreadState_Running:
//                    ProcessThread_AddListItem("Running", inputs.pBuffer[i].ulongdata2, inputs.pBuffer[i].pvoidaddressdata2, (PVOID)inputs.pBuffer[i].ulong64data1, inputs.pBuffer[i].ulongdata1);
//
//                    break;
//
//                case ThreadState_Standby:
//                    ProcessThread_AddListItem("Standby", inputs.pBuffer[i].ulongdata2, inputs.pBuffer[i].pvoidaddressdata2, (PVOID)inputs.pBuffer[i].ulong64data1, inputs.pBuffer[i].ulongdata1);
//
//                    break;
//
//                case ThreadState_Terminated:
//                    ProcessThread_AddListItem("Terminated", inputs.pBuffer[i].ulongdata2, inputs.pBuffer[i].pvoidaddressdata2, (PVOID)inputs.pBuffer[i].ulong64data1, inputs.pBuffer[i].ulongdata1);
//
//                    break;
//
//                case ThreadState_Waiting:
//                    ProcessThread_AddListItem("Waiting", inputs.pBuffer[i].ulongdata2, inputs.pBuffer[i].pvoidaddressdata2, (PVOID)inputs.pBuffer[i].ulong64data1, inputs.pBuffer[i].ulongdata1);
//
//                    break;
//
//                case ThreadState_Transition:
//                    ProcessThread_AddListItem("Transition", inputs.pBuffer[i].ulongdata2, inputs.pBuffer[i].pvoidaddressdata2, (PVOID)inputs.pBuffer[i].ulong64data1, inputs.pBuffer[i].ulongdata1);
//
//                    break;
//
//                case ThreadState_DeferredReady:
//                    ProcessThread_AddListItem("Deferred Ready", inputs.pBuffer[i].ulongdata2, inputs.pBuffer[i].pvoidaddressdata2, (PVOID)inputs.pBuffer[i].ulong64data1, inputs.pBuffer[i].ulongdata1);
//
//                    break;
//
//                case ThreadState_GateWait:
//                    ProcessThread_AddListItem("Gate Wait", inputs.pBuffer[i].ulongdata2, inputs.pBuffer[i].pvoidaddressdata2, (PVOID)inputs.pBuffer[i].ulong64data1, inputs.pBuffer[i].ulongdata1);
//
//                    break;
//
//                default:
//                    ProcessThread_AddListItem("UNKNOWN", inputs.pBuffer[i].ulongdata2, inputs.pBuffer[i].pvoidaddressdata2, (PVOID)inputs.pBuffer[i].ulong64data1, inputs.pBuffer[i].ulongdata1);
//                    break;
//                }
//
//            }
//        }
//
//        bRet = HeapFree(GetProcessHeap(), 0, inputs.pBuffer);
//    }
//    return bRet;
//}
//
//extern "C"  __declspec(dllexport)
//BOOL RefeshAll()
//{
//    Process_items.clear();
//    Process_items.shrink_to_fit();
//    EnumProcess();
//    Driver_items.clear();
//    Driver_items.shrink_to_fit();
//    EnumDrivers();
//    MiniFilter_items.clear();
//    MiniFilter_items.shrink_to_fit();
//    EnumMiniFilter();
//    ExCallback_items.clear();
//    ExCallback_items.shrink_to_fit();
//    EnumExCallback();
//    SystemThread_items.clear();
//    SystemThread_items.shrink_to_fit();
//    EnumSystemThread();
//    IrpHook_items.clear();
//    IrpHook_items.shrink_to_fit();
//    EnumIrpHook();
//    Notify_items.clear();
//    Notify_items.shrink_to_fit();
//    EnumCreateProcessNotify();
//    EnumCreateThreadNotify();
//    EnumLoadImage();
//    EnumRegistry();
//    EnumBugCheckCallback();
//    EnumBugCheckReasonCallback();
//    EnumShutdownNotify();
//    EnumLastShutdownNotify();
//    EnumFsNotify();
//    EnumPowerSettingNotify();
//    EnumPlugPlayNotify();
//    EnumCoalescingNotify();
//    EnumPrioriryNotify();
//    EnumDbgPrintCallback();
//    EnumEmpCallback();
//    EnumNmiCallbacks();
//    EnumObCallback();
//    EnumObCallback_Thread();
//    EnumObCallback_Desktop();
//    ObjectTypeTable_items.clear();
//    ObjectTypeTable_items.shrink_to_fit();
//    EnumObjectTypeTable();
//    IoTimer_items.clear();
//    IoTimer_items.shrink_to_fit();
//    EnumIoTimer();
//    IDT_items.clear();
//    IDT_items.shrink_to_fit();
//    EnumIDT();
//    SSDT_items.clear();
//    SSDT_items.shrink_to_fit();
//    EnumSSDT();
//    GDT_items.clear();
//    GDT_items.shrink_to_fit();
//    EnumGDT();
//    unloadeddriver_items.clear();
//    unloadeddriver_items.shrink_to_fit();
//    EnumUnloadedDrivers();
//    return 0;
//}
//
//bool copyToClipboard(const std::string& text) {
//    if (!OpenClipboard(nullptr)) {
//        return false;
//    }
//
//    int wchar_length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
//    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wchar_length * sizeof(wchar_t));
//    if (!hMem) {
//        CloseClipboard();
//        return false;
//    }
//    wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hMem));
//    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, pMem, wchar_length);
//    GlobalUnlock(hMem);
//    SetClipboardData(CF_UNICODETEXT, hMem);
//    CloseClipboard();
//    return true;
//}
//
//std::string trim(const std::string& s) {
//    auto start = s.begin();
//    while (start != s.end() && std::isspace(*start)) {
//        start++;
//    }
//    auto end = s.end();
//    while (end != start && std::isspace(*(end - 1))) {
//        end--;
//    }
//    return std::string(start, end);
//}
//
//BOOL PluginsINIT()
//{
//    const std::string targetDir = "plugins\\*";
//
//    WIN32_FIND_DATAA findData;
//    HANDLE hFind = FindFirstFileA(targetDir.c_str(), &findData);
//
//    if (hFind == INVALID_HANDLE_VALUE) {
//        DWORD err = GetLastError();
//        if (err == ERROR_FILE_NOT_FOUND) {
//            std::cerr << "plugins NONE" << std::endl;
//        }
//        else {
//            std::cerr << "Failed To INIT Plugins: " << err << std::endl;
//        }
//        return 1;
//    }
//    do {
//        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
//            strcmp(findData.cFileName, ".") != 0 &&
//            strcmp(findData.cFileName, "..") != 0) {
//
//            std::string filePath = "plugins\\" + std::string(findData.cFileName) + "\\Info.dat";
//
//            std::ifstream file(filePath);
//            if (file) {
//                std::string line;
//                std::string Name;
//                std::string Entry;
//                std::string author;
//                std::string version;
//
//                while (std::getline(file, line)) 
//                {
//                    if (line.find("Entry=") == 0) 
//                    {
//                        std::string entry = trim(line.substr(6));
//                        if (!entry.empty()) {
//                            Entry = std::string(std::filesystem::current_path().string()) + "\\plugins\\" + std::string(findData.cFileName) + "\\" + entry;
//                        }
//                    }
//                    if (line.find("Name=") == 0)
//                    {
//                        std::string Names = trim(line.substr(5));
//                        if (!Names.empty()) {
//                            Name = Names;
//                        }
//                    }
//                    if (line.find("author=") == 0)
//                    {
//                        std::string authors = trim(line.substr(7));
//                        if (!authors.empty()) {
//                            author = authors;
//                        }
//                    }
//                    if (line.find("version=") == 0)
//                    {
//                        std::string versions = trim(line.substr(8));
//                        if (!versions.empty()) {
//                            version = versions;
//                        }
//                    }
//                }
//                Plugins_AddListItem(Name, Entry, author, version);
//            }
//        }
//    } while (FindNextFileA(hFind, &findData));
//
//    FindClose(hFind);
//    return 0;
//}
//
//BOOL RemoveSysdiagMiniFIlter()
//{
//    for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//    {
//        if (strstr(MiniFilter_items[i].Module.c_str(), "sysdiag"))
//        {
//            RemoveMiniFilter(MiniFilter_items[i].Filter);
//            printf("Remove MiniFilter = %p\n", MiniFilter_items[i].Filter);
//            return 0;
//        }
//    }
//    return 0;
//}
//
//extern "C"  __declspec(dllexport)
//BOOL Kernel_EPThook(PVOID Function, PVOID TargetAddress)
//{
//    struct input 
//    {
//        PVOID TargetAddress;
//        PVOID FunctionAddress;
//    };
//    input inputs = { 0 };
//
//    inputs.TargetAddress = Function;
//    inputs.FunctionAddress = TargetAddress;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_EPT_HOOK, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (!status)
//    {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return 0;
//}
//
//DWORD WINAPI EnumShadowSSDT_Thread(LPVOID lpParam)
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 10000);
//    pInput.nSize = sizeof(DATA_INFO) * 10000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_SSSDT, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 3000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                if (pProcessInfo[i].pvoidaddressdata1 != NULL)
//                {
//                    if (pProcessInfo[i].pvoidaddressdata2 == NULL)
//                    {
//                        ShadowSSDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1, "UNSUPPORT", pProcessInfo[i].pvoidaddressdata2, FALSE);
//                    }
//                    else
//                    {
//                        if (pProcessInfo[i].ulongdata2 == TRUE)
//                        {
//                            ShadowSSDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1, "Inline-Hook", pProcessInfo[i].pvoidaddressdata2, TRUE);
//                        }
//                        else
//                        {
//                            if (pProcessInfo[i].pvoidaddressdata1 != pProcessInfo[i].pvoidaddressdata2)
//                            {
//                                ShadowSSDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1, "SSSDT-Hook", pProcessInfo[i].pvoidaddressdata2, FALSE);
//                            }
//                            else
//                            {
//                                ShadowSSDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1, "-", pProcessInfo[i].pvoidaddressdata2, FALSE);
//                            }
//                        }
//                    }
//                }
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum ShadowSSDT successfully\n");
//    ExitThread(ERROR_SUCCESS);
//}
//
//extern "C"  __declspec(dllexport)
//BOOL EnumSSSDT()
//{
//    CreateThread(NULL, 0, EnumShadowSSDT_Thread, NULL, 0, NULL);
//    return 0;
//}
//
//extern "C"  __declspec(dllexport)
//BOOL EnumSSSDT_NoThread()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 10000);
//    pInput.nSize = sizeof(DATA_INFO) * 10000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_SSSDT, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 3000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                if (pProcessInfo[i].pvoidaddressdata1 != NULL)
//                {
//                    if (pProcessInfo[i].pvoidaddressdata2 == NULL)
//                    {
//                        ShadowSSDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1, "UNSUPPORT", pProcessInfo[i].pvoidaddressdata2, FALSE);
//                    }
//                    else
//                    {
//                        if (pProcessInfo[i].ulongdata2 == TRUE)
//                        {
//                            ShadowSSDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1, "Inline-Hook", pProcessInfo[i].pvoidaddressdata2, TRUE);
//                        }
//                        else
//                        {
//                            if (pProcessInfo[i].pvoidaddressdata1 != pProcessInfo[i].pvoidaddressdata2)
//                            {
//                                ShadowSSDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1, "SSSDT-Hook", pProcessInfo[i].pvoidaddressdata2, FALSE);
//                            }
//                            else
//                            {
//                                ShadowSSDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1, "-", pProcessInfo[i].pvoidaddressdata2, FALSE);
//                            }
//                        }
//                    }
//                }
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum ShadowSSDT successfully\n");
//    return 0;
//}
//
//extern "C"  __declspec(dllexport)
//BOOL EnumProcess()
//{
//    typedef struct _ALL_PROCESSES_
//    {
//        ULONG_PTR nSize;
//        PVOID ProcessInfo;
//    }ALL_PROCESSES, * PALL_PROCESSES;
//
//    BOOL bRet = FALSE;
//    ALL_PROCESSES pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.ProcessInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_PROCESS2, &pInput, sizeof(ALL_PROCESSES), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status)
//    {
//        pProcessInfo = (PDATA_INFO)pInput.ProcessInfo;
//        for (ULONG i = 0; i < nRet; i++)
//        {
//            if (pProcessInfo[i].ulongdata1 == 0)
//            {
//                Process_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].Module1, "SYSTEM");
//            }
//        }
//        for (ULONG i = 0; i < nRet; i++)
//        {
//            if (pProcessInfo[i].ulongdata1 != 0)
//            {
//                if (pProcessInfo[i].ulongdata2 == FALSE)
//                {
//                    Process_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].Module1, "RUNNING");
//                }
//                else
//                {
//                    Process_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].Module1, "HIDDEN");
//                }
//            }
//        }
//    }
//    else
//    {
//        printf("Failed Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.ProcessInfo);
//    printf("Enum Process successfully\n");
//    return 0;
//}
//
//BOOL SetSandboxPID(ULONG Pid)
//{
//    struct input {
//        ULONG ProcessID;
//    };
//    input inputs = { 0 };
//    inputs.ProcessID = Pid;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_SET_SANDBOX_PID, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (!status) {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//
//BOOL ModifyToken(ULONG Pid, ULONG Type)
//{
//    struct input {
//        ULONG PID;
//        ULONG Type;
//    };
//    input inputs = { 0 };
//    inputs.PID = Pid;
//    inputs.Type = Type;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_MODIFY_PROCESS_TOKEN, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (!status) {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//
//extern "C"  __declspec(dllexport)
//BOOL RemoveMiniFilterByDriver(const char* Drivername)
//{
//    for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//    {
//        if (strstr(MiniFilter_items[i].Module.c_str(), Drivername))
//        {
//            RemoveMiniFilter(MiniFilter_items[i].Filter);
//            return 0;
//        }
//    }
//    return 0;
//}
//
//BOOL RemoveObCallback(PVOID Handle)
//{
//    RefeshAll();
//    for (size_t i = 0; i < Notify_items.size(); ++i)
//    {
//        if (Notify_items[i].NotifyTypes == Notify_ObCllback)
//        {
//            if (Notify_items[i].Handle == Handle)
//            {
//                RemoveKernelNotify(Notify_items[i].Address, Notify_items[i].Handle, Notify_items[i].NotifyTypes);
//                return 0;
//            }
//        }
//    }
//    return ERROR_NOT_FOUND;
//}
//
//extern "C"  __declspec(dllexport) BOOL RemoveNotifyByDriver(const char* DriverName)
//{
//    for (size_t i = 0; i < Notify_items.size(); ++i)
//    {
//        if (strstr(Notify_items[i].Module.c_str(), DriverName))
//        {
//            if (Notify_items[i].NotifyTypes == Notify_Priority || Notify_items[i].NotifyTypes == Notify_Coalescing || Notify_items[i].NotifyTypes == Notify_NmiCallback || Notify_items[i].NotifyTypes == Notify_BugCheck || Notify_items[i].NotifyTypes == Notify_BugCheckReason || Notify_items[i].NotifyTypes == Notify_Shutdown || Notify_items[i].NotifyTypes == Notify_LoastShutdown || Notify_items[i].NotifyTypes == Notify_PlugPlay)
//            {
//                RemoveKernelNotify(Notify_items[i].Handle, Notify_items[i].Address, Notify_items[i].NotifyTypes);
//            }
//            else
//            {
//                if (Notify_items[i].NotifyTypes == Notify_ObCllback)
//                {
//                    RemoveObCallback(Notify_items[i].Handle);
//                }
//                else
//                {
//                    RemoveKernelNotify(Notify_items[i].Address, Notify_items[i].Handle, Notify_items[i].NotifyTypes);
//                }
//            }
//        }
//    }
//    RefeshAll();
//    return 0;
//}
//
//extern "C"  __declspec(dllexport) BOOL RemoveNotifyByEntryAddress(PVOID ENTRY)
//{
//    for (size_t i = 0; i < Notify_items.size(); ++i)
//    {
//        if (Notify_items[i].Address == ENTRY)
//        {
//            if (Notify_items[i].NotifyTypes == Notify_Priority || Notify_items[i].NotifyTypes == Notify_Coalescing || Notify_items[i].NotifyTypes == Notify_NmiCallback || Notify_items[i].NotifyTypes == Notify_BugCheck || Notify_items[i].NotifyTypes == Notify_BugCheckReason || Notify_items[i].NotifyTypes == Notify_Shutdown || Notify_items[i].NotifyTypes == Notify_LoastShutdown || Notify_items[i].NotifyTypes == Notify_PlugPlay)
//            {
//                RemoveKernelNotify(Notify_items[i].Handle, Notify_items[i].Address, Notify_items[i].NotifyTypes);
//            }
//            else
//            {
//                if (Notify_items[i].NotifyTypes == Notify_ObCllback)
//                {
//                    RemoveObCallback(Notify_items[i].Handle);
//                }
//                else
//                {
//                    RemoveKernelNotify(Notify_items[i].Address, Notify_items[i].Handle, Notify_items[i].NotifyTypes);
//                }
//            }
//        }
//    }
//    RefeshAll();
//    return 0;
//}
//
//BOOL DisableXdriverAC()
//{
//    for (size_t i = 0; i < SystemThread_items.size(); ++i)
//    {
//        if (strstr(SystemThread_items[i].Module.c_str(), "xdriver"))
//        {
//            Kernel_TerminateThread(SystemThread_items[i].TID);
//        }
//    }
//    RemoveMiniFilterByDriver("xdriver");
//    RemoveNotifyByDriver("xdriver");
//    return 0;
//}
//
//DWORD WINAPI EnumWFPThread(LPVOID lpParam)
//{
//    GetWFPObject();
//    ExitThread(ERROR_SUCCESS);
//}
//
//void DisassembleMemory(ULONG64 Address) 
//{
//    csh handle;
//    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK) {
//        std::cerr << "Capstone engine initialization failed!" << std::endl;
//        return;
//    }
//
//    const uint8_t* code = g_mem_data.data();
//    size_t codeSize = g_mem_data.size();
//
//    cs_insn* insn;
//    size_t count = cs_disasm(handle, code, codeSize, Address, 0, &insn);
//    if (count > 0) {
//        for (size_t i = 0; i < count; ++i) 
//        {
//            Disassembly_AddListItem(insn[i].op_str, insn[i].mnemonic, (PVOID)insn[i].address);
//        }
//    }
//    else {
//        std::cout << "Disassembly failed!" << std::endl;
//    }
//    cs_free(insn, count);
//    cs_close(&handle);
//}
//
//
//BOOL EnumHalDispathTable()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_HALDISPATHTABLE, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                HalDispathTable_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].pvoidaddressdata1);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum HalDispathTable successfully\n");
//    return bRet;
//}
//
//BOOL EnumHalPrivateDispathTable()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_HALPRIVATEDISPATHTABLE, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                HalPrivateDispathTable_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, (PVOID)pProcessInfo[i].ulong64data1);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum HalPrivateDispathTable successfully\n");
//    return bRet;
//}
//
//BOOL SSSDT_UnHook(ULONG Index, ULONG64 OriginalAddress, ULONG IsInlineHook)
//{
//    struct input {
//        ULONG Index;
//        ULONG64 OriginalAddress;
//        ULONG IsInlineHook;
//    };
//    input inputs = { 0 };
//    inputs.Index = Index;
//    inputs.OriginalAddress = OriginalAddress;
//    inputs.IsInlineHook = IsInlineHook;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_UNHOOK_SSSDT, &inputs, sizeof(input), 0, 0, 0, NULL);
//    if (!status) {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    return status;
//}
//
//BOOL SSDT_UnHookAll()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_SSDT, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                if (pProcessInfo[i].pvoidaddressdata1 != NULL)
//                {
//                    if (pProcessInfo[i].pvoidaddressdata2 != NULL)
//                    {
//                        if (pProcessInfo[i].pvoidaddressdata1 != pProcessInfo[i].pvoidaddressdata2)
//                        {
//                            //SSDT_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].Module1, pProcessInfo[i].pvoidaddressdata1, pProcessInfo[i].pvoidaddressdata2, "SSDT-Hook", pProcessInfo[i].ulongdata1, FALSE);
//                            SSDT_UnHook(pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata2, FALSE);
//                            printf("[SSDT] UnHook Index = %d Name = %s OriginalAddress = %p Hook = SSDT-Hook\n", pProcessInfo[i].ulongdata1, pProcessInfo[i].Module1, pProcessInfo[i].pvoidaddressdata2);
//                        }
//                        else
//                        {
//                            if (pProcessInfo[i].ulongdata2 == TRUE)
//                            {
//                                SSDT_UnHook(pProcessInfo[i].ulongdata1, pProcessInfo[i].pvoidaddressdata2, TRUE);
//                                printf("[SSDT] UnHook Index = %d Name = %s OriginalAddress = %p Hook = Inline-Hook\n", pProcessInfo[i].ulongdata1, pProcessInfo[i].Module1, pProcessInfo[i].pvoidaddressdata2);
//                            }
//                        }
//                    }
//                }
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("UnHook SSDT successfully\n");
//    return 0;
//}
//
//BOOL EnumPiDDBCacheTable()
//{
//    typedef struct _ALL_MINIFILTERS_
//    {
//        ULONG_PTR nSize;
//        PVOID MiniFilterInfo;
//    }ALL_MINIFILTERS, * PALL_MINIFILTERS;
//    BOOL bRet = FALSE;
//    ALL_MINIFILTERS pInput = { 0 };
//
//    PDATA_INFO pProcessInfo = NULL;
//
//    pInput.MiniFilterInfo = (PVOID)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DATA_INFO) * 1000);
//    pInput.nSize = sizeof(DATA_INFO) * 1000;
//
//    ULONG nRet = 0;
//
//    DWORD bytesReturned;
//    BOOL status = DeviceIoControl(hDevice, IOCTL_ENUM_PIDDBCACHE_TABLE, &pInput, sizeof(ALL_MINIFILTERS), &nRet, sizeof(ULONG), &bytesReturned, NULL);
//    if (status) {
//        pProcessInfo = (PDATA_INFO)pInput.MiniFilterInfo;
//        if (nRet > 0 && nRet < 2000)
//        {
//            for (ULONG i = 0; i < nRet; i++)
//            {
//                PIDDB_AddListItem(pProcessInfo[i].Module, pProcessInfo[i].ulongdata1, pProcessInfo[i].ulongdata2);
//            }
//        }
//    }
//    else {
//        printf("Failed. Error %ld\n", GetLastError());
//    }
//    bRet = HeapFree(GetProcessHeap(), 0, pInput.MiniFilterInfo);
//    printf("Enum PiDDBCacheTable successfully\n");
//    return bRet;
//}
//
//BOOLEAN IsAddressInRange(PVOID Address, PVOID RangeStart, PVOID RangeEnd) {
//    return (Address >= RangeStart) && (Address <= RangeEnd);
//}
//
//std::string GetDriverNameByAddress(PVOID Address)
//{
//    if (Driver_items.size() == NULL)
//    {
//        return "UNKNOWN";
//    }
//    for (size_t i = 0; i < Driver_items.size(); ++i) 
//    {
//        ULONG64 EndAddr = ((ULONG64)Driver_items[i].Base + Driver_items[i].Size);
//        if (IsAddressInRange(Address, Driver_items[i].Base, (PVOID)EndAddr))
//        {
//            return Driver_items[i].name;
//        }
//    }
//    return "UNKNOWN";
//}
//
//bool ASM_IsPossibleAddress(const std::string& str) {
//    if (str.empty()) return false;
//
//    if (str.size() > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
//        return true;
//    }
//
//    for (char c : str) {
//        if (!isxdigit(c)) {
//            return false;
//        }
//    }
//
//    size_t len = str.size();
//    if (str.size() > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
//        len -= 2;
//    }
//
//    return len == 8 || len == 16;
//}
//
//bool ASM_Getasmaddr(const std::string& str, uint64_t& outValue) {
//    outValue = 0;
//
//    if (!ASM_IsPossibleAddress(str)) {
//        return false;
//    }
//
//    try {
//        size_t pos = 0;
//        outValue = std::stoull(str, &pos, 16);
//
//        return pos == str.size();
//    }
//    catch (...) {
//        return false;
//    }
//    return false;
//}
//
//int main(int, char**)
//{
//    printf("INIT\n");
//    if (DebugMode == FALSE)
//    {
//        ReleaseCoreDriver();
//        LPCWSTR DrvPath = L"C:\\Windows\\System32\\drivers\\ArkDrv64.sys";
//        LPCWSTR DrvName = L"SKT64-Kernel-Driver";
//        LoadDriver_SELF(DrvPath, DrvName);
//        Sleep(1000);
//    }
//    else
//    {
//        printf("Debug Mode\n");
//        //Process_AddListItem("DebugTest", 100, (PVOID)0xFFFFFFFFFFFFFFFF);
//    }
//    //ProcessThread_AddListItem(NULL, NULL, NULL);
//    SetConsoleTitle("SKT64-Log Console");
//    hDevice = CreateFile("\\\\.\\ArkDrv64", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
//    if (hDevice == INVALID_HANDLE_VALUE) {
//        printf("Failed To Open Driver\n");
//        SetConsoleTitle("SKT64-Log Console [Failed To Open Driver]");
//        getchar();
//        if (DebugMode == FALSE)
//        {
//            return 0;
//        }
//    }
//    DeviceIoControl(hDevice, IOCTL_DELETE_SELFDRV, NULL, 0, NULL, 0, NULL, NULL);
//
//    EnumProcess();
//    EnumDrivers();
//    EnumMiniFilter();
//    EnumExCallback();
//    EnumSystemThread();
//    EnumIrpHook();
//    EnumCreateProcessNotify();
//    EnumCreateThreadNotify();
//    EnumLoadImage();
//    EnumRegistry();
//    EnumBugCheckCallback();
//    EnumBugCheckReasonCallback();
//    EnumShutdownNotify();
//    EnumLastShutdownNotify();
//    EnumFsNotify();
//    EnumPowerSettingNotify();
//    EnumPlugPlayNotify();
//    EnumCoalescingNotify();
//    EnumPrioriryNotify();
//    EnumDbgPrintCallback();
//    EnumEmpCallback();
//    EnumNmiCallbacks();
//    EnumObCallback();
//    EnumObCallback_Thread();
//    EnumObCallback_Desktop();
//    EnumObjectTypeTable();
//    EnumIoTimer();
//    EnumIDT();
//    EnumSSDT();
//    EnumGDT();
//    EnumUnloadedDrivers();
//    EnumPiDDBCacheTable();
//
//    glfwSetErrorCallback(glfw_error_callback);
//    if (!glfwInit())
//        return 1;
//
//#if defined(IMGUI_IMPL_OPENGL_ES2)
//    const char* glsl_version = "#version 100";
//    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
//    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
//    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
//#elif defined(__APPLE__)
//    const char* glsl_version = "#version 150";
//    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
//    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
//    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
//    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
//#else
//    const char* glsl_version = "#version 130";
//    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
//    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
//    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
//#endif
//    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
//    GLFWwindow* window = glfwCreateWindow(1200, 600, "ARK", nullptr, nullptr);
//    if (window == nullptr)
//        return 1;
//    glfwMakeContextCurrent(window);
//    glfwSwapInterval(1);
//    glfwHideWindow(window);
//
//    IMGUI_CHECKVERSION();
//    ImGui::CreateContext();
//    ImGuiIO& io = ImGui::GetIO(); (void)io;
//    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
//    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
//    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
//    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
//    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
//    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
//    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
//    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
//    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 15, nullptr, io.Fonts->GetGlyphRangesChineseFull());
//    io.ConfigViewportsNoAutoMerge = true;
//    io.ConfigViewportsNoTaskBarIcon = true;
//
//    ImGui::StyleColorsDark();
//
//
//    ImGuiStyle& style = ImGui::GetStyle();
//    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
//    {
//        style.WindowRounding = 0.0f;
//        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
//    }
//
//    ImGui_ImplGlfw_InitForOpenGL(window, true);
//#ifdef __EMSCRIPTEN__
//    ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");
//#endif
//    ImGui_ImplOpenGL3_Init(glsl_version);
//
//
//    static bool opt_fullscreen = false;
//    bool MainWindow = true;
//    bool PPLWindow = false;
//    bool Process_SettingWindow = false;
//    bool DebugWindow = false;
//    bool MemoryWindow = false;
//    bool ASMMemoryWindow = false;
//    bool ProcessThreadInfoWindow = false;
//    bool ProcessInjectDllWindow = false;
//    bool DriverModifyBaseWindow = false;
//    bool DriverMajorInfoWindow = false;
//    bool ModifyTokenWindow = false;
//
//
//    if (DebugMode == TRUE || KDebugMode == TRUE)
//    {
//        DebugWindow = true;
//    }
//
//
//    ULONG attacttoeditprocesspid = NULL;
//
//    ULONG64 attacttoeditdriverbase = NULL;
//
//    static char editmemoryaddress[17];
//
//
//    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
//
//#ifdef __EMSCRIPTEN__
//
//    io.IniFilename = nullptr;
//    EMSCRIPTEN_MAINLOOP_BEGIN
//#else
//    while (!glfwWindowShouldClose(window))
//#endif
//    {
//        glfwPollEvents();
//        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0)
//        {
//            ImGui_ImplGlfw_Sleep(10);
//            continue;
//        }
//
//        ImGui_ImplOpenGL3_NewFrame();
//        ImGui_ImplGlfw_NewFrame();
//        ImGui::NewFrame();
//
//        if (MainWindow)
//        {
//            ImVec2 screen_size = ImGui::GetIO().DisplaySize;
//
//            ImVec2 window_pos = ImVec2(screen_size.x * 0.5f, screen_size.y * 0.5f);
//            window_pos.x -= ImGui::GetWindowSize().x * 0.5f;
//            window_pos.y -= ImGui::GetWindowSize().y * 0.5f;
//
//            ImGui::SetNextWindowPos(window_pos, ImGuiCond_Appearing);
//            static float width = 1200.0f;
//            static float height = 600.0f;
//            ImGui::Begin("ARK [Kernel Mode Core: \\SystemRoot\\System32\\drivers\\ARKDRV64.SYS version: 4.7-Internal]", NULL, windowFlags);
//            ImGui::SetWindowSize(ImVec2(width, height));
//            if (ImGui::BeginTabBar("MainTabControl"))
//            {
//                if (ImGui::BeginTabItem("Process"))
//                {
//                    NoExit = 0;
//                    NoEnableHvm = 0;
//                    NoEnableMonitor = 0;
//
//                    if (ImGui::BeginTable("process_table", 4, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders))
//                    {
//                        ImGui::TableSetupColumn("ProcessName", ImGuiTableColumnFlags_WidthStretch, 0.25f);
//                        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed);
//                        ImGui::TableSetupColumn("EPROCESS", ImGuiTableColumnFlags_WidthFixed);
//                        ImGui::TableSetupColumn("Path");
//                        ImGui::TableHeadersRow();
//                        for (size_t i = 0; i < Process_items.size(); ++i) {
//                            ImGui::PushID(static_cast<int>(i));
//                            ImGui::TableNextRow();
//                            //ImGui::TableNextColumn();
//                            ImGui::TableSetColumnIndex(0);
//                            if (ImGui::Selectable(Process_items[i].name.c_str(), Process_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                            {
//                                for (auto& item : Process_items) item.selected = false;
//                                Process_items[i].selected = true;
//                            }
//                            //ImGui::TableNextColumn();
//                            ImGui::TableSetColumnIndex(1);
//                            ImGui::Text("%d", Process_items[i].id);
//                            //ImGui::TableNextColumn();
//                            ImGui::TableSetColumnIndex(2);
//                            ImGui::Text("%p", Process_items[i].eprocess);
//                            //ImGui::TableNextColumn();
//                            ImGui::TableSetColumnIndex(3);
//                            ImGui::Text(Process_items[i].Path.c_str());
////                            ImGui::TableNextColumn();
////                            ImGui::Text(Process_items[i].Status.c_str());
//
//                            Process_context_menu_id = static_cast<int>(i);
//
//                            if (Process_items[i].eprocess == NULL && Process_items[i].id == NULL)
//                            {
//                                Process_items.erase(Process_items.begin() + i);
//                            }
//
//                            ImGui::PopID();
//                        }
//                    }
//                    ImGui::EndTable();
//
//                    if (ImGui::BeginPopupContextItem("Process_Menu"))
//                    {
//                        if (ImGui::MenuItem("Refesh"))
//                        {
//                            Process_items.clear();
//                            Process_items.shrink_to_fit();
//                            EnumProcess();
//                        }
//                        if (ImGui::MenuItem("Terminate"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i) 
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    printf("Terminate Process = %d\n", Process_items[i].id);
//                                    KernelTerminateProcess(Process_items[i].id);
//                                    Process_items.erase(Process_items.begin() + i);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Force Terminate[Ignore any process protections](PatchGuard WARNING)"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    printf("Terminate Process = %d\n", Process_items[i].id);
//                                    KernelForceTerminateProcess(Process_items[i].id);
//                                    Process_items.erase(Process_items.begin() + i);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Terminate and Delete File"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    printf("Terminate Process = %d\n", Process_items[i].id);
//                                    KillProcessAndDeleteFile(Process_items[i].id);
//                                    Process_items.erase(Process_items.begin() + i);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Set System Critical Process"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    printf("Set Critical Process = %d\n", Process_items[i].id);
//                                    SetCriticalProcess(Process_items[i].id);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Suspend Process"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    printf("Suspend Process = %d\n", Process_items[i].id);
//                                    Suspend(Process_items[i].id);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Resume Process"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    printf("Resume Process = %d\n", Process_items[i].id);
//                                    Resume(Process_items[i].id);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Hide Process"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    printf("Hide Process = %d\n", Process_items[i].id);
//                                    HiddenProcess(Process_items[i].id, FALSE);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Force Hide Process(System Crash WARNING)"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    printf("Hide Process = %d\n", Process_items[i].id);
//                                    HiddenProcess(Process_items[i].id, TRUE);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("PP/PPL"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    attacttoeditprocesspid = Process_items[i].id;
//                                    PPLWindow = true;
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("InjectDll"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    attacttoeditprocesspid = Process_items[i].id;
//                                    ProcessInjectDllWindow = true;
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Modify Token"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    attacttoeditprocesspid = Process_items[i].id;
//                                    ModifyTokenWindow = true;
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("View Thread Info"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    ProcessThread_items.clear();
//                                    ProcessThread_items.shrink_to_fit();
//                                    attacttoeditprocesspid = Process_items[i].id;
//                                    EnumProcThread(attacttoeditprocesspid);
//                                    ProcessThreadInfoWindow = true;
//                                }
//                            }
//                        }
//                        /*
//                        if (ImGui::MenuItem("Setting"))
//                        {
//                            Process_SettingWindow = true;
//                        }
//                        */
//                        if (ImGui::MenuItem("Copy Process Name"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    copyToClipboard(Process_items[i].name);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Copy PID"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    char setclipboarddata[MAX_PATH];
//                                    sprintf_s(setclipboarddata, "%d", Process_items[i].id);
//                                    copyToClipboard(setclipboarddata);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Copy EPROCESS"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    char setclipboarddata[MAX_PATH];
//                                    sprintf_s(setclipboarddata, "%p", Process_items[i].eprocess);
//                                    copyToClipboard(setclipboarddata);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Copy Process Path"))
//                        {
//                            for (size_t i = 0; i < Process_items.size(); ++i)
//                            {
//                                if (Process_items[i].selected == true)
//                                {
//                                    copyToClipboard(Process_items[i].Path);
//                                }
//                            }
//                        }
//                        ImGui::EndPopup();
//                    }
//                    ImGui::EndTabItem();
//                }
//
//                if (ImGui::BeginTabItem("Kernel-Module"))
//                {
//                    NoExit = 0;
//                    NoEnableHvm = 0;
//                    NoEnableMonitor = 0;
//
//                    if (ImGui::BeginTable("Driver_table", 5, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                    {
//                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.25f);
//                        ImGui::TableSetupColumn("ImageBase", ImGuiTableColumnFlags_WidthFixed);
//                        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed);
//                        ImGui::TableSetupColumn("DriverObject", ImGuiTableColumnFlags_WidthFixed);
//                        ImGui::TableSetupColumn("Path");
//                        ImGui::TableHeadersRow();
//                        for (size_t i = 0; i < Driver_items.size(); ++i) {
//                            ImGui::PushID(static_cast<int>(i));
//                            ImGui::TableNextRow();
//                            //ImGui::TableNextColumn();
//                            ImGui::TableSetColumnIndex(0);
//                            if (ImGui::Selectable(Driver_items[i].name.c_str(), Driver_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                            {
//                                for (auto& item : Driver_items) item.selected = false;
//                                Driver_items[i].selected = true;
//                            }
//                            //ImGui::TableNextColumn();
//                            ImGui::TableSetColumnIndex(1);
//                            ImGui::Text("%p", Driver_items[i].Base);
//                            //ImGui::TableNextColumn();
//                            ImGui::TableSetColumnIndex(2);
//                            ImGui::Text("0x%x", Driver_items[i].Size);
//                            //ImGui::TableNextColumn();
//                            ImGui::TableSetColumnIndex(3);
//                            ImGui::Text("%p", Driver_items[i].DriverObject);
//                            //ImGui::TableNextColumn();
//                            ImGui::TableSetColumnIndex(4);
//                            ImGui::Text(Driver_items[i].path.c_str());
//                            ImGui::PopID();
//                        }
//                    }
//                    ImGui::EndTable();
//
//                    if (ImGui::BeginPopupContextItem("Driver_Menu"))
//                    {
//                        if (ImGui::MenuItem("Refesh"))
//                        {
//                            Driver_items.clear();
//                            Driver_items.shrink_to_fit();
//                            EnumDrivers();
//                        }
//                        if (ImGui::MenuItem("Unload Driver"))
//                        {
//                            for (size_t i = 0; i < Driver_items.size(); ++i)
//                            {
//                                if (Driver_items[i].selected == true)
//                                {
//                                    printf("Unload = %p\n", Driver_items[i].DriverObject);
//                                    UnloadDriver(Driver_items[i].DriverObject);
//                                    Driver_items.erase(Driver_items.begin() + i);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Hidden Driver"))
//                        {
//                            for (size_t i = 0; i < Driver_items.size(); ++i)
//                            {
//                                if (Driver_items[i].selected == true)
//                                {
//                                    printf("Hidden = %p\n", Driver_items[i].DriverObject);
//                                    HiddenDriver(Driver_items[i].DriverObject);
//                                    Driver_items[i].DriverObject = NULL;
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Force Hidden Driver"))
//                        {
//                            for (size_t i = 0; i < Driver_items.size(); ++i)
//                            {
//                                if (Driver_items[i].selected == true)
//                                {
//                                    printf("Hidden = %p\n", Driver_items[i].DriverObject);
//                                    ForceHiddenDriver(Driver_items[i].DriverObject);
//                                    Driver_items.erase(Driver_items.begin() + i);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Modify Image Base"))
//                        {
//                            for (size_t i = 0; i < Driver_items.size(); ++i)
//                            {
//                                if (Driver_items[i].selected == true)
//                                {
//                                    attacttoeditdriverbase = (ULONG64)Driver_items[i].Base;
//                                    DriverModifyBaseWindow = true;
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("View Major Functions"))
//                        {
//                            for (size_t i = 0; i < Driver_items.size(); ++i)
//                            {
//                                if (Driver_items[i].selected == true)
//                                {
//                                    if (Driver_items[i].DriverObject != NULL)
//                                    {
//                                        DriverMajorInfo_items.clear();
//                                        DriverMajorInfo_items.shrink_to_fit();
//                                        EnumDriverMajorFunctions(Driver_items[i].DriverObject);
//                                        DriverMajorInfoWindow = true;
//                                    }
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("View Memory"))
//                        {
//                            for (size_t i = 0; i < Driver_items.size(); ++i)
//                            {
//                                if (Driver_items[i].selected == true)
//                                {
//                                    sprintf_s(editmemoryaddress, "%p", Driver_items[i].Base);
//                                    MemoryWindow = true;
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Copy Driver Name"))
//                        {
//                            for (size_t i = 0; i < Driver_items.size(); ++i)
//                            {
//                                if (Driver_items[i].selected == true)
//                                {
//                                    copyToClipboard(Driver_items[i].name);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Copy Image Base"))
//                        {
//                            for (size_t i = 0; i < Driver_items.size(); ++i)
//                            {
//                                if (Driver_items[i].selected == true)
//                                {
//                                    char setclipboarddata[MAX_PATH];
//                                    sprintf_s(setclipboarddata, "%p", Driver_items[i].Base);
//                                    copyToClipboard(setclipboarddata);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Copy Driver Object"))
//                        {
//                            for (size_t i = 0; i < Driver_items.size(); ++i)
//                            {
//                                if (Driver_items[i].selected == true)
//                                {
//                                    char setclipboarddata[MAX_PATH];
//                                    sprintf_s(setclipboarddata, "%p", Driver_items[i].DriverObject);
//                                    copyToClipboard(setclipboarddata);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Copy Driver Path"))
//                        {
//                            for (size_t i = 0; i < Driver_items.size(); ++i)
//                            {
//                                if (Driver_items[i].selected == true)
//                                {
//                                    copyToClipboard(Driver_items[i].path);
//                                }
//                            }
//                        }
//                        ImGui::EndPopup();
//                    }
//                    ImGui::EndTabItem();
//                }
//
//                if (ImGui::BeginTabItem("Kernel"))
//                {
//                    NoExit = 0;
//                    NoEnableHvm = 0;
//                    NoEnableMonitor = 0;
//                    if (ImGui::BeginTabBar("KernelTab"))
//                    {
//                        if (ImGui::BeginTabItem("Notify/Callbacks"))
//                        {
//                            isShadowSSDTRefeshed = FALSE;
//                            isHalRefeshed = FALSE;
//                            if (ImGui::BeginTable("Notify_table", 4, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                ImGui::TableSetupColumn("TYPE", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("ENTRY", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Handle", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Module");
//                                ImGui::TableHeadersRow();
//                                for (size_t i = 0; i < Notify_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(0);
//                                    ImGui::Text(Notify_items[i].Type.c_str());
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(1);
//                                    ImGui::Text("%p", Notify_items[i].Address);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(2);
//                                    ImGui::Text("%p", Notify_items[i].Handle);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(3);
//                                    if (ImGui::Selectable(Notify_items[i].Module.c_str(), Notify_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : Notify_items) item.selected = false;
//                                        Notify_items[i].selected = true;
//                                    }
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("Notify_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    Notify_items.clear();
//                                    Notify_items.shrink_to_fit();
//                                    EnumCreateProcessNotify();
//                                    EnumCreateThreadNotify();
//                                    EnumLoadImage();
//                                    EnumRegistry();
//                                    EnumBugCheckCallback();
//                                    EnumBugCheckReasonCallback();
//                                    EnumShutdownNotify();
//                                    EnumLastShutdownNotify();
//                                    EnumFsNotify();
//                                    EnumPowerSettingNotify();
//                                    EnumPlugPlayNotify();
//                                    EnumCoalescingNotify();
//                                    EnumPrioriryNotify();
//                                    EnumDbgPrintCallback();
//                                    EnumEmpCallback();
//                                    EnumNmiCallbacks();
//                                    EnumObCallback();
//                                    EnumObCallback_Thread();
//                                    EnumObCallback_Desktop();
//                                }
//                                if (ImGui::MenuItem("Remove"))
//                                {
//                                    for (size_t i = 0; i < Notify_items.size(); ++i)
//                                    {
//                                        if (Notify_items[i].selected == true)
//                                        {
//                                            if (Notify_items[i].NotifyTypes == Notify_Priority || Notify_items[i].NotifyTypes == Notify_Coalescing || Notify_items[i].NotifyTypes == Notify_NmiCallback || Notify_items[i].NotifyTypes == Notify_BugCheck || Notify_items[i].NotifyTypes == Notify_BugCheckReason || Notify_items[i].NotifyTypes == Notify_Shutdown || Notify_items[i].NotifyTypes == Notify_LoastShutdown || Notify_items[i].NotifyTypes == Notify_PlugPlay)
//                                            {
//                                                RemoveKernelNotify(Notify_items[i].Handle, Notify_items[i].Address, Notify_items[i].NotifyTypes);
//                                            }
//                                            else
//                                            {
//                                                RemoveKernelNotify(Notify_items[i].Address, Notify_items[i].Handle, Notify_items[i].NotifyTypes);
//                                            }
//                                            Notify_items.erase(Notify_items.begin() + i);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory"))
//                                {
//                                    for (size_t i = 0; i < Notify_items.size(); ++i)
//                                    {
//                                        if (Notify_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", Notify_items[i].Address);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory"))
//                                {
//                                    for (size_t i = 0; i < Notify_items.size(); ++i)
//                                    {
//                                        if (Notify_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", Notify_items[i].Address);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Notify Type"))
//                                {
//                                    for (size_t i = 0; i < Notify_items.size(); ++i)
//                                    {
//                                        if (Notify_items[i].selected == true)
//                                        {
//                                            copyToClipboard(Notify_items[i].Type);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Entry Address"))
//                                {
//                                    for (size_t i = 0; i < Notify_items.size(); ++i)
//                                    {
//                                        if (Notify_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", Notify_items[i].Address);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Handle"))
//                                {
//                                    for (size_t i = 0; i < Notify_items.size(); ++i)
//                                    {
//                                        if (Notify_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", Notify_items[i].Handle);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Notify Module"))
//                                {
//                                    for (size_t i = 0; i < Notify_items.size(); ++i)
//                                    {
//                                        if (Notify_items[i].selected == true)
//                                        {
//                                            copyToClipboard(Notify_items[i].Module);
//                                        }
//                                    }
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("MiniFilter"))
//                        {
//                            isShadowSSDTRefeshed = FALSE;
//                            isHalRefeshed = FALSE;
//                            if (ImGui::BeginTable("MiniFilter_table", 5, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                ImGui::TableSetupColumn("Filter", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("IRP", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("PreFilter", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("PostFilter", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Module");
//                                ImGui::TableHeadersRow();
//                                for (size_t i = 0; i < MiniFilter_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(0);
//                                    ImGui::Text("%p", MiniFilter_items[i].Filter);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(1);
//                                    ImGui::Text(MiniFilter_items[i].IRP.c_str());
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(2);
//                                    ImGui::Text("%p", MiniFilter_items[i].PreFunc);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(3);
//                                    ImGui::Text("%p", MiniFilter_items[i].PostFunc);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(4);
//                                    if (ImGui::Selectable(MiniFilter_items[i].Module.c_str(), MiniFilter_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : MiniFilter_items) item.selected = false;
//                                        MiniFilter_items[i].selected = true;
//                                    }
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("MiniFilter_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    MiniFilter_items.clear();
//                                    MiniFilter_items.shrink_to_fit();
//                                    EnumMiniFilter();
//                                }
//                                if (ImGui::MenuItem("Remove"))
//                                {
//                                    for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//                                    {
//                                        if (MiniFilter_items[i].selected == true)
//                                        {
//                                            RemoveMiniFilter(MiniFilter_items[i].Filter);
//                                            MiniFilter_items.clear();
//                                            MiniFilter_items.shrink_to_fit();
//                                            EnumMiniFilter();
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory(Pre)"))
//                                {
//                                    for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//                                    {
//                                        if (MiniFilter_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", MiniFilter_items[i].PreFunc);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory(Post)"))
//                                {
//                                    for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//                                    {
//                                        if (MiniFilter_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", MiniFilter_items[i].PostFunc);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory(Pre)"))
//                                {
//                                    for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//                                    {
//                                        if (MiniFilter_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", MiniFilter_items[i].PreFunc);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory(Post)"))
//                                {
//                                    for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//                                    {
//                                        if (MiniFilter_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", MiniFilter_items[i].PostFunc);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Filter Handle"))
//                                {
//                                    for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//                                    {
//                                        if (MiniFilter_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", MiniFilter_items[i].Filter);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy IRP Function"))
//                                {
//                                    for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//                                    {
//                                        if (MiniFilter_items[i].selected == true)
//                                        {
//                                            copyToClipboard(MiniFilter_items[i].IRP);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Entry Address(Pre)"))
//                                {
//                                    for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//                                    {
//                                        if (MiniFilter_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", MiniFilter_items[i].PreFunc);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Entry Address(Post)"))
//                                {
//                                    for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//                                    {
//                                        if (MiniFilter_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", MiniFilter_items[i].PostFunc);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Module"))
//                                {
//                                    for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//                                    {
//                                        if (MiniFilter_items[i].selected == true)
//                                        {
//                                            copyToClipboard(MiniFilter_items[i].Module);
//                                        }
//                                    }
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("SSDT"))
//                        {
//                            isShadowSSDTRefeshed = FALSE;
//                            isHalRefeshed = FALSE;
//                            if (ImGui::BeginTable("SSDT_table", 6, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("FunctionName", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Current Address", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Hook", ImGuiTableColumnFlags_WidthStretch, 0.2f);
//                                ImGui::TableSetupColumn("Original Address", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Module");
//                                ImGui::TableHeadersRow();
//                                for (size_t i = 0; i < SSDT_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(0);
//                                    ImGui::Text("%d", SSDT_items[i].Index);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(1);
//                                    ImGui::Text(SSDT_items[i].Name.c_str());
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(2);
//                                    ImGui::Text("%p", SSDT_items[i].Address);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(3);
//                                    ImGui::Text(SSDT_items[i].Hook.c_str());
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(4);
//                                    ImGui::Text("%p", SSDT_items[i].OriginalAddress);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(5);
//                                    if (ImGui::Selectable(SSDT_items[i].Module.c_str(), SSDT_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : SSDT_items) item.selected = false;
//                                        SSDT_items[i].selected = true;
//                                    }
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("SSDT_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    SSDT_items.clear();
//                                    SSDT_items.shrink_to_fit();
//                                    EnumSSDT();
//                                }
//                                if (ImGui::MenuItem("UnHook"))
//                                {
//                                    for (size_t i = 0; i < SSDT_items.size(); ++i)
//                                    {
//                                        if (SSDT_items[i].selected == true)
//                                        {
//                                            if (SSDT_items[i].OriginalAddress != NULL)
//                                            {
//                                                if (SSDT_items[i].isInlineHook == TRUE)
//                                                {
//                                                    SSDT_UnHook(SSDT_items[i].Index, NULL, SSDT_items[i].isInlineHook);
//                                                }
//                                                else
//                                                {
//                                                    if (SSDT_items[i].Address != SSDT_items[i].OriginalAddress)
//                                                    {
//                                                        SSDT_UnHook(SSDT_items[i].Index, SSDT_items[i].OriginalAddress, SSDT_items[i].isInlineHook);
//                                                    }
//                                                }
//                                                SSDT_items.clear();
//                                                SSDT_items.shrink_to_fit();
//                                                EnumSSDT();
//                                            }
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("UnHook all"))
//                                {
//                                    SSDT_UnHookAll();
//                                    SSDT_items.clear();
//                                    SSDT_items.shrink_to_fit();
//                                    EnumSSDT();
//                                }
//                                if (ImGui::MenuItem("View Memory(Current)"))
//                                {
//                                    for (size_t i = 0; i < SSDT_items.size(); ++i)
//                                    {
//                                        if (SSDT_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", SSDT_items[i].Address);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory(Original)"))
//                                {
//                                    for (size_t i = 0; i < SSDT_items.size(); ++i)
//                                    {
//                                        if (SSDT_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", SSDT_items[i].OriginalAddress);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory(Current)"))
//                                {
//                                    for (size_t i = 0; i < SSDT_items.size(); ++i)
//                                    {
//                                        if (SSDT_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", SSDT_items[i].Address);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory(Original)"))
//                                {
//                                    for (size_t i = 0; i < SSDT_items.size(); ++i)
//                                    {
//                                        if (SSDT_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", SSDT_items[i].OriginalAddress);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (SSDT_items[0].OriginalAddress == NULL)
//                                {
//                                    if (ImGui::MenuItem("Load Symbol To Get Original Address(Very slow!)"))
//                                    {
//                                        int result = MessageBoxW(NULL, L"Are you sure you want to enable symbols, it's very slow, and if you're not connected to the network, the system will crash?", L"SYMBOL", MB_YESNO | MB_ICONQUESTION);
//
//                                        if (result == IDYES)
//                                        {
//                                            SSDT_items.clear();
//                                            SSDT_items.shrink_to_fit();
//                                            DeviceIoControl(hDevice, IOCTL_ENABLE_SYMBOL, NULL, 0, NULL, 0, NULL, NULL);
//                                            EnumSSDT();
//                                        }
//                                    }
//                                    if (ImGui::MenuItem("Unload Symbol"))
//                                    {
//                                        SSDT_items.clear();
//                                        SSDT_items.shrink_to_fit();
//                                        DeviceIoControl(hDevice, IOCTL_DISABLE_SYMBOL, NULL, 0, NULL, 0, NULL, NULL);
//                                        EnumSSDT();
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Function Name"))
//                                {
//                                    for (size_t i = 0; i < SSDT_items.size(); ++i)
//                                    {
//                                        if (SSDT_items[i].selected == true)
//                                        {
//                                            copyToClipboard(SSDT_items[i].Name);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Index"))
//                                {
//                                    for (size_t i = 0; i < SSDT_items.size(); ++i)
//                                    {
//                                        if (SSDT_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%d", SSDT_items[i].Index);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Current Address"))
//                                {
//                                    for (size_t i = 0; i < SSDT_items.size(); ++i)
//                                    {
//                                        if (SSDT_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", SSDT_items[i].Address);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Original Address"))
//                                {
//                                    for (size_t i = 0; i < SSDT_items.size(); ++i)
//                                    {
//                                        if (SSDT_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", SSDT_items[i].OriginalAddress);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Hook Type"))
//                                {
//                                    for (size_t i = 0; i < SSDT_items.size(); ++i)
//                                    {
//                                        if (SSDT_items[i].selected == true)
//                                        {
//                                            copyToClipboard(SSDT_items[i].Hook);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Module"))
//                                {
//                                    for (size_t i = 0; i < SSDT_items.size(); ++i)
//                                    {
//                                        if (SSDT_items[i].selected == true)
//                                        {
//                                            copyToClipboard(SSDT_items[i].Module);
//                                        }
//                                    }
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("Shadow SSDT"))
//                        {
//                            isHalRefeshed = FALSE;
//                            if (isShadowSSDTRefeshed == FALSE)
//                            {
//                                ShadowSSDT_items.clear();
//                                ShadowSSDT_items.shrink_to_fit();
//                                EnumSSSDT_NoThread();
//                                isShadowSSDTRefeshed = TRUE;
//                            }
//                            if (ImGui::BeginTable("SSSDT_table", 6, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("FunctionName", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Current Address", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Hook", ImGuiTableColumnFlags_WidthStretch, 0.2f);
//                                ImGui::TableSetupColumn("Original Address", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Module");
//                                ImGui::TableHeadersRow();
//                                for (size_t i = 0; i < ShadowSSDT_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(0);
//                                    ImGui::Text("%d", ShadowSSDT_items[i].Index);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(1);
//                                    ImGui::Text(ShadowSSDT_items[i].Name.c_str());
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(2);
//                                    ImGui::Text("%p", ShadowSSDT_items[i].Address);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(3);
//                                    ImGui::Text(ShadowSSDT_items[i].Hook.c_str());
//                                    ImGui::TableSetColumnIndex(4);
//                                    ImGui::Text("%p", ShadowSSDT_items[i].OriginalAddress);
//                                    ImGui::TableSetColumnIndex(5);
//                                    if (ImGui::Selectable(ShadowSSDT_items[i].Module.c_str(), ShadowSSDT_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : ShadowSSDT_items) item.selected = false;
//                                        ShadowSSDT_items[i].selected = true;
//                                    }
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("SSSDT_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    ShadowSSDT_items.clear();
//                                    ShadowSSDT_items.shrink_to_fit();
//                                    EnumSSSDT();
//                                }
//                                if (ImGui::MenuItem("UnHook"))
//                                {
//                                    for (size_t i = 0; i < ShadowSSDT_items.size(); ++i)
//                                    {
//                                        if (ShadowSSDT_items[i].selected == true)
//                                        {
//                                            SSSDT_UnHook(ShadowSSDT_items[i].Index, (ULONG64)ShadowSSDT_items[i].OriginalAddress, ShadowSSDT_items[i].IsInlineHook);
//                                            ShadowSSDT_items.clear();
//                                            ShadowSSDT_items.shrink_to_fit();
//                                            EnumSSSDT_NoThread();
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory(Current)"))
//                                {
//                                    for (size_t i = 0; i < ShadowSSDT_items.size(); ++i)
//                                    {
//                                        if (ShadowSSDT_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", ShadowSSDT_items[i].Address);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory(Original)"))
//                                {
//                                    for (size_t i = 0; i < ShadowSSDT_items.size(); ++i)
//                                    {
//                                        if (ShadowSSDT_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", ShadowSSDT_items[i].OriginalAddress);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory(Current)"))
//                                {
//                                    for (size_t i = 0; i < ShadowSSDT_items.size(); ++i)
//                                    {
//                                        if (ShadowSSDT_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", ShadowSSDT_items[i].Address);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory(Original)"))
//                                {
//                                    for (size_t i = 0; i < ShadowSSDT_items.size(); ++i)
//                                    {
//                                        if (ShadowSSDT_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", ShadowSSDT_items[i].OriginalAddress);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Function Name"))
//                                {
//                                    for (size_t i = 0; i < ShadowSSDT_items.size(); ++i)
//                                    {
//                                        if (ShadowSSDT_items[i].selected == true)
//                                        {
//                                            copyToClipboard(ShadowSSDT_items[i].Name);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Index"))
//                                {
//                                    for (size_t i = 0; i < ShadowSSDT_items.size(); ++i)
//                                    {
//                                        if (ShadowSSDT_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%d", ShadowSSDT_items[i].Index);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Address"))
//                                {
//                                    for (size_t i = 0; i < ShadowSSDT_items.size(); ++i)
//                                    {
//                                        if (ShadowSSDT_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", ShadowSSDT_items[i].Address);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Module"))
//                                {
//                                    for (size_t i = 0; i < ShadowSSDT_items.size(); ++i)
//                                    {
//                                        if (ShadowSSDT_items[i].selected == true)
//                                        {
//                                            copyToClipboard(ShadowSSDT_items[i].Module);
//                                        }
//                                    }
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("IoTimer"))
//                        {
//                            isShadowSSDTRefeshed = FALSE;
//                            isHalRefeshed = FALSE;
//                            if (ImGui::BeginTable("IoTimer_table", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Module");
//                                ImGui::TableHeadersRow();
//                                for (size_t i = 0; i < IoTimer_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(0);
//                                    ImGui::Text("%p", IoTimer_items[i].Address);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(1);
//                                    ImGui::Text("%p", IoTimer_items[i].Object);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(2);
//                                    if (ImGui::Selectable(IoTimer_items[i].Module.c_str(), IoTimer_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : IoTimer_items) item.selected = false;
//                                        IoTimer_items[i].selected = true;
//                                    }
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("IoTimer_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    IoTimer_items.clear();
//                                    IoTimer_items.shrink_to_fit();
//                                    EnumIoTimer();
//                                }
//                                if (ImGui::MenuItem("View Memory"))
//                                {
//                                    for (size_t i = 0; i < IoTimer_items.size(); ++i)
//                                    {
//                                        if (IoTimer_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", IoTimer_items[i].Address);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory"))
//                                {
//                                    for (size_t i = 0; i < IoTimer_items.size(); ++i)
//                                    {
//                                        if (IoTimer_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", IoTimer_items[i].Address);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Address"))
//                                {
//                                    for (size_t i = 0; i < IoTimer_items.size(); ++i)
//                                    {
//                                        if (IoTimer_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", IoTimer_items[i].Address);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Object"))
//                                {
//                                    for (size_t i = 0; i < IoTimer_items.size(); ++i)
//                                    {
//                                        if (IoTimer_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", IoTimer_items[i].Object);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Module"))
//                                {
//                                    for (size_t i = 0; i < IoTimer_items.size(); ++i)
//                                    {
//                                        if (IoTimer_items[i].selected == true)
//                                        {
//                                            copyToClipboard(IoTimer_items[i].Module);
//                                        }
//                                    }
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("Object-Type"))
//                        {
//                            isShadowSSDTRefeshed = FALSE;
//                            isHalRefeshed = FALSE;
//                            if (ImGui::BeginTable("ObjectType_table", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                ImGui::TableSetupColumn("Object-Type", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Module");
//                                ImGui::TableHeadersRow();
//                                for (size_t i = 0; i < ObjectTypeTable_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(0);
//                                    ImGui::Text(ObjectTypeTable_items[i].Type.c_str());
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(1);
//                                    ImGui::Text("%p", ObjectTypeTable_items[i].Address);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(2);
//                                    if (ImGui::Selectable(ObjectTypeTable_items[i].Module.c_str(), ObjectTypeTable_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : ObjectTypeTable_items) item.selected = false;
//                                        ObjectTypeTable_items[i].selected = true;
//                                    }
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("ObjectTable_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    ObjectTypeTable_items.clear();
//                                    ObjectTypeTable_items.shrink_to_fit();
//                                    EnumObjectTypeTable();
//                                }
//                                if (ImGui::MenuItem("Copy ObjectType Name"))
//                                {
//                                    for (size_t i = 0; i < ObjectTypeTable_items.size(); ++i)
//                                    {
//                                        if (ObjectTypeTable_items[i].selected == true)
//                                        {
//                                            copyToClipboard(ObjectTypeTable_items[i].Type);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Object Address"))
//                                {
//                                    for (size_t i = 0; i < ObjectTypeTable_items.size(); ++i)
//                                    {
//                                        if (ObjectTypeTable_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", ObjectTypeTable_items[i].Address);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Module"))
//                                {
//                                    for (size_t i = 0; i < ObjectTypeTable_items.size(); ++i)
//                                    {
//                                        if (ObjectTypeTable_items[i].selected == true)
//                                        {
//                                            copyToClipboard(ObjectTypeTable_items[i].Module);
//                                        }
//                                    }
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("SystemThread"))
//                        {
//                            isShadowSSDTRefeshed = FALSE;
//                            isHalRefeshed = FALSE;
//                            if (ImGui::BeginTable("SystemThread_table", 4, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                ImGui::TableSetupColumn("TID", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("PETHREAD", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("StartAddress", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Module");
//                                ImGui::TableHeadersRow();
//                                for (size_t i = 0; i < SystemThread_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(0);
//                                    ImGui::Text("%d", SystemThread_items[i].TID);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(1);
//                                    ImGui::Text("%p", SystemThread_items[i].ethread);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(2);
//                                    ImGui::Text("%p", SystemThread_items[i].Address);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(3);
//                                    if (ImGui::Selectable(SystemThread_items[i].Module.c_str(), SystemThread_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : SystemThread_items) item.selected = false;
//                                        SystemThread_items[i].selected = true;
//                                    }
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("SystemThread_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    SystemThread_items.clear();
//                                    SystemThread_items.shrink_to_fit();
//                                    EnumSystemThread();
//                                }
//                                if (ImGui::MenuItem("Terminate"))
//                                {
//                                    for (size_t i = 0; i < SystemThread_items.size(); ++i)
//                                    {
//                                        if (SystemThread_items[i].selected == true)
//                                        {
//                                            Kernel_TerminateThread(SystemThread_items[i].TID);
//                                            SystemThread_items.erase(SystemThread_items.begin() + i);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Force Terminate"))
//                                {
//                                    for (size_t i = 0; i < SystemThread_items.size(); ++i)
//                                    {
//                                        if (SystemThread_items[i].selected == true)
//                                        {
//                                            Kernel_Force_TerminateThread(SystemThread_items[i].TID);
//                                            SystemThread_items.erase(SystemThread_items.begin() + i);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Suspend Thread"))
//                                {
//                                    for (size_t i = 0; i < SystemThread_items.size(); ++i)
//                                    {
//                                        if (SystemThread_items[i].selected == true)
//                                        {
//                                            K_SuspendThread(SystemThread_items[i].TID);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Resume Thread"))
//                                {
//                                    for (size_t i = 0; i < SystemThread_items.size(); ++i)
//                                    {
//                                        if (SystemThread_items[i].selected == true)
//                                        {
//                                            K_ResumeThread(SystemThread_items[i].TID);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory"))
//                                {
//                                    for (size_t i = 0; i < SystemThread_items.size(); ++i)
//                                    {
//                                        if (SystemThread_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", SystemThread_items[i].Address);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory"))
//                                {
//                                    for (size_t i = 0; i < SystemThread_items.size(); ++i)
//                                    {
//                                        if (SystemThread_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", SystemThread_items[i].Address);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy TID"))
//                                {
//                                    for (size_t i = 0; i < SystemThread_items.size(); ++i)
//                                    {
//                                        if (SystemThread_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%d", SystemThread_items[i].TID);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy ETHREAD"))
//                                {
//                                    for (size_t i = 0; i < SystemThread_items.size(); ++i)
//                                    {
//                                        if (SystemThread_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", SystemThread_items[i].ethread);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Address"))
//                                {
//                                    for (size_t i = 0; i < SystemThread_items.size(); ++i)
//                                    {
//                                        if (SystemThread_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", SystemThread_items[i].Address);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Module"))
//                                {
//                                    for (size_t i = 0; i < SystemThread_items.size(); ++i)
//                                    {
//                                        if (SystemThread_items[i].selected == true)
//                                        {
//                                            copyToClipboard(SystemThread_items[i].Module);
//                                        }
//                                    }
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("ExCallback"))
//                        {
//                            isShadowSSDTRefeshed = FALSE;
//                            isHalRefeshed = FALSE;
//
//                            if (ImGui::BeginTable("ExCallback_table", 5, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                ImGui::TableSetupColumn("ENTRY", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Handle", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Name");
//                                ImGui::TableSetupColumn("Module");
//                                ImGui::TableHeadersRow();
//                                for (size_t i = 0; i < ExCallback_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(0);
//                                    ImGui::Text("%p", ExCallback_items[i].Entry);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(1);
//                                    ImGui::Text("%p", ExCallback_items[i].Object);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(2);
//                                    ImGui::Text("%p", ExCallback_items[i].Handle);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(3);
//                                    ImGui::Text(ExCallback_items[i].Name.c_str());
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(4);
//                                    if (ImGui::Selectable(ExCallback_items[i].Module.c_str(), ExCallback_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : ExCallback_items) item.selected = false;
//                                        ExCallback_items[i].selected = true;
//                                    }
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("ExCallBack_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    ExCallback_items.clear();
//                                    ExCallback_items.shrink_to_fit();
//                                    EnumExCallback();
//                                }
//                                if (ImGui::MenuItem("Remove"))
//                                {
//                                    for (size_t i = 0; i < ExCallback_items.size(); ++i)
//                                    {
//                                        if (ExCallback_items[i].selected == true)
//                                        {
//                                            RemoveExCallback(ExCallback_items[i].Handle);
//                                            ExCallback_items.clear();
//                                            ExCallback_items.shrink_to_fit();
//                                            EnumExCallback();
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory"))
//                                {
//                                    for (size_t i = 0; i < ExCallback_items.size(); ++i)
//                                    {
//                                        if (ExCallback_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", ExCallback_items[i].Entry);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory"))
//                                {
//                                    for (size_t i = 0; i < ExCallback_items.size(); ++i)
//                                    {
//                                        if (ExCallback_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", ExCallback_items[i].Entry);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Address"))
//                                {
//                                    for (size_t i = 0; i < ExCallback_items.size(); ++i)
//                                    {
//                                        if (ExCallback_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", ExCallback_items[i].Entry);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Object"))
//                                {
//                                    for (size_t i = 0; i < ExCallback_items.size(); ++i)
//                                    {
//                                        if (ExCallback_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", ExCallback_items[i].Object);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Handle"))
//                                {
//                                    for (size_t i = 0; i < ExCallback_items.size(); ++i)
//                                    {
//                                        if (ExCallback_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", ExCallback_items[i].Handle);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Name"))
//                                {
//                                    for (size_t i = 0; i < ExCallback_items.size(); ++i)
//                                    {
//                                        if (ExCallback_items[i].selected == true)
//                                        {
//                                            copyToClipboard(ExCallback_items[i].Name);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Module"))
//                                {
//                                    for (size_t i = 0; i < ExCallback_items.size(); ++i)
//                                    {
//                                        if (ExCallback_items[i].selected == true)
//                                        {
//                                            copyToClipboard(ExCallback_items[i].Module);
//                                        }
//                                    }
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("IDTable"))
//                        {
//                            isShadowSSDTRefeshed = FALSE;
//                            isHalRefeshed = FALSE;
//
//                            if (ImGui::BeginTable("IDT_table", 4, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Module");
//                                ImGui::TableHeadersRow();
//                                for (size_t i = 0; i < IDT_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(0);
//                                    ImGui::Text("%d",IDT_items[i].Cpu);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(1);
//                                    ImGui::Text("%d", IDT_items[i].ID);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(2);
//                                    ImGui::Text("%p", IDT_items[i].Address);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(3);
//                                    if (ImGui::Selectable(IDT_items[i].Module.c_str(), IDT_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : IDT_items) item.selected = false;
//                                        IDT_items[i].selected = true;
//                                    }
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("IDT_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    IDT_items.clear();
//                                    IDT_items.shrink_to_fit();
//                                    EnumIDT();
//                                }
//                                if (ImGui::MenuItem("View Memory"))
//                                {
//                                    for (size_t i = 0; i < IDT_items.size(); ++i)
//                                    {
//                                        if (IDT_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", IDT_items[i].Address);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory"))
//                                {
//                                    for (size_t i = 0; i < IDT_items.size(); ++i)
//                                    {
//                                        if (IDT_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", IDT_items[i].Address);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy ID"))
//                                {
//                                    for (size_t i = 0; i < IDT_items.size(); ++i)
//                                    {
//                                        if (IDT_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%d", IDT_items[i].ID);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Address"))
//                                {
//                                    for (size_t i = 0; i < IDT_items.size(); ++i)
//                                    {
//                                        if (IDT_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", IDT_items[i].Address);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Module"))
//                                {
//                                    for (size_t i = 0; i < IDT_items.size(); ++i)
//                                    {
//                                        if (IDT_items[i].selected == true)
//                                        {
//                                            copyToClipboard(IDT_items[i].Module);
//                                        }
//                                    }
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("GDTable"))
//                        {
//                            isShadowSSDTRefeshed = FALSE;
//                            isHalRefeshed = FALSE;
//
//                            if (ImGui::BeginTable("GDT_table", 6, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Limit", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Dpi", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Flags");
//                                ImGui::TableHeadersRow();
//                                for (size_t i = 0; i < GDT_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(0);
//                                    ImGui::Text("%d", GDT_items[i].ID);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(1);
//                                    ImGui::Text("%p", GDT_items[i].Base);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(2);
//                                    ImGui::Text("%p", GDT_items[i].limit);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(3);
//                                    if (ImGui::Selectable(GDT_items[i].Name.c_str(), GDT_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : GDT_items) item.selected = false;
//                                        GDT_items[i].selected = true;
//                                    }
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(4);
//                                    ImGui::Text("%d", GDT_items[i].dpi);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(5);
//                                    ImGui::Text("%d", GDT_items[i].flags);
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("GDT_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    GDT_items.clear();
//                                    GDT_items.shrink_to_fit();
//                                    EnumGDT();
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("UnloadedDrivers"))
//                        {
//                            isShadowSSDTRefeshed = FALSE;
//                            isHalRefeshed = FALSE;
//                            if (ImGui::BeginTable("unloadeddrivers_table", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Base");
//                                ImGui::TableHeadersRow();
//                                for (size_t i = 0; i < unloadeddriver_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(0);
//                                    ImGui::Text("%d", unloadeddriver_items[i].ID);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(1);
//                                    if (ImGui::Selectable(unloadeddriver_items[i].Name.c_str(), unloadeddriver_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : unloadeddriver_items) item.selected = false;
//                                        unloadeddriver_items[i].selected = true;
//                                    }
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(2);
//                                    ImGui::Text("%p", unloadeddriver_items[i].Base);
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("unloadeddrivers_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    unloadeddriver_items.clear();
//                                    unloadeddriver_items.shrink_to_fit();
//                                    EnumUnloadedDrivers();
//                                }
//                                if (ImGui::MenuItem("Copy Address"))
//                                {
//                                    for (size_t i = 0; i < unloadeddriver_items.size(); ++i)
//                                    {
//                                        if (unloadeddriver_items[i].selected == true)
//                                        {
//                                            char setclipboarddata[MAX_PATH];
//                                            sprintf_s(setclipboarddata, "%p", unloadeddriver_items[i].Base);
//                                            copyToClipboard(setclipboarddata);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Module"))
//                                {
//                                    for (size_t i = 0; i < unloadeddriver_items.size(); ++i)
//                                    {
//                                        if (unloadeddriver_items[i].selected == true)
//                                        {
//                                            copyToClipboard(unloadeddriver_items[i].Name);
//                                        }
//                                    }
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("PiDDBCache"))
//                        {
//                            isShadowSSDTRefeshed = FALSE;
//                            isHalRefeshed = FALSE;
//                            ULONG piddbIndex = NULL;
//                            if (ImGui::BeginTable("PiDDBCache_table", 4, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Time");
//                                ImGui::TableHeadersRow();
//                                for (size_t i = 0; i < PiDDB_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(0);
//                                    ImGui::Text("%d", piddbIndex);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(1);
//                                    if (ImGui::Selectable(PiDDB_items[i].Name.c_str(), PiDDB_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : PiDDB_items) item.selected = false;
//                                        PiDDB_items[i].selected = true;
//                                    }
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(2);
//                                    ImGui::Text("0x%d", PiDDB_items[i].status);
//                                    ImGui::TableSetColumnIndex(3);
//                                    ImGui::Text("%d", PiDDB_items[i].Time);
//                                    ImGui::PopID();
//                                    piddbIndex++;
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("PiDDBCache_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    PiDDB_items.clear();
//                                    PiDDB_items.shrink_to_fit();
//                                    EnumPiDDBCacheTable();
//                                }
//                                if (ImGui::MenuItem("DELETE"))
//                                {
//                                    for (size_t i = 0; i < PiDDB_items.size(); ++i)
//                                    {
//                                        if (PiDDB_items[i].selected == true)
//                                        {
//                                            struct input {
//                                                ULONG Time;
//                                            };
//                                            input inputs = { 0 };
//                                            inputs.Time = PiDDB_items[i].Time;
//                                            BOOL status = DeviceIoControl(hDevice, IOCTL_REMOVE_PIDDBCACHE, &inputs, sizeof(input), 0, 0, 0, NULL);
//                                            if (!status) 
//                                            {
//                                                printf("Failed. Error %ld\n", GetLastError());
//                                            }
//                                            else
//                                            {
//                                                PiDDB_items.erase(PiDDB_items.begin() + i);
//                                            }
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Copy Name"))
//                                {
//                                    for (size_t i = 0; i < PiDDB_items.size(); ++i)
//                                    {
//                                        if (PiDDB_items[i].selected == true)
//                                        {
//                                            copyToClipboard(PiDDB_items[i].Name);
//                                        }
//                                    }
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("Hal"))
//                        {
//                            isShadowSSDTRefeshed = FALSE;
//                            if (isHalRefeshed == FALSE)
//                            {
//                                HalDispathTable_items.clear();
//                                HalDispathTable_items.shrink_to_fit();
//                                HalPrivateDispathTable_items.clear();
//                                HalPrivateDispathTable_items.shrink_to_fit();
//                                EnumHalDispathTable();
//                                EnumHalPrivateDispathTable();
//                                isHalRefeshed = TRUE;
//                            }
//                            if (ImGui::BeginTabBar("HalTable_Tab"))
//                            {
//                                if (ImGui::BeginTabItem("HalDispatchTable"))
//                                {
//                                    if (ImGui::BeginTable("HalDispathTable_table", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                                    {
//                                        ImGui::TableSetupColumn("FunctionName", ImGuiTableColumnFlags_WidthFixed);
//                                        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
//                                        ImGui::TableSetupColumn("Module");
//                                        ImGui::TableHeadersRow();
//                                        for (size_t i = 0; i < HalDispathTable_items.size(); ++i) {
//                                            ImGui::PushID(static_cast<int>(i));
//                                            ImGui::TableNextRow();
//                                            //ImGui::TableNextColumn();
//                                            ImGui::TableSetColumnIndex(0);
//                                            ImGui::Text(HalDispathTable_items[i].Name.c_str());
//                                            //ImGui::TableNextColumn();
//                                            ImGui::TableSetColumnIndex(1);
//                                            ImGui::Text("%p", HalDispathTable_items[i].Address);
//                                            //ImGui::TableNextColumn();
//                                            ImGui::TableSetColumnIndex(2);
//                                            if (ImGui::Selectable(HalDispathTable_items[i].Module.c_str(), HalDispathTable_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                            {
//                                                for (auto& item : HalDispathTable_items) item.selected = false;
//                                                HalDispathTable_items[i].selected = true;
//                                            }
//                                            ImGui::PopID();
//                                        }
//                                    }
//                                    ImGui::EndTable();
//
//                                    if (ImGui::BeginPopupContextItem("HalDispathTable_Menu"))
//                                    {
//                                        if (ImGui::MenuItem("Refesh"))
//                                        {
//                                            HalDispathTable_items.clear();
//                                            HalDispathTable_items.shrink_to_fit();
//                                            EnumHalDispathTable();
//                                        }
//                                        if (ImGui::MenuItem("View Memory"))
//                                        {
//                                            for (size_t i = 0; i < HalDispathTable_items.size(); ++i)
//                                            {
//                                                if (HalDispathTable_items[i].selected == true)
//                                                {
//                                                    sprintf_s(editmemoryaddress, "%p", HalDispathTable_items[i].Address);
//                                                    MemoryWindow = true;
//                                                }
//                                            }
//                                        }
//                                        if (ImGui::MenuItem("Disassembly Memory"))
//                                        {
//                                            for (size_t i = 0; i < HalDispathTable_items.size(); ++i)
//                                            {
//                                                if (HalDispathTable_items[i].selected == true)
//                                                {
//                                                    sprintf_s(editmemoryaddress, "%p", HalDispathTable_items[i].Address);
//                                                    ASMMemoryWindow = true;
//                                                }
//                                            }
//                                        }
//                                        if (ImGui::MenuItem("Copy Function Name"))
//                                        {
//                                            for (size_t i = 0; i < HalDispathTable_items.size(); ++i)
//                                            {
//                                                if (HalDispathTable_items[i].selected == true)
//                                                {
//                                                    copyToClipboard(HalDispathTable_items[i].Name);
//                                                }
//                                            }
//                                        }
//                                        if (ImGui::MenuItem("Copy Address"))
//                                        {
//                                            for (size_t i = 0; i < HalDispathTable_items.size(); ++i)
//                                            {
//                                                if (HalDispathTable_items[i].selected == true)
//                                                {
//                                                    char setclipboarddata[MAX_PATH];
//                                                    sprintf_s(setclipboarddata, "%p", HalDispathTable_items[i].Address);
//                                                    copyToClipboard(setclipboarddata);
//                                                }
//                                            }
//                                        }
//                                        if (ImGui::MenuItem("Copy Module"))
//                                        {
//                                            for (size_t i = 0; i < HalDispathTable_items.size(); ++i)
//                                            {
//                                                if (HalDispathTable_items[i].selected == true)
//                                                {
//                                                    copyToClipboard(HalDispathTable_items[i].Module);
//                                                }
//                                            }
//                                        }
//                                        ImGui::EndPopup();
//                                    }
//                                    ImGui::EndTabItem();
//                                }
//                                if (ImGui::BeginTabItem("HalPrivateDispatchTable"))
//                                {
//                                    if (ImGui::BeginTable("HalPrivateDispathTable_table", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                                    {
//                                        ImGui::TableSetupColumn("FunctionName", ImGuiTableColumnFlags_WidthFixed);
//                                        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
//                                        ImGui::TableSetupColumn("Module");
//                                        ImGui::TableHeadersRow();
//                                        for (size_t i = 0; i < HalPrivateDispathTable_items.size(); ++i) {
//                                            ImGui::PushID(static_cast<int>(i));
//                                            ImGui::TableNextRow();
//                                            //ImGui::TableNextColumn();
//                                            ImGui::TableSetColumnIndex(0);
//                                            ImGui::Text(HalPrivateDispathTable_items[i].Name.c_str());
//                                            //ImGui::TableNextColumn();
//                                            ImGui::TableSetColumnIndex(1);
//                                            ImGui::Text("%p", HalPrivateDispathTable_items[i].Address);
//                                            //ImGui::TableNextColumn();
//                                            ImGui::TableSetColumnIndex(2);
//                                            if (ImGui::Selectable(HalPrivateDispathTable_items[i].Module.c_str(), HalPrivateDispathTable_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                            {
//                                                for (auto& item : HalPrivateDispathTable_items) item.selected = false;
//                                                HalPrivateDispathTable_items[i].selected = true;
//                                            }
//                                            ImGui::PopID();
//                                        }
//                                    }
//                                    ImGui::EndTable();
//
//                                    if (ImGui::BeginPopupContextItem("HalPrivateDispathTable_Menu"))
//                                    {
//                                        if (ImGui::MenuItem("Refesh"))
//                                        {
//                                            HalPrivateDispathTable_items.clear();
//                                            HalPrivateDispathTable_items.shrink_to_fit();
//                                            EnumHalPrivateDispathTable();
//                                        }
//                                        if (ImGui::MenuItem("View Memory"))
//                                        {
//                                            for (size_t i = 0; i < HalPrivateDispathTable_items.size(); ++i)
//                                            {
//                                                if (HalPrivateDispathTable_items[i].selected == true)
//                                                {
//                                                    sprintf_s(editmemoryaddress, "%p", HalPrivateDispathTable_items[i].Address);
//                                                    MemoryWindow = true;
//                                                }
//                                            }
//                                        }
//                                        if (ImGui::MenuItem("Disassembly Memory"))
//                                        {
//                                            for (size_t i = 0; i < HalPrivateDispathTable_items.size(); ++i)
//                                            {
//                                                if (HalPrivateDispathTable_items[i].selected == true)
//                                                {
//                                                    sprintf_s(editmemoryaddress, "%p", HalPrivateDispathTable_items[i].Address);
//                                                    ASMMemoryWindow = true;
//                                                }
//                                            }
//                                        }
//                                        if (ImGui::MenuItem("Copy Function Name"))
//                                        {
//                                            for (size_t i = 0; i < HalPrivateDispathTable_items.size(); ++i)
//                                            {
//                                                if (HalPrivateDispathTable_items[i].selected == true)
//                                                {
//                                                    copyToClipboard(HalPrivateDispathTable_items[i].Name);
//                                                }
//                                            }
//                                        }
//                                        if (ImGui::MenuItem("Copy Address"))
//                                        {
//                                            for (size_t i = 0; i < HalPrivateDispathTable_items.size(); ++i)
//                                            {
//                                                if (HalPrivateDispathTable_items[i].selected == true)
//                                                {
//                                                    char setclipboarddata[MAX_PATH];
//                                                    sprintf_s(setclipboarddata, "%p", HalPrivateDispathTable_items[i].Address);
//                                                    copyToClipboard(setclipboarddata);
//                                                }
//                                            }
//                                        }
//                                        if (ImGui::MenuItem("Copy Module"))
//                                        {
//                                            for (size_t i = 0; i < HalPrivateDispathTable_items.size(); ++i)
//                                            {
//                                                if (HalPrivateDispathTable_items[i].selected == true)
//                                                {
//                                                    copyToClipboard(HalPrivateDispathTable_items[i].Module);
//                                                }
//                                            }
//                                        }
//                                        ImGui::EndPopup();
//                                    }
//                                    ImGui::EndTabItem();
//                                }
//                                ImGui::EndTabBar();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        ImGui::EndTabBar();
//                    }
//                    ImGui::EndTabItem();
//                }
//                if (ImGui::BeginTabItem("Kernel-Hooks"))
//                {
//                    NoExit = 0;
//                    NoEnableMonitor = 0;
//                    if (ImGui::BeginTabBar("HookTab"))
//                    {
//                        if (ImGui::BeginTabItem("MSR"))
//                        {
//                            if (ImGui::Button("Scan MSR Hook"))
//                            {
//                                Hook_MSR_items.clear();
//                                Hook_MSR_items.shrink_to_fit();
//                                ScanSysenterHook();
//                            }
//                            if (ImGui::BeginTable("Hook_MSR_table", 5, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                ImGui::TableSetupColumn("FunctionName", ImGuiTableColumnFlags_WidthStretch, 0.2f);
//                                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.18f);
//                                ImGui::TableSetupColumn("Original Address", ImGuiTableColumnFlags_WidthStretch, 0.18f);
//                                ImGui::TableSetupColumn("Hook-Type", ImGuiTableColumnFlags_WidthStretch, 0.15f);
//                                ImGui::TableSetupColumn("Module");
//                                ImGui::TableHeadersRow();
//                                for (size_t i = 0; i < Hook_MSR_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(0);
//                                    if (ImGui::Selectable(Hook_MSR_items[i].name.c_str(), Hook_MSR_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : Hook_MSR_items) item.selected = false;
//                                        Hook_MSR_items[i].selected = true;
//                                    }
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(1);
//                                    ImGui::Text("%p", Hook_MSR_items[i].CurrAddress);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(2);
//                                    ImGui::Text("%p", Hook_MSR_items[i].OriginalAddress);
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(3);
//                                    ImGui::Text(Hook_MSR_items[i].Hook.c_str());
//                                    //ImGui::TableNextColumn();
//                                    ImGui::TableSetColumnIndex(4);
//                                    ImGui::Text(Hook_MSR_items[i].Module.c_str());
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("MSRHook_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    Hook_MSR_items.clear();
//                                    Hook_MSR_items.shrink_to_fit();
//                                    ScanSysenterHook();
//                                }
//                                if (ImGui::MenuItem("UnHook"))
//                                {
//                                    for (size_t i = 0; i < Hook_MSR_items.size(); ++i)
//                                    {
//                                        if (Hook_MSR_items[i].selected == true)
//                                        {
//                                            struct input {
//                                                ULONG64 OriginalAddress;
//                                            };
//                                            input inputs = { 0 };
//
//                                           inputs.OriginalAddress = (ULONG64)Hook_MSR_items[i].OriginalAddress;
//                                           DeviceIoControl(hDevice, IOCTL_REMOVE_MSRHOOK, &inputs, sizeof(input), 0, 0, 0, NULL);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory(Current)"))
//                                {
//                                    for (size_t i = 0; i < Hook_MSR_items.size(); ++i)
//                                    {
//                                        if (Hook_MSR_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", Hook_MSR_items[i].CurrAddress);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory(Original)"))
//                                {
//                                    for (size_t i = 0; i < Hook_MSR_items.size(); ++i)
//                                    {
//                                        if (Hook_MSR_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", Hook_MSR_items[i].OriginalAddress);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory(Current)"))
//                                {
//                                    for (size_t i = 0; i < Hook_MSR_items.size(); ++i)
//                                    {
//                                        if (Hook_MSR_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", Hook_MSR_items[i].CurrAddress);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory(Original)"))
//                                {
//                                    for (size_t i = 0; i < Hook_MSR_items.size(); ++i)
//                                    {
//                                        if (Hook_MSR_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", Hook_MSR_items[i].OriginalAddress);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("Irp"))
//                        {
//                            if (ImGui::BeginTable("IrpHook_table", 7, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                ImGui::TableSetupColumn("DRIVER", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("IRP", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Current Address", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Hook", ImGuiTableColumnFlags_WidthStretch, 0.17f);
//                                ImGui::TableSetupColumn("Original Address", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("DriverObject", ImGuiTableColumnFlags_WidthFixed);
//                                ImGui::TableSetupColumn("Module");
//                                ImGui::TableHeadersRow();
//                                for (size_t i = 0; i < IrpHook_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    ImGui::TableSetColumnIndex(0);
//                                    if (ImGui::Selectable(IrpHook_items[i].Driver.c_str(), IrpHook_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : IrpHook_items) item.selected = false;
//                                        IrpHook_items[i].selected = true;
//                                    }
//                                    ImGui::TableSetColumnIndex(1);
//                                    ImGui::Text(IrpHook_items[i].IrpFunc.c_str());
//                                    ImGui::TableSetColumnIndex(2);
//                                    ImGui::Text("%p", IrpHook_items[i].Address);
//                                    ImGui::TableSetColumnIndex(3);
//                                    ImGui::Text(IrpHook_items[i].Hook.c_str());
//                                    ImGui::TableSetColumnIndex(4);
//                                    ImGui::Text("%p", IrpHook_items[i].OriginalAddress);
//                                    ImGui::TableSetColumnIndex(5);
//                                    ImGui::Text("%p", IrpHook_items[i].DriverObject);
//                                    ImGui::TableSetColumnIndex(6);
//                                    ImGui::Text(IrpHook_items[i].Module.c_str());
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("Irp_Hook_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    IrpHook_items.clear();
//                                    IrpHook_items.shrink_to_fit();
//                                    EnumIrpHook();
//                                }
//                                if (ImGui::MenuItem("UnHook"))
//                                {
//                                    for (size_t i = 0; i < IrpHook_items.size(); ++i)
//                                    {
//                                        if (IrpHook_items[i].selected == true)
//                                        {
//                                            struct input {
//                                                ULONG IrpType;
//                                                PVOID OriginalAddress;
//                                                PVOID DriverObject;
//                                            };
//                                            input inputs = { 0 };
//
//                                            inputs.OriginalAddress = IrpHook_items[i].OriginalAddress;
//                                            inputs.IrpType = IrpHook_items[i].irpfunccode;
//                                            inputs.DriverObject = IrpHook_items[i].DriverObject;
//                                            DeviceIoControl(hDevice, IOCTL_REMOVE_IRPHOOK, &inputs, sizeof(input), 0, 0, 0, NULL);
//                                            IrpHook_items.clear();
//                                            IrpHook_items.shrink_to_fit();
//                                            EnumIrpHook();
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory(Current)"))
//                                {
//                                    for (size_t i = 0; i < IrpHook_items.size(); ++i)
//                                    {
//                                        if (IrpHook_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", IrpHook_items[i].Address);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory(Original)"))
//                                {
//                                    for (size_t i = 0; i < IrpHook_items.size(); ++i)
//                                    {
//                                        if (IrpHook_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", IrpHook_items[i].OriginalAddress);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory(Current)"))
//                                {
//                                    for (size_t i = 0; i < IrpHook_items.size(); ++i)
//                                    {
//                                        if (IrpHook_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", IrpHook_items[i].Address);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("Disassembly Memory(Original)"))
//                                {
//                                    for (size_t i = 0; i < IrpHook_items.size(); ++i)
//                                    {
//                                        if (IrpHook_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", IrpHook_items[i].OriginalAddress);
//                                            ASMMemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        ImGui::EndTabBar();
//                    }
//                    ImGui::EndTabItem();
//                }
//                if (ImGui::BeginTabItem("Network"))
//                {
//                    NoExit = 0;
//                    NoEnableHvm = 0;
//                    NoEnableMonitor = 0;
//
//                    if (ImGui::BeginTabBar("Networktab"))
//                    {
//                        if (ImGui::BeginTabItem("WFP Function"))
//                        {
//                            if (ImGui::Button("Enum WFP Function"))
//                            {
//                                WFPFunction_items.clear();
//                                WFPFunction_items.shrink_to_fit();
//                                WFPCallout_items.clear();
//                                WFPCallout_items.shrink_to_fit();
//                                //GetWFPObject();
//                                CreateThread(NULL, 0, EnumWFPThread, NULL, 0, NULL);
//                            }
//                            if (ImGui::BeginTable("wfpfunc_table", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                for (size_t i = 0; i < WFPFunction_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    ImGui::TableNextColumn();
//                                    ImGui::Text("%d", WFPFunction_items[i].id);
//                                    ImGui::TableNextColumn();
//                                    if (ImGui::Selectable(WFPFunction_items[i].Name.c_str(), WFPFunction_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : WFPFunction_items) item.selected = false;
//                                        WFPFunction_items[i].selected = true;
//                                    }
//                                    ImGui::TableNextColumn();
//                                    ImGui::Text("%X-%X-%X-%X", WFPFunction_items[i].guid.Data1, WFPFunction_items[i].guid.Data2, WFPFunction_items[i].guid.Data3, WFPFunction_items[i].guid.Data4);
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("wfpfunc_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    WFPFunction_items.clear();
//                                    WFPFunction_items.shrink_to_fit();
//                                    WFPCallout_items.clear();
//                                    WFPCallout_items.shrink_to_fit();
//                                    //GetWFPObject();
//                                    CreateThread(NULL, 0, EnumWFPThread, NULL, 0, NULL);
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("WFP Callout"))
//                        {
//                            if (ImGui::Button("Enum WFP Callout"))
//                            {
//                                WFPFunction_items.clear();
//                                WFPFunction_items.shrink_to_fit();
//                                WFPCallout_items.clear();
//                                WFPCallout_items.shrink_to_fit();
//                                //GetWFPObject();
//                                CreateThread(NULL, 0, EnumWFPThread, NULL, 0, NULL);
//                            }
//                            if (ImGui::BeginTable("wfpcallout_table", 6, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                            {
//                                for (size_t i = 0; i < WFPCallout_items.size(); ++i) {
//                                    ImGui::PushID(static_cast<int>(i));
//                                    ImGui::TableNextRow();
//                                    ImGui::TableNextColumn();
//                                    ImGui::Text("%d", WFPCallout_items[i].Id);
//                                    ImGui::TableNextColumn();
//                                    ImGui::Text("%p", WFPCallout_items[i].Entry);
//                                    ImGui::TableNextColumn();
//                                    ImGui::Text("%p", WFPCallout_items[i].ClassFunc);
//                                    ImGui::TableNextColumn();
//                                    ImGui::Text("%p", WFPCallout_items[i].Notify_Func);
//                                    ImGui::TableNextColumn();
//                                    ImGui::Text("%p", WFPCallout_items[i].deletefunc);
//                                    ImGui::TableNextColumn();
//                                    if (ImGui::Selectable(WFPCallout_items[i].Module.c_str(), WFPCallout_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                    {
//                                        for (auto& item : WFPCallout_items) item.selected = false;
//                                        WFPCallout_items[i].selected = true;
//                                    }
//                                    ImGui::PopID();
//                                }
//                            }
//                            ImGui::EndTable();
//
//                            if (ImGui::BeginPopupContextItem("wfpcallout_Menu"))
//                            {
//                                if (ImGui::MenuItem("Refesh"))
//                                {
//                                    WFPFunction_items.clear();
//                                    WFPFunction_items.shrink_to_fit();
//                                    WFPCallout_items.clear();
//                                    WFPCallout_items.shrink_to_fit();
//                                    //GetWFPObject();
//                                    CreateThread(NULL, 0, EnumWFPThread, NULL, 0, NULL);
//                                }
//                                if (ImGui::MenuItem("Remove"))
//                                {
//                                    for (size_t i = 0; i < WFPCallout_items.size(); ++i)
//                                    {
//                                        if (WFPCallout_items[i].selected == true)
//                                        {
//                                            struct input {
//                                                ULONG Id;
//                                            };
//                                            input inputs = { 0 };
//
//                                            inputs.Id = WFPCallout_items[i].Id;
//                                            DeviceIoControl(hDevice, IOCTL_REMOVE_WFP, &inputs, sizeof(input), 0, 0, 0, NULL);
//                                            WFPCallout_items.erase(WFPCallout_items.begin() + i);
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory(ClassFunction)"))
//                                {
//                                    for (size_t i = 0; i < WFPCallout_items.size(); ++i)
//                                    {
//                                        if (WFPCallout_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", WFPCallout_items[i].ClassFunc);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory(NotifyFunction)"))
//                                {
//                                    for (size_t i = 0; i < WFPCallout_items.size(); ++i)
//                                    {
//                                        if (WFPCallout_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", WFPCallout_items[i].Notify_Func);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                if (ImGui::MenuItem("View Memory(DeleteFunction)"))
//                                {
//                                    for (size_t i = 0; i < WFPCallout_items.size(); ++i)
//                                    {
//                                        if (WFPCallout_items[i].selected == true)
//                                        {
//                                            sprintf_s(editmemoryaddress, "%p", WFPCallout_items[i].deletefunc);
//                                            MemoryWindow = true;
//                                        }
//                                    }
//                                }
//                                ImGui::EndPopup();
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        ImGui::EndTabBar();
//                    }
//
//                    ImGui::EndTabItem();
//                }
//                if (ImGui::BeginTabItem("MEMORY"))
//                {
//                    if (ImGui::BeginTabBar("MEMORY_Tab"))
//                    {
//                        if (ImGui::BeginTabItem("BYTE"))
//                        {
//                            static char Address[17] = "FFFFF00000000000";
//                            static char Size[10] = "256";
//                            static char memEditDataInput[1024] = "33C0C3";
//                            static bool memEditError = false;
//
//
//                            ImGui::Text("Address");
//                            ImGui::SameLine();
//                            ImGui::InputText("##addr", Address, IM_ARRAYSIZE(Address), ImGuiInputTextFlags_CharsHexadecimal);
//
//                            ImGui::SameLine();
//                            ImGui::Text("Size");
//                            ImGui::SameLine();
//                            ImGui::InputText("##size", Size, IM_ARRAYSIZE(Size), ImGuiInputTextFlags_CharsDecimal);
//                            ImGui::Text("Data     ");
//                            ImGui::SameLine();
//                            ImGui::InputText("###data", memEditDataInput, sizeof(memEditDataInput),
//                                ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
//
//                            if (ImGui::Button("READ"))
//                            {
//                                try {
//                                    uint64_t addr = std::stoull(Address, nullptr, 16);
//                                    uint32_t size = std::stoul(Size, nullptr, 10);
//
//                                    if (size < 16)
//                                    {
//                                        size = 16;
//                                    }
//
//                                    if (Kernel_ReadKernelMemory(reinterpret_cast<PVOID>(addr), size)) {
//                                    }
//                                }
//                                catch (...) {
//                                    g_mem_data.clear();
//                                }
//                            }
//                            if (ImGui::Button("WRITE")) {
//                                uintptr_t address = 0;
//                                std::vector<uint8_t> data;
//                                memEditError = false;
//
//                                try {
//                                    address = std::stoull(Address, nullptr, 16);
//                                }
//                                catch (...) {
//                                    memEditError = true;
//                                    MessageBox(NULL, "Invalid address format", "Failed", NULL);
//                                }
//
//                                if (!memEditError) {
//                                    std::string hexStr = memEditDataInput;
//                                    hexStr.erase(std::remove_if(hexStr.begin(), hexStr.end(),
//                                        [](char c) { return !isxdigit(c); }), hexStr.end());
//
//                                    if (hexStr.empty()) {
//                                        memEditError = true;
//                                        MessageBox(NULL, "No data to write", "Failed", NULL);
//
//                                    }
//                                    else if (hexStr.length() % 2 != 0) {
//                                        memEditError = true;
//                                        MessageBox(NULL, "Hex data must have even number of characters", "Failed", NULL);
//
//                                    }
//                                    else {
//                                        for (size_t i = 0; i < hexStr.length(); i += 2) {
//                                            try {
//                                                uint8_t byte = static_cast<uint8_t>(std::stoi(hexStr.substr(i, 2), nullptr, 16));
//                                                data.push_back(byte);
//                                            }
//                                            catch (...) {
//                                                memEditError = true;
//                                                MessageBox(NULL, "Invalid hex byte at position", "Failed", NULL);
//                                                break;
//                                            }
//                                        }
//                                    }
//                                }
//                                if (!memEditError && !data.empty())
//                                {
//                                    struct input {
//                                        PVOID Address;
//                                        PVOID Data;
//                                        SIZE_T Size;
//                                    };
//                                    input inputs = { 0 };
//                                    inputs.Address = (PVOID)address;
//                                    inputs.Size = static_cast<ULONG>(data.size());
//                                    memcpy(inputs.Data, data.data(), data.size());
//
//                                    DWORD bytesReturned = 0;
//                                    if (!DeviceIoControl(hDevice, IOCTL_WRITE_TO_KERNEL_MEMORY,
//                                        &inputs, sizeof(inputs),
//                                        NULL, 0, &bytesReturned, NULL)) {
//                                        memEditError = true;
//                                        MessageBox(NULL, "Write failed", "Failed", NULL);
//                                    }
//                                    else {
//                                        memEditError = false;
//                                    }
//                                }
//                            }
//                            ImGui::SameLine();
//                            if (!g_mem_data.empty())
//                            {
//                                ImGui::BeginChild("##MemoryView", ImVec2(0, 0), true);
//
//                                const int cols = 16;
//                                ImGuiListClipper clipper;
//                                clipper.Begin((g_mem_data.size() + cols - 1) / cols);
//
//                                while (clipper.Step())
//                                {
//                                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
//                                    {
//                                        const int offset = row * cols;
//                                        for (int col = 0; col < cols; col++)
//                                        {
//                                            if (offset + col >= g_mem_data.size()) break;
//
//                                            ImGui::Text("0x%02X ", g_mem_data[offset + col]);
//                                            if (col < cols - 1) ImGui::SameLine();
//                                        }
//                                    }
//                                }
//
//                                ImGui::EndChild();
//                            }
//                            else
//                            {
//                                ImGui::Text("No data available");
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("ASM"))
//                        {
//                            static char Address[17] = "FFFFF00000000000";
//                            static char Size[10] = "256";
//
//                            ImGui::Text("Address");
//                            ImGui::SameLine();
//                            ImGui::InputText("##asmaddr", Address, IM_ARRAYSIZE(Address), ImGuiInputTextFlags_CharsHexadecimal);
//
//                            ImGui::SameLine();
//                            ImGui::Text("Size");
//                            ImGui::SameLine();
//                            ImGui::InputText("##asmsize", Size, IM_ARRAYSIZE(Size), ImGuiInputTextFlags_CharsDecimal);
//
//                            if (ImGui::Button("READ"))
//                            {
//                                try {
//                                    g_mem_data.clear();
//                                    g_mem_data.shrink_to_fit();
//                                    Disassembly_items.clear();
//                                    Disassembly_items.shrink_to_fit();
//                                    uint64_t addr = std::stoull(Address, nullptr, 16);
//                                    uint32_t size = std::stoul(Size, nullptr, 10);
//                                    if (size < 16)
//                                    {
//                                        size = 16;
//                                    }
//                                    disamSizes = size;
//
//                                    if (Kernel_ReadKernelMemory(reinterpret_cast<PVOID>(addr), size)) {
//                                        DisassembleMemory(addr);
//                                    }
//                                }
//                                catch (...) {
//                                    g_mem_data.clear();
//                                    g_mem_data.shrink_to_fit();
//                                    Disassembly_items.clear();
//                                    Disassembly_items.shrink_to_fit();
//                                }
//                            }
//                            ImGui::SameLine();
//                            if (!g_mem_data.empty())
//                            {
//                                if (ImGui::BeginTable("asmmemry_table", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                                {
//                                    ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
//                                    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
//                                    ImGui::TableSetupColumn("ASM");
//                                    ImGui::TableHeadersRow();
//                                    for (size_t i = 0; i < Disassembly_items.size(); ++i) {
//                                        char ASMdatas[MAX_PATH];
//                                        uint64_t asmtoaddress = 0;
//                                        ImGui::PushID(static_cast<int>(i));
//                                        ImGui::TableNextRow();
//                                        ImGui::TableSetColumnIndex(0);
//                                        ImGui::Text("%d", i);
//                                        ImGui::TableSetColumnIndex(1);
//                                        ImGui::Text("%p", Disassembly_items[i].Address);
//                                        ImGui::TableSetColumnIndex(2);
//                                        if (ASM_Getasmaddr(Disassembly_items[i].DATA, asmtoaddress)) 
//                                        {
//                                            sprintf_s(ASMdatas, "%s %s(%s)", Disassembly_items[i].mnemonic.c_str(), Disassembly_items[i].DATA.c_str(), GetDriverNameByAddress((PVOID)asmtoaddress));
//                                        }
//                                        else 
//                                        {
//                                            sprintf_s(ASMdatas, "%s %s", Disassembly_items[i].mnemonic.c_str(), Disassembly_items[i].DATA.c_str());
//                                        }
//                                        if (ImGui::Selectable(ASMdatas, Disassembly_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                                        {
//                                            for (auto& item : Disassembly_items) item.selected = false;
//                                            Disassembly_items[i].selected = true;
//                                        }
//                                        ImGui::PopID();
//                                    }
//                                }
//                                ImGui::EndTable();
//                                if (ImGui::BeginPopupContextItem("ASM_Menu"))
//                                {
//                                    uint64_t Disassemblytoaddress = 0;
//                                    for (size_t i = 0; i < Disassembly_items.size(); ++i)
//                                    {
//                                        if (Disassembly_items[i].selected == true)
//                                        {
//                                            if (ASM_Getasmaddr(Disassembly_items[i].DATA, Disassemblytoaddress))
//                                            {
//                                                if (ImGui::MenuItem("Disassembly Target Address"))
//                                                {
//                                                    sprintf_s(Address, "%p", Disassemblytoaddress);
//                                                    g_mem_data.clear();
//                                                    g_mem_data.shrink_to_fit();
//                                                    Disassembly_items.clear();
//                                                    Disassembly_items.shrink_to_fit();
//                                                    if (Kernel_ReadKernelMemory(reinterpret_cast<PVOID>(Disassemblytoaddress), disamSizes)) {
//                                                        DisassembleMemory(Disassemblytoaddress);
//                                                    }
//                                                }
//                                            }
//                                        }
//                                    }
//                                    if (ImGui::MenuItem("Copy"))
//                                    {
//                                        for (size_t i = 0; i < Disassembly_items.size(); ++i)
//                                        {
//                                            if (Disassembly_items[i].selected == true)
//                                            {
//                                                char copyASMdatas[MAX_PATH];
//                                                sprintf_s(copyASMdatas, "%s %s", Disassembly_items[i].mnemonic.c_str(), Disassembly_items[i].DATA.c_str());
//                                                copyToClipboard(copyASMdatas);
//                                            }
//                                        }
//                                    }
//                                    ImGui::EndPopup();
//                                }
//                            }
//                            else
//                            {
//                                ImGui::Text("No data available");
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        ImGui::EndTabBar();
//                    }
//                    ImGui::EndTabItem();
//                }
//                if (ImGui::BeginTabItem("Files"))
//                {
//                    NoExit = 0;
//                    NoEnableMonitor = 0;
//                    static char Path[MAX_PATH] = "C:\\";
//                    ImGui::InputText(" ", Path, IM_ARRAYSIZE(Path));
//                    ImGui::SetCursorPos(ImVec2(800, 55));
//                    if (ImGui::Button("Query"))
//                    {
//                        printf("Path = %s\n", Path);
//                        WCHAR wsPath[MAX_PATH];
//                        charToWCHAR(Path, wsPath);
//                        File_items.clear();
//                        File_items.shrink_to_fit();
//                        Kernel_QueryFile(wsPath, Path);
//                    }
//                    if (ImGui::BeginTable("File_table", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                    {
//                        for (size_t i = 0; i < File_items.size(); ++i) {
//                            ImGui::PushID(static_cast<int>(i));
//                            ImGui::TableNextRow();
//                            ImGui::TableNextColumn();
//                            if (ImGui::Selectable(File_items[i].Type.c_str(), File_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                            {
//                                for (auto& item : File_items) item.selected = false;
//                                File_items[i].selected = true;
//                            }
//                            ImGui::TableNextColumn();
//                            ImGui::Text(File_items[i].name.c_str());
//                            ImGui::TableNextColumn();
//                            ImGui::Text(File_items[i].InFolder.c_str());
//                            ImGui::PopID();
//                        }
//                    }
//                    ImGui::EndTable();
//
//                    if (ImGui::BeginPopupContextItem("Files_Menu"))
//                    {
//                        if (ImGui::MenuItem("Refesh"))
//                        {
//                            printf("Path = %s\n", Path);
//                            WCHAR wsPath[MAX_PATH];
//                            charToWCHAR(Path, wsPath);
//                            File_items.clear();
//                            File_items.shrink_to_fit();
//                            Kernel_QueryFile(wsPath, Path);
//                        }
//                        if (ImGui::MenuItem("DELETE"))
//                        {
//                            for (size_t i = 0; i < File_items.size(); ++i)
//                            {
//                                if (File_items[i].selected == true)
//                                {
//                                    std::string deletefilepath = File_items[i].InFolder + "\\" + File_items[i].name;
//                                    WCHAR filePath[MAX_PATH];
//                                    charToWCHAR(deletefilepath.c_str(), filePath);
//                                    printf("Delete = %ws\n", filePath);
//                                    if (File_items[i].isFolder == TRUE)
//                                    {
//                                        enumerateFilestodelete(deletefilepath);
//                                    }
//                                    else
//                                    {
//                                        Kernel_DeleteFile_Auto(filePath);
//                                    }
//                                    File_items.erase(File_items.begin() + i);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("FORCE DELETE"))
//                        {
//                            for (size_t i = 0; i < File_items.size(); ++i)
//                            {
//                                if (File_items[i].selected == true)
//                                {
//                                    std::string deletefilepath = File_items[i].InFolder + "\\" + File_items[i].name;
//                                    WCHAR filePath[MAX_PATH];
//                                    charToWCHAR(deletefilepath.c_str(), filePath);
//                                    printf("Delete = %ws\n", filePath);
//                                    if (File_items[i].isFolder == TRUE)
//                                    {
//                                        enumerateFilestodelete_Force(deletefilepath);
//                                    }
//                                    else
//                                    {
//                                        Kernel_Force_DeleteFile_Auto(filePath);
//                                    }
//                                    File_items.erase(File_items.begin() + i);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("FORCE DELETE(Advanced)"))
//                        {
//                            for (size_t i = 0; i < File_items.size(); ++i)
//                            {
//                                if (File_items[i].selected == true)
//                                {
//                                    int result = MessageBoxW(NULL, L"Are you sure you want to continue, the process is very slow(The operation can take up to several hours!)?", L"FORCE_DELETE_FILE", MB_YESNO | MB_ICONQUESTION);
//
//                                    if (result == IDYES)
//                                    {
//                                        std::string deletefilepath = File_items[i].InFolder + "\\" + File_items[i].name;
//                                        WCHAR filePath[MAX_PATH];
//                                        std::string  Conversionfilepath = ConversionPath(deletefilepath);
//                                        charToWCHAR(Conversionfilepath.c_str(), filePath);
//                                        printf("Delete = %ws\n", filePath);
//                                        if (File_items[i].isFolder == TRUE)
//                                        {
//                                            MessageBoxW(NULL, L"This operation only supports a single file", L"WARNING", NULL);
//                                        }
//                                        else
//                                        {
//                                            NTFS_Search_Object_Delete_File(filePath);
//                                            MessageBoxW(NULL, L"The task has been created, please do not exit SKT64", L"TASK Createed", NULL);
//                                        }
//                                        File_items.erase(File_items.begin() + i);
//                                    }
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("ProtectFile"))
//                        {
//                            for (size_t i = 0; i < File_items.size(); ++i)
//                            {
//                                if (File_items[i].selected == true)
//                                {
//                                    if (File_items[i].isFolder == FALSE)
//                                    {
//                                        std::string deletefilepath = File_items[i].InFolder + "\\" + File_items[i].name;
//                                        WCHAR filePath[MAX_PATH];
//                                        charToWCHAR(deletefilepath.c_str(), filePath);
//                                        printf("Protect = %ws\n", filePath);
//                                        Kernel_LockFile(filePath);
//                                    }
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Copy Name"))
//                        {
//                            for (size_t i = 0; i < File_items.size(); ++i)
//                            {
//                                if (File_items[i].selected == true)
//                                {
//                                    copyToClipboard(File_items[i].name);
//                                }
//                            }
//                        }
//                        if (ImGui::MenuItem("Copy Path"))
//                        {
//                            for (size_t i = 0; i < File_items.size(); ++i)
//                            {
//                                if (File_items[i].selected == true)
//                                {
//                                    copyToClipboard(File_items[i].InFolder);
//                                }
//                            }
//                        }
//                        ImGui::EndPopup();
//                    }
//                    ImGui::EndTabItem();
//                }
//                if (ImGui::BeginTabItem("System-Monitor"))
//                {
//                    NoExit = 0;
//                    NoEnableHvm = 0;
//                    std::vector<const char*> items;
//                    for (const auto& item : Monitor_listItems)
//                    {
//                        items.push_back(item.c_str());
//                    }
//
//                    if (MonitorEnabled == NULL)
//                    {
//                        if (NoEnableMonitor == NULL)
//                        {
//                            int result = MessageBoxW(NULL, L"Are you sure you want to enable System-Monitor?", L"System-Monitor", MB_YESNO | MB_ICONQUESTION);
//
//                            if (result == IDYES)
//                            {
//                                DeviceIoControl(hDevice, IOCTL_Enbale_Monitor, NULL, 0, NULL, 0, NULL, NULL);
//                                hThread = CreateThread(NULL, 0, Monitor_Thread, NULL, 0, NULL);
//                                MonitorEnabled = 100;
//                            }
//                            else if (result == IDNO)
//                            {
//                                NoEnableMonitor = 100;
//                            }
//                        }
//                    }
//                    else
//                    {
//                        ImGui::SetNextWindowSize(ImVec2(1180, 500));
//                        ImGui::ListBox(" ", &Monitor_selectedIndex, items.data(), static_cast<int>(items.size()), 8);
//                        ImGui::Text("Event Count = %d", Monitor_listItems.size());
//                        if (ImGui::Button("Clear Log"))
//                        {
//                            items.clear();
//                            items.shrink_to_fit();
//                            Monitor_listItems.clear();
//                            Monitor_listItems.shrink_to_fit();
//                        }
//                    }
//
//
//                    ImGui::EndTabItem();
//                }
//                if (ImGui::BeginTabItem("Hvm"))
//                {
//                    NoExit = 0;
//                    NoEnableMonitor = 0;
//
//                    if (HvmEnabled == NULL)
//                    {
//                        if (NoEnableHvm == NULL)
//                        {
//                            int result = MessageBoxW(NULL, L"Are you sure you want to enable Hvm?", L"Hvm", MB_YESNO | MB_ICONQUESTION);
//
//                            if (result == IDYES)
//                            {
//                                if (DeviceIoControl(hDevice, IOCTL_CHECK_HVM, NULL, 0, NULL, 0, NULL, NULL))
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_INIT_HVM, NULL, 0, NULL, 0, NULL, NULL);
//                                    HvmEnabled = 100;
//                                }
//                                else
//                                {
//                                    MessageBoxW(NULL, L"Your Processer Unsupported Inte-VT", L"Hvm", MB_OK);
//                                }
//                            }
//                            else if (result == IDNO)
//                            {
//                                NoEnableHvm = 100;
//                            }
//                        }
//                    }
//                    else
//                    {
//                        if (ImGui::BeginTabBar("HvTab"))
//                        {
//                            static bool disableobcallbackhvm = false;
//                            static bool selfProtect = false;
//                            static bool syscallhook_proxy = false;
//                            static bool hvmsandbox = false;
//                            if (ImGui::Checkbox("Disable ObCallback", &disableobcallbackhvm))
//                            {
//                                if (disableobcallbackhvm == true)
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Disable_ObCallback_Hvm, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                                else
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Enable_ObCallback_Hvm, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                            }
//                            if (ImGui::Checkbox("Self Protect", &selfProtect))
//                            {
//                                if (selfProtect == true)
//                                {
//                                    if (SetProtectPid(GetCurrentProcessId()))
//                                    {
//                                        DeviceIoControl(hDevice, IOCTL_ProtectProcess_Hvm, NULL, 0, NULL, 0, NULL, NULL);
//                                        //SetWindowProtect(GetConsoleWindow());
//                                    }
//                                    else
//                                    {
//                                        selfProtect = false;
//                                    }
//                                }
//                                else
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_UnProtectProcess_Hvm, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                            }
//                            if (ImGui::Checkbox("Enable Hvm Sandbox", &hvmsandbox))
//                            {
//                                if (selfProtect == true)
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Enable_HVM_Sandbox, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                                else
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Disable_HVM_Sandbox, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                            }
//                            /*
//                            if (ImGui::BeginTabItem("Tools"))
//                            {
//
//                                ImGui::EndTabItem();
//                            }
//                            if (ImGui::BeginTabItem("System Function Proxy"))
//                            {
//                                ImGui::EndTabItem();
//                            }
//                            */
//                            ImGui::EndTabBar();
//                        }
//                    }
//                    ImGui::EndTabItem();
//                }
//                if (ImGui::BeginTabItem("Tools"))
//                {
//                    NoExit = 0;
//                    NoEnableHvm = 0;
//                    NoEnableMonitor = 0;
//
//                    if (ImGui::BeginTabBar("ToolsTab"))
//                    {
//                        if (ImGui::BeginTabItem("Disbale PatchGuard"))
//                        {
//                            static int FirmwareType = 0;
//                            DWORD bytesReturned;
//                            ImGui::Text("Please Select Firmware Type");
//                            ImGui::RadioButton("EFI", &FirmwareType, 0);
//                            ImGui::RadioButton("Legacy(BIOS)", &FirmwareType, 1);
//                            ImGui::RadioButton("Dynamic(System Crash WARNING)", &FirmwareType, 2);
//                            if (ImGui::Button("DISABLE"))
//                            {
//                                int result = MessageBoxW(NULL, L"Are you sure you want to disable PatchGuard?", L"Disable PatchGuard", MB_YESNO | MB_ICONQUESTION);
//
//                                if (result == IDYES) 
//                                {
//                                    if (FirmwareType == 0)
//                                    {
//                                        printf("Firmware Type = EFI\n");
//                                        BOOL status = DeviceIoControl(hDevice, IOCTL_Disable_PatchGuard_EFI, NULL, 0, NULL, 0, &bytesReturned, NULL);
//                                        if (status)
//                                        {
//                                            printf("successfully.\n");
//                                            MessageBox(NULL, "successfully", "successfully", NULL);
//                                        }
//                                        else {
//                                            MessageBox(NULL, "Failed", "Failed", NULL);
//                                        }
//                                    }
//                                    if (FirmwareType == 1)
//                                    {
//                                        printf("Firmware Type = BIOS\n");
//                                        BOOL status = DeviceIoControl(hDevice, IOCTL_Disable_PatchGuard_BIOS, NULL, 0, NULL, 0, &bytesReturned, NULL);
//                                        if (status)
//                                        {
//                                            printf("successfully.\n");
//                                            MessageBox(NULL, "successfully", "successfully", NULL);
//                                        }
//                                        else {
//                                            MessageBox(NULL, "Failed", "Failed", NULL);
//                                        }
//                                    }
//                                    if (FirmwareType == 2)
//                                    {
//                                        printf("Dynamic Mode\n");
//                                        BOOL status = DeviceIoControl(hDevice, IOCTL_Disable_PatchGuard_DYNAMIC, NULL, 0, NULL, 0, &bytesReturned, NULL);
//                                        if (status)
//                                        {
//                                            printf("successfully.\n");
//                                            MessageBox(NULL, "successfully", "successfully", NULL);
//                                        }
//                                        else {
//                                            MessageBox(NULL, "Failed", "Failed", NULL);
//                                        }
//                                    }
//                                }
//                                else if (result == IDNO) 
//                                {
//                                    printf("Cancel\n");
//                                }
//                            }
//                            if (ImGui::Button("Execute PatchGuard"))
//                            {
//                                int result = MessageBoxW(NULL, L"Are you sure you want to Execute PatchGuard?", L"Execute PatchGuard", MB_YESNO | MB_ICONQUESTION);
//
//                                if (result == IDYES)
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_EXECUTE_PATCHGUARD, NULL, 0, NULL, 0, &bytesReturned, NULL);
//                                }
//                                else if (result == IDNO)
//                                {
//                                    printf("Cancel\n");
//                                }
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("Load Driver"))
//                        {
//                            static bool DDSE = false;
//                            ImGui::Text("Driver File Path");
//                            static char DriverPath[MAX_PATH] = "C:\\Test.sys";
//                            ImGui::InputText("     ", DriverPath, IM_ARRAYSIZE(DriverPath));
//                            ImGui::Text("Serive Name");
//                            static char ServiceName[MAX_PATH] = "Test";
//                            ImGui::InputText("       ", ServiceName, IM_ARRAYSIZE(ServiceName));
//                            ImGui::Checkbox("Ignore Driver Signature Enforcement", &DDSE);
//                            WCHAR DPath[MAX_PATH];
//                            WCHAR DName[MAX_PATH];
//                            charToWCHAR(DriverPath, DPath);
//                            charToWCHAR(ServiceName, DName);
//                            if (ImGui::Button("Load"))
//                            {
//                                if (DDSE == false)
//                                {
//                                    BOOL status = LoadDriver(DPath, DName);
//                                    if (status)
//                                    {
//                                        MessageBox(NULL, "Load Driver successfully", "Successfully", NULL);
//                                    }
//                                    else
//                                    {
//                                        MessageBox(NULL, "Load Driver FAILED", "FAILED", NULL);
//                                    }
//                                }
//                                if (DDSE == true)
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Disable_DSE, NULL, 0, NULL, 0, NULL, NULL);
//                                    if (LoadDriver(DPath, DName))
//                                    {
//                                        MessageBox(NULL, "Load Driver successfully", "Successfully", NULL);
//                                    }
//                                    else
//                                    {
//                                        MessageBox(NULL, "Load Driver FAILED", "FAILED", NULL);
//                                    }
//                                    DeviceIoControl(hDevice, IOCTL_Enable_DSE, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                            }
//                            if (ImGui::Button("Unload"))
//                            {
//                                DriverUnload(ServiceName);
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("Firmware"))
//                        {
//                            //ImGui::Text("Flash the firmware");
//                            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Flash the firmware");
//                            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Please Disable Flash Protect in the firmware settings");
//                            ImGui::Text("ROM File Path");
//                            static char ROMPath[MAX_PATH] = "C:\\Test.ROM";
//                            ImGui::InputText("         ", ROMPath, IM_ARRAYSIZE(ROMPath));
//                            if (ImGui::Button("WRITE"))
//                            {
//                                int result = MessageBoxW(NULL, L"Are you sure you want to flash the firmware, if the firmware is damaged, the system will not boot", L"Flash-Firmware", MB_YESNO | MB_ICONQUESTION);
//                                if (result == IDYES)
//                                {
//                                    int results = MessageBoxW(NULL, L"Are you sure you want to flash the firmware, if the firmware is damaged, the system will not boot", L"Flash-Firmware", MB_YESNO | MB_ICONQUESTION);
//                                    if (results == IDYES)
//                                    {
//                                        WCHAR RomsPaths[MAX_PATH];
//                                        charToWCHAR(ROMPath, RomsPaths);
//                                        UNICODE_STRING Path[MAX_PATH];
//                                        RtlInitUnicodeString(Path, RomsPaths);
//                                        DWORD bytesReturned;
//                                        BOOL status = DeviceIoControl(hDevice, IOCTL_FLASH_FIRMWARE, Path, sizeof(Path), NULL, 0, &bytesReturned, NULL);
//                                        if (!status)
//                                        {
//                                            printf("Failed. Error %ld\n", GetLastError());
//                                        }
//                                    }
//                                }
//                            }
//                            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Lock Firmware");
//                            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "This feature will lock the firmware settings, and you will not be able to access the firmware settings as well as the system");
//                            if (ImGui::Button("LOCK"))
//                            {
//                                int results = MessageBoxW(NULL, L"Are you sure you want to lock the firmware, This feature will lock the firmware settings, and you will not be able to access the firmware settings as well as the system", L"Lock-Firmware", MB_YESNO | MB_ICONQUESTION);
//                                if (results == IDYES)
//                                {
//                                    int resultss = MessageBoxW(NULL, L"Are you sure you want to lock the firmware, This feature will lock the firmware settings, and you will not be able to access the firmware settings as well as the system", L"Lock-Firmware", MB_YESNO | MB_ICONQUESTION);
//                                    if (resultss == IDYES)
//                                    {
//                                        DeviceIoControl(hDevice, IOCTL_LOCK_FIRMWARE, NULL, 0, NULL, 0, NULL, NULL);
//                                    }
//                                }
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("Taskmgr-Editor"))
//                        {
//                            ImGui::Text("Cores");
//                            ImGui::SameLine();
//                            static char Cores[MAX_PATH] = "1024";
//                            ImGui::InputText("##cores", Cores, IM_ARRAYSIZE(Cores));
//                            uint32_t g_Cores = std::stoul(Cores, nullptr, 10);
//                            if (ImGui::Button("Modify"))
//                            {
//                                struct input {
//                                    ULONG cores;
//                                };
//                                input inputs = { 0 };
//                                inputs.cores = g_Cores;
//                                BOOL status = DeviceIoControl(hDevice, IOCTL_EDIT_TASKMGR, &inputs, sizeof(input), 0, 0, 0, NULL);
//                                if (!status) {
//                                    printf("Failed. Error %ld\n", GetLastError());
//                                }
//                                else
//                                {
//                                    MessageBox(NULL, "successfully.", "Success", NULL);
//                                }
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("MISC"))
//                        {
//                            if (ImGui::Button("Start TrustedInsatllerService"))
//                            {
//                                DeviceIoControl(hDevice, IOCTL_START_TRUSTEDINSTALLER, NULL, 0, NULL, 0, NULL, NULL);
//                            }
//                            if (ImGui::Button("Disable Windows Defender"))
//                            {
//                                RefeshAll();
//                                for (size_t i = 0; i < Process_items.size(); ++i)
//                                {
//                                    if (strstr(Process_items[i].name.c_str(), "MsMpEng.exe")|| strstr(Process_items[i].name.c_str(), "MpDefenderCoreService.exe"))
//                                    {
//                                        KernelTerminateProcess(Process_items[i].id);
//                                        printf("Terminate Process = %d\n", Process_items[i].id);
//                                    }
//                                }
//                                for (size_t i = 0; i < Notify_items.size(); ++i)
//                                {
//                                    if (strstr(Notify_items[i].Module.c_str(), "WdFilter"))
//                                    {
//                                        if (Notify_items[i].NotifyTypes != Notify_Coalescing || Notify_items[i].NotifyTypes != Notify_FsNotofy || Notify_items[i].NotifyTypes != Notify_LoastShutdown
//                                            || Notify_items[i].NotifyTypes != Notify_PowerSetting || Notify_items[i].NotifyTypes != Notify_Shutdown || Notify_items[i].NotifyTypes != Notify_ObCllback)
//                                        {
//                                            RemoveKernelNotify(Notify_items[i].Address, Notify_items[i].Handle, Notify_items[i].NotifyTypes);
//                                            printf("Remove Notify = %p\n", Notify_items[i].Address);
//                                        }
//                                    }
//                                }
//                                for (size_t i = 0; i < ExCallback_items.size(); ++i)
//                                {
//                                    if (strstr(ExCallback_items[i].Module.c_str(), "WdFilter"))
//                                    {
//                                        RemoveExCallback(ExCallback_items[i].Handle);
//                                        printf("Remove ExCallback = %p\n", ExCallback_items[i].Handle);
//                                    }
//                                }
//                                for (size_t i = 0; i < SystemThread_items.size(); ++i)
//                                {
//                                    if (strstr(SystemThread_items[i].Module.c_str(), "WdFilter"))
//                                    {
//                                        Kernel_TerminateThread(SystemThread_items[i].TID);
//                                        printf("Terminate Thread = %p\n", SystemThread_items[i].ethread);
//                                    }
//                                }
//                                for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//                                {
//                                    if (strstr(MiniFilter_items[i].Module.c_str(), "WdFilter"))
//                                    {
//                                        //RefeshAll();
//                                        RemoveMiniFilter(MiniFilter_items[i].Filter);
//                                        printf("Remove MiniFilter = %p\n", MiniFilter_items[i].Filter);
//                                        goto success;
//                                    }
//                                }
//                            success:
//                                {
//                                    RefeshAll();
//                                    printf("\n\n[Disable Windows Defender] successfully.\n");
//                                }
//                            }
//                            if (EnableAAV == TRUE)
//                            {
//                                if (ImGui::Button("Delete Sysdiag(HuoRong)"))
//                                {
//                                    HKEY hKey;
//                                    DWORD dwType = REG_SZ;
//                                    char szValue[1024] = { 0 };
//                                    DWORD dwSize = sizeof(szValue);
//                                    LONG result = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Huorong\\Sysdiag", 0, KEY_READ, &hKey);
//                                    if (result != ERROR_SUCCESS) 
//                                    {
//                                    }
//
//                                    result = RegQueryValueExA(hKey, "InstallPath", NULL, &dwType, (LPBYTE)szValue, &dwSize);
//                                    if (result != ERROR_SUCCESS) 
//                                    {
//                                        RegCloseKey(hKey);
//                                    }
//                                    std::string installPath(szValue);
//                                    std::string tergetpath = installPath + "\\bin";
//                                    enumerateFilestodelete(installPath);
//                                    enumerateFilestodelete(tergetpath);
//                                    RegCloseKey(hKey);
//                                }
//                                if (ImGui::Button("Disable Sysdiag Monitor"))
//                                {
//                                    for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//                                    {
//                                        if (strstr(MiniFilter_items[i].Module.c_str(), "sysdiag"))
//                                        {
//                                            RemoveMiniFilter(MiniFilter_items[i].Filter);
//                                            printf("Remove MiniFilter %p\n", MiniFilter_items[i].Filter);
//                                            goto RemoveSysdiagNotifys;
//                                        }
//                                    }
//                                RemoveSysdiagNotifys:
//                                    for (size_t i = 0; i < Notify_items.size(); ++i)
//                                    {
//                                        if (strstr(Notify_items[i].Module.c_str(), "sysdiag"))
//                                        {
//                                            if (Notify_items[i].NotifyTypes != Notify_CreateProcess || Notify_items[i].NotifyTypes != Notify_ObCllback)
//                                            {
//                                                printf("Remove Notify Type %d Address %p\n", Notify_items[i].NotifyTypes, Notify_items[i].Address);
//                                                RemoveKernelNotify(Notify_items[i].Address, Notify_items[i].Handle, Notify_items[i].NotifyTypes);
//                                            }
//                                        }
//                                    }
//                                    for (size_t i = 0; i < SystemThread_items.size(); ++i)
//                                    {
//                                        if (strstr(SystemThread_items[i].Module.c_str(), "sysdiag"))
//                                        {
//                                            Kernel_TerminateThread(SystemThread_items[i].TID);
//                                            printf("Terminate Thread = %p\n", SystemThread_items[i].ethread);
//                                        }
//                                    }
//                                }
//                                if (ImGui::Button("Disable 360"))
//                                {
//                                    RefeshAll();
//                                    RemoveNotifyByDriver("360");
//                                    RemoveMiniFilterByDriver("360");
//                                    for (size_t i = 0; i < Process_items.size(); ++i)
//                                    {
//                                        if (strstr(Process_items[i].name.c_str(), "ZhuDongFangYu.exe") || strstr(Process_items[i].name.c_str(), "360Tray.exe"))
//                                        {
//                                            printf("Terminate Process = %d\n", Process_items[i].id);
//                                            KernelTerminateProcess(Process_items[i].id);
//                                        }
//                                    }
//                                    /*
//                                    for (size_t i = 0; i < Process_items.size(); ++i)
//                                    {
//                                        if (strstr(Process_items[i].name.c_str(), "ZhuDongFangYu.exe") || strstr(Process_items[i].name.c_str(), "360Tray.exe"))
//                                        {
//                                            printf("Terminate Process = %d\n", Process_items[i].id);
//                                            KernelTerminateProcess(Process_items[i].id);
//                                        }
//                                    }
//                                    for (size_t i = 0; i < Notify_items.size(); ++i)
//                                    {
//                                        if (strstr(Notify_items[i].Module.c_str(), "360Box64") || strstr(Notify_items[i].Module.c_str(), "360AntiHacker64")
//                                            || strstr(Notify_items[i].Module.c_str(), "360Hvm64") || strstr(Notify_items[i].Module.c_str(), "360AntiHijack64")
//                                            || strstr(Notify_items[i].Module.c_str(), "360AntiSteal64") || strstr(Notify_items[i].Module.c_str(), "BAPIDRV64")
//                                            || strstr(Notify_items[i].Module.c_str(), "360FsFlt") || strstr(Notify_items[i].Module.c_str(), "360netmon")
//                                            || strstr(Notify_items[i].Module.c_str(), "360qpesv64") || strstr(Notify_items[i].Module.c_str(), "360Sensor64"))
//                                        {
//                                            if (Notify_items[i].NotifyTypes != Notify_Coalescing || Notify_items[i].NotifyTypes != Notify_FsNotofy || Notify_items[i].NotifyTypes != Notify_LoastShutdown
//                                                || Notify_items[i].NotifyTypes != Notify_PowerSetting || Notify_items[i].NotifyTypes != Notify_Shutdown || Notify_items[i].NotifyTypes != Notify_ObCllback)
//                                            {
//                                                if (!strstr(Notify_items[i].Type.c_str(), "Shutdown"))
//                                                {
//                                                    if (!strstr(Notify_items[i].Type.c_str(), "Ob"))
//                                                    {
//                                                        if (Notify_items[i].NotifyTypes == Notify_Priority || Notify_items[i].NotifyTypes == Notify_Coalescing || Notify_items[i].NotifyTypes == Notify_NmiCallback || Notify_items[i].NotifyTypes == Notify_BugCheck || Notify_items[i].NotifyTypes == Notify_BugCheckReason)
//                                                        {
//                                                            RemoveKernelNotify(Notify_items[i].Handle, Notify_items[i].Address, Notify_items[i].NotifyTypes);
//                                                        }
//                                                        RemoveKernelNotify(Notify_items[i].Address, Notify_items[i].Handle, Notify_items[i].NotifyTypes);
//                                                        printf("Remove Notify = %p\n", Notify_items[i].Address);
//                                                    }
//                                                }
//                                            }
//                                        }
//                                    }
//                                    for (size_t i = 0; i < SystemThread_items.size(); ++i)
//                                    {
//                                        if (strstr(SystemThread_items[i].Module.c_str(), "360Box64") || strstr(SystemThread_items[i].Module.c_str(), "360AntiHacker64")
//                                            || strstr(SystemThread_items[i].Module.c_str(), "360Hvm64") || strstr(SystemThread_items[i].Module.c_str(), "360AntiHijack64")
//                                            || strstr(SystemThread_items[i].Module.c_str(), "360AntiSteal64") || strstr(SystemThread_items[i].Module.c_str(), "BAPIDRV64")
//                                            || strstr(SystemThread_items[i].Module.c_str(), "360FsFlt") || strstr(SystemThread_items[i].Module.c_str(), "360netmon")
//                                            || strstr(SystemThread_items[i].Module.c_str(), "360qpesv64") || strstr(SystemThread_items[i].Module.c_str(), "360Sensor64"))
//                                        {
//                                            Kernel_TerminateThread(SystemThread_items[i].TID);
//                                            printf("Terminate Thread = %p\n", SystemThread_items[i].ethread);
//                                        }
//                                    }
//                                    for (size_t i = 0; i < MiniFilter_items.size(); ++i)
//                                    {
//                                        if (strstr(MiniFilter_items[i].Module.c_str(), "360FsFlt"))
//                                        {
//                                            RemoveMiniFilter(MiniFilter_items[i].Filter);
//                                            printf("Remove MiniFilter = %p\n", MiniFilter_items[i].Filter);
//                                            goto kill360success;
//                                        }
//                                    }
//                                kill360success:
//                                    {
//                                        printf("\n\n[Disable 360] successfully.\n");
//                                    }
//                                    */
//                                }
//                                if (ImGui::Button("Disable Kaspersky"))
//                                {
//                                    RemoveNotifyByDriver("klhk");
//                                    RemoveNotifyByDriver("klif");
//                                    RemoveNotifyByDriver("klupd");
//                                    RemoveNotifyByDriver("klflt");
//                                    RemoveMiniFilterByDriver("klif");
//                                    RemoveMiniFilterByDriver("klbackup");
//                                }
//                                if (ImGui::Button("Disable coldewsoft"))
//                                {
//                                    DisableXdriverAC();
//                                }
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        ImGui::EndTabBar();
//                    }
//                    ImGui::EndTabItem();
//                }
//                if (ImGui::BeginTabItem("plugins"))
//                {
//                    if (ImGui::Button("INIT PLUGINS"))
//                    {
//                        Plugins_items.clear();
//                        Plugins_items.shrink_to_fit();
//                        PluginsINIT();
//                    }
//                    if (ImGui::BeginTable("Plugins_table", 4, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                    {
//                        for (size_t i = 0; i < Plugins_items.size(); ++i) {
//                            ImGui::PushID(static_cast<int>(i));
//                            ImGui::TableNextRow();
//                            ImGui::TableNextColumn();
//                            if (ImGui::Selectable(Plugins_items[i].Name.c_str(), Plugins_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                            {
//                                for (auto& item : Plugins_items) item.selected = false;
//                                Plugins_items[i].selected = true;
//                            }
//                            ImGui::TableNextColumn();
//                            ImGui::Text(Plugins_items[i].author.c_str());
//                            ImGui::TableNextColumn();
//                            ImGui::Text(Plugins_items[i].version.c_str());
//                            ImGui::TableNextColumn();
//                            ImGui::Text(Plugins_items[i].Entry.c_str());
//                            ImGui::PopID();
//                        }
//                    }
//                    ImGui::EndTable();
//
//                    if (ImGui::BeginPopupContextItem("Plugins_Menu"))
//                    {
//                        if (ImGui::MenuItem("Load Plugins"))
//                        {
//                            for (size_t i = 0; i < Plugins_items.size(); ++i)
//                            {
//                                if (Plugins_items[i].selected == true)
//                                {
//                                    HMODULE PlginsEntryFile = LoadLibraryA(Plugins_items[i].Entry.c_str());
//                                    if (PlginsEntryFile == NULL)
//                                    {
//                                        printf("Load Entry %s Failed Error Code = %d\n", Plugins_items[i].Entry.c_str(), GetLastError());
//                                    }
//                                    else
//                                    {
//                                        typedef void(__stdcall* SKT64_PluginsEntryFunc)(HANDLE);
//                                        SKT64_PluginsEntryFunc SKT64_PluginsEntrys = reinterpret_cast<SKT64_PluginsEntryFunc>(
//                                            GetProcAddress(PlginsEntryFile, "SKT64_PluginsEntry")
//                                            );
//                                        SKT64_PluginsEntrys(hDevice);
//                                        FreeLibrary(PlginsEntryFile);
//                                    }
//                                }
//                            }
//                        }
//                        ImGui::EndPopup();
//                    }
//                    ImGui::EndTabItem();
//                }
//                if (ImGui::BeginTabItem("Sandbox"))
//                {
//                    static char ExecuteFilePath[MAX_PATH] = "C:\\Test.exe";
//                    if (ImGui::Checkbox("Enable Sandbox", &sandboxinited))
//                    {
//                        if (sandboxinited == true)
//                        {
//                            DeviceIoControl(hDevice, IOCTL_Enbale_SandBox, NULL, 0, NULL, 0, NULL, NULL);
//                        }
//                        else
//                        {
//                            DeviceIoControl(hDevice, IOCTL_Disable_SandBox, NULL, 0, NULL, 0, NULL, NULL);
//                        }
//                    }
//                    ImGui::Text("WARNING: This sandbox is not completely secure");
//                    ImGui::Text("Only the system can be protected from being breached");
//                    ImGui::Text("It doesn't keep your data safe");
//                    ImGui::Text("Execute File Path");
//                    ImGui::InputText("##path", ExecuteFilePath, IM_ARRAYSIZE(ExecuteFilePath));
//                    if (ImGui::Button("Create Process"))
//                    {
//                        STARTUPINFOA si = { sizeof(si) };
//                        PROCESS_INFORMATION pi;
//                        ZeroMemory(&si, sizeof(si));
//                        si.cb = sizeof(si);
//                        ZeroMemory(&pi, sizeof(pi));
//                        if (!CreateProcessA(
//                            ExecuteFilePath,
//                            NULL,
//                            NULL,
//                            NULL,
//                            FALSE,
//                            0,
//                            NULL,
//                            NULL,
//                            &si,
//                            &pi)
//                            )
//                        {
//                            std::cerr << "CreateProcess failed (" << GetLastError() << ")." << std::endl;
//                        }
//                        SetSandboxPID(pi.dwProcessId);
//                        CloseHandle(pi.hProcess);
//                        CloseHandle(pi.hThread);
//                    }
//                    ImGui::EndTabItem();
//                }
//                if (ImGui::BeginTabItem("MISC"))
//                {
//                    NoExit = 0;
//                    NoEnableHvm = 0;
//                    NoEnableMonitor = 0;
//                    static bool Prohibitcreateproc = false;
//                    static bool Prohibitcreatefile = false;
//                    static bool Prohibitloaddriver = false;
//                    static bool Prohibitmodifyregistry = false;
//                    static bool Prohibitmodifybsector = false;
//                    static bool disableptmnotify = false;
//                    static bool disableobcallback = false;
//                    static bool disablecmcallback = false;
//                    static bool disabledse = false;
//                    static bool prohibitudrv = false;
//                    if (ImGui::BeginTabBar("MISCTab"))
//                    {
//                        if (ImGui::BeginTabItem("System-Setting"))
//                        {
//                            if (ImGui::Checkbox("Prohibit Create Process", &Prohibitcreateproc))
//                            {
//                                if (Prohibitcreateproc == true)
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Prohibit_CreateProcess, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                                else
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_UnProhibit_CreateProcess, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                            }
//                            if (ImGui::Checkbox("Prohibit Load Driver", &Prohibitloaddriver))
//                            {
//                                if (Prohibitloaddriver == true)
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Prohibit_LoadDriver, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                                else
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_UnProhibit_LoadDriver, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                            }
//                            if (ImGui::Checkbox("Prohibit Create File", &Prohibitcreatefile))
//                            {
//                                if (Prohibitcreatefile == true)
//                                {
//                                    isprohibitcreatefileenableed = TRUE;
//                                    DeviceIoControl(hDevice, IOCTL_Prohibit_CreateFile, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                                else
//                                {
//                                    isprohibitcreatefileenableed = FALSE;
//                                    DeviceIoControl(hDevice, IOCTL_UnProhibit_CreateFile, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                            }
//                            if (ImGui::Checkbox("Prohibit Modify Registry", &Prohibitmodifyregistry))
//                            {
//                                if (Prohibitmodifyregistry == true)
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Prohibit_Modify_Registry, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                                else
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_UnProhibit_Modify_Registry, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                            }
//                            if (ImGui::Checkbox("Prohibit Modify Disk Boot Sector", &Prohibitmodifybsector))
//                            {
//                                if (Prohibitmodifybsector == true)
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Protect_Disk, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                                else
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_UnProtect_Disk, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                            }
//                            if (ImGui::Checkbox("Prohibit Unload Driver(PatchGuard WARNING)", &prohibitudrv))
//                            {
//                                if (prohibitudrv == true)
//                                {
//                                    BOOL status = DeviceIoControl(hDevice, IOCTL_Prohibit_UnloadDriver, NULL, 0, NULL, 0, NULL, NULL);
//                                    if (!status)
//                                    {
//                                        printf("Failed To Set Prohibit Unload Driver %d\n", GetLastError());
//                                        prohibitudrv = false;
//                                    }
//                                }
//                                else
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_UnProhibit_UnloadDriver, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                            }
//                            if (ImGui::Checkbox("Disabled Thread/Process/Module Notify(PatchGuard WARNING)", &disableptmnotify))
//                            {
//                                if (disableptmnotify == true)
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Disable_Notifys, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                                else
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Enable_Notifys, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                            }
//                            if (ImGui::Checkbox("Disabled ObCallback(PatchGuard WARNING)", &disableobcallback))
//                            {
//                                if (disableobcallback == true)
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Disable_ObCallback, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                                else
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Enable_ObCallback, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                            }
//                            if (ImGui::Checkbox("Disabled Registry Callback(PatchGuard WARNING)", &disablecmcallback))
//                            {
//                                if (disablecmcallback == true)
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Disable_CmpCallback, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                                else
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Enable_CmpCallback, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                            }
//                            if (ImGui::Checkbox("Disabled Driver Signature Enforcement(PatchGuard WARNING)", &disabledse))
//                            {
//                                if (disabledse == true)
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Disable_DSE, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                                else
//                                {
//                                    DeviceIoControl(hDevice, IOCTL_Enable_DSE, NULL, 0, NULL, 0, NULL, NULL);
//                                }
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("System-Power"))
//                        {
//                            static int PowerType = 0;
//                            ImGui::RadioButton("ShutDown", &PowerType, 0);
//                            ImGui::RadioButton("REBOOT", &PowerType, 1);
//                            ImGui::RadioButton("Force REBOOT", &PowerType, 2);
//                            ImGui::RadioButton("Blue Screen", &PowerType, 3);
//                            if (ImGui::Button("Set Power"))
//                            {
//                                int result = MessageBoxW(NULL, L"Are you sure you want to set it up?", L"POWER", MB_YESNO | MB_ICONQUESTION);
//
//                                if (result == IDYES)
//                                {
//                                    switch (PowerType)
//                                    {
//                                    case 0:
//                                    {
//                                        DeviceIoControl(hDevice, IOCTL_ShutDown, NULL, 0, NULL, 0, NULL, NULL);
//                                        break;
//                                    }
//                                    case 1:
//                                    {
//                                        DeviceIoControl(hDevice, IOCTL_REBOOT, NULL, 0, NULL, 0, NULL, NULL);
//                                        break;
//                                    }
//                                    case 2:
//                                    {
//                                        DeviceIoControl(hDevice, IOCTL_FORCE_REBOOT, NULL, 0, NULL, 0, NULL, NULL);
//                                        break;
//                                    }
//                                    case 3:
//                                    {
//                                        DeviceIoControl(hDevice, IOCTL_BLUESCREEN, NULL, 0, NULL, 0, NULL, NULL);
//                                        break;
//                                    }
//                                    default:
//                                        break;
//                                    }
//                                }
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("AdvancedSystemCrash"))
//                        {
//                            static int CrashColor = 0;
//                            ImGui::Text("Select Color");
//                            ImGui::RadioButton("Red", &CrashColor, CrashColor_Red);
//                            ImGui::RadioButton("Green", &CrashColor, CrashColor_Green);
//                            ImGui::RadioButton("Blue", &CrashColor, CrashColor_Blue);
//                            ImGui::RadioButton("Yellow", &CrashColor, CrashColor_Yellow);
//                            ImGui::RadioButton("Cyan", &CrashColor, CrashColor_Cyan);
//                            ImGui::RadioButton("Magenta", &CrashColor, CrashColor_Magenta);
//                            ImGui::RadioButton("Black", &CrashColor, CrashColor_Black);
//                            ImGui::RadioButton("White", &CrashColor, CrashColor_White);
//                            ImGui::RadioButton("Orange", &CrashColor, CrashColor_Orange);
//                            ImGui::RadioButton("Purple", &CrashColor, CrashColor_Purple);
//                            ImGui::RadioButton("Pink", &CrashColor, CrashColor_Pink);
//                            ImGui::RadioButton("Grey", &CrashColor, CrashColor_Grey);
//                            ImGui::RadioButton("Brown", &CrashColor, CrashColor_Brown);
//                            ImGui::RadioButton("Gold", &CrashColor, CrashColor_Gold);
//                            ImGui::RadioButton("Silver", &CrashColor, CrashColor_Silver);
//                            if (ImGui::Button("Crash"))
//                            {
//                                int result = MessageBoxW(NULL, L"Are you sure you want to set it up?", L"Crash", MB_YESNO | MB_ICONQUESTION);
//
//                                if (result == IDYES)
//                                {
//                                    struct input {
//                                        ULONG color;
//                                    };
//                                    input inputs = { 0 };
//                                    inputs.color = CrashColor;
//                                    BOOL status = DeviceIoControl(hDevice, IOCTL_CRASH_SYSTEM_SET_COLOR, &inputs, sizeof(input), 0, 0, 0, NULL);
//                                    if (!status) {
//                                        printf("Failed. Error %ld\n", GetLastError());
//                                    }
//                                }
//                            }
//                            ImGui::EndTabItem();
//                        }
//                        if (ImGui::BeginTabItem("Setting"))
//                        {
//                            static bool enableaavs = false;
//                            static bool SelfProtect = false;
//                            if (ImGui::Checkbox("Show Exited/Hidden Process", &EnableShowhideproc))
//                            {
//                                Process_items.clear();
//                                Process_items.shrink_to_fit();
//                                EnumProcess();
//                            }
//                            if (ImGui::Checkbox("Enable Disable More AntiVirus(Tools->MISC)", &enableaavs))
//                            {
//                                if (enableaavs == true)
//                                {
//                                    EnableAAV = TRUE;
//                                }
//                                else
//                                {
//                                    EnableAAV = FALSE;
//                                }
//                            }
//                            if (ImGui::Checkbox("Enable Self Process Protection", &SelfProtect))
//                            {
//                                if (SelfProtect == true)
//                                {
//                                    SetPPL(GetCurrentProcessId(), PP_System);
//                                }
//                                else
//                                {
//                                    SetPPL(GetCurrentProcessId(), PPL_NONE);
//                                }
//                            }
//                            ImGui::Checkbox("System-Monitor Monitor File Operation", &System_Monitor_ShowFileRW);
//                            ImGui::EndTabItem();
//                        }
//                        ImGui::EndTabBar();
//                    }
//
//                    ImGui::EndTabItem();
//                }
//                if (ImGui::BeginTabItem("EXIT"))
//                {
//                    if (NoExit == NULL)
//                    {
//                        if (ExitArk() == 0)
//                        {
//                            if (hThread == NULL)
//                            {
//                            }
//                            else
//                            {
//                                TerminateThread(hThread, NULL);
//                            }
//                            if (MonitorEnabled == 100)
//                            {
//                                DeviceIoControl(hDevice, IOCTL_Disable_Monitor, NULL, 0, NULL, 0, NULL, NULL);
//                            }
//                            Sleep(100);
//                            if (isprohibitcreatefileenableed == TRUE)
//                            {
//                                DeviceIoControl(hDevice, IOCTL_UnProhibit_CreateFile, NULL, 0, NULL, 0, NULL, NULL);
//                            }
//                            if (sandboxinited == true)
//                            {
//                                DeviceIoControl(hDevice, IOCTL_Disable_SandBox, NULL, 0, NULL, 0, NULL, NULL);
//                            }
//                            CloseHandle(hDevice);
//                            if (ISUnload == NULL)
//                            {
//                                DriverUnload("SKT64-Kernel-Driver");
//                            }
//                            MainWindow = false;
//                            return 0;
//                        }
//                        else
//                        {
//                            NoExit = 100;
//                        }
//                    }
//                    ImGui::EndTabItem();
//                }
//
//                ImGui::EndTabBar();
//            }
//
//            ImGui::End();
//        }
//        if (PPLWindow)
//        {
//            ImGui::Begin("Set PPL", &PPLWindow);
//            static int PPLType = 0;
//            ImGui::Text("Please Select PPL Type");
//            ImGui::Text("Target Process = %d", attacttoeditprocesspid);
//            ImGui::RadioButton("PP_System", &PPLType, 0);
//            ImGui::RadioButton("PP_WinTcb", &PPLType, 1);
//            ImGui::RadioButton("PP_Windows", &PPLType, 2);
//            ImGui::RadioButton("PP_Lsa", &PPLType, 3);
//            ImGui::RadioButton("PP_Antimalware", &PPLType, 4);
//            ImGui::RadioButton("PP_CodeGen", &PPLType, 5);
//            ImGui::RadioButton("PP_AuthentiCode", &PPLType, 6);
//            ImGui::RadioButton("PPL_System", &PPLType, 7);
//            ImGui::RadioButton("PPL_WinTcb", &PPLType, 8);
//            ImGui::RadioButton("PPL_Windows", &PPLType, 9);
//            ImGui::RadioButton("PPL_Lsa", &PPLType, 10);
//            ImGui::RadioButton("PPL_Antimalware", &PPLType, 11);
//            ImGui::RadioButton("PPL_CodeGen", &PPLType, 12);
//            ImGui::RadioButton("PPL_AuthentiCode", &PPLType, 13);
//            ImGui::RadioButton("NONE", &PPLType, 14);
//            if (ImGui::Button("Protection"))
//            {
//                switch (PPLType)
//                {
//                case 0:
//                {
//                    SetPPL(attacttoeditprocesspid, PP_System);
//                    break;
//                }
//                case 1:
//                {
//                    SetPPL(attacttoeditprocesspid, PP_WinTcb);
//                    break;
//                }
//                case 2:
//                {
//                    SetPPL(attacttoeditprocesspid, PP_Windows);
//                    break;
//                }
//                case 3:
//                {
//                    SetPPL(attacttoeditprocesspid, PP_Lsa);
//                    break;
//                }
//                case 4:
//                {
//                    SetPPL(attacttoeditprocesspid, PP_Antimalware);
//                    break;
//                }
//                case 5:
//                {
//                    SetPPL(attacttoeditprocesspid, PP_CodeGen);
//                    break;
//                }
//                case 6:
//                {
//                    SetPPL(attacttoeditprocesspid, PP_AuthentiCode);
//                    break;
//                }
//                case 7:
//                {
//                    SetPPL(attacttoeditprocesspid, PPL_System);
//                    break;
//                }
//                case 8:
//                {
//                    SetPPL(attacttoeditprocesspid, PPL_WinTcb);
//                    break;
//                }
//                case 9:
//                {
//                    SetPPL(attacttoeditprocesspid, PPL_Windows);
//                    break;
//                }
//                case 10:
//                {
//                    SetPPL(attacttoeditprocesspid, PPL_Lsa);
//                    break;
//                }
//                case 11:
//                {
//                    SetPPL(attacttoeditprocesspid, PPL_Antimalware);
//                    break;
//                }
//                case 12:
//                {
//                    SetPPL(attacttoeditprocesspid, PPL_CodeGen);
//                    break;
//                }
//                case 13:
//                {
//                    SetPPL(attacttoeditprocesspid, PPL_AuthentiCode);
//                    break;
//                }
//                case 14:
//                {
//                    SetPPL(attacttoeditprocesspid, PPL_NONE);
//                    break;
//                }
//                default:
//                    break;
//                }
//                PPLWindow = false;
//            }
//            ImGui::End();
//        }
//        if (ProcessThreadInfoWindow)
//        {
//            char WindowName[MAX_PATH];
//            sprintf_s(WindowName, "ThreadInfo PID  = %d", attacttoeditprocesspid, ImGuiWindowFlags_NoCollapse);
//            ImGui::Begin(WindowName, &ProcessThreadInfoWindow);
//            ImGui::SetWindowSize(ImVec2(650, 350));
//            if (ImGui::BeginTable("pthrinfo_table", 5, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//            {
//                for (size_t i = 0; i < ProcessThread_items.size(); ++i) {
//                    ImGui::PushID(static_cast<int>(i));
//                    ImGui::TableNextRow();
//                    ImGui::TableNextColumn();
//                    ImGui::Text("%d", ProcessThread_items[i].TID);
//                    ImGui::TableNextColumn();
//                    ImGui::Text("%p", ProcessThread_items[i].ethread);
//                    ImGui::TableNextColumn();
//                    ImGui::Text("%p", ProcessThread_items[i].Address);
//                    ImGui::TableNextColumn();
//                    if (ImGui::Selectable(ProcessThread_items[i].Status.c_str(), ProcessThread_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                    {
//                        for (auto& item : ProcessThread_items) item.selected = false;
//                        ProcessThread_items[i].selected = true;
//                    }
//                    ImGui::TableNextColumn();
//                    ImGui::Text("%d", ProcessThread_items[i].Prioriry);
//                    ImGui::PopID();
//                }
//            }
//            ImGui::EndTable();
//
//            if (ImGui::BeginPopupContextItem("pthrinfo_Menu"))
//            {
//                if (ImGui::MenuItem("Refesh"))
//                {
//                    for (size_t i = 0; i < ProcessThread_items.size(); ++i)
//                    {
//                        if (ProcessThread_items[i].selected == true)
//                        {
//                            ProcessThread_items.clear();
//                            ProcessThread_items.shrink_to_fit();
//                            EnumProcThread(attacttoeditprocesspid);
//                        }
//                    }
//                }
//                if (ImGui::MenuItem("Terminate"))
//                {
//                    for (size_t i = 0; i < ProcessThread_items.size(); ++i)
//                    {
//                        if (ProcessThread_items[i].selected == true)
//                        {
//                            Kernel_TerminateThread(ProcessThread_items[i].TID);
//                            ProcessThread_items.erase(ProcessThread_items.begin() + i);
//                        }
//                    }
//                }
//                if (ImGui::MenuItem("Suspend"))
//                {
//                    for (size_t i = 0; i < ProcessThread_items.size(); ++i)
//                    {
//                        if (ProcessThread_items[i].selected == true)
//                        {
//                            K_SuspendThread(ProcessThread_items[i].TID);
//                        }
//                    }
//                }
//                if (ImGui::MenuItem("Resume"))
//                {
//                    for (size_t i = 0; i < ProcessThread_items.size(); ++i)
//                    {
//                        if (ProcessThread_items[i].selected == true)
//                        {
//                            K_SuspendThread(ProcessThread_items[i].TID);
//                        }
//                    }
//                }
//                ImGui::EndPopup();
//            }
//            ImGui::End();
//        }
//        if (MemoryWindow)
//        {
//            ImGui::Begin("Memory-View", &MemoryWindow);
//            ImGui::SetWindowSize(ImVec2(1000, 500));
//            static char Size[10] = "256";
//
//            ImGui::Text("Address");
//            ImGui::SameLine();
//            ImGui::InputText("##addr", editmemoryaddress, IM_ARRAYSIZE(editmemoryaddress), ImGuiInputTextFlags_CharsHexadecimal);
//
//            ImGui::SameLine();
//            ImGui::Text("Size");
//            ImGui::SameLine();
//            ImGui::InputText("##size", Size, IM_ARRAYSIZE(Size), ImGuiInputTextFlags_CharsDecimal);
//
//            if (ImGui::Button("READ"))
//            {
//                try {
//                    uint64_t addr = std::stoull(editmemoryaddress, nullptr, 16);
//                    uint32_t size = std::stoul(Size, nullptr, 10);
//
//                    if (size < 16)
//                    {
//                        size = 16;
//                    }
//
//                    if (Kernel_ReadKernelMemory(reinterpret_cast<PVOID>(addr), size)) {
//                    }
//                }
//                catch (...) {
//                    g_mem_data.clear();
//                }
//            }
//
//            ImGui::SameLine();
//            if (!g_mem_data.empty())
//            {
//                ImGui::BeginChild("##MemoryView", ImVec2(0, 0), true);
//
//                const int cols = 16;
//                ImGuiListClipper clipper;
//                clipper.Begin((g_mem_data.size() + cols - 1) / cols);
//
//                while (clipper.Step())
//                {
//                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
//                    {
//                        const int offset = row * cols;
//                        for (int col = 0; col < cols; col++)
//                        {
//                            if (offset + col >= g_mem_data.size()) break;
//
//                            ImGui::Text("0x%02X ", g_mem_data[offset + col]);
//                            if (col < cols - 1) ImGui::SameLine();
//                        }
//                    }
//                }
//
//                ImGui::EndChild();
//            }
//            else
//            {
//                ImGui::Text("No data available");
//            }
//            ImGui::End();
//        }
//        if (ASMMemoryWindow)
//        {
//            ImGui::Begin("ASM", &ASMMemoryWindow);
//            ImGui::SetWindowSize(ImVec2(1000, 500));
//            static char Size[10] = "256";
//
//            ImGui::Text("Address");
//            ImGui::SameLine();
//            ImGui::InputText("##asmaddr", editmemoryaddress, IM_ARRAYSIZE(editmemoryaddress), ImGuiInputTextFlags_CharsHexadecimal);
//
//            ImGui::SameLine();
//            ImGui::Text("Size");
//            ImGui::SameLine();
//            ImGui::InputText("##asmsize", Size, IM_ARRAYSIZE(Size), ImGuiInputTextFlags_CharsDecimal);
//
//            if (ImGui::Button("READ"))
//            {
//                try {
//                    g_mem_data.clear();
//                    g_mem_data.shrink_to_fit();
//                    Disassembly_items.clear();
//                    Disassembly_items.shrink_to_fit();
//                    uint64_t addr = std::stoull(editmemoryaddress, nullptr, 16);
//                    uint32_t size = std::stoul(Size, nullptr, 10);
//
//                    if (size < 16)
//                    {
//                        size = 16;
//                    }
//                    disamSizes = size;
//                    if (Kernel_ReadKernelMemory(reinterpret_cast<PVOID>(addr), size)) {
//                        DisassembleMemory(addr);
//                    }
//                }
//                catch (...) {
//                    g_mem_data.clear();
//                    g_mem_data.shrink_to_fit();
//                    Disassembly_items.clear();
//                    Disassembly_items.shrink_to_fit();
//                }
//            }
//            ImGui::SameLine();
//            if (!g_mem_data.empty())
//            {
//                if (ImGui::BeginTable("asmmemry_table", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//                {
//                    ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
//                    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
//                    ImGui::TableSetupColumn("ASM");
//                    ImGui::TableHeadersRow();
//                    for (size_t i = 0; i < Disassembly_items.size(); ++i) {
//                        char ASMdatas[MAX_PATH];
//                        uint64_t asmtoaddress = 0;
//                        ImGui::PushID(static_cast<int>(i));
//                        ImGui::TableNextRow();
//                        ImGui::TableSetColumnIndex(0);
//                        ImGui::Text("%d", i);
//                        ImGui::TableSetColumnIndex(1);
//                        ImGui::Text("%p", Disassembly_items[i].Address);
//                        ImGui::TableSetColumnIndex(2);
//                        if (ASM_Getasmaddr(Disassembly_items[i].DATA, asmtoaddress))
//                        {
//                            sprintf_s(ASMdatas, "%s %s(%s)", Disassembly_items[i].mnemonic.c_str(), Disassembly_items[i].DATA.c_str(), GetDriverNameByAddress((PVOID)asmtoaddress));
//                        }
//                        else
//                        {
//                            sprintf_s(ASMdatas, "%s %s", Disassembly_items[i].mnemonic.c_str(), Disassembly_items[i].DATA.c_str());
//                        }
//                        if (ImGui::Selectable(ASMdatas, Disassembly_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                        {
//                            for (auto& item : Disassembly_items) item.selected = false;
//                            Disassembly_items[i].selected = true;
//                        }
//                        ImGui::PopID();
//                    }
//                }
//                ImGui::EndTable();
//                if (ImGui::BeginPopupContextItem("ASM_Menu"))
//                {
//                    uint64_t Disassemblytoaddress = 0;
//                    for (size_t i = 0; i < Disassembly_items.size(); ++i)
//                    {
//                        if (Disassembly_items[i].selected == true)
//                        {
//                            if (ASM_Getasmaddr(Disassembly_items[i].DATA, Disassemblytoaddress))
//                            {
//                                if (ImGui::MenuItem("Disassembly Target Address"))
//                                {
//                                    sprintf_s(editmemoryaddress, "%p", Disassemblytoaddress);
//                                    g_mem_data.clear();
//                                    g_mem_data.shrink_to_fit();
//                                    Disassembly_items.clear();
//                                    Disassembly_items.shrink_to_fit();
//                                    if (Kernel_ReadKernelMemory(reinterpret_cast<PVOID>(Disassemblytoaddress), disamSizes)) {
//                                        DisassembleMemory(Disassemblytoaddress);
//                                    }
//                                }
//                            }
//                        }
//                    }
//                    if (ImGui::MenuItem("Copy"))
//                    {
//                        for (size_t i = 0; i < Disassembly_items.size(); ++i)
//                        {
//                            if (Disassembly_items[i].selected == true)
//                            {
//                                char copyASMdatas[MAX_PATH];
//                                sprintf_s(copyASMdatas, "%s %s", Disassembly_items[i].mnemonic.c_str(), Disassembly_items[i].DATA.c_str());
//                                copyToClipboard(copyASMdatas);
//                            }
//                        }
//                    }
//                    ImGui::EndPopup();
//                }
//            }
//            else
//            {
//                ImGui::Text("No data available");
//            }
//            ImGui::End();
//        }
//        if (Process_SettingWindow)
//        {
//            ImGui::Begin("Setting", NULL);
//            ImGui::Checkbox("Show Exited/Hidden Process", &EnableShowhideproc);
//            if (ImGui::Button("Save"))
//            {
//                Process_items.clear();
//                Process_items.shrink_to_fit();
//                EnumProcess();
//                Process_SettingWindow = false;
//            }
//            ImGui::End();
//        }
//        if (ProcessInjectDllWindow)
//        {
//            ImGui::Begin("InjectDll", &ProcessInjectDllWindow);
//            ImGui::SetWindowSize(ImVec2(400, 135));
//            ImGui::Text("PID = %d",attacttoeditprocesspid);
//            ImGui::Text("Dll File Path");
//            static char dllPath[MAX_PATH] = "C:\\Test.dll";
//            ImGui::InputText("         ", dllPath, IM_ARRAYSIZE(dllPath));
//            WCHAR wsdllpath[MAX_PATH];
//            charToWCHAR(dllPath, wsdllpath);
//            static int Mode = 0;
//            ImGui::RadioButton("Create Thread Mode", &Mode, 0);
//            ImGui::SameLine();
//            ImGui::RadioButton("Ldr Mode", &Mode, 1);
//            if (ImGui::Button("Inject"))
//            {
//                if (Mode == 0)
//                {
//                    std::string strpath(dllPath);
//                    std::string fullstrpath = "\\??\\" + strpath;
//                    charToWCHAR(fullstrpath.c_str(), wsdllpath);
//                    Kernel_InjectDll_Thread(attacttoeditprocesspid, wsdllpath);
//                }
//                else if (Mode == 1)
//                {
//                    Kernel_InjectDll(attacttoeditprocesspid, wsdllpath);
//                }
//                ProcessInjectDllWindow = false;
//            }
//            ImGui::End();
//        }
//        if (DriverModifyBaseWindow)
//        {
//            ImGui::Begin("Modify Image Base", &DriverModifyBaseWindow);
//            ImGui::SetWindowSize(ImVec2(400, 135));
//            ImGui::Text("Image Base = %p", attacttoeditdriverbase);
//            ImGui::Text("New Image Base");
//            static char NewImageBase[17] = "FFFFF00000000000";
//            ImGui::InputText("           ", NewImageBase, IM_ARRAYSIZE(NewImageBase));
//            if (ImGui::Button("Edit"))
//            {
//                std::string str2(NewImageBase);
//                unsigned long long address = 0;
//
//                try {
//                    address = std::stoull(str2, nullptr, 16);
//                }
//                catch (const std::invalid_argument& e) {
//                    std::cerr << "Invalid argument: " << e.what() << std::endl;
//                }
//                catch (const std::out_of_range& e) {
//                    std::cerr << "Out of range: " << e.what() << std::endl;
//                }
//                printf("New Image base = %p\n", address);
//                ModifyDriverBase((PVOID)attacttoeditdriverbase, (PVOID)address);
//                DriverModifyBaseWindow = false;
//            }
//            ImGui::End();
//        }
//        if (DriverMajorInfoWindow)
//        {
//            ImGui::Begin("Kernel-Module Major Functions", &DriverMajorInfoWindow, ImGuiWindowFlags_NoCollapse);
//            ImGui::SetWindowSize(ImVec2(650, 350));
//            if (ImGui::BeginTable("drvmajor_table", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_NoHostExtendX))
//            {
//                for (size_t i = 0; i < DriverMajorInfo_items.size(); ++i) {
//                    ImGui::PushID(static_cast<int>(i));
//                    ImGui::TableNextRow();
//                    ImGui::TableNextColumn();
//                    ImGui::Text(DriverMajorInfo_items[i].Name.c_str());
//                    ImGui::TableNextColumn();
//                    ImGui::Text("%p", DriverMajorInfo_items[i].Address);
//                    ImGui::TableNextColumn();
//                    if (ImGui::Selectable(DriverMajorInfo_items[i].Module.c_str(), DriverMajorInfo_items[i].selected, ImGuiSelectableFlags_SpanAllColumns))
//                    {
//                        for (auto& item : DriverMajorInfo_items) item.selected = false;
//                        DriverMajorInfo_items[i].selected = true;
//                    }
//                    ImGui::PopID();
//                }
//            }
//            ImGui::EndTable();
//
//            if (ImGui::BeginPopupContextItem("drvmajor_Menu"))
//            {
//                if (ImGui::MenuItem("View Memory"))
//                {
//                    for (size_t i = 0; i < DriverMajorInfo_items.size(); ++i)
//                    {
//                        if (DriverMajorInfo_items[i].selected == true)
//                        {
//                            sprintf_s(editmemoryaddress, "%p", DriverMajorInfo_items[i].Address);
//                            MemoryWindow = true;
//                        }
//                    }
//                }
//                ImGui::EndPopup();
//            }
//            ImGui::End();
//        }
//        if (DebugWindow)
//        {
//            ImGui::Begin("Debug", NULL);
//            if (ImGui::Button("test"))
//            {
//                while (TRUE)
//                {
//                    RefeshAll();
//                }
//            }
//            ImGui::End();
//        }
//        if (ModifyTokenWindow)
//        {
//            ImGui::Begin("Token", &ModifyTokenWindow);
//            ImGui::SetWindowSize(ImVec2(400, 210));
//            static int TokenType = 0;
//            ImGui::Text("PID = %d", attacttoeditprocesspid);
//            ImGui::Text("Select Token");
//            ImGui::RadioButton("Current", &TokenType, 0);
//            ImGui::RadioButton("WINDOW MANAGER\\DWM", &TokenType, 1);
//            ImGui::RadioButton("NT AUTHORITY\\SYSTEM", &TokenType, 2);
//            ImGui::RadioButton("NT SERVICE\\TrustedInstaller", &TokenType, 3);
//            ImGui::RadioButton("UMDF", &TokenType, 4);
//            if (ImGui::Button("WRITE"))
//            {
//                switch (TokenType)
//                {
//                case 0:
//                {
//                    break;
//                }
//                case 1:
//                {
//                    ModifyToken(attacttoeditprocesspid, MToken_DWM);
//                    break;
//                }
//                case 2:
//                {
//                    ModifyToken(attacttoeditprocesspid, MToken_System);
//                    break;
//                }
//                case 3:
//                {
//                    ModifyToken(attacttoeditprocesspid, MToken_TrustedInstaller);
//                    break;
//                }
//                case 4:
//                {
//                    ModifyToken(attacttoeditprocesspid, MToken_UMDF);
//                    break;
//                }
//                default:
//                    break;
//                }
//                ModifyTokenWindow = false;
//            }
//            ImGui::End();
//        }
//
//
//        ImGui::Render();
//        int display_w, display_h;
//        glfwGetFramebufferSize(window, &display_w, &display_h);
//        glViewport(0, 0, display_w, display_h);
//        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
//        glClear(GL_COLOR_BUFFER_BIT);
//        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
//
//        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
//        {
//            GLFWwindow* backup_current_context = glfwGetCurrentContext();
//            ImGui::UpdatePlatformWindows();
//            ImGui::RenderPlatformWindowsDefault();
//            glfwMakeContextCurrent(backup_current_context);
//        }
//
//        glfwSwapBuffers(window);
//    }
//#ifdef __EMSCRIPTEN__
//    EMSCRIPTEN_MAINLOOP_END;
//#endif
//
//    ImGui_ImplOpenGL3_Shutdown();
//    ImGui_ImplGlfw_Shutdown();
//    ImGui::DestroyContext();
//
//    glfwDestroyWindow(window);
//    glfwTerminate();
//
//    return 0;
//}
