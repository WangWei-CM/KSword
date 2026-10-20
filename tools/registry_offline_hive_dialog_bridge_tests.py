"""Compile actual offline Hive worker with memory file/registry adapters, never real Hive I/O."""
import argparse,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--header',type=Path,required=True);p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--ui-dir',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
fixture=r"""
#include <Windows.h>
#include <QCoreApplication>
#include <QPointer>
#include <deque>
#include <QThreadPool>
#include <QTimer>
#include <QElapsedTimer>
#include <QThread>
#include <condition_variable>
#include <thread>
#include <vector>
#include <atomic>
#include "AsyncOperation.h"
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QMap>
#include <QVector>
#include <QStringList>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <source_location>
int checks=0;void check(bool ok,std::source_location location=std::source_location::current()){++checks;if(!ok){std::fprintf(stderr,"FAIL %d line %u\n",checks,location.line());std::abort();}}
QMap<QString,QByteArray> mockFiles;QStringList trace;bool copyAllowed=true,writeAllowed=true,commitAllowed=true,readAllowed=true;int removeFailures=0; qint64 maxWrite=0;int tempCounter=0;
QString canonical(const QString&p){return QDir::cleanPath(p);}
struct MockInfo {QString p;explicit MockInfo(const QString&v):p(canonical(v)){}bool exists()const{return mockFiles.contains(p);}bool isFile()const{return exists();}QString canonicalFilePath()const{return exists()?p:QString();}QString absoluteFilePath()const{return p;}};
struct MockFile {QString p;QByteArray bytes;qsizetype at=0;bool errorFlag=false;explicit MockFile(const QString&v):p(canonical(v)){}static bool copy(const QString&from,const QString&to){trace<<QStringLiteral("copy");if(!copyAllowed||!mockFiles.contains(from)||mockFiles.contains(to))return false;mockFiles[to]=mockFiles[from];return true;}bool open(QIODevice::OpenMode){if(!readAllowed||!mockFiles.contains(p))return false;bytes=mockFiles[p];return true;}bool atEnd()const{return at>=bytes.size();}QByteArray read(qint64 n){trace<<QStringLiteral("read-block");auto b=bytes.mid(at,n);at+=b.size();return b;}QFileDevice::FileError error()const{return errorFlag?QFileDevice::ReadError:QFileDevice::NoError;}void close(){trace<<QStringLiteral("input-close");}};
struct MockSave {QString p;QByteArray bytes;explicit MockSave(const QString&v):p(canonical(v)){}void setDirectWriteFallback(bool enabled){check(!enabled);}bool open(QIODevice::OpenMode){trace<<QStringLiteral("output-open");return writeAllowed;}qint64 write(const char*data,qint64 n){if(!writeAllowed)return -1;const auto accepted=maxWrite?std::min(n,maxWrite):n;bytes.append(data,accepted);return accepted;}bool commit(){trace<<QStringLiteral("commit");if(!commitAllowed)return false;mockFiles[p]=bytes;return true;}void cancelWriting(){trace<<QStringLiteral("cancel");}};
struct MockTemp {QString root;bool autoRemove=true;explicit MockTemp(const QString&){root=QStringLiteral("/mocktemp/%1").arg(++tempCounter);}bool isValid()const{return true;}QString filePath(const QString&p)const{return root+QLatin1Char('/')+p;}void setAutoRemove(bool value){autoRemove=value;}bool remove(){trace<<QStringLiteral("remove-temp");if(removeFailures){--removeFailures;return false;}for(auto it=mockFiles.begin();it!=mockFiles.end();)if(it.key().startsWith(root+QLatin1Char('/')))it=mockFiles.erase(it);else++it;return true;}~MockTemp(){if(autoRemove)remove();}};
struct NativeValue{DWORD type=REG_BINARY;QByteArray raw;DWORD advertised=0;};QMap<QString,NativeValue> mockValues;QVector<QString> mockKeys;QMap<quintptr,QString> handles;quintptr handleSequence=100;bool mockLink=false;LSTATUS loadStatus=ERROR_SUCCESS,flushStatus=ERROR_SUCCESS,closeStatus=ERROR_SUCCESS;int mutations=0;DWORD childCount=0,keyValueCount=0;bool corruptReadback=false;bool throwReadback=false;bool denyWrites=false;REGSAM lastOpenAccess=0;QString loadedPath;
HKEY rootHandle(){return reinterpret_cast<HKEY>(1);}QString nativeName(LPCWSTR p){return QString::fromWCharArray(p);}
LSTATUS mockLoad(LPCWSTR p,PHKEY out,REGSAM access,DWORD options,DWORD zero){check(access==(KEY_READ|KEY_WRITE)&&options==REG_PROCESS_APPKEY&&zero==0);loadedPath=nativeName(p);
check(loadedPath!=QStringLiteral("/source.hive"));trace<<QStringLiteral("load");if(loadStatus!=ERROR_SUCCESS)return loadStatus;*out=rootHandle();return ERROR_SUCCESS;}
LSTATUS mockClose(HKEY key){if(key==rootHandle()){trace<<QStringLiteral("close-root");return closeStatus;}check(handles.remove(reinterpret_cast<quintptr>(key))==1);return ERROR_SUCCESS;}
LSTATUS mockFlush(HKEY key){check(key==rootHandle());trace<<QStringLiteral("flush");return flushStatus;}
LSTATUS mockOpen(HKEY parent,LPCWSTR name,DWORD options,REGSAM access,PHKEY out){lastOpenAccess=access;if(denyWrites&&(access&KEY_SET_VALUE))return ERROR_ACCESS_DENIED;check(options==REG_OPTION_OPEN_LINK);
check(parent==rootHandle()||handles.contains(reinterpret_cast<quintptr>(parent)));const QString prefix=parent==rootHandle()?QString():handles[reinterpret_cast<quintptr>(parent)];const QString part=nativeName(name);QString path=prefix.isEmpty()?part:prefix+(part.isEmpty()?QString():QLatin1Char('\\')+part);*out=reinterpret_cast<HKEY>(++handleSequence);handles[handleSequence]=path;return ERROR_SUCCESS;}
LSTATUS mockQuery(HKEY,LPCWSTR name,LPDWORD,LPDWORD type,LPBYTE output,LPDWORD size){const auto n=nativeName(name);if(n==QStringLiteral("SymbolicLinkValue")){if(!mockLink)return ERROR_FILE_NOT_FOUND;*type=REG_LINK;*size=4;return ERROR_SUCCESS;}if(throwReadback&&mutations)throw std::bad_alloc();if(!mockValues.contains(n))return ERROR_FILE_NOT_FOUND;const auto&v=mockValues[n];*type=v.type;if(!output){*size=v.advertised?v.advertised:static_cast<DWORD>(v.raw.size());return ERROR_SUCCESS;}if(*size<static_cast<DWORD>(v.raw.size())){*size=static_cast<DWORD>(v.raw.size());return ERROR_MORE_DATA;}std::memcpy(output,v.raw.constData(),v.raw.size());*size=static_cast<DWORD>(v.raw.size());if(corruptReadback&&mutations>=2&&*size)output[0]^=1;return ERROR_SUCCESS;}
LSTATUS mockEnumKey(HKEY,DWORD index,LPWSTR out,LPDWORD length,LPDWORD,LPWSTR,LPDWORD,PFILETIME){if(index>=static_cast<DWORD>(mockKeys.size()))return ERROR_NO_MORE_ITEMS;auto name=mockKeys[index];
check(*length>static_cast<DWORD>(name.size()));std::memcpy(out,name.utf16(),name.size()*sizeof(wchar_t));*length=static_cast<DWORD>(name.size());return ERROR_SUCCESS;}
LSTATUS mockEnumValue(HKEY,DWORD index,LPWSTR out,LPDWORD length,LPDWORD,LPDWORD type,LPBYTE,LPDWORD size){if(index>=static_cast<DWORD>(mockValues.size()))return ERROR_NO_MORE_ITEMS;auto it=mockValues.begin();std::advance(it,index);
check(*length>static_cast<DWORD>(it.key().size()));std::memcpy(out,it.key().utf16(),it.key().size()*sizeof(wchar_t));*length=static_cast<DWORD>(it.key().size());*type=it->type;*size=it->advertised?it->advertised:static_cast<DWORD>(it->raw.size());return ERROR_SUCCESS;}
LSTATUS mockSet(HKEY,LPCWSTR name,DWORD zero,DWORD type,const BYTE*data,DWORD size){check(zero==0);++mutations;mockValues[nativeName(name)]={type,QByteArray(reinterpret_cast<const char*>(data),size),0};return ERROR_SUCCESS;}
LSTATUS mockDeleteValue(HKEY,LPCWSTR name){++mutations;return mockValues.remove(nativeName(name))?ERROR_SUCCESS:ERROR_FILE_NOT_FOUND;}
LSTATUS mockCreate(HKEY,LPCWSTR name,DWORD,LPWSTR,DWORD options,REGSAM,LPSECURITY_ATTRIBUTES,PHKEY out,LPDWORD disposition){check(options==REG_OPTION_NON_VOLATILE);auto n=nativeName(name);*disposition=mockKeys.contains(n)?REG_OPENED_EXISTING_KEY:REG_CREATED_NEW_KEY;if(*disposition==REG_CREATED_NEW_KEY){++mutations;mockKeys<<n;}*out=reinterpret_cast<HKEY>(++handleSequence);handles[handleSequence]=n;return ERROR_SUCCESS;}
LSTATUS mockInfo(HKEY,LPWSTR,LPDWORD,LPDWORD,LPDWORD children,LPDWORD,LPDWORD,LPDWORD values,LPDWORD,LPDWORD,LPDWORD,PFILETIME){*children=childCount;*values=keyValueCount;return ERROR_SUCCESS;}
LSTATUS mockDeleteKey(HKEY,LPCWSTR name,REGSAM,DWORD){++mutations;mockKeys.removeAll(nativeName(name));return ERROR_SUCCESS;}
#define QFile MockFile
#define QFileInfo MockInfo
#define QSaveFile MockSave
#define QTemporaryDir MockTemp
#define RegLoadAppKeyW mockLoad
#define RegCloseKey mockClose
#define RegFlushKey mockFlush
#define RegOpenKeyExW mockOpen
#define RegQueryValueExW mockQuery
#define RegEnumKeyExW mockEnumKey
#define RegEnumValueW mockEnumValue
#define RegSetValueExW mockSet
#define RegDeleteValueW mockDeleteValue
#define RegCreateKeyExW mockCreate
#define RegQueryInfoKeyW mockInfo
#define RegDeleteKeyExW mockDeleteKey
"""
fixture+='\n#include "'+str(a.header.resolve()).replace('\\','/')+'"\n'
fixture+=r"""
using namespace ks::registry::offline;
void reset(){check(handles.isEmpty());mockFiles.clear();mockFiles[QStringLiteral("/source.hive")]=QByteArray(2*1024*1024+9,'S');mockValues.clear();mockKeys.clear();trace.clear();loadStatus=flushStatus=closeStatus=ERROR_SUCCESS;copyAllowed=writeAllowed=commitAllowed=readAllowed=true;maxWrite=0;removeFailures=mutations=0;childCount=keyValueCount=0;mockLink=corruptReadback=throwReadback=denyWrites=false;}
Request command(Action action,quint64 generation=9){Request r;r.action=action;r.generation=generation;return r;}
void open(State&s){auto r=command(Action::Open);r.source=QStringLiteral("/source.hive");auto result=execute(s,r);
check(result.outcome==Outcome::Ok&&result.snapshot.loaded&&result.snapshot.generation==9);
check(result.snapshot.copyPath!=result.snapshot.originalPath&&mockFiles[result.snapshot.copyPath]==mockFiles[QStringLiteral("/source.hive")]);}
int workerChecks(){
 {reset();State s;open(s);auto original=mockFiles[QStringLiteral("/source.hive")];for(int i=0;i<301;++i)mockKeys<<QStringLiteral("子键 %1").arg(i);auto r=command(Action::Keys);r.path=QStringLiteral("空间\\Leaf");auto first=execute(s,r);
check(first.keys.size()==300&&first.more&&first.next==300);r.next=first.next;auto second=execute(s,r);
check(second.keys.size()==1&&!second.more&&second.next==301);
check(handles.isEmpty());mockLink=true;
check(execute(s,r).status==ERROR_ACCESS_DENIED);mockLink=false;r.path=QStringLiteral("a\\\\b");
check(execute(s,r).status==ERROR_INVALID_PARAMETER);r.path=QStringLiteral("a")+QChar(0);
check(execute(s,r).status==ERROR_INVALID_PARAMETER);r=command(Action::Keys,8);
check(execute(s,r).outcome==Outcome::Stale);
check(cleanup(s,true));
check(mockFiles[QStringLiteral("/source.hive")]==original);}
 {reset();State s;open(s);for(int i=0;i<301;++i)mockValues[QStringLiteral("value %1").arg(i)]={REG_BINARY,QByteArray(100,'x'),0};mockValues[QString()]={REG_SZ,QByteArray::fromHex("61000000"),0};mockValues[QStringLiteral("huge")]={REG_BINARY,{},MaximumValueBytes+1};mockValues[QStringLiteral("ondemand")]={REG_BINARY,QByteArray(65537,'z'),0};auto r=command(Action::Values);auto values=execute(s,r);
check(values.values.size()==300&&values.more);for(const auto&v:values.values){check(v.raw.size()<=64);if(v.name==QStringLiteral("huge"))check(v.status==ERROR_FILE_TOO_LARGE);if(v.name==QStringLiteral("ondemand"))check(v.raw.isEmpty()&&v.length==65537);}r.next=values.next;
check(execute(s,r).values.size()==4);auto read=command(Action::ReadValue);read.name=QStringLiteral("ondemand");auto full=execute(s,read);
check(full.value.raw.size()==65537&&full.value.raw==mockValues[read.name].raw);read.name.clear();
check(execute(s,read).value.raw==QByteArray::fromHex("61000000"));read.name=QStringLiteral("huge");
check(execute(s,read).status==ERROR_FILE_TOO_LARGE);
check(cleanup(s,true));}
 {reset();State s;open(s);auto create=command(Action::CreateValue);create.name=QString();create.type=REG_BINARY;create.data=QByteArray(1024*1024,'q');const auto frozen=create;create.data.clear();auto result=execute(s,frozen);
check(result.submitted&&result.verified&&result.snapshot.dirty&&mutations==1);
check(mockValues[QString()].raw==frozen.data);
check(execute(s,frozen).outcome==Outcome::Conflict&&mutations==1);auto change=command(Action::UpdateValue);change.original={QString(),REG_BINARY,frozen.data,frozen.data.size(),ERROR_SUCCESS};change.type=REG_DWORD;change.data=QByteArray::fromHex("01000000");mockValues[QString()].raw[0]='c';
check(execute(s,change).outcome==Outcome::Conflict&&mutations==1);mockValues[QString()].raw=frozen.data;corruptReadback=true;result=execute(s,change);
check(result.submitted&&!result.verified&&mutations==2);corruptReadback=false;auto del=command(Action::DeleteValue);del.original={QString(),REG_DWORD,change.data,4,ERROR_SUCCESS};result=execute(s,del);
check(result.submitted&&result.verified&&mutations==3&&!mockValues.contains(QString()));
check(cleanup(s,true));}
 {reset();State s;open(s);auto r=command(Action::CreateKey);r.name=QStringLiteral("  子键  ");auto result=execute(s,r);
check(result.submitted&&mockKeys.contains(r.name));
check(execute(s,r).outcome==Outcome::AlreadyExists&&mutations==1);r.name=QStringLiteral("a\\b");
check(execute(s,r).outcome==Outcome::InvalidName);r=command(Action::DeleteKey);r.path=QStringLiteral("parent\\  子键  ");childCount=1;
check(execute(s,r).outcome==Outcome::NotEmpty&&mutations==1);childCount=0;keyValueCount=1;
check(execute(s,r).outcome==Outcome::NotEmpty);keyValueCount=0;
check(execute(s,r).submitted&&mutations==2);
check(cleanup(s,true));}
 {reset();State s;open(s);s.snapshot.dirty=true;auto r=command(Action::SaveCopy);r.target=QStringLiteral("/source.hive");
check(execute(s,r).outcome==Outcome::InvalidTarget);r.target=s.snapshot.copyPath;
check(execute(s,r).outcome==Outcome::InvalidTarget);r.target=QStringLiteral("/saved.hive");flushStatus=ERROR_ACCESS_DENIED;
check(execute(s,r).outcome==Outcome::FlushFailed);
check(!trace.contains(QStringLiteral("close-root")));flushStatus=ERROR_SUCCESS;closeStatus=ERROR_BUSY;
check(execute(s,r).outcome==Outcome::CloseFailed);
check(!mockFiles.contains(r.target));closeStatus=ERROR_SUCCESS;trace.clear();maxWrite=7;auto result=execute(s,r);
check(result.published&&result.snapshot.loaded&&!result.snapshot.dirty&&result.reloadStatus==ERROR_SUCCESS);
check(mockFiles[r.target]==mockFiles[QStringLiteral("/source.hive")]);
check(trace.indexOf(QStringLiteral("flush"))<trace.indexOf(QStringLiteral("close-root")));
check(trace.indexOf(QStringLiteral("close-root"))<trace.indexOf(QStringLiteral("read-block")));
check(trace.indexOf(QStringLiteral("commit"))<trace.lastIndexOf(QStringLiteral("load")));
check(trace.indexOf(QStringLiteral("input-close"))<trace.lastIndexOf(QStringLiteral("load")));
check(cleanup(s,true));}
 {reset();State s;open(s);s.snapshot.dirty=true;auto r=command(Action::SaveCopy);r.target=QStringLiteral("/saved.hive");mockFiles[r.target]=QByteArray("old");commitAllowed=false;auto result=execute(s,r);
check(!result.published&&result.outcome==Outcome::SaveFailed&&result.snapshot.dirty&&result.snapshot.loaded);
check(mockFiles[r.target]==QByteArray("old"));commitAllowed=true;loadStatus=ERROR_ACCESS_DENIED;result=execute(s,r);
check(result.published&&!result.snapshot.loaded&&!result.snapshot.dirty&&result.reloadStatus==ERROR_ACCESS_DENIED);
check(cleanup(s,true));}
 {reset();State s;open(s);s.snapshot.dirty=true;auto copy=s.snapshot.copyPath;closeStatus=ERROR_BUSY;
check(!cleanup(s,false));
check(!cleanupReceipt(s).complete&&s.hive&&s.temporary);closeStatus=ERROR_SUCCESS;
check(cleanup(s,false));auto receipt=cleanupReceipt(s);
check(receipt.complete&&receipt.retained&&receipt.retainedPath==copy);
check(mockFiles.contains(copy)&&s.hive==nullptr&&!s.temporary);check(cleanup(s,false));check(cleanupReceipt(s).retained&&cleanupReceipt(s).retainedPath==copy);}
 {reset();State s;open(s);s.snapshot.dirty=true;auto copy=s.snapshot.copyPath;removeFailures=1;
check(!cleanup(s,true));
check(!s.hive&&s.temporary&&mockFiles.contains(copy));
check(cleanup(s,true));
check(!mockFiles.contains(copy)&&cleanupReceipt(s).complete);}
 check(handles.isEmpty());std::printf("offline_hive_worker: %d checks PASS; files=memory registry=fake realHive=0 GUI=0\n",checks);return 0;
}
"""
production=a.source.read_text(encoding='utf-8-sig')
def method(name):
    begin=production.index('        '+name)
    brace=production.index('{',begin);depth=1;end=brace+1
    while depth:
        if production[end]=='{':depth+=1
        elif production[end]=='}':depth-=1
        end+=1
    return production[begin:end]+'\n'
