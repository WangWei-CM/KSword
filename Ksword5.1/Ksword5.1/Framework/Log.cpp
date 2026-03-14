#include "../Framework.h"

// 为了控制 Windows 控制台颜色与 GUID 生成，本文件依赖 Win32 API。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Objbase.h>

#include <algorithm>  // std::replace
#include <cstdio>     // std::snprintf
#include <cstring>    // std::memcmp
#include <filesystem> // std::filesystem::u8path
#include <fstream>    // std::ofstream
#include <iomanip>    // std::setw / std::setfill
#include <iostream>   // std::cout
#include <unordered_map> // std::unordered_map：线程局部缓存容器。
#include <utility>       // std::move

// 链接 CoCreateGuid 需要的系统库。
#pragma comment(lib, "Ole32.lib")

namespace
{
    // g_consoleOutputMutex 作用：
    // - 保护控制台输出，确保多线程下单条日志不会互相穿插。
    std::mutex g_consoleOutputMutex;

    // DefaultWhiteColor：普通正文颜色（白字黑底）。
    constexpr WORD DefaultWhiteColor = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

    // CreateRandomGuid 作用：
    // - 通过 CoCreateGuid 生成随机 GUID；
    // - 失败时返回全零 GUID（极少发生，留作兜底）。
    GUID CreateRandomGuid()
    {
        GUID newGuid{};
        if (::CoCreateGuid(&newGuid) != S_OK)
        {
            // 失败时返回默认值（全 0 GUID），避免抛异常影响主流程。
            return GUID{};
        }
        return newGuid;
    }

    // GetLevelBadge 作用：
    // - 返回每个日志等级对应的控制台徽标文本。
    // 参数 level：日志等级。
    // 返回值：如 "[ + ]" / "[ x ]" / "[***]"。
    std::string GetLevelBadge(const kLogLevel level)
    {
        switch (level)
        {
        case kLogLevel::Debug:
            return "[   ]";
        case kLogLevel::Info:
            return "[ + ]";
        case kLogLevel::Warn:
            return "[ ! ]";
        case kLogLevel::Error:
            return "[ x ]";
        case kLogLevel::Fatal:
            return "[***]";
        default:
            return "[ ? ]";
        }
    }

    // GetPrefixColor 作用：
    // - 返回每个日志等级在控制台前缀（徽标 + 时间）对应的颜色属性。
    // 参数 level：日志等级。
    // 返回值：SetConsoleTextAttribute 可用的 WORD 属性。
    WORD GetPrefixColor(const kLogLevel level)
    {
        switch (level)
        {
        case kLogLevel::Debug:
            return FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        case kLogLevel::Info:
            return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case kLogLevel::Warn:
            return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case kLogLevel::Error:
            return FOREGROUND_RED | FOREGROUND_INTENSITY;
        case kLogLevel::Fatal:
            return BACKGROUND_RED | BACKGROUND_INTENSITY | DefaultWhiteColor;
        default:
            return DefaultWhiteColor;
        }
    }

    // ShouldPrintLocation 作用：
    // - 按需求仅在 Error/Fatal 输出调用位置信息。
    // 参数 level：日志等级。
    // 返回值：true 需要输出位置，false 不输出。
    bool ShouldPrintLocation(const kLogLevel level)
    {
        return level == kLogLevel::Error || level == kLogLevel::Fatal;
    }

    // ExtractFileNameOnly 作用：
    // - 从完整路径中提取文件名（用于控制台显示更简短）。
    // 参数 fullPath：完整文件路径。
    // 返回值：文件名或原始文本。
    std::string ExtractFileNameOnly(const std::string& fullPath)
    {
        const std::size_t slashPosition = fullPath.find_last_of("\\/");
        if (slashPosition == std::string::npos)
        {
            return fullPath;
        }
        return fullPath.substr(slashPosition + 1);
    }

    // SanitizeFieldForTsv 作用：
    // - 把字段中的换行和制表符替换为空格，保证导出 TSV 结构稳定。
    // 参数 fieldValue：原始字段文本。
    // 返回值：可安全写入 TSV 的文本。
    std::string SanitizeFieldForTsv(std::string fieldValue)
    {
        std::replace(fieldValue.begin(), fieldValue.end(), '\t', ' ');
        std::replace(fieldValue.begin(), fieldValue.end(), '\r', ' ');
        std::replace(fieldValue.begin(), fieldValue.end(), '\n', ' ');
        return fieldValue;
    }

    // BuildLocationString 作用：
    // - 构建 “file:line” 形式的文件定位字符串。
    // 参数 filePath：文件路径；lineNumber：行号。
    // 返回值：组合后的定位文本。
    std::string BuildLocationString(const char* filePath, const int lineNumber)
    {
        const std::string safeFilePath = (filePath == nullptr) ? "" : std::string(filePath);
        return safeFilePath + ":" + std::to_string(lineNumber);
    }

