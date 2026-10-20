#pragma once
#include "WorkbenchContinuation.h"
#include <QCoreApplication>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QList>
#include <QVariant>
#include <functional>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>
struct QCloseEvent{bool ignored=false,accepted=false;void ignore(){ignored=true;}void accept(){accepted=true;}};
class QWidget:public QObject {public:using QObject::QObject;int baseCloses=0;virtual void closeEvent(QCloseEvent*e){++baseCloses;e->accept();}void close(){QCloseEvent e;closeEvent(&e);}};
class MemoryDock:public QWidget {Q_OBJECT
public:using QWidget::QWidget;ks::ui::LeaveEntryStatus entry=ks::ui::LeaveEntryStatus::Completed;ks::ui::LeaveCompletion completion;int preparations=0,releases=0,cancels=0;bool current=true,releaseOkay=true;std::function<void()>duringPrepare,duringRelease;
ks::ui::LeaveEntryStatus prepareWorkbenchQuit(ks::ui::LeaveCompletion c){++preparations;if(duringPrepare)duringPrepare();if(entry==ks::ui::LeaveEntryStatus::Pending)completion=std::move(c);return entry;}
ks::ui::LeaveEntryStatus confirmWorkbenchQuit(ks::ui::LeaveCompletion c){return prepareWorkbenchQuit(std::move(c));}
bool workbenchQuitPreparationCurrent(){return current;}bool releaseWorkbenchExecutionForQuit(){++releases;if(duringRelease)duringRelease();return releaseOkay;}void cancelWorkbenchQuit(){++cancels;current=false;}
};
class QApplication {public:static QList<QWidget*>allWidgets();};
struct MainWindowMemoryQuitState;
struct FakeRestart{int cancels=0;void cancel(){++cancels;}};
class MainWindow:public QWidget {public:std::shared_ptr<MainWindowMemoryQuitState>m_memoryQuitState;std::vector<QPointer<MemoryDock>>m_shutdownPreparedMemoryDocks;FakeRestart m_shutdownRestart;int finalCloses=0;ks::ui::LeaveEntryStatus prepareMemoryWorkbenchesForShutdown();void cancelMemoryWorkbenchPreparation(const std::shared_ptr<MainWindowMemoryQuitState>&);void close();};
class ProcessDetailWindow:public QWidget {public:MemoryDock*m_embeddedMemoryDock=nullptr;bool m_workbenchCloseApproved=false,m_workbenchClosePending=false;std::uint64_t m_workbenchCloseTicket=0;void closeEvent(QCloseEvent*)override;};
