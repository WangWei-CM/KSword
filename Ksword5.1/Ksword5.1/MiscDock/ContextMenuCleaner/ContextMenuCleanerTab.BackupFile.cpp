#include "ContextMenuCleanerTab.h"
#include "ContextMenuCleanerTab.Internal.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <array>

namespace ks::misc
{
using namespace context_menu_cleaner_detail;
namespace
{
    using namespace shell_transaction;
    QString classRelativePath(const Key& key)
    {
        if(key.root=="HKCR")return key.path;
        const QString prefix="Software\\Classes\\";
        return key.path.startsWith(prefix,Qt::CaseInsensitive)?key.path.mid(prefix.size()):QString();
    }
    bool legacyClassContainer(const Key& key)
    {
        static const QSet<QString> paths{"*","allfilesystemobjects","directory","directory\\background","folder","drive"};
        return key.root!="HKCR"&&paths.contains(classRelativePath(key).toCaseFolded());
    }
    bool reservedClassNamespace(const QString& name)
    {
        static const QSet<QString> names{"clsid","interface","typelib","appid","record","component categories",
            "installer","licenses","mime","extensions","activatableclasses","packagedcom","packagedappxdebug",
            "wow6432node","virtualstore","local settings"};
        return names.contains(name.toCaseFolded());
    }
    bool protocolName(const QString& name)
    {return !reservedClassNamespace(name)&&name.compare("Applications",Qt::CaseInsensitive)!=0&&name.compare("SystemFileAssociations",Qt::CaseInsensitive)!=0&&QRegularExpression("^[A-Za-z][A-Za-z0-9+.-]*$").match(name).hasMatch();}
    bool wholeProtocolScope(const Key& key)
    {const auto path=classRelativePath(key);return !path.isEmpty()&&!path.contains('\\')&&protocolName(path);}
    bool allowedBackupScope(const Key& key)
    {
        if(!validKey(key))return false;
        const auto path=classRelativePath(key);
        if(!path.isEmpty())
        {
            const auto parts=path.split('\\');
            if(reservedClassNamespace(parts[0]))return false;
            if(parts.size()==1)return key.root!="HKCR"&&protocolName(parts[0]);
            const int branch=(parts[0].compare("Applications",Qt::CaseInsensitive)==0
                ||parts[0].compare("SystemFileAssociations",Qt::CaseInsensitive)==0
                ||(parts[0].compare("Directory",Qt::CaseInsensitive)==0&&parts[1].compare("Background",Qt::CaseInsensitive)==0))?2:1;
            if(parts.size()<=branch)return false;
            const auto& name=parts[branch];
            // Descendants of a managed branch support older partial files; a deeper occurrence
            // of 'shell' cannot establish a new writable namespace.
            return name.compare("shell",Qt::CaseInsensitive)==0||name.compare("shellex",Qt::CaseInsensitive)==0
                ||name.compare("OpenWithList",Qt::CaseInsensitive)==0||name.compare("OpenWithProgids",Qt::CaseInsensitive)==0;
        }
        if(key.root=="HKCR")return false;
        const QString ie="Software\\Microsoft\\Internet Explorer\\MenuExt";
        if(key.path.compare(ie,Qt::CaseInsensitive)==0||key.path.startsWith(ie+'\\',Qt::CaseInsensitive))return true;
        const QString fileExts="Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\";
        if(key.root=="HKCU"&&key.view==0&&key.path.startsWith(fileExts,Qt::CaseInsensitive))
        {
            const auto tail=key.path.mid(fileExts.size()).split('\\');
            return tail.size()>=2&&tail[0].size()>1&&tail[0].startsWith('.')
                &&(tail[1].compare("OpenWithList",Qt::CaseInsensitive)==0||tail[1].compare("OpenWithProgids",Qt::CaseInsensitive)==0);
        }
        const QString explorer="Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\";
        if(key.path.startsWith(explorer,Qt::CaseInsensitive))
        {
            const auto tail=key.path.mid(explorer.size()).split('\\');
            return tail.size()>=2&&(tail[0].compare("Desktop",Qt::CaseInsensitive)==0
                ||tail[0].compare("MyComputer",Qt::CaseInsensitive)==0||tail[0].compare("HomeFolder",Qt::CaseInsensitive)==0)
                &&(tail[1].compare("NameSpace",Qt::CaseInsensitive)==0
                    ||(tail[1].size()>10&&tail[1].startsWith("NameSpace_",Qt::CaseInsensitive)));
        }
        const QString url="Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\";
        if(key.root=="HKCU"&&key.view==0&&key.path.startsWith(url,Qt::CaseInsensitive))
        {const auto tail=key.path.mid(url.size()).split('\\');return tail.size()>=2&&protocolName(tail[0])&&tail[1].compare("UserChoice",Qt::CaseInsensitive)==0;}
        return false;
    }
    const Node* snapshotNode(const Snapshot& snapshot,const Key& key)
    {for(const auto& node:snapshot.nodes)if(identity(node.key)==identity(key))return &node;return nullptr;}
    bool hasNamedValue(const Snapshot& snapshot,const Key& key,const QString& name)
    {if(const auto* node=snapshotNode(snapshot,key))for(const auto& value:node->values)if(value.name.compare(name,Qt::CaseInsensitive)==0)return true;return false;}
    bool validateProtocolScopes(const Snapshot& snapshot,QString* error)
    {
        for(const auto& scope:snapshot.scopes)if(wholeProtocolScope(scope)&&!(snapshot.mergeOnly&&legacyClassContainer(scope)))
            if(snapshotNode(snapshot,scope)&&!hasNamedValue(snapshot,scope,"URL Protocol"))
                return fail(error,QStringLiteral("Protocol backup lacks URL Protocol identity:"),scope,ERROR_INVALID_DATA);
        return true;
    }
    bool decodeBackupDocument(const QJsonObject& document,Snapshot& snapshot,QString* error)
    {const bool legacy=document.value("formatVersion").toInt()==1||document.value("mergeOnly").toBool(false);
        return decode(document,snapshot,[legacy](const Key& key){return allowedBackupScope(key)||(legacy&&validKey(key)&&legacyClassContainer(key));},error)&&validateProtocolScopes(snapshot,error);}
    bool protectReadOnlyProtocols(Snapshot& backup,const Snapshot& current,QString* error)
    {
        QVector<Key> protocols;
        for(const auto& node:current.nodes)if(wholeProtocolScope(node.key)&&hasNamedValue(current,node.key,"URL Protocol"))protocols.push_back(node.key);
        for(const auto& node:backup.nodes)if(wholeProtocolScope(node.key)&&hasNamedValue(backup,node.key,"URL Protocol"))protocols.push_back(node.key);
        // Absent desired roots may only remove an actual URL protocol, never an unrelated ProgID.
        for(const auto& scope:backup.scopes)if(wholeProtocolScope(scope)&&!(backup.mergeOnly&&legacyClassContainer(scope))
            &&snapshotNode(current,scope)&&!hasNamedValue(current,scope,"URL Protocol"))
            return fail(error,QStringLiteral("Current key is not a URL protocol:"),scope,ERROR_INVALID_DATA);
        for(const auto& protocol:normalizedScopes(std::move(protocols)))
        {
            if(protocol.root!="HKLM")continue;
            const auto name=classRelativePath(protocol).toCaseFolded();
            static const QSet<QString> protectedNames{"windowsdefender","ms-device-enrollment","ms-search","ms-windows-search",
                "ms-actioncenter","ms-print-addprinter","explorer.zipselection"};
            bool protectedProtocol=protectedNames.contains(name)||name.startsWith("ms-");
            for(const auto* snapshot:{static_cast<const Snapshot*>(&backup),&current})
            {
                const auto has=[&](const QString& suffix,const QString& value){Key key=protocol;key.path+=suffix;return hasNamedValue(*snapshot,key,value);};
                protectedProtocol=protectedProtocol||has({},"EditFlags")||has("\\shell\\open\\command","DelegateExecute")
                    ||has("\\shell\\open","PackageId")||has("\\shell\\open","ActivatableClassId")
                    ||has("\\shell\\open","ContractId")||has("\\Application","AppUserModelId");
            }
            if(!protectedProtocol)continue;
            Snapshot saved,actual;saved.mergeOnly=backup.mergeOnly;actual.mergeOnly=backup.mergeOnly;
            for(const auto& scope:backup.scopes)if(beneath(scope,protocol)){saved.scopes.push_back(scope);actual.scopes.push_back(scope);}
            for(const auto& node:backup.nodes)if(beneath(node.key,protocol))saved.nodes.push_back(node);
            for(const auto& node:current.nodes)
                for(const auto& scope:saved.scopes)if(beneath(node.key,scope)){actual.nodes.push_back(node);break;}
            if(backup.mergeOnly?!recordedProjectionMatches(saved,actual):!equivalent(saved,actual))
                return fail(error,QStringLiteral("Protected URL protocol differs; restore refused:"),protocol,ERROR_ACCESS_DENIED);
            backup.scopes.erase(std::remove_if(backup.scopes.begin(),backup.scopes.end(),[&](const Key& key){return beneath(key,protocol);}),backup.scopes.end());
            backup.nodes.erase(std::remove_if(backup.nodes.begin(),backup.nodes.end(),[&](const Node& node){return beneath(node.key,protocol);}),backup.nodes.end());
        }
        return true;
    }
    bool discoverBackupScopes(Port& port,QVector<Key>& scopes,QString* error)
    {
        const auto read=[&](const Key& key,QVector<Value>& values,QStringList& children,bool required=false){
            const auto status=port.read(key,values,children);
            if((status==ERROR_FILE_NOT_FOUND||status==ERROR_PATH_NOT_FOUND)&&!required)return true;
            return status==ERROR_SUCCESS||fail(error,QStringLiteral("Registry scope discovery failed:"),key,status);
        };
        const auto branches=[&](const Key& parent,const QStringList& children){
            for(const auto& child:children)
                if(child.compare("shell",Qt::CaseInsensitive)==0||child.compare("shellex",Qt::CaseInsensitive)==0
                    ||child.compare("OpenWithList",Qt::CaseInsensitive)==0||child.compare("OpenWithProgids",Qt::CaseInsensitive)==0)
                    scopes.push_back(Key{parent.root,parent.view,parent.path+'\\'+child});
        };
        for(const auto& location:QVector<Key>{{"HKCU",0,{}},{"HKLM",KEY_WOW64_64KEY,{}},{"HKLM",KEY_WOW64_32KEY,{}}})
        {
            const auto& root=location.root;const REGSAM view=location.view;
            const Key classes{root,view,QStringLiteral("Software\\Classes")};QVector<Value> values;QStringList names;
            if(!read(classes,values,names))return false;
            for(const auto& name:names)
            {
                if(reservedClassNamespace(name))continue;
                const Key key{root,view,classes.path+'\\'+name};values.clear();QStringList children;
                if(!read(key,values,children,true))return false;
                for(const auto& value:values)if(protocolName(name)&&value.name.compare("URL Protocol",Qt::CaseInsensitive)==0){scopes.push_back(key);break;}
                branches(key,children);
                if(name.compare("Directory",Qt::CaseInsensitive)==0)
                    for(const auto& sub:children)if(sub.compare("Background",Qt::CaseInsensitive)==0)
                    {Key background{root,view,key.path+'\\'+sub};values.clear();QStringList leaves;if(!read(background,values,leaves,true))return false;branches(background,leaves);}
                if(name.compare("SystemFileAssociations",Qt::CaseInsensitive)==0||name.compare("Applications",Qt::CaseInsensitive)==0)
                    for(const auto& sub:children){Key nested{root,view,key.path+'\\'+sub};values.clear();QStringList leaves;if(!read(nested,values,leaves,true))return false;branches(nested,leaves);}
            }
            const QString explorer="Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\";
            for(const auto& section:QStringList{"Desktop","MyComputer","HomeFolder"})
            {
                const Key parent{root,view,explorer+section};values.clear();QStringList children;if(!read(parent,values,children))return false;
                // Keep all namespace data in the snapshot; UI can hide protected rows independently.
                for(const auto& child:children)if(child.compare("NameSpace",Qt::CaseInsensitive)==0||child.startsWith("NameSpace_",Qt::CaseInsensitive))
                    scopes.push_back(Key{root,view,parent.path+'\\'+child});
            }
            scopes.push_back(Key{root,view,QStringLiteral("Software\\Microsoft\\Internet Explorer\\MenuExt")});
        }
        for(const auto& base:QStringList{"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts",
            "Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations"})
        {
            const Key parent{"HKCU",0,base};QVector<Value> values;QStringList names;if(!read(parent,values,names))return false;
            for(const auto& name:names)
            {
                const Key key{"HKCU",0,base+'\\'+name};values.clear();QStringList children;if(!read(key,values,children,true))return false;
                for(const auto& child:children)
                    if((base.endsWith("FileExts")&&(child.compare("OpenWithList",Qt::CaseInsensitive)==0||child.compare("OpenWithProgids",Qt::CaseInsensitive)==0))
                        ||(base.endsWith("UrlAssociations")&&child.compare("UserChoice",Qt::CaseInsensitive)==0))
                        scopes.push_back(Key{key.root,key.view,key.path+'\\'+child});
            }
        }
        return true;
    }
    bool readSnapshotFile(const QString& path,Snapshot& snapshot,QString* error)
    {
        QFile file(path);
        if(!file.open(QIODevice::ReadOnly)){*error=QStringLiteral("无法打开备份文件：%1").arg(path);return false;}
        const auto bytes=file.readAll();if(file.error()!=QFileDevice::NoError){*error=file.errorString();return false;}
        QJsonParseError parse;const auto document=QJsonDocument::fromJson(bytes,&parse);
        if(parse.error!=QJsonParseError::NoError||!document.isObject()){*error=parse.errorString();return false;}
        return decodeBackupDocument(document.object(),snapshot,error);
    }
}

QString ContextMenuCleanerTab::contextMenuBackupDirectory()
{
    const QString beside=QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("ContextMenuBackups"));
    QDir directory(beside);
    if(directory.mkpath(QStringLiteral(".")))
    {
        const QString probePath=directory.filePath(QStringLiteral(".write_probe-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QFile probe(probePath);if(probe.open(QIODevice::WriteOnly|QIODevice::NewOnly)){probe.close();if(probe.remove())return beside;}
    }
    const QString fallback=QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("ContextMenuBackups"));
    QDir().mkpath(fallback);return fallback;
}

bool ContextMenuCleanerTab::exportContextMenuBackupToFile(QString* pathOut,QString* error,
    QJsonObject* snapshotOut,const QString& suffix,const QVector<shell_transaction::Key>& extraScopes,const bool mergeOnly)
{
    using namespace shell_transaction;
    QVector<Key> scopes=extraScopes;
    NativePort port;
    if(!discoverBackupScopes(port,scopes,error))return false;
    for(const auto& scope:scopes)if(!allowedBackupScope(scope)&&!(mergeOnly&&legacyClassContainer(scope)))return fail(error,QStringLiteral("Unsupported backup scope:"),scope,ERROR_ACCESS_DENIED);
    Snapshot snapshot;
    if(!collect(port,std::move(scopes),snapshot,error))return false;
    snapshot.completeFamilies=true;snapshot.mergeOnly=mergeOnly; // Legacy wide-container safety files remain explicitly merge-only.
    QJsonObject document=encode(snapshot);
    if(snapshotOut)*snapshotOut=document;
    document.insert("createdUtc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    const QString path=QDir(contextMenuBackupDirectory()).filePath(QStringLiteral("context-menu-%1-%2%3.json")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz")),
            QUuid::createUuid().toString(QUuid::WithoutBraces),suffix));
    QSaveFile file(path);
    if(!commitDocument(file,document,error))
    {if(error)*error=QStringLiteral("无法写入备份文件：%1").arg(path)+*error;return false;}
    if(pathOut)*pathOut=path;
    kLogEvent event;info<<event<<"[ContextMenuCleanerTab] complete backup receipt: path="<<path.toStdString()
        <<", keys="<<snapshot.nodes.size()<<", scopes="<<snapshot.scopes.size()<<eol;
    return true;
}

