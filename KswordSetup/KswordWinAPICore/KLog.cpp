#include "KLog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <unordered_map>
#include <utility>

// CoCreateGuid is provided by Ole32.lib. The vcxproj also lists this dependency
// so command-line and IDE builds both resolve the symbol.
#pragma comment(lib, "Ole32.lib")

namespace {

// The default text color is restored after every colored prefix write.
constexpr WORD DefaultConsoleColor = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

// ConsoleMutex serializes complete log-line printing, including color changes.
std::mutex& ConsoleMutex() {
    static std::mutex mutex;
    return mutex;
}

// CreateGuid asks Windows for a GUID and returns an all-zero GUID only on failure.
GUID CreateGuid() {
    GUID guidValue{};
    if (::CoCreateGuid(&guidValue) != S_OK) {
        return GUID{};
    }
    return guidValue;
}

// LevelBadge returns the compact severity marker displayed before each line.
const char* LevelBadge(kLogLevel level) {
    switch (level) {
    case kLogLevel::Debug:
        return "[   ]";
    case kLogLevel::Info:
        return "[ + ]";
    case kLogLevel::Warn:
        return "[ ! ]";
    case kLogLevel::Error:
        return "[ x ]";
    case kLogLevel::Fatal:
        return "[!!!]";
    default:
        return "[ ? ]";
    }
}

// PrefixColor maps each severity to a Windows console attribute.
WORD PrefixColor(kLogLevel level) {
    switch (level) {
    case kLogLevel::Debug:
        return FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    case kLogLevel::Info:
        return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    case kLogLevel::Warn:
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    case kLogLevel::Error:
        return FOREGROUND_RED | FOREGROUND_INTENSITY;
    case kLogLevel::Fatal:
        return BACKGROUND_RED | BACKGROUND_INTENSITY | DefaultConsoleColor;
    default:
        return DefaultConsoleColor;
    }
}

// ShouldPrintLocation limits file and line output to Error/Fatal console records.
bool ShouldPrintLocation(kLogLevel level) {
    return level == kLogLevel::Error || level == kLogLevel::Fatal;
}

// OutputStreamForLevel routes Error/Fatal to stderr and other levels to stdout.
std::ostream& OutputStreamForLevel(kLogLevel level) {
    return ShouldPrintLocation(level) ? std::cerr : std::cout;
}

// ConsoleHandleForLevel picks the matching Windows console handle for the stream.
DWORD ConsoleHandleForLevel(kLogLevel level) {
    return ShouldPrintLocation(level) ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE;
}

// BuildLocationString combines file and line into the archived "file:line" form.
std::string BuildLocationString(const char* filePath, int lineNumber) {
    const std::string safePath = (filePath == nullptr) ? "" : std::string(filePath);
    return safePath + ":" + std::to_string(lineNumber);
}

// ResolveFunctionName prefers a compiler function signature and falls back to a name.
std::string ResolveFunctionName(const char* functionName, const char* functionSignature) {
    const std::string signature = (functionSignature == nullptr) ? "" : std::string(functionSignature);
    if (!signature.empty()) {
        return signature;
    }
    return (functionName == nullptr) ? "" : std::string(functionName);
}

// FileNameOnly shortens console location text while archive data keeps full paths.
std::string FileNameOnly(const std::string& filePath) {
    const std::size_t slashPosition = filePath.find_last_of("\\/");
    if (slashPosition == std::string::npos) {
        return filePath;
    }
    return filePath.substr(slashPosition + 1);
}

// SanitizeTsvField replaces separators so Save keeps a stable tab-separated shape.
std::string SanitizeTsvField(std::string fieldValue) {
    std::replace(fieldValue.begin(), fieldValue.end(), '\t', ' ');
    std::replace(fieldValue.begin(), fieldValue.end(), '\r', ' ');
    std::replace(fieldValue.begin(), fieldValue.end(), '\n', ' ');
    return fieldValue;
}

} // namespace

// The process-wide event archive instance.
kEventEntry KswordARKEventEntry;

// The five public stream objects exposed through KLog.h and ksword.h.
LogStream dbg(kLogLevel::Debug);
LogStream info(kLogLevel::Info);
LogStream warn(kLogLevel::Warn);
LogStream err(kLogLevel::Error);
LogStream fatal(kLogLevel::Fatal);

