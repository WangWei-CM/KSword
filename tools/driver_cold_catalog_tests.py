"""Compile actual Driver catalog/provider/navigation over QtCore value-only fake tabs."""
import argparse,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--catalog-header',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
s=a.source.read_text(encoding='utf-8-sig');begin=s.index('namespace\n{\n    struct DriverNavigationEntry');end=s.index('void DriverDock::changeEvent',begin);production=s[begin:end]
fixture=r"""
#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QPointer>
#include <QVector>
#include <functional>
#include <memory>
#include <iterator>
#include <cstdio>
#include <cstdlib>
int checks=0;void check(bool ok){++checks;if(!ok){std::fprintf(stderr,"FAIL %d\n",checks);std::abort();}}
class QWidget:public QObject{public:using QObject::QObject;};class QIcon{};
class QTabWidget:public QWidget {public:using QWidget::QWidget;struct Tab{QPointer<QWidget>page;QString title,tip;bool visible=true,enabled=true;};QVector<Tab>tabs;int active=0;std::function<void(int)>changed;int addTab(QWidget*page,const QIcon&,const QString&title){tabs.push_back({page,title,{}});return tabs.size()-1;}int indexOf(QWidget*page)const{for(int i=0;i<tabs.size();++i)if(tabs[i].page==page)return i;return -1;}bool isTabVisible(int i)const{return tabs[i].visible;}bool isTabEnabled(int i)const{return tabs[i].enabled;}QString tabText(int i)const{return tabs[i].title;}QString tabToolTip(int i)const{return tabs[i].tip;}int currentIndex()const{return active;}QWidget* currentWidget()const{return active>=0&&active<tabs.size()?tabs[active].page.data():nullptr;}void setCurrentIndex(int i){active=i;if(changed)changed(i);}};
class KernelThreadAuditTab:public QWidget{public:using QWidget::QWidget;};
"""
fixture+='\n#include "'+str(a.catalog_header.resolve()).replace('\\','/')+'"\n'
fixture+=r"""
QString language=QStringLiteral("zh");int constructions=0;QString driverText(const char*key,const QString&source){return language+QLatin1Char('|')+QString::fromLatin1(key)+QLatin1Char('|')+source;}
class DriverDock:public QWidget {public:
 QTabWidget*m_tabWidget=nullptr,*m_objectDetailTabWidget=nullptr;QWidget*m_servicePage=nullptr,*m_kernelModulePage=nullptr,*m_operatePage=nullptr,*m_debugOutputPage=nullptr,*m_objectInfoPage=nullptr,*m_driverObjectPage=nullptr,*m_deviceObjectPage=nullptr,*m_driverExtensionPage=nullptr,*m_majorFunctionPage=nullptr,*m_fastIoPage=nullptr,*m_moduleCrossViewPage=nullptr,*m_integrityPage=nullptr,*m_unloadedPiddbPage=nullptr,*m_kswordSelfDriverPage=nullptr;KernelThreadAuditTab*m_systemThreadAuditTab=nullptr;
 explicit DriverDock(bool attach=true);static QString navigationTitle(const char*);int addNavigationPage(QTabWidget*,QWidget*,const char*,const QIcon&);QWidget*navigationPage(const QString&)const;static ks::ui::PageNavigationCatalog navigationCatalog(const DriverDock*existing=nullptr);QWidget*activateNavigationDescriptor(const QString&);
};
"""
fixture+=production
fixture+=r"""
DriverDock::DriverDock(bool attach){++constructions;m_tabWidget=new QTabWidget(this);m_objectDetailTabWidget=new QTabWidget(this);QWidget**pageSlots[]={&m_servicePage,&m_kernelModulePage,&m_operatePage,&m_debugOutputPage,&m_objectInfoPage,&m_driverObjectPage,&m_deviceObjectPage,&m_driverExtensionPage,&m_majorFunctionPage,&m_fastIoPage,&m_moduleCrossViewPage,&m_integrityPage,&m_unloadedPiddbPage};for(auto slot:pageSlots)*slot=new QWidget(this);m_systemThreadAuditTab=new KernelThreadAuditTab(this);m_kswordSelfDriverPage=new QWidget(this);for(const auto&entry:DriverNavigationEntries){if(!attach&&QString::fromLatin1(entry.id)==QStringLiteral("self_driver"))continue;auto*tabs=*entry.parent?m_objectDetailTabWidget:m_tabWidget;check(addNavigationPage(tabs,navigationPage(QString::fromLatin1(entry.id)),entry.id,{})>=0);}}
int main(int argc,char**argv){QCoreApplication app(argc,argv);auto cold=DriverDock::navigationCatalog();check(cold.size()==15&&constructions==0);check(DriverNavigationRegistered&&!ks::ui::RegisteredPageNavigationCatalogSearchComplete(QStringLiteral("driver")));auto registered=ks::ui::RegisteredPageNavigationCatalog(QStringLiteral("driver"),nullptr);check(registered&&registered->size()==15&&constructions==0);auto resolved=ks::ui::ResolvePageNavigationCatalog(cold,QStringLiteral("Driver"));check(resolved&&resolved->size()==15);for(const auto&page:cold)check(page.visible&&page.enabled&&!page.current);const auto zh=cold[0].title;language=QStringLiteral("en");check(DriverDock::navigationCatalog()[0].title!=zh&&constructions==0);
 auto* dock=new DriverDock;check(dock->activateNavigationDescriptor(QStringLiteral("objects.fastio"))==dock->m_fastIoPage);check(dock->m_tabWidget->currentWidget()==dock->m_objectInfoPage&&dock->m_objectDetailTabWidget->currentWidget()==dock->m_fastIoPage);auto warm=ks::ui::ResolvePageNavigationCatalog(DriverDock::navigationCatalog(dock),QStringLiteral("Driver"));bool current=false;for(const auto&entry:*warm)if(entry.page.id==QStringLiteral("objects.fastio"))current=entry.page.current;check(current);check(dock->activateNavigationDescriptor(QStringLiteral("unknown"))==nullptr);
 const int parent=dock->m_tabWidget->indexOf(dock->m_objectInfoPage);dock->m_tabWidget->tabs[parent].enabled=false;check(dock->activateNavigationDescriptor(QStringLiteral("objects.major"))==nullptr);dock->m_tabWidget->tabs[parent].enabled=true;const int fast=dock->m_objectDetailTabWidget->indexOf(dock->m_fastIoPage);dock->m_objectDetailTabWidget->tabs[fast].visible=false;check(dock->activateNavigationDescriptor(QStringLiteral("objects.fastio"))==nullptr);dock->m_objectDetailTabWidget->tabs[fast].visible=true;
 dock->m_tabWidget->changed=[&](int){dock->m_objectDetailTabWidget->tabs[fast].enabled=false;};check(dock->activateNavigationDescriptor(QStringLiteral("objects.fastio"))==nullptr);dock->m_tabWidget->changed={};dock->m_objectDetailTabWidget->tabs[fast].enabled=true;
 dock->m_objectDetailTabWidget->changed=[&](int){dock->m_tabWidget->setCurrentIndex(0);};check(dock->activateNavigationDescriptor(QStringLiteral("objects.fastio"))==nullptr);dock->m_objectDetailTabWidget->changed={};delete dock;
 dock=new DriverDock(false);auto absent=DriverDock::navigationCatalog(dock);check(!absent.last().visible&&!absent.last().enabled);check(dock->activateNavigationDescriptor(QStringLiteral("self_driver"))==nullptr);delete dock;
 dock=new DriverDock;auto*victim=dock;dock->m_tabWidget->changed=[&](int){delete dock;dock=nullptr;};check(victim->activateNavigationDescriptor(QStringLiteral("objects.fastio"))==nullptr&&dock==nullptr);
 dock=new DriverDock;dock->m_objectDetailTabWidget->changed=[&](int){delete dock->m_fastIoPage;};check(dock->activateNavigationDescriptor(QStringLiteral("objects.fastio"))==nullptr);delete dock;
 dock=new DriverDock;dock->m_objectDetailTabWidget->changed=[&](int){delete dock->m_objectDetailTabWidget;};check(dock->activateNavigationDescriptor(QStringLiteral("objects.fastio"))==nullptr);delete dock;
 std::printf("driver_cold_catalog: %d checks PASS coldNoConstruct=PASS QObjectFakeTabs=only realWidgets=0 backend=0\n",checks);return 0;}
"""
source=a.out/'driver_catalog.cpp';source.write_text(fixture,encoding='utf-8');qt=Path('D:/Software/Qt/6.9.3/msvc2022_64');exe=a.out/'driver_catalog.exe';cmd=a.out/'test.cmd';lines=['@echo off','call "D:\\Software\\VS\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64','if errorlevel 1 exit /b %errorlevel%',f'cl.exe /nologo /std:c++20 /Zc:__cplusplus /EHsc /utf-8 /W4 /WX /MD /I"{qt / "include"}" /I"{qt / "include/QtCore"}" "{source.resolve()}" /Fo"{(a.out / "test.obj").resolve()}" /Fe"{exe.resolve()}" /link /LIBPATH:"{qt / "lib"}" Qt6Core.lib','if errorlevel 1 exit /b %errorlevel%',f'set "PATH={qt / "bin"};%PATH%"',f'"{exe.resolve()}"'];cmd.write_text('\n'.join(lines)+'\n',encoding='utf-8');result=subprocess.run(['cmd.exe','/d','/c',str(cmd.resolve())],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace');(a.out/'test.log').write_text(result.stdout,encoding='utf-8');print(result.stdout);raise SystemExit(result.returncode)
