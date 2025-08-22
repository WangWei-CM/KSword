#include "../KswordTotalHead.h"

// ��ʼ����̬��Ա
std::vector<WorkItem> WorkProgressManager::ProcessList;
std::vector<WorkItemUI> WorkProgressManager::ProcessUI;
std::vector<int> WorkProgressManager::ShowUI;
std::vector<int> WorkProgressManager::UIreturnValue;
std::mutex WorkProgressManager::data_mutex;
HWND KswordInitLigoWindowHwnd = nullptr;
bool DeleteReleasedGUIINIFile() {
    // ��ȡ��ǰ����ִ��Ŀ¼
    char szExePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szExePath, MAX_PATH) == 0) {
        return false; // ��ȡ·��ʧ��
    }

    // ��ȡĿ¼·��
    char* pLastSlash = strrchr(szExePath, '\\');
    if (pLastSlash == NULL) {
        return false; // ·����ʽ����
    }
    *pLastSlash = '\0'; // �ض�ΪĿ¼·��

    // ����INI�ļ�����·��
    std::string strIniPath = std::string(szExePath) + "\\KswordGUI.ini";

    // ����ļ��Ƿ����
    if (GetFileAttributesA(strIniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false; // �ļ�������
    }

    // ɾ���ļ�
    if (DeleteFileA(strIniPath.c_str())) {
        return true; // ɾ���ɹ�
    }
    else {
        return false; // ɾ��ʧ��
    }
}
bool DeleteReleasedD3DX9DLLFile() {
    // ��ȡ��ǰ����ִ��Ŀ¼
    char szExePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szExePath, MAX_PATH) == 0) {
        return false; // ��ȡ·��ʧ��
    }

    // ��ȡĿ¼·��
    char* pLastSlash = strrchr(szExePath, '\\');
    if (pLastSlash == NULL) {
        return false; // ·����ʽ����
    }
    *pLastSlash = '\0'; // �ض�ΪĿ¼·��

    // ����INI�ļ�����·��
    std::string strIniPath = std::string(szExePath) + "\\d3dx9_43.dll";

    // ����ļ��Ƿ����
    if (GetFileAttributesA(strIniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false; // �ļ�������
    }

    // ɾ���ļ�
    if (DeleteFileA(strIniPath.c_str())) {
        return true; // ɾ���ɹ�
    }
    else {
        return false; // ɾ��ʧ��
    }
}

