#include "Ksword5.h"
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    Ksword5 window;
    window.show();
    return app.exec();
}