QString ContextMenuCleanerTab::backupContextMenuToFile(const bool showSuccessMessage)
{
    if(!beginShellAction())return {};
    submitShellTransaction([]{
        ShellTransactionResult result;result.stage=ShellTransactionStage::Completed;
        result.ok=exportContextMenuBackupToFile(&result.path,&result.error);return result;
    },[this,showSuccessMessage](const ShellTransactionResult& result){
        if(!showSuccessMessage)return;
        if(result.ok)QMessageBox::information(this,QStringLiteral("备份右键菜单"),
            QStringLiteral("已导出整份右键菜单注册表：\n%1").arg(QDir::toNativeSeparators(result.path)));
        else QMessageBox::warning(this,QStringLiteral("备份右键菜单"),QStringLiteral("备份失败：%1").arg(result.failureText()));
    });
    return {}; // Path is delivered by the accepted receipt, never synchronously queried by GUI.
}

ContextMenuCleanerTab::ShellTransactionResult ContextMenuCleanerTab::prepareContextMenuRestore(const QString& path)
{
    using namespace shell_transaction;
    ShellTransactionResult result;result.stage=ShellTransactionStage::Completed;
    Snapshot backup;if(!readSnapshotFile(path,backup,&result.error))return result;
    QJsonObject safety;
    if(!exportContextMenuBackupToFile(&result.path,&result.error,&safety,QStringLiteral("-before-restore"),backup.scopes,backup.mergeOnly))return result;
    result.payload={{"backup",encode(backup)},{"current",safety},{"source",path},{"limited",!backup.completeFamilies||backup.mergeOnly}};
    result.ok=true;return result;
}

