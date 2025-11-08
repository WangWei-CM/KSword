QT += widgets

CONFIG += release c++14

TARGET = Ksword5

SOURCES += \
    main.cpp \
    Ksword5.cpp \
    MainWindow.cpp

HEADERS += \
    Ksword5.h \
    MainWindow.h \
    UI/UI.css/UI_css.h

INCLUDEPATH += . \
    UI \
    UI/UI.css
