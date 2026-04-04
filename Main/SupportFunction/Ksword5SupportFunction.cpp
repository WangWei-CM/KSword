#include "../KswordTotalHead.h"

// 初始化静态成员
std::vector<WorkItem> WorkProgressManager::ProcessList;
std::vector<WorkItemUI> WorkProgressManager::ProcessUI;
std::vector<int> WorkProgressManager::ShowUI;
std::vector<int> WorkProgressManager::UIreturnValue;
std::mutex WorkProgressManager::data_mutex;
HWND KswordInitLigoWindowHwnd = nullptr;
bool DeleteReleasedGUIINIFile() {
    // 获取当前程序执行目录
    char szExePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szExePath, MAX_PATH) == 0) {
        return false; // 获取路径失败
    }

    // 提取目录路径
    char* pLastSlash = strrchr(szExePath, '\\');
    if (pLastSlash == NULL) {
        return false; // 路径格式错误
    }
    *pLastSlash = '\0'; // 截断为目录路径

    // 构建INI文件完整路径
    std::string strIniPath = std::string(szExePath) + "\\KswordGUI.ini";

    // 检查文件是否存在
    if (GetFileAttributesA(strIniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false; // 文件不存在
    }

    // 删除文件
    if (DeleteFileA(strIniPath.c_str())) {
        return true; // 删除成功
    }
    else {
        return false; // 删除失败
    }
}
bool DeleteReleasedD3DX9DLLFile() {
    // 获取当前程序执行目录
    char szExePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szExePath, MAX_PATH) == 0) {
        return false; // 获取路径失败
    }

    // 提取目录路径
    char* pLastSlash = strrchr(szExePath, '\\');
    if (pLastSlash == NULL) {
        return false; // 路径格式错误
    }
    *pLastSlash = '\0'; // 截断为目录路径

    // 构建INI文件完整路径
    std::string strIniPath = std::string(szExePath) + "\\d3dx9_43.dll";

    // 检查文件是否存在
    if (GetFileAttributesA(strIniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false; // 文件不存在
    }

    // 删除文件
    if (DeleteFileA(strIniPath.c_str())) {
        return true; // 删除成功
    }
    else {
        return false; // 删除失败
    }
}

