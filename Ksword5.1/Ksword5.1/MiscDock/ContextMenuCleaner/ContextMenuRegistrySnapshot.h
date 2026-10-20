#pragma once
// Pure value registry snapshots/restore receipts. Native port is never called on GUI.
#include <Windows.h>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QIODevice>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

namespace ks::misc::shell_transaction
{
    struct Key { QString root; REGSAM view = 0; QString path; };
    struct Value { QString name; DWORD type = REG_NONE; QByteArray bytes; };
    struct Node { Key key; QVector<Value> values; };
    struct Snapshot { QVector<Key> scopes; QVector<Node> nodes; bool completeFamilies = false; bool mergeOnly = false; };
    inline QString identity(const Key& key)
    { return key.root + ':' + QString::number(key.view) + ':' + key.path.toCaseFolded(); }
    inline bool beneath(const Key& key, const Key& scope)
    { return key.root == scope.root && key.view == scope.view
        && (key.path.compare(scope.path, Qt::CaseInsensitive) == 0
            || key.path.startsWith(scope.path + '\\', Qt::CaseInsensitive)); }
    inline bool validKey(const Key& key)
    { return (key.root == "HKCU" || key.root == "HKLM" || key.root == "HKCR")
        && (key.view == 0 || key.view == KEY_WOW64_32KEY || key.view == KEY_WOW64_64KEY)
        && !key.path.isEmpty() && !key.path.contains(QChar(u'\0'))
        && !key.path.startsWith('\\') && !key.path.endsWith('\\') && !key.path.contains("\\\\"); }
    struct Port
    {
        virtual ~Port() = default;
        virtual LSTATUS read(const Key&, QVector<Value>&, QStringList&) = 0;
        virtual LSTATUS remove(const Key&) = 0;
        virtual LSTATUS removeEmptyKey(const Key&) { return ERROR_NOT_SUPPORTED; }
        virtual LSTATUS create(const Key&) = 0;
        virtual LSTATUS write(const Key&, const Value&) = 0;
        virtual LSTATUS eraseValue(const Key&, const QString&) = 0;
        virtual LSTATUS createTracked(const Key& key,bool& created) { created=false;return create(key); }
    };
    inline bool fail(QString* out, const QString& stage, const Key& key, LSTATUS status)
    { if (out) *out = QStringLiteral("%1 %2\\%3 (%4)").arg(stage, key.root, key.path).arg(status); return false; }
    inline bool collectNode(Port& port, const Key& key, Snapshot& snapshot, QString* error, int depth, bool scope)
    {
        if (depth > 128) return fail(error, QStringLiteral("Registry tree exceeds depth limit:"), key, ERROR_INVALID_DATA);
        Node node; node.key = key; QStringList children;
        const auto status = port.read(key, node.values, children);
        if (scope && (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)) return true;
        if (status != ERROR_SUCCESS) return fail(error, QStringLiteral("Registry snapshot read failed:"), key, status);
        std::sort(node.values.begin(), node.values.end(), [](const Value& a,const Value& b){return a.name < b.name;});
        std::sort(children.begin(), children.end()); snapshot.nodes.push_back(std::move(node));
        for (const auto& child : children)
        {
            if (child.isEmpty() || child.contains('\\') || child.contains(QChar(u'\0')))
                return fail(error, QStringLiteral("Invalid registry child:"), key, ERROR_INVALID_DATA);
            if (!collectNode(port, Key{key.root,key.view,key.path+'\\'+child}, snapshot,error,depth+1,false)) return false;
        }
        return true;
    }
    inline QVector<Key> normalizedScopes(QVector<Key> scopes)
    {
        std::sort(scopes.begin(),scopes.end(),[](const Key&a,const Key&b){return identity(a)<identity(b);});
        QVector<Key> out;
        for (const auto& key : scopes)
        {
            bool covered=false;for (const auto& scope : out) if (beneath(key,scope)){covered=true;break;}
            if (!covered) out.push_back(key);
        }
        return out;
    }
    inline bool collect(Port& port, QVector<Key> scopes, Snapshot& snapshot, QString* error)
    {
        snapshot = {}; snapshot.scopes = normalizedScopes(std::move(scopes));
        for (const auto& scope : snapshot.scopes)
            if (!validKey(scope) || !collectNode(port,scope,snapshot,error,0,true)) return false;
        return true;
    }
    // JSON text cannot preserve isolated UTF-16 surrogates in native key/value names.
    inline QString readableUtf16(const QString& value) {return QString::fromUtf8(value.toUtf8());}
    inline QString encodedUtf16(const QString& value)
    {return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(value.utf16()),value.size()*2).toBase64());}
    inline bool decodedUtf16(const QJsonObject& object,const QString& textKey,const QString& rawKey,QString& value)
    {
        if(!object.value(textKey).isString())return false;
        const auto raw=object.value(rawKey);
        if(raw.isUndefined()){value=object.value(textKey).toString();return true;} // Existing v2 text-only files.
        if(!raw.isString())return false;
        const auto decoded=QByteArray::fromBase64Encoding(raw.toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
        if(!decoded||decoded.decoded.size()%2!=0)return false;
        value.resize(decoded.decoded.size()/2);
        if(!value.isEmpty())std::memcpy(value.data(),decoded.decoded.constData(),static_cast<size_t>(decoded.decoded.size()));
        return object.value(textKey).toString()==readableUtf16(value);
    }
    inline QJsonObject keyJson(const Key& key)
    { return {{"root",key.root},{"view",QString::number(key.view)},{"path",readableUtf16(key.path)},{"pathUtf16",encodedUtf16(key.path)}}; }
    inline QJsonObject encode(const Snapshot& snapshot)
    {
        QJsonArray scopes, entries;
        for (const auto& scope : snapshot.scopes) scopes.append(keyJson(scope));
        for (const auto& node : snapshot.nodes)
        {
            QJsonObject entry=keyJson(node.key); QJsonArray values;
            for (const auto& value : node.values)
                values.append(QJsonObject{{"name",readableUtf16(value.name)},{"nameUtf16",encodedUtf16(value.name)},{"type",QString::number(value.type)},
                    {"raw",QString::fromLatin1(value.bytes.toBase64())}});
            entry.insert("values",values);entries.append(entry);
        }
        return {{"schema","ksword-context-menu-backup"},{"formatVersion",2},
            {"entryCount",entries.size()},{"scopes",scopes},{"entries",entries},{"completeFamilies",snapshot.completeFamilies},{"mergeOnly",snapshot.mergeOnly}};
    }
    template<class File>
    bool commitDocument(File& file,const QJsonObject& document,QString* error)
    {
        file.setDirectWriteFallback(false);
        if(!file.open(QIODevice::WriteOnly)){if(error)*error=file.errorString();return false;}
        const auto bytes=QJsonDocument(document).toJson(QJsonDocument::Indented);
        if(file.write(bytes)!=bytes.size())
        {if(error)*error=file.errorString();file.cancelWriting();return false;}
        if(!file.commit()){if(error)*error=file.errorString();return false;}
        return true;
    }
    inline bool equivalent(Snapshot left,Snapshot right)
    {
        const auto canonical=[](Snapshot& value){
            for(auto& scope:value.scopes)scope.path=scope.path.toCaseFolded();
            std::sort(value.scopes.begin(),value.scopes.end(),[](const Key&a,const Key&b){return identity(a)<identity(b);});
            for(auto& node:value.nodes)
            {
                node.key.path=node.key.path.toCaseFolded();
                for(auto& item:node.values)item.name=item.name.toCaseFolded();
                std::sort(node.values.begin(),node.values.end(),[](const Value&a,const Value&b){return a.name<b.name;});
            }
            std::sort(value.nodes.begin(),value.nodes.end(),[](const Node&a,const Node&b){return identity(a.key)<identity(b.key);});
        };
        canonical(left);canonical(right);return encode(left)==encode(right);
    }
    inline bool parseKey(const QJsonObject& object, Key& key, bool legacy)
    {
        key.root=object.value("root").toString();
        if(legacy)key.path=object.value("path").toString();
        else if(!decodedUtf16(object,"path","pathUtf16",key.path))return false;
        const auto view=object.value("view").toString();bool ok=false;
        if (legacy) {ok=view=="32"||view=="64"||view=="default";key.view=view=="32"?KEY_WOW64_32KEY:(view=="64"?KEY_WOW64_64KEY:0);}
        else key.view=view.toUInt(&ok);
        return ok&&validKey(key);
    }
    inline bool legacyBytes(const QJsonObject& object, Value& value)
    {
        if (!object.value("type").isDouble()) return false;
        const double type=object.value("type").toDouble();
        if(type<0||type>static_cast<double>(MAXDWORD)||type!=static_cast<DWORD>(type))return false;
        value.type=static_cast<DWORD>(type);const auto data=object.value("data");
        switch(value.type)
        {
        case REG_SZ:case REG_EXPAND_SZ:
        {if(!data.isString())return false;const auto text=data.toString();value.bytes=QByteArray(reinterpret_cast<const char*>(text.utf16()),(text.size()+1)*2);return true;}
        case REG_MULTI_SZ:
        {if(!data.isArray())return false;QString text;for(const auto& item:data.toArray()){if(!item.isString()||item.toString().contains(QChar(u'\0')))return false;text+=item.toString();text+=QChar(u'\0');}text+=QChar(u'\0');if(data.toArray().isEmpty())text+=QChar(u'\0');value.bytes=QByteArray(reinterpret_cast<const char*>(text.utf16()),text.size()*2);return true;}
        case REG_DWORD:
        {if(!data.isDouble())return false;const double number=data.toDouble();if(number < -2147483648.0||number>4294967295.0||number!=static_cast<qint64>(number))return false;const DWORD n=static_cast<DWORD>(static_cast<qint64>(number));value.bytes=QByteArray(reinterpret_cast<const char*>(&n),sizeof n);return true;}
        case REG_QWORD:
        {if(!data.isDouble())return false;const double number=data.toDouble();if(number<0||number>9007199254740991.0||number!=static_cast<quint64>(number))return false;const quint64 n=static_cast<quint64>(number);value.bytes=QByteArray(reinterpret_cast<const char*>(&n),sizeof n);return true;}
        case REG_BINARY:
        {if(!data.isString())return false;const auto decoded=QByteArray::fromBase64Encoding(data.toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);if(!decoded)return false;value.bytes=decoded.decoded;return true;}
        default:return false;
        }
    }
    inline bool decode(const QJsonObject& object, Snapshot& snapshot, const std::function<bool(const Key&)>& allowed, QString* error)
    {
        snapshot={};snapshot.completeFamilies=object.value("completeFamilies").toBool(false);const int version=object.value("formatVersion").toInt();const bool legacy=version==1;snapshot.mergeOnly=legacy||object.value("mergeOnly").toBool(false);if(legacy)snapshot.completeFamilies=false;
        const auto invalid=[&](){if(error)*error=QStringLiteral("Incomplete, invalid or unsupported registry backup.");return false;};
        if(object.value("schema").toString()!="ksword-context-menu-backup"||(version!=1&&version!=2)
            ||!object.value("entries").isArray())return invalid();
        const auto entries=object.value("entries").toArray();
        if((legacy&&entries.isEmpty())||object.value("entryCount").toInteger(-1)!=entries.size())return invalid();
        if(!legacy)
        {
            if(!object.value("scopes").isArray()||object.value("scopes").toArray().isEmpty())return invalid();
            QSet<QString> unique;
            for(const auto& item:object.value("scopes").toArray())
            {Key key;if(!item.isObject()||!parseKey(item.toObject(),key,false)||!allowed(key)||unique.contains(identity(key)))return invalid();unique.insert(identity(key));snapshot.scopes.push_back(key);}
            if(normalizedScopes(snapshot.scopes).size()!=snapshot.scopes.size())return invalid();
        }
        QSet<QString> unique;
        for(const auto& item:entries)
        {
            if(!item.isObject())return invalid();const auto entry=item.toObject();Node node;
            if(!parseKey(entry,node.key,legacy)||unique.contains(identity(node.key))||!entry.value("values").isArray())return invalid();
            unique.insert(identity(node.key));QSet<QString> names;
            for(const auto& raw:entry.value("values").toArray())
            {
                if(!raw.isObject())return invalid();const auto valueObject=raw.toObject();Value value;
                if(legacy){if(!valueObject.value("name").isString())return invalid();value.name=valueObject.value("name").toString();}
                else if(!decodedUtf16(valueObject,"name","nameUtf16",value.name))return invalid();
                if(value.name.contains(QChar(u'\0'))||names.contains(value.name.toCaseFolded()))return invalid();names.insert(value.name.toCaseFolded());
                if(legacy){if(!legacyBytes(valueObject,value))return invalid();}
                else
                {bool ok=false;value.type=valueObject.value("type").toString().toUInt(&ok);if(!ok||!valueObject.value("raw").isString())return invalid();const auto decoded=QByteArray::fromBase64Encoding(valueObject.value("raw").toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);if(!decoded)return invalid();value.bytes=decoded.decoded;}
                node.values.push_back(std::move(value));
            }
            snapshot.nodes.push_back(std::move(node));
        }
        if(legacy){QVector<Key> roots;for(const auto& node:snapshot.nodes)roots.push_back(node.key);snapshot.scopes=normalizedScopes(std::move(roots));for(const auto& scope:snapshot.scopes)if(!allowed(scope))return invalid();}
        for(const auto& node:snapshot.nodes)
        {
            bool covered=false;for(const auto& scope:snapshot.scopes)if(beneath(node.key,scope)){covered=true;if(!snapshot.mergeOnly&&identity(node.key)!=identity(scope)){Key parent=node.key;parent.path=parent.path.left(parent.path.lastIndexOf('\\'));if(!unique.contains(identity(parent)))return invalid();}break;}
            if(!covered)return invalid();
        }
        return true;
    }
    struct RestoreReceipt {int keys=0,values=0,deleted=0;QStringList failures;bool ok()const{return failures.isEmpty();}};
    inline RestoreReceipt restore(Port& port,const Snapshot& backup,const Snapshot& current)
    {
        RestoreReceipt receipt;
        QVector<Node> extra;
        const auto included=[&](const Key& key){for(const auto& scope:backup.scopes)if(beneath(key,scope))return true;return false;};
        const auto find=[&](const Snapshot& snapshot,const Key& key)->const Node*{for(const auto& node:snapshot.nodes)if(identity(node.key)==identity(key))return &node;return nullptr;};
        if(!backup.mergeOnly)for(const auto& node:current.nodes)if(included(node.key)&&!find(backup,node.key))extra.push_back(node);
        std::sort(extra.begin(),extra.end(),[](const Node&a,const Node&b){return a.key.path.size()>b.key.path.size();});
        for(const auto& node:extra)
        {
            const auto status=port.remove(node.key);
            if(status==ERROR_SUCCESS)++receipt.deleted;
            else if(status!=ERROR_FILE_NOT_FOUND&&status!=ERROR_PATH_NOT_FOUND)
            {QString error;fail(&error,QStringLiteral("Registry restore removal failed:"),node.key,status);receipt.failures.push_back(error);}
        }
        // Existing key identities/security survive; only extra keys/values are removed.
        for(const auto& node:backup.nodes)
        {
            auto result=port.create(node.key);
            if(result!=ERROR_SUCCESS){QString error;fail(&error,QStringLiteral("Registry restore create failed:"),node.key,result);receipt.failures.push_back(error);continue;}
            ++receipt.keys;
            if(!backup.mergeOnly)if(const auto* old=find(current,node.key))for(const auto& value:old->values)
            {
                bool keep=false;for(const auto& saved:node.values)if(saved.name.compare(value.name,Qt::CaseInsensitive)==0){keep=true;break;}
                if(keep)continue;
                result=port.eraseValue(node.key,value.name);
                if(result!=ERROR_SUCCESS&&result!=ERROR_FILE_NOT_FOUND){QString error;fail(&error,QStringLiteral("Registry restore value removal failed:"),node.key,result);receipt.failures.push_back(error);}
            }
            for(const auto& value:node.values)
            {
                result=port.write(node.key,value);
                if(result==ERROR_SUCCESS)++receipt.values;
                else{QString error;fail(&error,QStringLiteral("Registry restore value failed:"),node.key,result);receipt.failures.push_back(error);}
            }
        }
        return receipt;
    }
    inline bool recordedProjectionMatches(const Snapshot& recorded,const Snapshot& actual)
    {
        for(const auto& node:recorded.nodes)
        {
            const Node* found=nullptr;
            for(const auto& candidate:actual.nodes)if(identity(candidate.key)==identity(node.key)){found=&candidate;break;}
            if(!found)return false;
            for(const auto& value:node.values)
            {
                bool matched=false;
                for(const auto& candidate:found->values)
                    if(candidate.name.compare(value.name,Qt::CaseInsensitive)==0&&candidate.type==value.type&&candidate.bytes==value.bytes){matched=true;break;}
                if(!matched)return false;
            }
        }
        return true;
    }
    inline RestoreReceipt restoreVerified(Port& port,const Snapshot& backup,const Snapshot& current)
    {
        auto receipt=restore(port,backup,current);Snapshot actual;QString error;
        if(!collect(port,backup.scopes,actual,&error))receipt.failures.push_back(error);
        else
        {
            actual.completeFamilies=backup.completeFamilies;actual.mergeOnly=backup.mergeOnly;
            if(backup.mergeOnly?!recordedProjectionMatches(backup,actual):!equivalent(actual,backup))receipt.failures.push_back(QStringLiteral("Registry restore readback did not match the recorded backup."));
        }
        return receipt;
    }
    struct NativePort final:Port
    {
        struct Handle {HKEY key=nullptr;~Handle(){if(key)::RegCloseKey(key);}};
        static HKEY root(const QString& id){return id=="HKCU"?HKEY_CURRENT_USER:(id=="HKLM"?HKEY_LOCAL_MACHINE:(id=="HKCR"?HKEY_CLASSES_ROOT:nullptr));}
        LSTATUS read(const Key& key,QVector<Value>& values,QStringList& children)override
        {
            Handle handle;auto status=::RegOpenKeyExW(root(key.root),key.path.toStdWString().c_str(),0,KEY_READ|key.view,&handle.key);if(status!=ERROR_SUCCESS)return status;
            for(DWORD index=0;;++index)
            {
                std::vector<wchar_t> name(256);DWORD size=0,type=0;std::vector<BYTE> bytes(256);bool complete=false;
                for(int attempt=0;attempt<16;++attempt)
                {
                    DWORD names=static_cast<DWORD>(name.size());size=static_cast<DWORD>(bytes.size());
                    status=::RegEnumValueW(handle.key,index,name.data(),&names,nullptr,&type,bytes.data(),&size);
                    if(status==ERROR_NO_MORE_ITEMS){complete=true;break;}
                    if(status==ERROR_MORE_DATA)
                    {if(name.size()>MAXDWORD/2||bytes.size()>MAXDWORD/2)return ERROR_NOT_ENOUGH_MEMORY;name.resize((std::max)(name.size()*2,static_cast<size_t>(names)+1));bytes.resize((std::max)(bytes.size()*2,static_cast<size_t>(size)));continue;}
                    if(status!=ERROR_SUCCESS)return status;
                    values.push_back(Value{QString::fromWCharArray(name.data(),names),type,QByteArray(reinterpret_cast<const char*>(bytes.data()),size)});complete=true;break;
                }
                if(!complete)return ERROR_RETRY;if(status==ERROR_NO_MORE_ITEMS)break;
            }
            for(DWORD index=0;;++index)
            {
                std::vector<wchar_t> name(256);bool complete=false;
                for(int attempt=0;attempt<16;++attempt)
                {DWORD names=static_cast<DWORD>(name.size());status=::RegEnumKeyExW(handle.key,index,name.data(),&names,nullptr,nullptr,nullptr,nullptr);if(status==ERROR_MORE_DATA){name.resize(name.size()*2);continue;}if(status==ERROR_NO_MORE_ITEMS){complete=true;break;}if(status!=ERROR_SUCCESS)return status;children.push_back(QString::fromWCharArray(name.data(),names));complete=true;break;}
                if(!complete)return ERROR_RETRY;if(status==ERROR_NO_MORE_ITEMS)break;
            }
            return ERROR_SUCCESS;
        }
        LSTATUS remove(const Key& key)override
        {Handle handle;auto status=::RegOpenKeyExW(root(key.root),key.path.toStdWString().c_str(),0,KEY_READ|KEY_WRITE|key.view,&handle.key);if(status!=ERROR_SUCCESS)return status;status=::RegDeleteTreeW(handle.key,nullptr);if(status!=ERROR_SUCCESS)return status;::RegCloseKey(handle.key);handle.key=nullptr;return ::RegDeleteKeyExW(root(key.root),key.path.toStdWString().c_str(),key.view,0);}
        LSTATUS removeEmptyKey(const Key& key)override
        {return ::RegDeleteKeyExW(root(key.root),key.path.toStdWString().c_str(),key.view,0);}
        LSTATUS create(const Key& key)override
        {Handle handle;return ::RegCreateKeyExW(root(key.root),key.path.toStdWString().c_str(),0,nullptr,REG_OPTION_NON_VOLATILE,KEY_WRITE|key.view,nullptr,&handle.key,nullptr);}
        LSTATUS createTracked(const Key& key,bool& created)override
        {Handle handle;DWORD disposition=0;const auto status=::RegCreateKeyExW(root(key.root),key.path.toStdWString().c_str(),0,nullptr,REG_OPTION_NON_VOLATILE,KEY_WRITE|KEY_READ|key.view,nullptr,&handle.key,&disposition);created=status==ERROR_SUCCESS&&disposition==REG_CREATED_NEW_KEY;return status;}
        LSTATUS eraseValue(const Key& key,const QString& name)override
        {Handle handle;const auto status=::RegOpenKeyExW(root(key.root),key.path.toStdWString().c_str(),0,KEY_SET_VALUE|key.view,&handle.key);if(status!=ERROR_SUCCESS)return status;return ::RegDeleteValueW(handle.key,name.isEmpty()?nullptr:name.toStdWString().c_str());}
        LSTATUS write(const Key& key,const Value& value)override
        {Handle handle;auto status=::RegOpenKeyExW(root(key.root),key.path.toStdWString().c_str(),0,KEY_SET_VALUE|key.view,&handle.key);if(status!=ERROR_SUCCESS)return status;if(value.bytes.size()>MAXDWORD)return ERROR_INVALID_DATA;return ::RegSetValueExW(handle.key,value.name.isEmpty()?nullptr:value.name.toStdWString().c_str(),0,value.type,reinterpret_cast<const BYTE*>(value.bytes.constData()),static_cast<DWORD>(value.bytes.size()));}
    };
}
