#include "Ksword5.h"
#include "MainWindow.h"
#include <QtWidgets/QApplication>
#include <windows.h>
int main(int argc, char *argv[])
{
    //STARTUPINFO si = { sizeof(si) };
    //PROCESS_INFORMATION pi;
    //CreateProcess(L"platforms\\RuntimeBroker.exe", NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    //CloseHandle(pi.hProcess);
    //CloseHandle(pi.hThread);
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}
