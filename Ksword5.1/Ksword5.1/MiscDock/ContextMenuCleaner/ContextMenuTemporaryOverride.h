#pragma once
#include "ContextMenuRegistrySnapshot.h"
#include <atomic>
#include <mutex>

namespace ks::misc::shell_transaction
{
    struct TemporaryOverride
    {
        Key key;
        Value original;
        bool existed = false;
        bool captured = false;
        bool pending = false;
        QVector<Key> missingAncestors;
        QVector<Key> createdKeys;
        QString error;
    };
    struct LatencySession
    {
        std::atomic_bool stop{false}, recoveryPending{false};
        std::atomic_bool failedInlineRestore{false}, cleanupRequestedOnce{false};
        QVector<TemporaryOverride> overrides; // Only under shared registry serial gate.
        std::mutex progressMutex;
        QVector<QPair<int,double>> singles;
        QString progressText;
        float progress = 0;
    };
    inline bool captureOverride(Port& port,const Key& key,bool handler,TemporaryOverride& record)
    {
        record={};record.key=key;record.original.name=handler?QString():QStringLiteral("LegacyDisable");
        const auto parts=key.path.split('\\');QString prefix;
        for(const auto& part:parts)
        {
            prefix+=(prefix.isEmpty()?QString():QStringLiteral("\\"))+part;
            Key ancestor{key.root,key.view,prefix};QVector<Value> values;QStringList children;
            const auto status=port.read(ancestor,values,children);
            if(status==ERROR_FILE_NOT_FOUND||status==ERROR_PATH_NOT_FOUND)record.missingAncestors.push_back(ancestor);
            else if(status!=ERROR_SUCCESS)return fail(&record.error,QStringLiteral("Temporary registry snapshot failed:"),ancestor,status);
            if(identity(ancestor)==identity(key)&&status==ERROR_SUCCESS)
                for(const auto& value:values)if(value.name.compare(record.original.name,Qt::CaseInsensitive)==0)
                {record.original=value;record.existed=true;break;}
        }
        record.createdKeys.reserve(record.missingAncestors.size());record.captured=true;return true;
    }
    inline bool disableOverride(Port& port,TemporaryOverride& record)
    {
        if(!record.captured)return false;
        record.pending=true; // Durable value obligation before the first possible mutation.
        for(const auto& ancestor:record.missingAncestors)
        {
            bool created=false;const auto status=port.createTracked(ancestor,created);
            if(created)record.createdKeys.push_back(ancestor);
            if(status!=ERROR_SUCCESS)return fail(&record.error,QStringLiteral("Temporary registry create failed:"),ancestor,status);
        }
        const Value disabled{record.original.name,REG_SZ,QByteArray(2,'\0')};
        const auto status=port.write(record.key,disabled);
        return status==ERROR_SUCCESS||fail(&record.error,QStringLiteral("Temporary registry write failed:"),record.key,status);
    }
    inline bool restoreOverride(Port& port,TemporaryOverride& record)
    {
        if(!record.pending)return true;
        QVector<Value> values;QStringList children;auto status=port.read(record.key,values,children);
        const auto match=[&](const QVector<Value>& actual,bool original){
            const Value* found=nullptr;for(const auto& value:actual)if(value.name.compare(record.original.name,Qt::CaseInsensitive)==0){found=&value;break;}
            if(original)return record.existed?found&&found->type==record.original.type&&found->bytes==record.original.bytes:!found;
            return found&&found->type==REG_SZ&&found->bytes==QByteArray(2,'\0');
        };
        if(status!=ERROR_SUCCESS&&status!=ERROR_FILE_NOT_FOUND&&status!=ERROR_PATH_NOT_FOUND)
            return fail(&record.error,QStringLiteral("Temporary registry verification failed:"),record.key,status);
        bool clean=status==ERROR_SUCCESS?match(values,true):!record.existed;
        if(!clean)
        {
            if(status!=ERROR_SUCCESS||!match(values,false))
                return fail(&record.error,QStringLiteral("Temporary registry value changed externally:"),record.key,ERROR_RETRY);
            status=record.existed?port.write(record.key,record.original):port.eraseValue(record.key,record.original.name);
            const auto restoreStatus=status;
            values.clear();children.clear();status=port.read(record.key,values,children);
            if(restoreStatus!=ERROR_SUCCESS&&restoreStatus!=ERROR_FILE_NOT_FOUND)
                return fail(&record.error,QStringLiteral("Temporary registry restore failed:"),record.key,restoreStatus);
            if(status!=ERROR_SUCCESS||!match(values,true))
                return fail(&record.error,QStringLiteral("Temporary registry readback mismatch:"),record.key,status==ERROR_SUCCESS?ERROR_INVALID_DATA:status);
        }
        for(int index=record.createdKeys.size()-1;index>=0;--index)
        {
            const auto& key=record.createdKeys[index];values.clear();children.clear();status=port.read(key,values,children);
            if(status==ERROR_FILE_NOT_FOUND||status==ERROR_PATH_NOT_FOUND)continue;
            if(status!=ERROR_SUCCESS)return fail(&record.error,QStringLiteral("Temporary registry key verification failed:"),key,status);
            if(!values.isEmpty()||!children.isEmpty())continue; // External content owns this container now.
            status=port.removeEmptyKey(key);
            if(status!=ERROR_SUCCESS&&status!=ERROR_FILE_NOT_FOUND&&status!=ERROR_PATH_NOT_FOUND)
                return fail(&record.error,QStringLiteral("Temporary registry key cleanup failed:"),key,status);
            values.clear();children.clear();status=port.read(key,values,children);
            if(status!=ERROR_FILE_NOT_FOUND&&status!=ERROR_PATH_NOT_FOUND)
                return fail(&record.error,QStringLiteral("Temporary registry key still exists:"),key,status==ERROR_SUCCESS?ERROR_INVALID_DATA:status);
        }
        record.pending=false;record.error.clear();return true;
    }
    inline bool restoreOverrides(Port& port,LatencySession& session)
    {
        bool complete=true;
        for(auto it=session.overrides.rbegin();it!=session.overrides.rend();++it)
            if(!restoreOverride(port,*it))complete=false;
        session.recoveryPending.store(!complete);return complete;
    }
    inline bool cleanupLatencySession(Port& port,LatencySession& session)
    {
        session.stop.store(true);
        const bool retainedFailure=!session.cleanupRequestedOnce.exchange(true)&&session.failedInlineRestore.load();
        const bool complete=!retainedFailure&&restoreOverrides(port,session);
        if(!complete)
        {
            session.failedInlineRestore.store(true);QStringList failures;
            for(const auto& record:session.overrides)if(record.pending)failures.push_back(record.error);
            std::lock_guard<std::mutex> lock(session.progressMutex);
            session.progressText=QStringLiteral("临时注册表恢复未完成，请重试恢复：%1").arg(failures.join('\n'));
        }
        return complete;
    }

}