fixture+=r"""
namespace hive=ks::registry::offline;
namespace ks::ui {enum class StatusRole {Info,Warning,Success,Error};}
QString trText(const QString&s){return s;}
QString winError(LSTATUS code){return QString::number(code);}
class FakeDialogBase:public QObject {public:bool closed=false;void reject(){closed=true;}};using QDialog=FakeDialogBase;
struct QMessageBox {enum{Yes=1,No=2,Save=4,Discard=8,Cancel=16};};int closeAnswer=QMessageBox::Cancel;QString selectedSave;
struct QFileDialog {static QString getSaveFileName(QObject*,const QString&){return selectedSave;}};
struct StatusValue {QString text;};void setStatus(StatusValue* value,const QString&text,ks::ui::StatusRole){value->text=text;}
class OfflineHiveDialog final:public QDialog {public:
 struct Command {std::shared_ptr<hive::State>native;std::shared_ptr<hive::Session::Reservation>reservation;hive::Request request;};
 std::unique_ptr<ks::ui::AsyncOperation>m_operation;std::shared_ptr<hive::Session>m_session;hive::Snapshot m_snapshot;bool m_busy=false,m_receipt=false,m_projecting=false,m_closing=false,m_recoveryNeeded=false;int idle=0;bool m_closeRequested=false,m_closeApproved=false,m_discardApproved=false,m_failedOpen=false;quint64 m_reportedAttempt=0;QTimer*m_lifecycle=nullptr;StatusValue status;StatusValue*m_status=&status;std::deque<std::function<void()>>m_afterIdle;
 OfflineHiveDialog(){m_operation=std::make_unique<ks::ui::AsyncOperation>(this);m_session=hive::Session::create();m_snapshot.generation=9;m_lifecycle=new QTimer(this);m_lifecycle->setInterval(1);connect(m_lifecycle,&QTimer::timeout,this,[this]{pollCleanup();});m_lifecycle->start();m_operation->setStateChangedCallback([this]{if(m_busy&&!m_receipt&&!m_operation->isBusy())finishWithoutReceipt();});}
 ~OfflineHiveDialog()override{m_operation->close();m_session.reset();}
 bool ready()const{return m_session&&!m_busy&&!m_closing&&!m_recoveryNeeded;}bool busy()const{return m_busy||m_projecting;}hive::Request request(hive::Action action)const{hive::Request r;r.action=action;r.generation=m_snapshot.generation;return r;}void updateControls(){}static int question(QObject*,const QString&,const QString&,int=QMessageBox::Yes|QMessageBox::No,int=QMessageBox::No){return closeAnswer;}
"""
fixture+=''.join(method(x) for x in ['void finishWithoutReceipt()','bool submit(','void finishIdle()','bool nativeOk(','void saveCopy(','void requestClose()','void continueClose()','void beginCleanup(','void pollCleanup()'])
fixture+='};\n'
fixture+=r"""
void until(const std::function<bool()>&done){QElapsedTimer timer;timer.start();while(!done()&&timer.elapsed()<10000){QCoreApplication::processEvents();QThread::msleep(1);}check(done());}
std::mutex blockerMutex;std::condition_variable blockerWake;bool blockerRelease=false;std::atomic_bool blockerEntered{false};
void blockPool(){blockerRelease=false;blockerEntered.store(false);QThreadPool::globalInstance()->start(QRunnable::create([]{std::unique_lock<std::mutex>lock(blockerMutex);blockerEntered.store(true);blockerWake.wait(lock,[]{return blockerRelease;});}));until([]{return blockerEntered.load();});}
void releasePool(){{std::lock_guard<std::mutex>lock(blockerMutex);blockerRelease=true;}blockerWake.notify_all();}

void loadHost(OfflineHiveDialog*host){auto r=command(Action::Open);r.source=QStringLiteral("/source.hive");check(host->submit(r,true,[](const Result&){}));until([&]{return !host->m_busy;});}
void dirtyHost(OfflineHiveDialog*host){auto r=command(Action::CreateValue);r.name=QStringLiteral("new value");r.type=REG_BINARY;r.data=QByteArray("changed");check(host->submit(r,true,[](const Result&){}));until([&]{return !host->m_busy;});check(host->m_snapshot.dirty);}
void closeChecks(){auto& registry=ks::ui::AsyncCleanupRegistry::global();
 // A failed Open settles native cleanup, then allows a fresh working-copy epoch.
 {reset();auto* failed=new OfflineHiveDialog;loadStatus=ERROR_ACCESS_DENIED;auto r=command(Action::Open);r.source=QStringLiteral("/source.hive");check(failed->submit(r,true,[failed](const Result&result){check(result.outcome==Outcome::LoadFailed);failed->m_failedOpen=true;failed->beginCleanup(false);}));until([&]{return !failed->m_session;});check(!failed->closed&&!failed->m_recoveryNeeded&&registry.pending()==0);delete failed;loadStatus=ERROR_SUCCESS;
 auto* reopened=new OfflineHiveDialog;loadHost(reopened);check(reopened->m_snapshot.loaded);closeAnswer=QMessageBox::Discard;reopened->requestClose();until([&]{return reopened->closed;});delete reopened;}
 // Main Cancel unfreezes global admission; an old accepted epoch finishes and a new dialog can open.
 {reset();auto* canceled=new OfflineHiveDialog;loadHost(canceled);blockPool();auto r=command(Action::CreateValue);r.name=QStringLiteral("accepted-before-main-Cancel");r.data=QByteArray("retained");check(canceled->submit(r,true,[](const Result&r){check(r.submitted&&r.verified);}));registry.beginShutdown();registry.cancelShutdown();check(!canceled->m_session->reserve());releasePool();until([&]{return canceled->closed&&registry.pending()==0;});check(mockValues.contains(r.name)&&mockFiles.contains(canceled->m_snapshot.copyPath));delete canceled;
 auto* reopened=new OfflineHiveDialog;loadHost(reopened);check(reopened->m_snapshot.loaded);closeAnswer=QMessageBox::Discard;reopened->requestClose();until([&]{return reopened->closed;});delete reopened;}

 {reset();State native;open(native);denyWrites=true;auto probe=command(Action::ProbeWrite);check(execute(native,probe).status==ERROR_ACCESS_DENIED&&mutations==0&&handles.isEmpty());auto read=command(Action::ReadValue);check(execute(native,read).status==ERROR_ACCESS_DENIED);denyWrites=false;check(execute(native,probe).outcome==Outcome::Ok&&(lastOpenAccess&KEY_SET_VALUE)&&mutations==0);check(cleanup(native,true));}

 reset();auto* host=new OfflineHiveDialog;loadHost(host);dirtyHost(host);const QString copy=host->m_snapshot.copyPath;closeAnswer=QMessageBox::Cancel;host->requestClose();check(!host->m_closeRequested&&!host->closed&&host->m_session&&!host->m_session->job()->isCleanupRequested());closeAnswer=QMessageBox::Save;selectedSave.clear();host->requestClose();check(!host->m_closeRequested&&!host->closed&&host->m_snapshot.dirty);selectedSave=QStringLiteral("/saved.hive");host->requestClose();until([&]{return host->closed;});check(mockFiles.contains(selectedSave)&&!mockFiles.contains(copy)&&registry.pending()==0);delete host;
 reset();host=new OfflineHiveDialog;loadHost(host);dirtyHost(host);const auto discarded=host->m_snapshot.copyPath;closeAnswer=QMessageBox::Discard;flushStatus=ERROR_ACCESS_DENIED;host->requestClose();until([&]{return host->closed;});check(!mockFiles.contains(discarded)&&registry.pending()==0);delete host;
 reset();host=new OfflineHiveDialog;loadHost(host);dirtyHost(host);const auto retained=host->m_snapshot.copyPath;registry.beginShutdown();until([&]{return host->closed;});check(mockFiles.contains(retained)&&registry.pending()==0);delete host;registry.cancelShutdown();
 reset();host=new OfflineHiveDialog;loadHost(host);blockPool();auto r=command(Action::CreateValue);r.name=QStringLiteral("busy accepted");r.type=REG_BINARY;r.data=QByteArray("full");check(host->submit(r,true,[](const Result&){}));closeAnswer=QMessageBox::Cancel;host->requestClose();check(host->m_closeRequested&&!host->m_session->job()->isCleanupRequested()&&!host->closed);releasePool();until([&]{return !host->m_busy&&!host->m_closeRequested;});check(host->m_snapshot.dirty&&host->m_session&&!host->closed&&host->ready());
 auto afterCancel=command(Action::CreateValue);afterCancel.name=QStringLiteral("accepted-after-Cancel");afterCancel.data=QByteArray("full-after-cancel");check(host->submit(afterCancel,true,[](const Result&r){check(r.submitted&&r.verified);}));until([&]{return !host->m_busy;});check(mockValues[afterCancel.name].raw==afterCancel.data);
 closeAnswer=QMessageBox::Discard;host->requestClose();until([&]{return host->closed;});delete host;
 reset();host=new OfflineHiveDialog;loadHost(host);dirtyHost(host);closeAnswer=QMessageBox::Discard;closeStatus=ERROR_BUSY;host->requestClose();until([&]{return host->m_session->job()->hasFailed();});QCoreApplication::processEvents();check(!host->closed&&registry.pending()==1&&host->m_session);const auto failedAttempt=host->m_session->job()->attemptCount();check(!host->m_session->job()->notifyReadyToAttempt());QThreadPool::globalInstance()->waitForDone();check(host->m_session->job()->attemptCount()==failedAttempt&&host->m_session->job()->hasFailed());closeStatus=ERROR_SUCCESS;host->requestClose();until([&]{return host->closed;});check(registry.pending()==0);delete host;
 reset();host=new OfflineHiveDialog;loadHost(host);dirtyHost(host);closeAnswer=QMessageBox::Save;selectedSave=QStringLiteral("/failed-save.hive");commitAllowed=false;host->requestClose();until([&]{return !host->m_busy&&!host->m_closeRequested;});check(!host->closed&&host->m_snapshot.dirty&&host->m_snapshot.loaded);closeAnswer=QMessageBox::Discard;host->requestClose();until([&]{return host->closed;});delete host;
 reset();host=new OfflineHiveDialog;loadHost(host);dirtyHost(host);closeAnswer=QMessageBox::Save;selectedSave=QStringLiteral("/saved-reload-denied.hive");loadStatus=ERROR_ACCESS_DENIED;host->requestClose();until([&]{return host->closed;});check(mockFiles.contains(selectedSave)&&registry.pending()==0);delete host;
}
int main(int argc,char**argv){QCoreApplication app(argc,argv);workerChecks();auto& registry=ks::ui::AsyncCleanupRegistry::global();registry.cancelShutdown();QThreadPool::globalInstance()->setMaxThreadCount(1);reset();auto* host=new OfflineHiveDialog;int deliveries=0;auto open=command(Action::Open);open.source=QStringLiteral("/source.hive");
 blockPool();check(host->submit(open,true,[&](const Result&r){++deliveries;check(r.snapshot.loaded);}));open.source=QStringLiteral("/not-the-frozen-source");check(!host->submit(open,true,[](const Result&){}));int beats=0;QTimer heartbeat;QObject::connect(&heartbeat,&QTimer::timeout,[&]{++beats;});heartbeat.start(1);QElapsedTimer elapsed;elapsed.start();while(elapsed.elapsed()<30){QCoreApplication::processEvents();QThread::msleep(1);}check(beats>=5);releasePool();until([&]{return deliveries==1&&!host->m_busy;});check(host->m_snapshot.loaded&&loadedPath==host->m_snapshot.copyPath);check(loadedPath!=QStringLiteral("/source.hive"));
 auto create=command(Action::CreateValue);create.name=QStringLiteral("完整 原值");create.type=REG_BINARY;create.data=QByteArray(16*1024*1024,'V');const auto frozen=create;check(host->submit(create,true,[&](const Result&r){++deliveries;check(r.submitted&&r.verified&&r.snapshot.dirty);}));create.name=QStringLiteral("别的目标");create.data.clear();until([&]{return deliveries==2&&!host->m_busy;});check(mockValues[frozen.name].raw==frozen.data&&!mockValues.contains(create.name));
 auto read=command(Action::ReadValue);read.name=frozen.name;check(host->submit(read,false,[&](const Result&r){++deliveries;check(r.value.raw==frozen.data);}));until([&]{return deliveries==3&&!host->m_busy;});
 // GUI callback destruction must not fall through to finishIdle on freed this.
 check(host->submit(read,false,[&](const Result&){++deliveries;delete host;host=nullptr;}));until([&]{return host==nullptr;});until([&]{return registry.pending()==0;});check(handles.isEmpty());
 reset();host=new OfflineHiveDialog;open.source=QStringLiteral("/source.hive");check(host->submit(open,true,[&](const Result&){++deliveries;}));until([&]{return !host->m_busy;});throwReadback=true;auto mutation=command(Action::CreateValue);mutation.name=QStringLiteral("committed-before-readback-exception");mutation.type=REG_BINARY;mutation.data=QByteArray("safe raw");check(host->submit(mutation,true,[](const Result&){check(false);}));until([&]{return host->m_recoveryNeeded&&!host->m_busy;});check(!host->ready()&&mockValues.contains(mutation.name));throwReadback=false;delete host;host=nullptr;until([&]{return registry.pending()==0;});
 reset();host=new OfflineHiveDialog;blockPool();const int prior=deliveries;check(host->submit(open,true,[&](const Result&){++deliveries;}));elapsed.restart();delete host;host=nullptr;check(elapsed.elapsed()<50);releasePool();until([&]{return registry.pending()==0;});check(deliveries==prior&&handles.isEmpty());heartbeat.stop();closeChecks();QThreadPool::globalInstance()->setMaxThreadCount(8);std::printf("offline_hive_dialog_bridge: %d checks PASS fullRaw16MiB=PASS busyNoSupersede=PASS callbackOwnerDelete=PASS readbackException=unknownReceipt closeQueuedNative=PASS saveDiscardCancelAndRetry=PASS beats=%d GUI=0 realHive=0\n",checks,beats);return 0;
}
"""
source=a.out/'hive_worker.cpp';source.write_text(fixture,encoding='utf-8');qt=Path('D:/Software/Qt/6.9.3/msvc2022_64');exe=a.out/'hive_worker.exe';cmd=a.out/'test.cmd'
lines=['@echo off','call "D:\\Software\\VS\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64','if errorlevel 1 exit /b %errorlevel%',f'cl.exe /nologo /std:c++20 /Zc:__cplusplus /EHsc /utf-8 /W4 /WX /MD /wd4100 /DUNICODE /D_UNICODE /I"{a.ui_dir.resolve()}" /I"{qt / "include"}" /I"{qt / "include/QtCore"}" "{source.resolve()}" "{(a.ui_dir / "AsyncOperation.cpp").resolve()}" /Fo"{a.out.resolve()}/" /Fe"{exe.resolve()}" /link /LIBPATH:"{qt / "lib"}" Qt6Core.lib','if errorlevel 1 exit /b %errorlevel%',f'set "PATH={qt / "bin"};%PATH%"',f'"{exe.resolve()}"']
cmd.write_text('\n'.join(lines)+'\n',encoding='utf-8');result=subprocess.run(['cmd.exe','/d','/c',str(cmd.resolve())],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace');(a.out/'test.log').write_text(result.stdout,encoding='utf-8');print(result.stdout);raise SystemExit(result.returncode)
