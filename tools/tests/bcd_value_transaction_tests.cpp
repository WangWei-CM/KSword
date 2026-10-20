#include <QCoreApplication>
#include <QStringList>
#include <QVariantMap>
#include <QRegularExpression>
#include <vector>
#include <iostream>
#include <stdexcept>
struct kLogEvent {};
struct EndLog {};
static EndLog eol;
static int logCount=0;
struct NullLog { template<class T> NullLog& operator<<(const T&) { return *this; } NullLog& operator<<(EndLog) { ++logCount; return *this; } };
static NullLog info,warn,err;
    constexpr char kEnumResultStartSucceededKey[] = "startSucceeded";
    constexpr char kEnumResultTimeoutKey[] = "timeout";
    constexpr char kEnumResultExitCodeKey[] = "exitCode";
    constexpr char kEnumResultStandardOutputKey[] = "standardOutput";
    constexpr char kEnumResultStandardErrorKey[] = "standardError";
constexpr int kDefaultCommandTimeoutMs=30000;
    struct BcdCommandResult
    {
        bool pending = false;         // pending：只读枚举尚在后台执行，保留当前界面状态。
        bool startSucceeded = false;  // startSucceeded：进程是否成功启动。
        bool timeout = false;         // timeout：命令是否超时。
        int exitCode = -1;            // exitCode：bcdedit 进程退出码。
        QString standardOutputText;   // standardOutputText：标准输出文本。
        QString standardErrorText;    // standardErrorText：标准错误文本。
        QString mergedOutputText;     // mergedOutputText：stdout + stderr 合并文本。
    };
    enum class BcdErrorStyle { Generic, Copy, Custom };
    struct BcdPlannedCommand { QStringList args; QString operation; bool missingOkay = false; };
    struct BcdCommandPlan
    {
        std::vector<BcdPlannedCommand> commands;
        QString successTitle;
        QString successText;
        BcdErrorStyle errorStyle = BcdErrorStyle::Generic;
        bool refreshAfter = true;
        bool copyAndAppend = false;
    };
    struct StepReceipt { BcdPlannedCommand command; BcdCommandResult result; bool success = false; bool tolerated = false; bool optional = false; };
    struct Receipt { std::vector<StepReceipt> steps; BcdCommandResult finalEnum; bool success = true; bool hasEnum = false; };

