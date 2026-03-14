QT += widgets opengl openglwidgets
win32:LIBS += -lopengl32
CONFIG += release c++20

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
