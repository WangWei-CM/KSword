#pragma once

// ==============================
// Framework.h
// 该头文件是 Framework 模块对外唯一入口：
// 1) 暴露日志等级、事件结构与日志管理器；
// 2) 暴露五个全局流式日志对象（dbg/info/warn/err/fatal）；
// 3) 暴露 eol 宏用于携带文件/行号/函数信息并触发一次日志提交；
// 4) 暴露 kProgress 任务进度管理器（kPro）。
// ==============================

#include <cstddef>    // std::size_t：用于修订号与容器索引。
#include <ctime>      // std::time_t：用于记录日志时间戳。
#include <mutex>      // std::mutex：用于日志容器线程安全。
#include <sstream>    // std::ostringstream：用于流式拼接日志文本。
#include <string>     // std::string：用于保存日志内容/文件名/函数名。
#include <vector>     // std::vector：用于日志管理器内部存储。

// Windows 平台下使用 GUID 作为事件唯一标识。
#include <guiddef.h>

// Ksword.h：统一包含 ksword/ 目录下的 Win32 工具封装（进程/字符串等）。
// 规范要求：Framework.h 作为全局入口，需要级联引入 Ksword.h。
#include "Ksword.h"

// 日志等级枚举：用于统一控制颜色、图标、筛选与导出文本。
enum class kLogLevel
{
    Debug = 0, // 调试级别：蓝色前缀。
    Info,      // 信息级别：绿色前缀。
    Warn,      // 警告级别：黄色前缀。
    Error,     // 错误级别：红色前缀，额外输出位置信息。
    Fatal      // 致命级别：红底白字前缀，额外输出位置信息。
};

// kLogEvent：单次业务事件对象，仅包含一个 const GUID。
// 说明：
// - 该类型原本命名为 event；
// - 但在 Qt 的 QObject 派生类成员函数内，event 名称容易与 QObject::event() 冲突；
// - 因此改为 kLogEvent 以避免歧义与编译错误。
class kLogEvent
{
public:
    // 构造函数作用：
    // - 自动创建随机 GUID；
    // - 初始化 const 成员 guid，确保创建后不可修改。
    kLogEvent();

    // guid 作用：
    // - 当前事件唯一标识；
    // - 用于“跟踪事件”功能在日志列表中筛选关联日志。
    const GUID guid;
};

// kEvent：日志系统存档结构。
// 每次通过 dbg/info/warn/err/fatal 输出并遇到 eol 时都会产生一条 kEvent。
struct kEvent
{
    GUID guid{};              // 本条日志关联的事件 GUID（来自 kLogEvent 对象）。
    kLogLevel level{};        // 本条日志的等级（Debug/Info/Warn/Error/Fatal）。
    std::string content;      // 日志正文（流式拼接后的文本内容）。
    std::string fileLocation; // 文件位置信息（格式通常为 xxx.cpp:123）。
    std::string functionName; // 函数签名或函数名（优先使用 MSVC __FUNCSIG）。
    std::time_t timestamp{};  // 日志时间戳（本地时间基准）。
};

// kEventEntry：日志管理器，负责存储、清理、导出、按 GUID 追踪。
class kEventEntry
{
public:
    // add 作用：
    // - 把一条 kEvent 追加到内部 vector；
    // - 线程安全；
    // 参数 eventItem：待保存的日志项（按值传入后内部 move）。
    void add(kEvent eventItem);

    // clear 作用：
    // - 清空全部日志记录；
    // - 线程安全。
    void clear();

    // Save 作用：
    // - 把内部全部日志保存到 outputPath 指向的文件；
    // - 按“字段\t字段\t字段\n”格式输出；
    // 返回值：
    // - true  表示保存成功；
    // - false 表示打开文件失败或写入失败。
    bool Save(std::string outputPath);

    // Track 作用：
    // - 返回与 targetGuid 完全相同 GUID 的全部日志项副本；
    // 参数 targetGuid：用于追踪的 GUID。
    // 返回值：匹配 GUID 的日志列表。
    std::vector<kEvent> Track(GUID targetGuid);

    // Snapshot 作用：
    // - 返回当前全部日志的快照副本（给 UI 渲染使用）；
    // - 与 Track 不同，Snapshot 不做 GUID 过滤。
    std::vector<kEvent> Snapshot() const;

    // Revision 作用：
    // - 返回日志版本号（每次 add/clear 自增）；
    // - UI 可据此判断是否需要刷新表格。
    std::size_t Revision() const;

private:
    mutable std::mutex m_mutex;      // 保护 m_events/m_revision 的互斥锁。
    std::vector<kEvent> m_events;    // 全量日志容器。
    std::size_t m_revision = 0;      // 容器版本号（用于增量刷新判断）。
};

// LogEndToken：eol 宏展开后的尾标记对象。
// 该对象承载调用位置，供日志流在最终提交时使用。
struct LogEndToken
{
    const char* filePath = "";           // 调用点文件路径（__FILE__）。
    int lineNumber = 0;                  // 调用点行号（__LINE__）。
    const char* functionName = "";       // 调用点函数名（__FUNCTION__ / __func__）。
    const char* functionSignature = "";  // 调用点函数签名（MSVC 优先 __FUNCSIG__）。
};

