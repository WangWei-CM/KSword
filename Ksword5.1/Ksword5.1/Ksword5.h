#pragma once

#include <QtWidgets/QMainWindow>
#include "ui_Ksword5.h"

class Ksword5 : public QMainWindow
{
    Q_OBJECT

public:
    Ksword5(QWidget *parent = nullptr);
    ~Ksword5();

private:
    Ui::Ksword5Class ui;
};