    // ResolveFunctionDescription 作用：
    // - 优先选择函数签名文本；若为空则退回函数名。
    // 参数 functionName：简短函数名；functionSignature：完整函数签名。
    // 返回值：最终函数描述文本。
    std::string ResolveFunctionDescription(const char* functionName, const char* functionSignature)
    {
        const std::string signatureText = (functionSignature == nullptr) ? "" : std::string(functionSignature);
        if (!signatureText.empty())
        {
            return signatureText;
        }
        return (functionName == nullptr) ? "" : std::string(functionName);
    }
} // namespace

// ------------------------------
// 全局对象定义区
// ------------------------------

// 全局日志管理器实例定义（extern 声明位于 Framework.h）。
kEventEntry KswordARKEventEntry;

// 五个全局日志流对象定义。
LogStream dbg(kLogLevel::Debug);
LogStream info(kLogLevel::Info);
LogStream warn(kLogLevel::Warn);
LogStream err(kLogLevel::Error);
LogStream fatal(kLogLevel::Fatal);

// ------------------------------
// kLogEvent 实现
// ------------------------------

kLogEvent::kLogEvent()
    : guid(CreateRandomGuid()) // 构造时直接为 const GUID 赋随机值。
{
}

// ------------------------------
// 工具函数实现
// ------------------------------

std::string GuidToString(const GUID& guidValue)
{
    // 固定格式：8-4-4-4-12，总长度 36。
    char guidBuffer[64] = {};
    std::snprintf(
        guidBuffer,
        sizeof(guidBuffer),
        "%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X",
        static_cast<unsigned long>(guidValue.Data1),
        static_cast<unsigned short>(guidValue.Data2),
        static_cast<unsigned short>(guidValue.Data3),
        static_cast<unsigned int>(guidValue.Data4[0]),
        static_cast<unsigned int>(guidValue.Data4[1]),
        static_cast<unsigned int>(guidValue.Data4[2]),
        static_cast<unsigned int>(guidValue.Data4[3]),
        static_cast<unsigned int>(guidValue.Data4[4]),
        static_cast<unsigned int>(guidValue.Data4[5]),
        static_cast<unsigned int>(guidValue.Data4[6]),
        static_cast<unsigned int>(guidValue.Data4[7]));
    return guidBuffer;
}

bool IsSameGuid(const GUID& leftGuid, const GUID& rightGuid)
{
    // GUID 是 POD 结构，可直接按字节比较。
    return std::memcmp(&leftGuid, &rightGuid, sizeof(GUID)) == 0;
}

std::string LogLevelToString(const kLogLevel logLevel)
{
    switch (logLevel)
    {
    case kLogLevel::Debug:
        return "DEBUG";
    case kLogLevel::Info:
        return "INFO";
    case kLogLevel::Warn:
        return "WARN";
    case kLogLevel::Error:
        return "ERROR";
    case kLogLevel::Fatal:
        return "FATAL";
    default:
        return "UNKNOWN";
    }
}

std::string FormatTimeToString(const std::time_t timeValue)
{
    // localtime_s 是 MSVC 线程安全版本。
    std::tm localTime{};
    if (::localtime_s(&localTime, &timeValue) != 0)
    {
        return "1970-01-01 00:00:00";
    }

    std::ostringstream formattedTime;
    formattedTime << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return formattedTime.str();
}

// ------------------------------
// kEventEntry 实现
// ------------------------------

void kEventEntry::add(kEvent eventItem)
{
    std::lock_guard<std::mutex> lockGuard(m_mutex);
    m_events.emplace_back(std::move(eventItem));
    ++m_revision;
}

void kEventEntry::clear()
{
    std::lock_guard<std::mutex> lockGuard(m_mutex);
    m_events.clear();
    ++m_revision;
}

bool kEventEntry::Save(std::string outputPath)
{
    // 先复制快照，尽快释放锁，减少与写入磁盘操作的锁竞争。
    std::vector<kEvent> eventsSnapshot;
    {
        std::lock_guard<std::mutex> lockGuard(m_mutex);
        eventsSnapshot = m_events;
    }

    // 使用 u8path 支持 UTF-8 字符串路径（含中文目录）。
    const std::filesystem::path outputFilePath = std::filesystem::path(outputPath);
    std::ofstream outputFile(outputFilePath, std::ios::out | std::ios::trunc);
    if (!outputFile.is_open())
    {
        return false;
    }

    // 输出字段顺序：
    // Level\tTime\tGUID\tContent\tFile\tFunction\n
    for (const kEvent& singleEvent : eventsSnapshot)
    {
        outputFile
            << LogLevelToString(singleEvent.level) << '\t'
            << FormatTimeToString(singleEvent.timestamp) << '\t'
            << GuidToString(singleEvent.guid) << '\t'
            << SanitizeFieldForTsv(singleEvent.content) << '\t'
            << SanitizeFieldForTsv(singleEvent.fileLocation) << '\t'
            << SanitizeFieldForTsv(singleEvent.functionName) << '\n';
    }

    return outputFile.good();
}