// LogStream：支持“像 std::cout 一样”连续 << 的日志流对象。
class LogStream
{
public:
    // 构造函数作用：
    // - 绑定当前日志流的固定等级（例如 info 对象固定为 Info）。
    // 参数 level：该日志流的等级。
    explicit LogStream(kLogLevel level);

    // operator<<(kLogEvent) 作用：
    // - 绑定本条日志的事件 GUID；
    // - 必须在同一条日志表达式中最先传入 kLogEvent。
    // 参数 logEvent：调用方创建的 kLogEvent 对象。
    // 返回值：当前日志流引用，支持链式调用。
    LogStream& operator<<(const kLogEvent& logEvent);

    // 模板 operator<< 作用：
    // - 接收任意可被 std::ostringstream 输出的类型；
    // - 把文本追加到当前线程、当前日志流的缓存中。
    // 参数 value：待输出对象（字符串/数字等）。
    // 返回值：当前日志流引用，支持链式调用。
    template <typename TValue>
    LogStream& operator<<(const TValue& value)
    {
        PendingLogState& pendingState = getPendingState();
        pendingState.messageBuffer << value;
        return *this;
    }

    // operator<<(ostream manipulator) 作用：
    // - 支持 std::endl 等标准流操纵符写入缓存。
    // 参数 streamManipulator：标准流操纵函数指针。
    // 返回值：当前日志流引用，支持链式调用。
    LogStream& operator<<(std::ostream& (*streamManipulator)(std::ostream&));

    // operator<<(LogEndToken) 作用：
    // - 接收 eol 宏携带的文件/行号/函数信息；
    // - 触发一次最终提交（控制台输出 + 归档到 kEventEntry）。
    // 参数 logEndToken：结束标记对象（通常由 eol 宏自动生成）。
    // 返回值：当前日志流引用，支持链式调用。
    LogStream& operator<<(const LogEndToken& logEndToken);

private:
    // PendingLogState：每个线程、每个日志流对象独立持有的临时拼接状态。
    struct PendingLogState
    {
        bool hasEvent = false;             // 当前缓存是否已绑定 kLogEvent GUID。
        GUID currentGuid{};                // 当前缓存绑定的 GUID。
        std::ostringstream messageBuffer;  // 当前缓存的日志正文拼接器。
    };

    // getPendingState 作用：
    // - 获取“当前线程 + 当前 LogStream 实例”的缓存状态对象。
    // 返回值：可读写的 PendingLogState 引用。
    PendingLogState& getPendingState();

    // flushPendingState 作用：
    // - 根据缓存内容组装一条 kEvent；
    // - 输出彩色控制台日志；
    // - 写入全局日志管理器；
    // - 清理本线程缓存。
    // 参数 logEndToken：本条日志的调用位置信息。
    void flushPendingState(const LogEndToken& logEndToken);

    // m_level 作用：
    // - 记录该日志流对象固定绑定的等级。
    const kLogLevel m_level;
};

// kProgressTask：单个进度任务的可视快照数据。
// 该结构用于 UI 渲染“当前操作”卡片列表。
struct kProgressTask
{
    int pid = 0;                             // 任务唯一 ID（由 add 返回）。
    std::string taskName;                    // 任务标题（如“任务1”）。
    std::string stepName;                    // 当前步骤（如“步骤2”）。
    int stepCode = 0;                        // 步骤状态码（保留字段，供业务自定义）。
    float progress = 0.0f;                   // 规范化进度值，范围 [0.0, 1.0]。
    bool hiddenInList = false;               // true 表示该任务卡片应从列表隐藏（如完成）。
    bool hideProgressBarTemporarily = false; // true 表示临时隐藏进度条（UI 选项弹窗期间）。
};

// kProgress：进度条管理器。
// 对外能力：
// 1) add：新增任务卡片并返回 PID；
// 2) set：更新步骤与进度；
// 3) UI：阻塞弹出选项对话框，返回用户选择序号（从 1 开始）。
class kProgress
{
public:
    // 构造函数作用：
    // - 初始化 PID 自增计数与容器。
    kProgress();

    // add 作用：
    // - 新增一条任务记录并显示到任务卡片列表；
    // - 返回新任务的 PID，后续 set/UI 都用它定位任务。
    // 参数 taskName：任务名称（卡片标题）。
    // 参数 stepName：初始步骤文本。
    // 返回值：新任务 PID（大于 0）。
    int add(const std::string& taskName, const std::string& stepName);