ContextMenuCleanerTab::ShellTransactionResult ContextMenuCleanerTab::applyContextMenuRestore(const QJsonObject& plan)
{
    using namespace shell_transaction;
    ShellTransactionResult result;result.stage=ShellTransactionStage::Completed;
    Snapshot backup,current;
    if(!decodeBackupDocument(plan.value("backup").toObject(),backup,&result.error)
        ||!decodeBackupDocument(plan.value("current").toObject(),current,&result.error))return result;
    NativePort port;Snapshot actual;
    auto scopes=current.scopes;
    if(!discoverBackupScopes(port,scopes,&result.error) || !collect(port,std::move(scopes),actual,&result.error))return result;
    actual.completeFamilies=current.completeFamilies;actual.mergeOnly=current.mergeOnly;
    if(!equivalent(actual,current))
    {result.error=QStringLiteral("当前注册表状态已变化，未执行恢复；请重新选择备份后确认。");return result;}
    if(backup.completeFamilies&&!backup.mergeOnly){backup.scopes+=actual.scopes;backup.scopes=normalizedScopes(std::move(backup.scopes));}
    if(!protectReadOnlyProtocols(backup,actual,&result.error))return result;
    const auto receipt=restoreVerified(port,backup,actual);result.ok=receipt.ok();result.count=receipt.keys;result.failures=receipt.failures;
    result.payload={{"values",receipt.values},{"deleted",receipt.deleted}};return result;
}