std::vector<kEvent> kEventEntry::Track(GUID targetGuid)
{
    std::vector<kEvent> trackedEvents;

    // 遍历时全程加锁，保证一致性。
    std::lock_guard<std::mutex> lockGuard(m_mutex);
    for (const kEvent& singleEvent : m_events)
    {
        if (IsSameGuid(singleEvent.guid, targetGuid))
        {
            trackedEvents.push_back(singleEvent);
        }
    }
    return trackedEvents;
}

std::vector<kEvent> kEventEntry::Snapshot() const
{
    std::lock_guard<std::mutex> lockGuard(m_mutex);
    return m_events;
}

std::size_t kEventEntry::Revision() const
{
    std::lock_guard<std::mutex> lockGuard(m_mutex);
    return m_revision;
}

// ------------------------------
// LogStream 实现
// ------------------------------

LogStream::LogStream(const kLogLevel level)
    : m_level(level)
{
}

LogStream& LogStream::operator<<(const kLogEvent& logEvent)
{
    // 绑定本次日志的事件 GUID。
    PendingLogState& pendingState = getPendingState();
    pendingState.hasEvent = true;
    pendingState.currentGuid = logEvent.guid;
    return *this;
}

LogStream& LogStream::operator<<(std::ostream& (*streamManipulator)(std::ostream&))
{
    // 支持 std::endl 等标准操纵符。
    PendingLogState& pendingState = getPendingState();
    pendingState.messageBuffer << streamManipulator;
    return *this;
}

LogStream& LogStream::operator<<(const LogEndToken& logEndToken)
{
    flushPendingState(logEndToken);
    return *this;
}

LogStream::PendingLogState& LogStream::getPendingState()
{
    // 关键点：
    // - thread_local：线程隔离；
    // - map key = this：同一线程内不同日志对象（dbg/info/...）也互不干扰。
    thread_local std::unordered_map<const LogStream*, PendingLogState> threadLocalStates;
    return threadLocalStates[this];
}

void LogStream::flushPendingState(const LogEndToken& logEndToken)
{
    PendingLogState& pendingState = getPendingState();

    // 取出消息文本。
    const std::string messageText = pendingState.messageBuffer.str();

    // 若调用方未传 kLogEvent，这里自动补一个 GUID，防止丢数据。
    const GUID activeGuid = pendingState.hasEvent ? pendingState.currentGuid : CreateRandomGuid();

    // 当前时间戳（秒级）。
    const std::time_t nowTime = std::time(nullptr);
    const std::string formattedTime = FormatTimeToString(nowTime);

    // 组装文件与函数信息（无论等级如何，均归档到 kEvent）。
    const std::string locationString = BuildLocationString(logEndToken.filePath, logEndToken.lineNumber);
    const std::string functionString = ResolveFunctionDescription(logEndToken.functionName, logEndToken.functionSignature);

    // 把日志写入全局日志管理器，供 GUI 查询、导出、追踪。
    kEvent archivedEvent;
    archivedEvent.guid = activeGuid;
    archivedEvent.level = m_level;
    archivedEvent.content = messageText;
    archivedEvent.fileLocation = locationString;
    archivedEvent.functionName = functionString;
    archivedEvent.timestamp = nowTime;
    KswordARKEventEntry.add(std::move(archivedEvent));

    // 控制台输出部分必须串行，避免多线程下行内容交错。
    {
        std::lock_guard<std::mutex> lockGuard(g_consoleOutputMutex);

        // 获取标准输出句柄。
        HANDLE outputHandle = ::GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO originalConsoleInfo{};
        const bool hasConsoleInfo =
            outputHandle != INVALID_HANDLE_VALUE &&
            outputHandle != nullptr &&
            ::GetConsoleScreenBufferInfo(outputHandle, &originalConsoleInfo) != 0;

        // 1) 先输出彩色前缀：徽标 + 时间。
        const WORD prefixColor = GetPrefixColor(m_level);
        if (hasConsoleInfo)
        {
            ::SetConsoleTextAttribute(outputHandle, prefixColor);
        }
        std::cout << GetLevelBadge(m_level) << "[" << formattedTime << "]";

        // 2) 再切回白色输出正文，满足“像 cout 那样输出后续内容”。
        if (hasConsoleInfo)
        {
            ::SetConsoleTextAttribute(outputHandle, DefaultWhiteColor);
        }
        std::cout << messageText;

        // 3) 仅 Error/Fatal 输出位置信息。
        if (ShouldPrintLocation(m_level))
        {
            const std::string shortFileName = ExtractFileNameOnly(logEndToken.filePath == nullptr ? "" : logEndToken.filePath);
            std::cout
                << "(File:" << shortFileName
                << ", Line " << logEndToken.lineNumber
                << ", " << functionString
                << ")";
        }

        // 4) 结束当前日志行并刷新。
        std::cout << std::endl;

        // 5) 恢复控制台原始颜色，避免污染后续非日志输出。
        if (hasConsoleInfo)
        {
            ::SetConsoleTextAttribute(outputHandle, originalConsoleInfo.wAttributes);
        }
    }

    // 清理当前线程缓存，准备下一条日志。
    pendingState.hasEvent = false;
    pendingState.currentGuid = GUID{};
    pendingState.messageBuffer.str("");
    pendingState.messageBuffer.clear();
}