static std::vector<QStringList> calls;
static std::vector<QVariantMap> results;
static QVariantMap response(int exit=0, const QString& text=QStringLiteral("ok"), bool started=true, bool timeout=false)
{
    return {{QString::fromLatin1(kEnumResultStartSucceededKey),started},
        {QString::fromLatin1(kEnumResultTimeoutKey),timeout},
        {QString::fromLatin1(kEnumResultExitCodeKey),exit},
        {QString::fromLatin1(kEnumResultStandardOutputKey),text},
        {QString::fromLatin1(kEnumResultStandardErrorKey),QString()}};
}
static QVariantMap runBcdEditInCallingThread(const QStringList& args, int)
{
    const auto index=calls.size(); calls.push_back(args);
    if(index>=results.size()) throw std::runtime_error("unexpected extra BCD command");
    return results[index];
}
static void reset(std::initializer_list<QVariantMap> outputs) { calls.clear();logCount=0;results=outputs; }
static unsigned checks=0;
static void check(bool value,const char* message) { ++checks;if(!value)throw std::runtime_error(message); }
int main(int argc,char**argv)
{
    QCoreApplication app(argc,argv);
    const auto worker = [](const BcdCommandPlan& request) {
            Receipt receipt;
            const auto execute = [](const BcdPlannedCommand& command) {
                const QVariantMap map = runBcdEditInCallingThread(command.args, kDefaultCommandTimeoutMs);
                BcdCommandResult result;
                result.startSucceeded = map.value(QString::fromLatin1(kEnumResultStartSucceededKey)).toBool();
                result.timeout = map.value(QString::fromLatin1(kEnumResultTimeoutKey)).toBool();
                result.exitCode = map.value(QString::fromLatin1(kEnumResultExitCodeKey)).toInt();
                result.standardOutputText = map.value(QString::fromLatin1(kEnumResultStandardOutputKey)).toString();
                result.standardErrorText = map.value(QString::fromLatin1(kEnumResultStandardErrorKey)).toString();
                result.mergedOutputText = result.standardOutputText;
                if (!result.standardErrorText.trimmed().isEmpty())
                {
                    if (!result.mergedOutputText.trimmed().isEmpty()) result.mergedOutputText += QStringLiteral("\n");
                    result.mergedOutputText += result.standardErrorText;
                }
                kLogEvent event;
                if (!result.startSucceeded) err << event << "[BootEditor] 命令启动失败: " << command.operation.toStdString() << ", error=" << result.standardErrorText.toStdString() << eol;
                else if (result.timeout) err << event << "[BootEditor] 命令执行超时: " << command.operation.toStdString() << ", timeoutMs=" << kDefaultCommandTimeoutMs << eol;
                else if (result.exitCode == 0) info << event << "[BootEditor] 命令执行完成: " << command.operation.toStdString() << ", exitCode=" << result.exitCode << eol;
                else warn << event << "[BootEditor] 命令返回非零: " << command.operation.toStdString() << ", exitCode=" << result.exitCode << eol;
                return result;
            };
            for (const auto& command : request.commands)
            {
                StepReceipt step{command, execute(command)};
                step.success = step.result.startSucceeded && !step.result.timeout && step.result.exitCode == 0;
                const QString lower = step.result.mergedOutputText.toLower();
                step.tolerated = !step.success && command.missingOkay &&
                    (lower.contains(QStringLiteral("not found")) || lower.contains(QStringLiteral("cannot find")) || lower.contains(QStringLiteral("does not exist"))
                     || lower.contains(QStringLiteral("找不到")) || lower.contains(QStringLiteral("不存在")) || lower.contains(QStringLiteral("未找到")));
                if (step.tolerated)
                {
                    kLogEvent warning;
                    warn << warning << "[BootEditor] deletevalue 字段不存在，按目标状态继续: element="
                        << command.args.value(2).toStdString() << ", identifier=" << command.args.value(1).toStdString() << eol;
                }
                receipt.steps.push_back(std::move(step));
                if (!receipt.steps.back().success && !receipt.steps.back().tolerated) { receipt.success = false; break; }
            }
            if (request.copyAndAppend && receipt.success)
            {
                const QRegularExpression regex(QStringLiteral("\\{[^\\}\\s]+\\}"));
                auto matches = regex.globalMatch(receipt.steps.front().result.mergedOutputText);
                QString identifier;
                while (matches.hasNext()) identifier = matches.next().captured(0);
                if (!identifier.trimmed().isEmpty())
                {
                    BcdPlannedCommand command{{QStringLiteral("/displayorder"), identifier.trimmed(), QStringLiteral("/addlast")}, QStringLiteral("追加到 displayorder"), false};
                    StepReceipt step{command, execute(command)};
                    step.success = step.result.startSucceeded && !step.result.timeout && step.result.exitCode == 0;
                    step.optional = true; // Copy success remains true even if the optional displayorder update failed.
                    receipt.steps.push_back(std::move(step));
                }
            }
            if (request.refreshAfter)
            {
                receipt.finalEnum = execute({{QStringLiteral("/enum"), QStringLiteral("all"), QStringLiteral("/v")}, QStringLiteral("枚举全部 BCD 条目"), false});
                receipt.hasEnum = true;
            }
            return receipt;
        };
    const BcdPlannedCommand first{{QStringLiteral("/set"),QStringLiteral("{id}"),QStringLiteral("description"),QStringLiteral("path with spaces 中文")},QStringLiteral("write"),false};
    const BcdPlannedCommand second{{QStringLiteral("/set"),QStringLiteral("{id}"),QStringLiteral("debug"),QStringLiteral("on")},QStringLiteral("write2"),false};
    const BcdPlannedCommand third{{QStringLiteral("/set"),QStringLiteral("{id}"),QStringLiteral("bootlog"),QStringLiteral("yes")},QStringLiteral("write3"),false};
    BcdCommandPlan plan;plan.commands={first,second,third};
    reset({response(),response(),response(),response(0,QStringLiteral("final enum"))});
    auto receipt=worker(plan);
    check(receipt.success&&receipt.hasEnum&&receipt.steps.size()==3,"all steps and final query retained");
    check(calls.size()==4&&calls[0]==first.args&&calls[1]==second.args&&calls[2]==third.args,"exact frozen args and ordering");
    check(calls[3]==QStringList({QStringLiteral("/enum"),QStringLiteral("all"),QStringLiteral("/v")}),"final query after commands");
    check(receipt.finalEnum.mergedOutputText==QStringLiteral("final enum")&&logCount==4,"complete final receipt and one audit per command");
    reset({response(),response(5,QStringLiteral("denied")),response(0,QStringLiteral("partial state"))});receipt=worker(plan);
    check(!receipt.success&&receipt.steps.size()==2&&calls.size()==3,"strict failure stops later write and reads actual state");
    check(receipt.steps[1].result.exitCode==5&&receipt.steps[1].result.mergedOutputText==QStringLiteral("denied"),"failed receipt preserved");
    for(const auto& missing:QStringList{QStringLiteral("not found"),QStringLiteral("cannot find"),QStringLiteral("does not exist"),QStringLiteral("找不到"),QStringLiteral("不存在"),QStringLiteral("未找到")})
    {
        plan.commands={{{QStringLiteral("/deletevalue"),QStringLiteral("{id}"),QStringLiteral("safeboot")},QStringLiteral("delete"),true},second};
        reset({response(1,missing),response(),response()});receipt=worker(plan);
        check(receipt.success&&receipt.steps.front().tolerated&&calls.size()==3,"all original missing-element tolerances retained");
    }
    plan.commands={{{QStringLiteral("/copy"),QStringLiteral("{id}"),QStringLiteral("/d"),QStringLiteral("copy")},QStringLiteral("copy"),false}};plan.copyAndAppend=true;
    reset({response(0,QStringLiteral("copied {11111111-2222-3333-4444-555555555555}")),response(1,QStringLiteral("append failed")),response()});receipt=worker(plan);
    check(receipt.success&&receipt.steps.size()==2&&receipt.steps[1].optional&&!receipt.steps[1].success,"copy success and optional append failure semantics retained");
    check(calls[1]==QStringList({QStringLiteral("/displayorder"),QStringLiteral("{11111111-2222-3333-4444-555555555555}"),QStringLiteral("/addlast")}),"copy GUID-derived followup executes inside accepted plan");
    reset({response(0,QStringLiteral("no guid")),response()});receipt=worker(plan);check(receipt.success&&calls.size()==2,"no guessed GUID appended");
    plan.copyAndAppend=false;plan.refreshAfter=false;plan.commands={first};
    const QString large(1024*1024,QLatin1Char('X'));
    reset({response(0,large)});receipt=worker(plan);check(receipt.success&&!receipt.hasEnum&&calls.size()==1&&receipt.steps[0].result.mergedOutputText==large,"custom full output is never capped or replaced");
    reset({response(-1,QStringLiteral("start failed"),false)});receipt=worker(plan);check(!receipt.success&&!receipt.steps[0].result.startSucceeded,"startup failure preserved");
    reset({response(-1,QStringLiteral("timeout"),true,true)});receipt=worker(plan);check(!receipt.success&&receipt.steps[0].result.timeout,"timeout preserved");
    plan.commands={{{QStringLiteral("/deletevalue"),QStringLiteral("{id}"),QStringLiteral("safeboot")},QStringLiteral("delete"),false}};
    reset({response(1,QStringLiteral("not found"))});receipt=worker(plan);check(!receipt.success&&!receipt.steps[0].tolerated,"only declared delete tolerance is accepted");
    std::cout<<"BCD_VALUE_PLAN_CHECKS="<<checks<<" FAILURES=0 GUI=0 BCD_PROCESS_CALLS=0\n";
}