// kLogEvent construction creates a GUID immediately for later stream binding.
kLogEvent::kLogEvent()
    : guid(CreateGuid()) {
}

// GuidToString formats a GUID as 8-4-4-4-12 uppercase hexadecimal text.
std::string GuidToString(const GUID& guidValue) {
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

// IsSameGuid compares GUID memory because GUID is a plain Win32 value type.
bool IsSameGuid(const GUID& leftGuid, const GUID& rightGuid) {
    return std::memcmp(&leftGuid, &rightGuid, sizeof(GUID)) == 0;
}

// LogLevelToString returns a stable uppercase label for exports and diagnostics.
std::string LogLevelToString(kLogLevel logLevel) {
    switch (logLevel) {
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

// FormatTimeToString converts time_t to local time using MSVC's thread-safe helper.
std::string FormatTimeToString(std::time_t timeValue) {
    std::tm localTime{};
    if (::localtime_s(&localTime, &timeValue) != 0) {
        return "1970-01-01 00:00:00";
    }

    std::ostringstream formattedTime;
    formattedTime << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return formattedTime.str();
}

// add appends an item under lock and increments the change counter.
void kEventEntry::add(kEvent eventItem) {
    std::lock_guard<std::mutex> lockGuard(m_mutex);
    m_events.emplace_back(std::move(eventItem));
    ++m_revision;
}

// clear removes every archived event under lock and increments the change counter.
void kEventEntry::clear() {
    std::lock_guard<std::mutex> lockGuard(m_mutex);
    m_events.clear();
    ++m_revision;
}

// Save copies a snapshot first, then writes it to disk without holding the mutex.
bool kEventEntry::Save(std::string outputPath) {
    std::vector<kEvent> eventsSnapshot;
    {
        std::lock_guard<std::mutex> lockGuard(m_mutex);
        eventsSnapshot = m_events;
    }

    std::ofstream outputFile(outputPath, std::ios::out | std::ios::trunc);
    if (!outputFile.is_open()) {
        return false;
    }

    for (const kEvent& singleEvent : eventsSnapshot) {
        outputFile
            << LogLevelToString(singleEvent.level) << '\t'
            << FormatTimeToString(singleEvent.timestamp) << '\t'
            << GuidToString(singleEvent.guid) << '\t'
            << SanitizeTsvField(singleEvent.content) << '\t'
            << SanitizeTsvField(singleEvent.fileLocation) << '\t'
            << SanitizeTsvField(singleEvent.functionName) << '\n';
    }

    return outputFile.good();
}

// Track filters archived records by GUID and returns a detached result vector.
std::vector<kEvent> kEventEntry::Track(GUID targetGuid) {
    std::vector<kEvent> trackedEvents;
    std::lock_guard<std::mutex> lockGuard(m_mutex);
    for (const kEvent& singleEvent : m_events) {
        if (IsSameGuid(singleEvent.guid, targetGuid)) {
            trackedEvents.push_back(singleEvent);
        }
    }
    return trackedEvents;
}

// Snapshot returns a full copy so callers can inspect records without holding locks.
std::vector<kEvent> kEventEntry::Snapshot() const {
    std::lock_guard<std::mutex> lockGuard(m_mutex);
    return m_events;
}

// Revision returns the current archive revision under lock.
std::size_t kEventEntry::Revision() const {
    std::lock_guard<std::mutex> lockGuard(m_mutex);
    return m_revision;
}

// LogStream binds a severity to this public stream instance.
LogStream::LogStream(kLogLevel level)
    : m_level(level) {
}

// operator<<(kLogEvent) stores the event GUID for the next eol commit.
LogStream& LogStream::operator<<(const kLogEvent& logEvent) {
    PendingLogState& pendingState = GetPendingState();
    pendingState.hasEvent = true;
    pendingState.currentGuid = logEvent.guid;
    return *this;
}

// operator<<(OStreamManipulator) supports std::endl and flushes it for compatibility.
LogStream& LogStream::operator<<(OStreamManipulator manipulator) {
    PendingLogState& pendingState = GetPendingState();
    manipulator(pendingState.messageBuffer);

    if (manipulator == static_cast<OStreamManipulator>(std::endl<char, std::char_traits<char>>)) {
        LogEndToken compatibilityToken{};
        FlushPendingState(compatibilityToken);
    }
    return *this;
}

// operator<<(IOSManipulator) forwards stream state manipulators into the buffer.
LogStream& LogStream::operator<<(IOSManipulator manipulator) {
    PendingLogState& pendingState = GetPendingState();
    manipulator(pendingState.messageBuffer);
    return *this;
}

// operator<<(IOSBaseManipulator) forwards base stream manipulators into the buffer.
LogStream& LogStream::operator<<(IOSBaseManipulator manipulator) {
    PendingLogState& pendingState = GetPendingState();
    manipulator(pendingState.messageBuffer);
    return *this;
}

// operator<<(LogEndToken) is the planned commit path for all new log statements.
LogStream& LogStream::operator<<(const LogEndToken& logEndToken) {
    FlushPendingState(logEndToken);
    return *this;
}

// GetPendingState returns this stream's state in a thread-local map keyed by object.
LogStream::PendingLogState& LogStream::GetPendingState() {
    thread_local std::unordered_map<const LogStream*, PendingLogState> threadLocalStates;
    return threadLocalStates[this];
}

// FlushPendingState creates an archive record, prints a colored line, and clears state.
void LogStream::FlushPendingState(const LogEndToken& logEndToken) {
    PendingLogState& pendingState = GetPendingState();
    std::string messageText = pendingState.messageBuffer.str();

    if (!messageText.empty() && messageText.back() == '\n') {
        messageText.pop_back();
        if (!messageText.empty() && messageText.back() == '\r') {
            messageText.pop_back();
        }
    }

    const GUID activeGuid = pendingState.hasEvent ? pendingState.currentGuid : CreateGuid();
    const std::time_t nowTime = std::time(nullptr);
    const std::string locationString = BuildLocationString(logEndToken.filePath, logEndToken.lineNumber);
    const std::string functionString = ResolveFunctionName(logEndToken.functionName, logEndToken.functionSignature);

    kEvent archivedEvent;
    archivedEvent.guid = activeGuid;
    archivedEvent.level = m_level;
    archivedEvent.content = messageText;
    archivedEvent.fileLocation = locationString;
    archivedEvent.functionName = functionString;
    archivedEvent.timestamp = nowTime;
    KswordARKEventEntry.add(std::move(archivedEvent));

    {
        std::lock_guard<std::mutex> lockGuard(ConsoleMutex());
        std::ostream& outputStream = OutputStreamForLevel(m_level);
        HANDLE outputHandle = ::GetStdHandle(ConsoleHandleForLevel(m_level));

        CONSOLE_SCREEN_BUFFER_INFO originalConsoleInfo{};
        const bool hasConsoleColor =
            outputHandle != nullptr &&
            outputHandle != INVALID_HANDLE_VALUE &&
            ::GetConsoleScreenBufferInfo(outputHandle, &originalConsoleInfo) != 0;

        if (hasConsoleColor) {
            ::SetConsoleTextAttribute(outputHandle, PrefixColor(m_level));
        }

        outputStream << LevelBadge(m_level) << "[" << FormatTimeToString(nowTime) << "] ";

        if (hasConsoleColor) {
            ::SetConsoleTextAttribute(outputHandle, DefaultConsoleColor);
        }

        outputStream << messageText;

        if (ShouldPrintLocation(m_level)) {
            const std::string filePath = (logEndToken.filePath == nullptr) ? "" : std::string(logEndToken.filePath);
            outputStream
                << " (File:" << FileNameOnly(filePath)
                << ", Line " << logEndToken.lineNumber;

            if (!functionString.empty()) {
                outputStream << ", " << functionString;
            }

            outputStream << ")";
        }

        outputStream << std::endl;

        if (hasConsoleColor) {
            ::SetConsoleTextAttribute(outputHandle, originalConsoleInfo.wAttributes);
        }
    }

    pendingState.hasEvent = false;
    pendingState.currentGuid = GUID{};
    pendingState.messageBuffer.str("");
    pendingState.messageBuffer.clear();
}