// 宽字符版本（支持Unicode路径）
bool DeleteReleasedIniFileW() {
    WCHAR szExePath[MAX_PATH];
    if (GetModuleFileNameW(NULL, szExePath, MAX_PATH) == 0) {
        return false;
    }

    WCHAR* pLastSlash = wcsrchr(szExePath, L'\\');
    if (pLastSlash == NULL) {
        return false;
    }
    *pLastSlash = L'\0';

    std::wstring strIniPath = std::wstring(szExePath) + L"\\KswordGUI.ini";

    if (GetFileAttributesW(strIniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    return DeleteFileW(strIniPath.c_str()) ? true : false;
}


bool ExtractGUIINIResourceToFile()
{
    // 获取当前程序执行目录
    char szExePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szExePath, MAX_PATH) == 0)
        return false;

    // 提取目录路径
    char* pLastSlash = strrchr(szExePath, '\\');
    if (pLastSlash == NULL)
        return false;
    *pLastSlash = '\0';

    // 构建目标文件路径
    std::string strOutputPath = std::string(szExePath) + "\\KswordGUI.ini";

    // 检查文件是否已存在，存在则直接返回true
    if (GetFileAttributesA(strOutputPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;

    // 查找资源
    HRSRC hResource = FindResource(NULL, MAKEINTRESOURCE(IDR_INI1), L"INI");
    if (hResource == NULL)
        return false;

    // 加载资源
    HGLOBAL hLoadedResource = LoadResource(NULL, hResource);
    if (hLoadedResource == NULL)
        return false;

    // 锁定资源并获取数据指针
    LPVOID pResourceData = LockResource(hLoadedResource);
    if (pResourceData == NULL)
        return false;

    // 获取资源大小
    DWORD dwResourceSize = SizeofResource(NULL, hResource);
    if (dwResourceSize == 0)
        return false;

    // 写入文件
    std::ofstream outFile(strOutputPath, std::ios::binary);
    if (!outFile)
        return false;

    outFile.write(static_cast<const char*>(pResourceData), dwResourceSize);
    outFile.close();

    return true;
}
int WorkProgressManager::AddProcess(WorkItem item) {
    std::lock_guard<std::mutex> lock(data_mutex);
    ProcessList .push_back(item);
    ProcessUI   .push_back(WorkItemUI());            // 添加默认UI结构
    ShowUI      .push_back(0);                       // 初始不显示UI
    UIreturnValue.push_back(0);                      // 初始返回值0
    return static_cast<int>(ProcessList.size() - 1); // 返回新添加的索引
                                                     // 以后通过[pid]直接访问数组
}
int WorkProgressManager::AddProcess(std::string Name,std::string StepName,bool* cancel,float Progress) {
    std::lock_guard<std::mutex> lock(data_mutex);
    WorkItem temp;
    temp.canceled = cancel;
    temp.currentStep = StepName;
    temp.name = Name;
    temp.progress = Progress;
    ProcessList.push_back(temp);
    ProcessUI.push_back(WorkItemUI());            // 添加默认UI结构
    ShowUI.push_back(0);                       // 初始不显示UI
    UIreturnValue.push_back(0);                      // 初始返回值0
    return static_cast<int>(ProcessList.size() - 1); // 返回新添加的索引
    // 以后通过[pid]直接访问数组
}


void WorkProgressManager::SetProcess(int pid,WorkItem item) {
    std::lock_guard<std::mutex> lock(data_mutex);
    ProcessList[pid] = item;
}

void WorkProgressManager::SetProcess(int pid, std::string StepName, float Progress)

{
    std::lock_guard<std::mutex> lock(data_mutex);
    ProcessList[pid].currentStep = StepName;
    ProcessList[pid].progress = Progress;
}

int WorkProgressManager::UI(int pid, WorkItemUI ui) {

    ShowUI[pid] = 1;
    ProcessUI[pid] = ui;
    while (UIreturnValue[pid] == 0)
    {
        Sleep(10);
    }
    ShowUI[pid] = 0;
    int returnValue = UIreturnValue[pid];
    UIreturnValue[pid] = 0;
    return returnValue;
}


int WorkProgressManager::UI(int pid, std::string Info, int OperateNum) {
    //std::lock_guard<std::mutex> lock(data_mutex);
    ShowUI[pid] = 1;
    ProcessUI[pid].Info = Info;
    ProcessUI[pid].OperateNum = OperateNum;
    while (UIreturnValue[pid] == 0)
    {
        Sleep(10);
    }
    ShowUI[pid] = 0;
    int returnValue = UIreturnValue[pid];
    UIreturnValue[pid] = 0;
    return returnValue;
}

void WorkProgressManager::Render() {

    bool showWorkWindow = 0;
    //std::lock_guard<std::mutex> lock(data_mutex);
    for (size_t i = 0; i < ProcessList.size(); ++i) {
        if (ProcessList[i].progress != 1.0f)showWorkWindow = 1;
    }
    if (showWorkWindow) {
        ImGui::Begin(C("任务进度监控"), nullptr, ImGuiWindowFlags_AlwaysAutoResize |ImGuiWindowFlags_NoDocking);

        for (size_t i = 0; i < ProcessList.size(); ++i) {
            if (ProcessList[i].progress == 1.0f)continue;
            ImGui::PushID(static_cast<int>(i));

            // 显示任务信息
            ImGui::Text(C("正在执行: "));
            ImGui::SameLine();
            ImGui::Text(ProcessList[i].currentStep.c_str());
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.8f, 0.4f, 1.0f));
            ImGui::ProgressBar(ProcessList[i].progress, ImVec2(200, 20));
            ImGui::PopStyleColor();

            // 检查是否需要显示UI
            if (ShowUI[i]) {
                //ImGui::Separator();
                ImGui::Text("%s", ProcessUI[i].Info.c_str());

                // 渲染操作按钮
                for (int btn = 0; btn < ProcessUI[i].OperateNum; ++btn) {
                    if (ImGui::Button((C("操作") + std::to_string(btn + 1)).c_str())) {
                        UIreturnValue[i] = btn + 1; // 设置返回值
                        ShowUI[i] = false;          // 隐藏UI
                    }
                    if (btn < ProcessUI[i].OperateNum - 1) ImGui::SameLine();
                }
                //ImGui::Separator();
            }
            ImGui::PopID();
            ImGui::Spacing();
        }
        ImGui::End();
    }
}

// 函数声明
/*---------------------------------------
   BITBLT.C -- BitBlt Demonstration
               (c) Charles Petzold, 1998
  ---------------------------------------*/

#include <windows.h>

LRESULT CALLBACK KswordLogoInitWndProc(HWND, UINT, WPARAM, LPARAM);

