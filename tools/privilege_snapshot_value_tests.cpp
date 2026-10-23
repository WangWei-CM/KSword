// QtCore-only fake permission evidence. No collectors, widgets or account/token APIs.
#include "../Ksword5.1/Ksword5.1/PrivilegeDock/PrivilegeSnapshotModel.h"
#include "../Ksword5.1/Ksword5.1/Internationalization/LanguageManager.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QDateTime>
#include <QMap>
#include <QSet>
#include <cstdio>
#include <thread>
namespace { int checks=0, failures=0, translationCalls=0;
void expect(bool ok,const char* what) { ++checks; if(!ok){++failures; std::printf("FAIL %s\n",what);} }
QJsonObject section(QJsonArray entries={},QJsonArray errors={}) { return {{"entries",entries},{"errors",errors}}; }
QJsonObject entry(QString key,QJsonValue value,QString state="read") { return {{"key",key},{"value",value},{"state",state}}; }
QJsonObject snapshot(QJsonObject policy,QJsonObject token=section()) { return {{"schema","ksword.permission.snapshot"},{"version",1},{"computer","fixture"},{"tokenIncluded",true},{"capturedUtc","2026-10-20T00:00:00.000Z"},{"policy",policy},{"token",token}}; }
}
namespace ks::i18n {
struct LanguageManager::State {};
LanguageManager::LanguageManager():QObject(nullptr),m_state(std::make_unique<State>()){}
LanguageManager::~LanguageManager()=default;
bool LanguageManager::eventFilter(QObject*,QEvent*){return false;}
LanguageManager& LanguageManager::instance(){static LanguageManager value;return value;}
QString LanguageManager::sourceText(const QString& source)const{++translationCalls;return "translated:"+source;}
}
namespace reference { struct SnapshotDifference {QString key,before,after; enum class Kind{Added,Removed,Modified,Enabled,Disabled,Uncertain} kind=Kind::Modified;};
bool validatePermissionSnapshot(const QJsonObject&,QString*);
QVector<SnapshotDifference> comparePermissionSnapshots(const QJsonObject&,const QJsonObject&,QString*);
}
// Exact pre-optimization comparator is retained only as a test oracle.


namespace reference
{
    namespace
    {
        constexpr int MaximumEntries = 200000;
        struct Evidence { QString value; QString state; };
        QString message(const QString& source) { return ks::i18n::sourceText(source); }

        bool collect(const QJsonObject& section, const QString& prefix,
            QMap<QString, Evidence>* entries, QString* error)
        {
            if (!section.value(QStringLiteral("entries")).isArray()
                || !section.value(QStringLiteral("errors")).isArray())
            {
                if (error) *error = message(QStringLiteral("快照缺少条目或采集错误信息。"));
                return false;
            }
            const QJsonArray rows = section.value(QStringLiteral("entries")).toArray();
            if (rows.size() > MaximumEntries)
            {
                if (error) *error = message(QStringLiteral("快照条目数量超过限制。"));
                return false;
            }
            for (const QJsonValue& row : rows)
            {
                const QJsonObject object = row.toObject();
                const QString key = object.value(QStringLiteral("key")).toString();
                const QJsonValue value = object.value(QStringLiteral("value"));
                const QString state = object.value(QStringLiteral("state")).toString(QStringLiteral("read"));
                if (!row.isObject() || key.isEmpty() || key.size() > 4096
                    || value.isUndefined() || value.isNull()
                    || (!value.isString() && !value.isBool() && !value.isDouble())
                    || (object.contains(QStringLiteral("state")) && !object.value(QStringLiteral("state")).isString())
                    || (state != QLatin1String("read") && state != QLatin1String("absent") && state != QLatin1String("unreadable"))
                    || (value.isString() && value.toString().size() > 65536)
                    || entries->contains(prefix + key))
                {
                    if (error) *error = message(QStringLiteral("快照含无效或重复的条目。"));
                    return false;
                }
                // Keep a type-tagged representation: string "1" and number 1 differ.
                entries->insert(prefix + key, {QString::fromUtf8(
                    QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)), state});
            }
            if (section.value(QStringLiteral("errors")).toArray().size() > 10000)
            {
                if (error) *error = message(QStringLiteral("快照采集错误信息格式无效。"));
                return false;
            }
            for (const QJsonValue& diagnostic : section.value(QStringLiteral("errors")).toArray())
            {
                if (!diagnostic.isString() || diagnostic.toString().size() > 16384)
                {
                    if (error) *error = message(QStringLiteral("快照采集错误信息格式无效。"));
                    return false;
                }
            }
            return true;
        }

