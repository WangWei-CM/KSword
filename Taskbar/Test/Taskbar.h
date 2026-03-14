#ifndef TASKBAR_H
#define TASKBAR_H

#include <QMainWindow>

class SpectrumWidget;
class AudioSpectrumAnalyzer;

class Taskbar : public QMainWindow
{
    Q_OBJECT

public:
    explicit Taskbar(QWidget* parent = nullptr);
    ~Taskbar();

private:
    SpectrumWidget* m_spectrumWidget = nullptr;
    AudioSpectrumAnalyzer* m_analyzer = nullptr;
};

#endif // TASKBAR_H