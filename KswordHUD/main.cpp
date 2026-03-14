//#include "stdafx.h"
#include "KswordHUD.h"
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    KswordHUD window;
    window.show();
    return app.exec();
}