    // set 作用：
    // - 更新指定 PID 的步骤文本与进度；
    // - 当 progressValue 最终归一化到 1.0 时自动隐藏该任务卡片。
    // 参数 pid：目标任务 PID。
    // 参数 stepName：新的步骤文本。
    // 参数 stepCode：步骤状态码（业务保留字段）。
    // 参数 progressValue：
    // - 支持 [0,1] 比例值（如 0.7）；
    // - 也支持 [0,100] 百分值（如 70.0f，会自动转换到 0.7）。
    void set(int pid, const std::string& stepName, int stepCode, float progressValue);

    // UI（vector 版本）作用：
    // - 阻塞弹出选项对话框；
    // - 对话框显示期间临时隐藏该 PID 的进度条，选择结束后恢复。
    // 参数 pid：目标任务 PID。
    // 参数 prompt：对话框提示文本。
    // 参数 options：按钮选项数组（按顺序映射返回序号）。
    // 返回值：
    // - 1..N：用户选择了第几个选项；
    // - 0：用户取消或无可用选项。
    int UI(int pid, const std::string& prompt, const std::vector<std::string>& options);

    // UI（可变参版本）作用：
    // - 语法糖重载，支持 UI(pid, "提示", "选项A", "选项B", ...)。
    // 参数 pid/prompt：含义同上。
    // 参数 options：可变参数选项列表，需可构造成 std::string。
    // 返回值：同 vector 版本。
    template <typename... TOptions>
    int UI(int pid, const std::string& prompt, const TOptions&... options)
    {
        // 把可变参数统一收敛为 vector，复用主实现。
        const std::vector<std::string> optionList{ std::string(options)... };
        return UI(pid, prompt, optionList);
    }

    // Snapshot 作用：
    // - 返回当前所有任务的快照副本（含隐藏标记）。
    // 返回值：任务数组副本。
    std::vector<kProgressTask> Snapshot() const;

    // Revision 作用：
    // - 每次 add/set/UI 状态切换都会递增；
    // - UI 可用此值判断是否需要刷新。
    // 返回值：当前修订号。
    std::size_t Revision() const;

private:
    // normalizeProgress 作用：
    // - 统一把进度值转换为 [0,1]；
    // - 自动支持“70.0f”这类百分值写法。
    // 参数 rawProgress：原始进度。
    // 返回值：归一化进度。
    static float normalizeProgress(float rawProgress);

    // setProgressBarHiddenForUi 作用：
    // - 在 UI 弹窗前后切换“临时隐藏进度条”状态。
    // 参数 pid：目标任务 PID。
    // 参数 hidden：true 隐藏，false 恢复。
    void setProgressBarHiddenForUi(int pid, bool hidden);

private:
    mutable std::mutex m_mutex;             // 保护任务容器与修订号的线程锁。
    std::vector<kProgressTask> m_tasks;     // 进度任务容器。
    int m_nextPid = 1;                      // 下一个可分配 PID（自增）。
    std::size_t m_revision = 0;             // 任务数据修订号。
};

// 全局日志管理器：供 UI 和业务统一访问。
extern kEventEntry KswordARKEventEntry;

// 全局进度管理器：供 UI 与业务统一访问。
extern kProgress kPro;

// 五个全局日志流对象：调用方式与 cout 类似。
extern LogStream dbg;   // Debug 级别日志对象。
extern LogStream info;  // Info  级别日志对象。
extern LogStream warn;  // Warn  级别日志对象。
extern LogStream err;   // Error 级别日志对象。
extern LogStream fatal; // Fatal 级别日志对象。

// GuidToString 作用：
// - 把 GUID 转成可读字符串（xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx）。
// 参数 guidValue：待转换 GUID。
// 返回值：GUID 文本。
std::string GuidToString(const GUID& guidValue);

// IsSameGuid 作用：
// - 判断两个 GUID 是否完全一致。
// 参数 leftGuid/rightGuid：待比较的两个 GUID。
// 返回值：true 表示一致，false 表示不一致。
bool IsSameGuid(const GUID& leftGuid, const GUID& rightGuid);

// LogLevelToString 作用：
// - 把日志等级枚举转换为字符串（DEBUG/INFO/WARN/ERROR/FATAL）。
// 参数 logLevel：待转换等级。
// 返回值：等级文本。
std::string LogLevelToString(kLogLevel logLevel);

// FormatTimeToString 作用：
// - 把 time_t 转成“YYYY-MM-DD HH:MM:SS”文本。
// 参数 timeValue：待格式化时间戳。
// 返回值：格式化后的时间字符串。
std::string FormatTimeToString(std::time_t timeValue);

// 在 MSVC 下优先使用 __FUNCSIG__ 获得完整函数签名。
#if defined(_MSC_VER)
#define KSWORDARK_FUNCTION_SIGNATURE __FUNCSIG__
#else
#define KSWORDARK_FUNCTION_SIGNATURE __PRETTY_FUNCTION__
#endif

// eol：日志结束宏（end of log）。
// 作用：
// 1) 自动采集文件、行号、函数名、完整函数签名；
// 2) 触发日志最终输出与归档。
#define eol LogEndToken{__FILE__, __LINE__, __FUNCTION__, KSWORDARK_FUNCTION_SIGNATURE}