        bool partial(const QJsonObject& section)
        {
            return !section.value(QStringLiteral("errors")).toArray().isEmpty();
        }
    }

    bool validatePermissionSnapshot(const QJsonObject& snapshot, QString* error)
    {
        if (error) error->clear();
        if (snapshot.value(QStringLiteral("schema")).toString() != QLatin1String("ksword.permission.snapshot")
            || snapshot.value(QStringLiteral("version")).toInt() != 1
            || snapshot.value(QStringLiteral("computer")).toString().isEmpty()
            || snapshot.value(QStringLiteral("computer")).toString().size() > 255
            || !snapshot.value(QStringLiteral("tokenIncluded")).isBool()
            || snapshot.value(QStringLiteral("capturedUtc")).toString().size() > 64
            || !QDateTime::fromString(snapshot.value(QStringLiteral("capturedUtc")).toString(), Qt::ISODateWithMs).isValid()
            || !snapshot.value(QStringLiteral("policy")).isObject()
            || !snapshot.value(QStringLiteral("token")).isObject())
        {
            if (error) *error = message(QStringLiteral("快照格式或版本不受支持。"));
            return false;
        }
        QMap<QString, Evidence> entries;
        if (!collect(snapshot.value(QStringLiteral("policy")).toObject(), QStringLiteral("policy/"), &entries, error)
            || !collect(snapshot.value(QStringLiteral("token")).toObject(), QStringLiteral("token/"), &entries, error)) return false;
        if (entries.size() > MaximumEntries)
        {
            if (error) *error = message(QStringLiteral("快照条目数量超过限制。"));
            return false;
        }
        return true;
    }

    QVector<SnapshotDifference> comparePermissionSnapshots(
        const QJsonObject& before, const QJsonObject& after, QString* error)
    {
        QVector<SnapshotDifference> differences;
        if (!validatePermissionSnapshot(before, error) || !validatePermissionSnapshot(after, error))
            return differences;
        if (before.value(QStringLiteral("computer")) != after.value(QStringLiteral("computer")))
        {
            if (error) *error = message(QStringLiteral("两份快照来自不同计算机，无法比较本地权限变化。"));
            return differences;
        }
        if (before.value(QStringLiteral("tokenIncluded")) != after.value(QStringLiteral("tokenIncluded")))
        {
            if (error) *error = message(QStringLiteral("两份快照的令牌采集选项不同，请使用相同选项重新采集。"));
            return differences;
        }
        for (const QString& sectionName : {QStringLiteral("policy"), QStringLiteral("token")})
        {
            const QJsonObject previous = before.value(sectionName).toObject();
            const QJsonObject current = after.value(sectionName).toObject();
            QMap<QString, Evidence> a, b;
            collect(previous, sectionName + QLatin1Char('/'), &a, error);
            collect(current, sectionName + QLatin1Char('/'), &b, error);
            QSet<QString> keys;
            for (auto it = a.cbegin(); it != a.cend(); ++it) keys.insert(it.key());
            for (auto it = b.cbegin(); it != b.cend(); ++it) keys.insert(it.key());
            QStringList ordered = keys.values();
            ordered.sort();
            for (const QString& key : ordered)
            {
                const bool hasBefore = a.contains(key), hasAfter = b.contains(key);
                const Evidence av = a.value(key), bv = b.value(key);
                if (hasBefore && hasAfter && ((av.state == QLatin1String("unreadable") && bv.state == QLatin1String("unreadable"))
                    || (av.value == bv.value && av.state == bv.state))) continue;
                SnapshotDifference row{key, av.value, bv.value};
                if (av.state == QLatin1String("unreadable") || bv.state == QLatin1String("unreadable"))
                    row.kind = SnapshotDifference::Kind::Uncertain;
                else if (!hasBefore)
                    row.kind = partial(previous) ? SnapshotDifference::Kind::Uncertain : SnapshotDifference::Kind::Added;
                else if (!hasAfter)
                    row.kind = partial(current) ? SnapshotDifference::Kind::Uncertain : SnapshotDifference::Kind::Removed;
                else if (av.state == QLatin1String("absent") && bv.state == QLatin1String("read"))
                    row.kind = SnapshotDifference::Kind::Added;
                else if (av.state == QLatin1String("read") && bv.state == QLatin1String("absent"))
                    row.kind = SnapshotDifference::Kind::Removed;
                else if (key.endsWith(QLatin1String("/enabled")))
                {
                    const auto enabled = [](const QString& value) { return value == QLatin1String("[true]") || value == QLatin1String("[\"1\"]"); };
                    const auto disabled = [](const QString& value) { return value == QLatin1String("[false]") || value == QLatin1String("[\"0\"]"); };
                    if (disabled(av.value) && enabled(bv.value)) row.kind = SnapshotDifference::Kind::Enabled;
                    else if (enabled(av.value) && disabled(bv.value)) row.kind = SnapshotDifference::Kind::Disabled;
                }
                differences.push_back(row);
            }
        }
        return differences;
    }
}
int main(int argc,char**argv){QCoreApplication app(argc,argv);using namespace ks::privilege;
QString error; const auto a=snapshot(section({entry("same",true),entry("remove","x"),entry("account/S-1/enabled",false)}));
const auto b=snapshot(section({entry("same",true),entry("add",3),entry("account/S-1/enabled",true)}));
auto prepared=preparePermissionSnapshot(a,&error);auto after=preparePermissionSnapshot(b,&error);
expect(prepared&&after&&error.isEmpty(),"prepare valid");expect(prepared->document()==a&&prepared->entryCount()==3,"complete original document retained");
auto changes=comparePreparedPermissionSnapshots(*prepared,*after,&error);
expect(changes.size()==3&&changes[0].kind==SnapshotDifference::Kind::Enabled&&changes[1].kind==SnapshotDifference::Kind::Added&&changes[2].kind==SnapshotDifference::Kind::Removed,"sorted typed differences");
expect(comparePreparedPermissionSnapshots(*prepared,*prepared,&error).isEmpty()&&error.isEmpty(),"identical prepared snapshots");
for(int variant=0;variant<18;++variant){auto bad=a;
switch(variant){case 0:bad["version"]=2;break;case 1:bad["schema"]="other";break;case 2:bad["computer"]="";break;case 3:bad["computer"]=QString(256,'a');break;case 4:bad["capturedUtc"]="yesterday";break;case 5:bad["tokenIncluded"]="true";break;case 6:bad["policy"]=3;break;case 7:bad["policy"]=section({entry("same",1),entry("same",2)});break;case 8:bad["policy"]=section({entry("",1)});break;case 9:bad["policy"]=section({entry(QString(4097,'x'),1)});break;case 10:bad["policy"]=section({entry("x",QJsonValue())});break;case 11:bad["policy"]=section({entry("x",QJsonObject{})});break;case 12:bad["policy"]=section({entry("x",QString(65537,'v'))});break;case 13:bad["policy"]=section({entry("x",1,"unknown")});break;case 14:bad["policy"]=section({}, {1});break;case 15:bad["policy"]=section({}, {QString(16385,'e')});break;case 16:{auto e=entry("x",1);e["state"]=true;bad["policy"]=section({e});break;}case 17:bad["policy"]=QJsonObject{{"entries",QJsonArray{}}};break;}
expect(!preparePermissionSnapshot(bad,&error)&&!error.isEmpty(),"reject malformed original contract");expect(!error.startsWith("translated:"),"worker errors are source text");}
for(int variant=0;variant<50;++variant){QJsonArray x,y,ex,ey;
for(int i=0;i<100;++i){if((i+variant)%7) x.append(entry(QString::number(i),i, i%19==0?"unreadable":"read"));if((i+variant)%11)y.append(entry(QString::number(i),i+variant%3,i%17==0?"absent":"read"));}
if(variant%2)ex.append("partial a");if(variant%3)ey.append("partial b");auto left=snapshot(section(x,ex));auto right=snapshot(section(y,ey));QString oldError;
auto old=reference::comparePermissionSnapshots(left,right,&oldError);auto lp=preparePermissionSnapshot(left,&error),rp=preparePermissionSnapshot(right,&error);auto actual=comparePreparedPermissionSnapshots(*lp,*rp,&error);
bool equal=actual.size()==old.size();for(qsizetype i=0;equal&&i<actual.size();++i)equal=actual[i].key==old[i].key&&actual[i].before==old[i].before&&actual[i].after==old[i].after&&static_cast<int>(actual[i].kind)==static_cast<int>(old[i].kind);
expect(equal&&error.isEmpty(),"prepared merge equals original comparator");}
auto another=b;another["computer"]="other";auto anotherPrepared=preparePermissionSnapshot(another,&error);expect(comparePreparedPermissionSnapshots(*prepared,*anotherPrepared,&error).isEmpty()&&!error.isEmpty(),"cross machine refused");another=b;another["tokenIncluded"]=false;anotherPrepared=preparePermissionSnapshot(another,&error);expect(comparePreparedPermissionSnapshots(*prepared,*anotherPrepared,&error).isEmpty()&&!error.isEmpty(),"different token capture refused");
auto typed=preparePermissionSnapshot(snapshot(section({entry("type","1")})),&error);auto numeric=preparePermissionSnapshot(snapshot(section({entry("type",1)})),&error);expect(comparePreparedPermissionSnapshots(*typed,*numeric,&error).size()==1,"string numeric type preserved");
QJsonArray largeA,largeB;for(int i=0;i<200000;++i){largeA.append(entry(QString("a%1").arg(i),i));largeB.append(entry(QString("b%1").arg(i),i));}
const int oldCalls=translationCalls;bool workerOK=false;std::thread worker([&]{auto one=preparePermissionSnapshot(snapshot(section(largeA)),&error);auto two=preparePermissionSnapshot(snapshot(section(largeB)),&error);auto all=comparePreparedPermissionSnapshots(*one,*two,&error);workerOK=all.size()==400000&&one->entryCount()==200000&&two->document().value("policy").toObject().value("entries").toArray().size()==200000;largeB.append(entry("one-too-many",1));workerOK=workerOK&&!preparePermissionSnapshot(snapshot(section(largeB)),&error);});worker.join();expect(workerOK,"200k per complete snapshot and full 400k differences, reject over cap");expect(oldCalls==translationCalls,"worker no singleton/language calls");
expect(!validatePermissionSnapshot(QJsonObject{},&error)&&error.startsWith("translated:"),"legacy localized validation preserved");
std::printf("PRIVILEGE_SNAPSHOT_VALUE_CHECKS=%d FAILURES=%d\n",checks,failures);return failures?1:0;}