void ContextMenuCleanerTab::restoreContextMenuFromFile()
{
    if(!beginShellAction())return;
    const QPointer<ContextMenuCleanerTab> alive(this);
    // Directory suggestion does no filesystem probing; all I/O is in accepted worker phases.
    const auto path=QFileDialog::getOpenFileName(this,QStringLiteral("选择右键菜单备份文件"),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("ContextMenuBackups")),
        QStringLiteral("Ksword 菜单备份 (*.json)"));
    if(!alive)return;
    if(path.isEmpty()){endShellAction();return;}
    submitShellTransaction([path]{return prepareContextMenuRestore(path);},[this,path](const ShellTransactionResult& prepared){
        if(!prepared.ok){QMessageBox::warning(this,QStringLiteral("恢复备份"),prepared.failureText());return;}
        // Reserve the second modal before another action can change the current snapshot.
        if(!beginShellAction())return;
        const QPointer<ContextMenuCleanerTab> alive(this);
        const QString limited=prepared.payload.value("limited").toBool()
            ? QStringLiteral("\n\n这是旧版或有限范围备份；旧版仅合并文件明确记录的键值，保留未记录内容。") : QString();
        const auto answer=QMessageBox::question(this,QStringLiteral("恢复备份"),
            QStringLiteral("将根据备份恢复右键菜单注册表：\n%1\n\n恢复前的当前状态已自动另存为：\n%2\n\n完整新版备份会移除范围内新增内容；旧版仅合并已记录内容。继续吗？")
                .arg(QDir::toNativeSeparators(path),QDir::toNativeSeparators(prepared.path))+limited,
            QMessageBox::Yes|QMessageBox::No,QMessageBox::No);
        if(!alive)return;
        if(answer!=QMessageBox::Yes)
        {
            endShellAction();QMessageBox::information(this,QStringLiteral("恢复备份"),
                QStringLiteral("已取消。当前状态备份保留在：\n%1").arg(QDir::toNativeSeparators(prepared.path)));return;
        }
        const auto plan=prepared.payload;const auto safetyPath=prepared.path;
        submitShellTransaction([plan]{return applyContextMenuRestore(plan);},[this,safetyPath](const ShellTransactionResult& result){
            const std::array<MenuArea,7> areas{MenuArea::InternetExplorer,MenuArea::Desktop,MenuArea::File,
                MenuArea::UrlBinding,MenuArea::OpenWith,MenuArea::FormatMenu,MenuArea::ExplorerHome};
            for(const auto area:areas)if(widgetsForArea(area)->hasLoaded)refreshArea(area);
            QString summary=QStringLiteral("恢复结果。\n\n写回注册表键：%1\n写回注册表值：%2\n删除当前多出的键：%3\n恢复前状态备份：%4")
                .arg(result.count).arg(result.payload.value("values").toInt()).arg(result.payload.value("deleted").toInt()).arg(QDir::toNativeSeparators(safetyPath));
            if(result.stage!=ShellTransactionStage::Completed||!result.error.isEmpty())summary=result.failureText()+QStringLiteral("\n恢复前状态备份：%1").arg(QDir::toNativeSeparators(safetyPath));
            if(!result.failures.isEmpty())summary+=QStringLiteral("\n\n失败项（前 5 条）：\n%1").arg(result.failures.mid(0,5).join('\n'));
            if(result.ok)QMessageBox::information(this,QStringLiteral("恢复备份"),summary);
            else QMessageBox::warning(this,QStringLiteral("恢复备份"),summary);
        });
    });
}
} // namespace ks::misc