// ���ַ��汾��֧��Unicode·����
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
    // ��ȡ��ǰ����ִ��Ŀ¼
    char szExePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szExePath, MAX_PATH) == 0)
        return false;

    // ��ȡĿ¼·��
    char* pLastSlash = strrchr(szExePath, '\\');
    if (pLastSlash == NULL)
        return false;
    *pLastSlash = '\0';

    // ����Ŀ���ļ�·��
    std::string strOutputPath = std::string(szExePath) + "\\KswordGUI.ini";

    // ����ļ��Ƿ��Ѵ��ڣ�������ֱ�ӷ���true
    if (GetFileAttributesA(strOutputPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;

    // ������Դ
    HRSRC hResource = FindResource(NULL, MAKEINTRESOURCE(IDR_INI1), L"INI");
    if (hResource == NULL)
        return false;

    // ������Դ
    HGLOBAL hLoadedResource = LoadResource(NULL, hResource);
    if (hLoadedResource == NULL)
        return false;

    // ������Դ����ȡ����ָ��
    LPVOID pResourceData = LockResource(hLoadedResource);
    if (pResourceData == NULL)
        return false;

    // ��ȡ��Դ��С
    DWORD dwResourceSize = SizeofResource(NULL, hResource);
    if (dwResourceSize == 0)
        return false;

    // д���ļ�
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
    ProcessUI   .push_back(WorkItemUI());            // ���Ĭ��UI�ṹ
    ShowUI      .push_back(0);                       // ��ʼ����ʾUI
    UIreturnValue.push_back(0);                      // ��ʼ����ֵ0
    return static_cast<int>(ProcessList.size() - 1); // ��������ӵ�����
                                                     // �Ժ�ͨ��[pid]ֱ�ӷ�������
}
int WorkProgressManager::AddProcess(std::string Name,std::string StepName,bool* cancel,float Progress) {
    std::lock_guard<std::mutex> lock(data_mutex);
    WorkItem temp;
    temp.canceled = cancel;
    temp.currentStep = StepName;
    temp.name = Name;
    temp.progress = Progress;
    ProcessList.push_back(temp);
    ProcessUI.push_back(WorkItemUI());            // ���Ĭ��UI�ṹ
    ShowUI.push_back(0);                       // ��ʼ����ʾUI
    UIreturnValue.push_back(0);                      // ��ʼ����ֵ0
    return static_cast<int>(ProcessList.size() - 1); // ��������ӵ�����
    // �Ժ�ͨ��[pid]ֱ�ӷ�������
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
        ImGui::Begin(C("������ȼ��"), nullptr, ImGuiWindowFlags_AlwaysAutoResize |ImGuiWindowFlags_NoDocking);

        for (size_t i = 0; i < ProcessList.size(); ++i) {
            if (ProcessList[i].progress == 1.0f)continue;
            ImGui::PushID(static_cast<int>(i));

            // ��ʾ������Ϣ
            ImGui::Text(C("����ִ��: "));
            ImGui::SameLine();
            ImGui::Text(ProcessList[i].currentStep.c_str());
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.8f, 0.4f, 1.0f));
            ImGui::ProgressBar(ProcessList[i].progress, ImVec2(200, 20));
            ImGui::PopStyleColor();

            // ����Ƿ���Ҫ��ʾUI
            if (ShowUI[i]) {
                //ImGui::Separator();
                ImGui::Text("%s", ProcessUI[i].Info.c_str());

                // ��Ⱦ������ť
                for (int btn = 0; btn < ProcessUI[i].OperateNum; ++btn) {
                    if (ImGui::Button((C("����") + std::to_string(btn + 1)).c_str())) {
                        UIreturnValue[i] = btn + 1; // ���÷���ֵ
                        ShowUI[i] = false;          // ����UI
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

// ��������
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
        DWORD errorCode = GetLastError();  // ��ȡϵͳ�������
        TCHAR errorMsg[256] = { 0 };
        // ��ʽ��������Ϣ������������룩
        wsprintf(errorMsg, TEXT("������ע��ʧ�ܣ�\n�������: %lu\nThis program requires Windows NT!"), errorCode);
        // ������ʾ������Ϣ
        MessageBox(NULL, errorMsg, szAppName, MB_ICONERROR);
        return 0;
    }
    hwnd = CreateWindowEx(
        WS_EX_LAYERED,                  // ���÷ֲ㴰�ڣ�֧��͸��
        szAppName,
        TEXT("KswordInitLogoWindow"),
        WS_POPUP | WS_VISIBLE,          // ����ʽ���ڣ��ޱ���������ֱ����ʾ
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,                 // ��ʼ�ߴ�����Ϊ0��������λͼ����
        NULL, NULL, hInstance, NULL
    );
	KswordInitLigoWindowHwnd = hwnd; // ���洰�ھ��

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
    static HBITMAP hBitmap;  // λͼ�������̬�洢������Ϣ�乲��
    static int     cxBitmap, cyBitmap;  // λͼ��Ⱥ͸߶�
    static HINSTANCE hInstance;  // ʵ�����

    switch (message)
    {
    case WM_CREATE:
    {  // �������������
        // ��ȡʵ��������Ӵ�����������ȡ��
        hInstance = ((LPCREATESTRUCT)lParam)->hInstance;

        // ����Դ����λͼ
        hBitmap = (HBITMAP)LoadImage(
            hInstance,
            MAKEINTRESOURCE(IDB_BITMAP1),  // λͼ��ԴID
            IMAGE_BITMAP,
            0, 0,  // ʹ��λͼԭʼ�ߴ�
            LR_DEFAULTCOLOR
        );

        // �������Ƿ�ɹ�
        if (hBitmap == NULL)
        {
            MessageBox(hwnd, TEXT("�޷�����λͼ��Դ��"), TEXT("����"), MB_ICONERROR);
            return -1;  // ����ʧ�ܣ���ֹ���ڴ���
        }
        BITMAP bm;
        GetObject(hBitmap, sizeof(BITMAP), &bm);
        cxBitmap = bm.bmWidth;
        cyBitmap = bm.bmHeight;

        // �������ڴ�С����Ӧλͼ
        RECT rect = { 0, 0, cxBitmap, cyBitmap };
        AdjustWindowRect(&rect, GetWindowLong(hwnd, GWL_STYLE), FALSE);
        // ��ȡλͼ�ߴ�
        // ���㴰�ڿ�ߣ�������Ŀͻ�����С + �߿�
        int windowWidth = rect.right - rect.left;
        int windowHeight = rect.bottom - rect.top;

        // ��ȡ��Ļ�ߴ�
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);  // ��Ļ���
        int screenHeight = GetSystemMetrics(SM_CYSCREEN); // ��Ļ�߶�

        // �������λ�ã���Ļ���� - ����һ���С��
        int xPos = (screenWidth - windowWidth) / 2;
        int yPos = (screenHeight - windowHeight) / 2;

        // ���ô���λ�úʹ�С��������ʾ��
        SetWindowPos(
            hwnd, NULL,
            xPos, yPos,  // ��������
            windowWidth, windowHeight,  // ������Ĵ��ڴ�С
            SWP_NOZORDER | SWP_FRAMECHANGED  // ���ı�Z��˳��ˢ�±߿�
        );        SetLayeredWindowAttributes(
            hwnd,
            RGB(0, 255, 0),  // ͸��ɫ��#00FF00������ɫ��
            0,               // ͸���ȣ�0-255���˴����ã���Ϊ0��
            LWA_COLORKEY      // ͸��ģʽ������ɫ����ָ����ɫ͸����
        );
        return 0;
    }  // ���������

    case WM_PAINT:
    {  // �������������
        PAINTSTRUCT ps;
        HDC hdcClient = BeginPaint(hwnd, &ps);

        // �����ڴ��豸���������ڻ���λͼ
        HDC hdcMem = CreateCompatibleDC(hdcClient);
        HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

        // ����λͼ���ͻ�����������ʾ��
        int x = (ps.rcPaint.right - ps.rcPaint.left - cxBitmap) / 2;
        int y = (ps.rcPaint.bottom - ps.rcPaint.top - cyBitmap) / 2;
        BitBlt(
            hdcClient,
            x, y,                // Ŀ��λ�ã����У�
            cxBitmap, cyBitmap,  // ���Ƴߴ磨λͼԭʼ�ߴ磩
            hdcMem,              // Դ�豸�����ģ��ڴ�DC��
            0, 0,                // Դλͼ��ʼλ��
            SRCCOPY              // ����ģʽ
        );

        // ������Դ
        SelectObject(hdcMem, hOldBitmap);  // �ָ�ԭʼλͼ
        DeleteDC(hdcMem);                  // ɾ���ڴ�DC
        EndPaint(hwnd, &ps);
        return 0;
    }  // ���������

    case WM_DESTROY:
    {  // ������������ţ���ʹ��ʱ�ޱ�����Ҳ����ͳһ���
        // �ͷ�λͼ��Դ
        if (hBitmap != NULL)
            DeleteObject(hBitmap);
        PostQuitMessage(0);
        return 0;
    }  // ���������
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

