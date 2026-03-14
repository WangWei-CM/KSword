#include "Taskbar.h"
#include <QtWidgets/QApplication>

int main(int argc, char* argv[])
{
    QCoreApplication::setAttribute(Qt::AA_UseOpenGLES);
    QApplication app(argc, argv);
    Taskbar window;
    window.show();
    return app.exec();
}
