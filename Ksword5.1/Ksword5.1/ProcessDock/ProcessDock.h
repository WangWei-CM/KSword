#pragma once
#include <QWidget>

#include "../ui/UI_All.h"

#include "Process_Tool.h"

class ProcessDock : public QWidget
{
    Q_OBJECT
public:
    explicit ProcessDock(QWidget* parent = nullptr);
};