int showInitLogoWindow(HINSTANCE hInstance, HINSTANCE hPrevInstance,
    LPTSTR szCmdLine, int iCmdShow)
{
    static TCHAR szAppName[] = TEXT("BitBlt");
    HWND         hwnd;
    MSG          msg;
    WNDCLASS     wndclass;

    wndclass.style = CS_HREDRAW | CS_VREDRAW;
    wndclass.lpfnWndProc = KswordLogoInitWndProc;
    wndclass.cbClsExtra = 0;
    wndclass.cbWndExtra = 0;
    wndclass.hInstance = hInstance;
    wndclass.hIcon = LoadIcon(NULL, IDI_INFORMATION);
    wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
    wndclass.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wndclass.lpszMenuName = NULL;
    wndclass.lpszClassName = szAppName;

    if (!RegisterClass(&wndclass))
    {
        DWORD errorCode = GetLastError();  // 获取系统错误代码
        TCHAR errorMsg[256] = { 0 };
        // 格式化错误信息（包含错误代码）
        wsprintf(errorMsg, TEXT("窗口类注册失败！\n错误代码: %lu\nThis program requires Windows NT!"), errorCode);
        // 弹窗显示错误信息
        MessageBox(NULL, errorMsg, szAppName, MB_ICONERROR);
        return 0;
    }
    hwnd = CreateWindowEx(
        WS_EX_LAYERED,                  // 启用分层窗口，支持透明
        szAppName,
        TEXT("KswordInitLogoWindow"),
        WS_POPUP | WS_VISIBLE,          // 弹出式窗口（无标题栏），直接显示
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,                 // 初始尺寸暂设为0，后续按位图调整
        NULL, NULL, hInstance, NULL
    );
	KswordInitLigoWindowHwnd = hwnd; // 保存窗口句柄

    ShowWindow(hwnd, iCmdShow);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return msg.wParam;
}
LRESULT CALLBACK KswordLogoInitWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    static HBITMAP hBitmap;  // 位图句柄（静态存储以在消息间共享）
    static int     cxBitmap, cyBitmap;  // 位图宽度和高度
    static HINSTANCE hInstance;  // 实例句柄

    switch (message)
    {
    case WM_CREATE:
    {  // 添加作用域括号
        // 获取实例句柄（从创建参数中提取）
        hInstance = ((LPCREATESTRUCT)lParam)->hInstance;

        // 从资源加载位图
        hBitmap = (HBITMAP)LoadImage(
            hInstance,
            MAKEINTRESOURCE(IDB_BITMAP1),  // 位图资源ID
            IMAGE_BITMAP,
            0, 0,  // 使用位图原始尺寸
            LR_DEFAULTCOLOR
        );

        // 检查加载是否成功
        if (hBitmap == NULL)
        {
            MessageBox(hwnd, TEXT("无法加载位图资源！"), TEXT("错误"), MB_ICONERROR);
            return -1;  // 加载失败，终止窗口创建
        }
        BITMAP bm;
        GetObject(hBitmap, sizeof(BITMAP), &bm);
        cxBitmap = bm.bmWidth;
        cyBitmap = bm.bmHeight;

        // 调整窗口大小以适应位图
        RECT rect = { 0, 0, cxBitmap, cyBitmap };
        AdjustWindowRect(&rect, GetWindowLong(hwnd, GWL_STYLE), FALSE);
        // 获取位图尺寸
        // 计算窗口宽高（调整后的客户区大小 + 边框）
        int windowWidth = rect.right - rect.left;
        int windowHeight = rect.bottom - rect.top;

        // 获取屏幕尺寸
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);  // 屏幕宽度
        int screenHeight = GetSystemMetrics(SM_CYSCREEN); // 屏幕高度

        // 计算居中位置（屏幕中心 - 窗口一半大小）
        int xPos = (screenWidth - windowWidth) / 2;
        int yPos = (screenHeight - windowHeight) / 2;

        // 设置窗口位置和大小（居中显示）
        SetWindowPos(
            hwnd, NULL,
            xPos, yPos,  // 居中坐标
            windowWidth, windowHeight,  // 调整后的窗口大小
            SWP_NOZORDER | SWP_FRAMECHANGED  // 不改变Z轴顺序，刷新边框
        );        SetLayeredWindowAttributes(
            hwnd,
            RGB(0, 255, 0),  // 透明色：#00FF00（纯绿色）
            0,               // 透明度（0-255，此处不用，设为0）
            LWA_COLORKEY      // 透明模式：按颜色键（指定颜色透明）
        );
        return 0;
    }  // 作用域结束

    case WM_PAINT:
    {  // 添加作用域括号
        PAINTSTRUCT ps;
        HDC hdcClient = BeginPaint(hwnd, &ps);

        // 创建内存设备上下文用于绘制位图
        HDC hdcMem = CreateCompatibleDC(hdcClient);
        HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

        // 绘制位图到客户区（居中显示）
        int x = (ps.rcPaint.right - ps.rcPaint.left - cxBitmap) / 2;
        int y = (ps.rcPaint.bottom - ps.rcPaint.top - cyBitmap) / 2;
        BitBlt(
            hdcClient,
            x, y,                // 目标位置（居中）
            cxBitmap, cyBitmap,  // 绘制尺寸（位图原始尺寸）
            hdcMem,              // 源设备上下文（内存DC）
            0, 0,                // 源位图起始位置
            SRCCOPY              // 复制模式
        );

        // 清理资源
        SelectObject(hdcMem, hOldBitmap);  // 恢复原始位图
        DeleteDC(hdcMem);                  // 删除内存DC
        EndPaint(hwnd, &ps);
        return 0;
    }  // 作用域结束

    case WM_DESTROY:
    {  // 添加作用域括号（即使暂时无变量，也建议统一风格）
        // 释放位图资源
        if (hBitmap != NULL)
            DeleteObject(hBitmap);
        PostQuitMessage(0);
        return 0;
    }  // 作用域结束
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

