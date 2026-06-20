#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <io.h>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../shared/KswordArkLogProtocol.h"
#include "../shared/driver/KswordArkAlpcIoctl.h"
#include "../shared/driver/KswordArkCallbackIoctl.h"
#include "../shared/driver/KswordArkCapabilityIoctl.h"
#include "../shared/driver/KswordArkDynDataIoctl.h"
#include "../shared/driver/KswordArkFileIoctl.h"
#include "../shared/driver/KswordArkFileMonitorIoctl.h"
#include "../shared/driver/KswordArkHandleIoctl.h"
#include "../shared/driver/KswordArkKeyboardIoctl.h"
#include "../shared/driver/KswordArkKernelIoctl.h"
#include "../shared/driver/KswordArkMemoryIoctl.h"
#include "../shared/driver/KswordArkMutationIoctl.h"
#include "../shared/driver/KswordArkNetworkIoctl.h"
#include "../shared/driver/KswordArkPreflightIoctl.h"
#include "../shared/driver/KswordArkProcessIoctl.h"
#include "../shared/driver/KswordArkRedirectIoctl.h"
#include "../shared/driver/KswordArkRegistryIoctl.h"
#include "../shared/driver/KswordArkSafetyIoctl.h"
#include "../shared/driver/KswordArkSectionIoctl.h"
#include "../shared/driver/KswordArkThreadIoctl.h"
#include "../shared/driver/KswordArkTrustIoctl.h"
#include "../shared/driver/KswordArkWslSiloIoctl.h"

namespace
{
    constexpr DWORD kDefaultShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
    constexpr DWORD kDefaultDesiredAccess = GENERIC_READ | GENERIC_WRITE;
    constexpr std::size_t kSmallResponseBytes = 64U * 1024U;
    constexpr std::size_t kLargeResponseBytes = 2U * 1024U * 1024U;
    constexpr std::size_t kHugeResponseBytes = 4U * 1024U * 1024U;
    constexpr std::size_t kMaxCommandBytes = 64U * 1024U * 1024U;
    constexpr std::size_t kMaxHexBytes = 256U;

    // IoctlResult mirrors the Win32 DeviceIoControl completion state.
    // Inputs: fields are assigned by sendIoctl after each kernel request.
    // Processing: no ownership is stored here; this is a plain result packet.
    // Returns: no behavior; callers inspect ok, win32Error, and bytesReturned.
    struct IoctlResult
    {
        bool ok = false;
        DWORD win32Error = ERROR_SUCCESS;
        DWORD bytesReturned = 0;
    };

    // DriverHandle owns a Win32 HANDLE returned by CreateFileW.
    // Inputs: constructed from a raw HANDLE or default-invalid state.
    // Processing: closes the handle in reset/destructor and supports moves.
    // Returns: native() exposes the raw handle for Win32 calls without transfer.
    class DriverHandle
    {
    public:
        DriverHandle() noexcept = default;

        explicit DriverHandle(HANDLE handle) noexcept
            : handle_(handle)
        {
        }

        ~DriverHandle()
        {
            reset();
        }

        DriverHandle(const DriverHandle&) = delete;
        DriverHandle& operator=(const DriverHandle&) = delete;

        DriverHandle(DriverHandle&& other) noexcept
            : handle_(other.release())
        {
        }

        DriverHandle& operator=(DriverHandle&& other) noexcept
        {
            if (this != &other)
            {
                reset(other.release());
            }
            return *this;
        }

        bool valid() const noexcept
        {
            return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
        }

        HANDLE native() const noexcept
        {
            return handle_;
        }

        HANDLE release() noexcept
        {
            HANDLE detached = handle_;
            handle_ = INVALID_HANDLE_VALUE;
            return detached;
        }

        void reset(HANDLE next = INVALID_HANDLE_VALUE) noexcept
        {
            if (valid())
            {
                ::CloseHandle(handle_);
            }
            handle_ = next;
        }

    private:
        HANDLE handle_ = INVALID_HANDLE_VALUE;
    };

    struct ParsedOption
    {
        bool present = false;
        std::wstring value;
    };

    struct NamedArgs
    {
        std::vector<std::wstring> positionals;
        std::map<std::wstring, std::wstring> options;
    };

    // Numeric parser forward declarations are needed because option helpers are
    // defined before the concrete parser implementations in this single-file
    // CLI. Inputs are raw argv tokens; return values are unsigned scalars.
    std::uint32_t parseU32(const wchar_t* token, const char* name);
    std::uint64_t parseU64(const wchar_t* token, const char* name);

    // printWin32Error renders a failed Win32 operation in a compact form.
    // Inputs: operation describes the API/IOCTL; error is GetLastError output.
    // Processing: prints decimal and hexadecimal forms for quick triage.
    // Returns: no value; output goes to stderr.
    void printWin32Error(const wchar_t* operation, DWORD error)
    {
        std::wcerr << L"error: " << operation << L" failed, win32=" << error
                   << L" (0x" << std::hex << error << std::dec << L")\n";
    }

    // openDriver opens the shared KswordARK control/log device.
    // Inputs: desiredAccess usually includes read/write because some IOCTLs are gated.
    // Processing: calls CreateFileW on the path exported by shared protocol headers.
    // Returns: RAII DriverHandle; caller checks valid() before issuing requests.
    DriverHandle openDriver(DWORD desiredAccess = kDefaultDesiredAccess)
    {
        return DriverHandle(::CreateFileW(
            KSWORD_ARK_LOG_WIN32_PATH,
            desiredAccess,
            kDefaultShareMode,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
    }

    // sendIoctl wraps synchronous DeviceIoControl for fixed and variable buffers.
    // Inputs: open handle, control code, optional input and output buffers.
    // Processing: forwards arguments without interpreting protocol contents.
    // Returns: IoctlResult with Win32 success bit, error code, and returned byte count.
    IoctlResult sendIoctl(
        DriverHandle& handle,
        DWORD code,
        void* input,
        DWORD inputBytes,
        void* output,
        DWORD outputBytes)
    {
        IoctlResult result{};
        if (!handle.valid())
        {
            result.ok = false;
            result.win32Error = ERROR_INVALID_HANDLE;
            return result;
        }

        DWORD bytesReturned = 0;
        const BOOL ok = ::DeviceIoControl(
            handle.native(),
            code,
            input,
            inputBytes,
            output,
            outputBytes,
            &bytesReturned,
            nullptr);
        result.ok = (ok != FALSE);
        result.win32Error = result.ok ? ERROR_SUCCESS : ::GetLastError();
        result.bytesReturned = bytesReturned;
        return result;
    }

    // parseNamedArgs collects positional tokens after the command family/subcommand.
    // Inputs: argc/argv plus the start index for first option token.
    // Processing: preserves ordering for positional file/blob paths and flags.
    // Returns: NamedArgs with a positional list; option helpers scan argv directly.
    NamedArgs parseNamedArgs(int argc, wchar_t* argv[], int startIndex)
    {
        NamedArgs args{};
        for (int index = startIndex; index < argc; ++index)
        {
            if (argv[index] == nullptr || argv[index][0] == L'\0')
            {
                continue;
            }

            if (argv[index][0] == L'-')
            {
                if (index + 1 < argc && argv[index + 1] != nullptr && argv[index + 1][0] != L'-')
                {
                    args.options[argv[index]] = argv[index + 1];
                    ++index;
                }
                else
                {
                    args.options[argv[index]] = L"";
                }
                continue;
            }

            args.positionals.emplace_back(argv[index]);
        }
        return args;
    }

    // getOptionText returns a named option value from parsed args.
    // Inputs: option map and key with leading dashes.
    // Processing: caller can use fallback when the option is absent.
    // Returns: pointer to the stable internal string or nullptr.
    const std::wstring* getOptionText(const NamedArgs& args, const wchar_t* key)
    {
        const auto it = args.options.find(key);
        return it == args.options.end() ? nullptr : &it->second;
    }

    // getOptionU32 parses an option value as unsigned 32-bit.
    // Inputs: parsed args, key, and default value.
    // Processing: falls back to default when the switch is absent.
    // Returns: parsed or default value.
    std::uint32_t getOptionU32(const NamedArgs& args, const wchar_t* key, std::uint32_t defaultValue)
    {
        const std::wstring* value = getOptionText(args, key);
        return value == nullptr ? defaultValue : parseU32(value->c_str(), "option");
    }

    // getOptionU64 parses an option value as unsigned 64-bit.
    // Inputs: parsed args, key, and default value.
    // Processing: falls back to default when the switch is absent.
    // Returns: parsed or default value.
    std::uint64_t getOptionU64(const NamedArgs& args, const wchar_t* key, std::uint64_t defaultValue)
    {
        const std::wstring* value = getOptionText(args, key);
        return value == nullptr ? defaultValue : parseU64(value->c_str(), "option");
    }

    // getOptionBool reports whether the option is present.
    // Inputs: parsed args and key.
    // Processing: presence-only flag; no value is consumed.
    // Returns: true when present.
    bool getOptionBool(const NamedArgs& args, const wchar_t* key)
    {
        return args.options.find(key) != args.options.end();
    }

    // requireOptionText fetches a mandatory named option.
    // Inputs: parsed args and key.
    // Processing: throws when the switch is absent.
    // Returns: option value reference.
    const std::wstring& requireOptionText(const NamedArgs& args, const wchar_t* key)
    {
        const std::wstring* value = getOptionText(args, key);
        if (value == nullptr)
        {
            throw std::invalid_argument("missing option");
        }
        return *value;
    }

    // requireOptionU32 parses a mandatory unsigned 32-bit option.
    // Inputs: parsed args and key.
    // Processing: throws when the switch is absent or malformed.
    // Returns: parsed value.
    std::uint32_t requireOptionU32(const NamedArgs& args, const wchar_t* key)
    {
        return parseU32(requireOptionText(args, key).c_str(), "option");
    }

    // requireOptionU64 parses a mandatory unsigned 64-bit option.
    // Inputs: parsed args and key.
    // Processing: throws when the switch is absent or malformed.
    // Returns: parsed value.
    std::uint64_t requireOptionU64(const NamedArgs& args, const wchar_t* key)
    {
        return parseU64(requireOptionText(args, key).c_str(), "option");
    }

    // hasOption reports whether a named switch is present in argv.
    // Inputs: option name includes the leading dashes, for example "--pid".
    // Processing: exact case-sensitive match against the command line.
    // Returns: true when the option exists.
    bool hasOption(int argc, wchar_t* argv[], const wchar_t* option)
    {
        for (int index = 0; index < argc; ++index)
        {
            if (argv[index] != nullptr && std::wcscmp(argv[index], option) == 0)
            {
                return true;
            }
        }
        return false;
    }

    // findOptionValue returns the next token after a named switch.
    // Inputs: argv and option name; if missing, returns nullptr.
    // Processing: scans for exact option match and returns following token.
    // Returns: pointer to the following argument or nullptr.
    const wchar_t* findOptionValue(int argc, wchar_t* argv[], const wchar_t* option)
    {
        for (int index = 0; index + 1 < argc; ++index)
        {
            if (argv[index] != nullptr && std::wcscmp(argv[index], option) == 0)
            {
                return argv[index + 1];
            }
        }
        return nullptr;
    }

    // readFileBytes loads a whole file into memory for blob-style IOCTL inputs.
    // Inputs: path and an upper size bound to reject oversized protocol blobs.
    // Processing: opens the file in binary mode and validates the actual length.
    // Returns: byte vector on success; throws on IO or size violations.
    std::vector<std::uint8_t> readFileBytes(const std::wstring& path, std::size_t maxBytes)
    {
        if (path.empty())
        {
            throw std::invalid_argument("file path");
        }

        HANDLE file = ::CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("open file");
        }

        LARGE_INTEGER size{};
        if (::GetFileSizeEx(file, &size) == FALSE)
        {
            ::CloseHandle(file);
            throw std::runtime_error("file size");
        }
        if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > maxBytes)
        {
            ::CloseHandle(file);
            throw std::out_of_range("file too large");
        }

        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
        DWORD readBytes = 0;
        if (!bytes.empty())
        {
            if (::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &readBytes, nullptr) == FALSE)
            {
                ::CloseHandle(file);
                throw std::runtime_error("file read");
            }
            bytes.resize(readBytes);
        }
        ::CloseHandle(file);
        return bytes;
    }

    // parseHexBytes accepts either 0x-prefixed numbers or raw hex byte strings.
    // Inputs: text token containing bytes like "DE AD BE EF" or "0x1234".
    // Processing: strips separators and decodes two hex digits per byte.
    // Returns: decoded bytes; throws on malformed input.
    std::vector<std::uint8_t> parseHexBytes(const std::wstring& text)
    {
        std::wstring normalized = text;
        if (normalized.size() >= 2U && normalized[0] == L'0' && (normalized[1] == L'x' || normalized[1] == L'X'))
        {
            normalized.erase(0U, 2U);
        }

        std::wstring cleaned;
        cleaned.reserve(text.size());
        for (wchar_t ch : normalized)
        {
            if (std::iswxdigit(ch))
            {
                cleaned.push_back(static_cast<wchar_t>(std::towupper(ch)));
            }
        }

        if (cleaned.empty())
        {
            return {};
        }
        if ((cleaned.size() % 2U) != 0U)
        {
            throw std::invalid_argument("hex bytes");
        }

        std::vector<std::uint8_t> bytes;
        bytes.reserve(cleaned.size() / 2U);
        for (std::size_t index = 0U; index < cleaned.size(); index += 2U)
        {
            const wchar_t hi = cleaned[index];
            const wchar_t lo = cleaned[index + 1U];
            const auto nibble = [](wchar_t c) -> int {
                if (c >= L'0' && c <= L'9') return static_cast<int>(c - L'0');
                if (c >= L'A' && c <= L'F') return static_cast<int>(10 + (c - L'A'));
                return -1;
            };
            const int hiValue = nibble(hi);
            const int loValue = nibble(lo);
            if (hiValue < 0 || loValue < 0)
            {
                throw std::invalid_argument("hex bytes");
            }
            bytes.push_back(static_cast<std::uint8_t>((hiValue << 4) | loValue));
        }
        return bytes;
    }

    // fixedAnsi copies a fixed char array into std::string safely.
    // Inputs: text points at protocol storage; maxBytes is the field capacity.
    // Processing: scans until NUL or capacity, whichever comes first.
    // Returns: a possibly empty string without embedded protocol padding.
    std::string fixedAnsi(const char* text, std::size_t maxBytes)
    {
        if (text == nullptr || maxBytes == 0U)
        {
            return {};
        }

        std::size_t length = 0U;
        while (length < maxBytes && text[length] != '\0')
        {
            ++length;
        }
        return std::string(text, text + length);
    }

    // fixedWide copies a fixed wchar_t array into std::wstring safely.
    // Inputs: text points at protocol storage; maxChars is the field capacity.
    // Processing: scans until NUL or capacity, whichever comes first.
    // Returns: a possibly empty wide string without trailing padding.
    std::wstring fixedWide(const wchar_t* text, std::size_t maxChars)
    {
        if (text == nullptr || maxChars == 0U)
        {
            return {};
        }

        std::size_t length = 0U;
        while (length < maxChars && text[length] != L'\0')
        {
            ++length;
        }
        return std::wstring(text, text + length);
    }

    // fixedUtf16 copies unsigned-short UTF-16 protocol fields into std::wstring.
    // Inputs: text points at UTF-16 code units; maxChars bounds the scan.
    // Processing: casts code units to wchar_t, matching Windows UTF-16 wchar_t.
    // Returns: a std::wstring suitable for console output on Windows.
    std::wstring fixedUtf16(const unsigned short* text, std::size_t maxChars)
    {
        if (text == nullptr || maxChars == 0U)
        {
            return {};
        }

        std::wstring value;
        for (std::size_t index = 0U; index < maxChars && text[index] != 0U; ++index)
        {
            value.push_back(static_cast<wchar_t>(text[index]));
        }
        return value;
    }

    // copyWideToFixed writes a command-line string into a protocol wchar_t field.
    // Inputs: destination/capacity are the protocol field; source is user text.
    // Processing: truncates to leave room for NUL and clears the destination first.
    // Returns: no value; destination is always NUL terminated when capacity > 0.
    void copyWideToFixed(wchar_t* destination, std::size_t capacity, const std::wstring& source)
    {
        if (destination == nullptr || capacity == 0U)
        {
            return;
        }

        std::fill(destination, destination + capacity, L'\0');
        const std::size_t chars = std::min<std::size_t>(source.size(), capacity - 1U);
        if (chars != 0U)
        {
            std::copy(source.data(), source.data() + chars, destination);
        }
    }

    // parseU32 parses a decimal or 0x-prefixed unsigned 32-bit number.
    // Inputs: token is a command-line argument; name is used in error text.
    // Processing: wcstoull is used so both decimal and hex are accepted.
    // Returns: parsed value, or throws invalid_argument/out_of_range.
    std::uint32_t parseU32(const wchar_t* token, const char* name)
    {
        if (token == nullptr || token[0] == L'\0')
        {
            throw std::invalid_argument(name);
        }

        wchar_t* end = nullptr;
        const unsigned long long value = std::wcstoull(token, &end, 0);
        if (end == token || *end != L'\0')
        {
            throw std::invalid_argument(name);
        }
        if (value > std::numeric_limits<std::uint32_t>::max())
        {
            throw std::out_of_range(name);
        }
        return static_cast<std::uint32_t>(value);
    }

    // parseU64 parses a decimal or 0x-prefixed unsigned 64-bit number.
    // Inputs: token is a command-line argument; name is used in error text.
    // Processing: wcstoull is used to preserve native Windows argument encoding.
    // Returns: parsed value, or throws invalid_argument.
    std::uint64_t parseU64(const wchar_t* token, const char* name)
    {
        if (token == nullptr || token[0] == L'\0')
        {
            throw std::invalid_argument(name);
        }

        wchar_t* end = nullptr;
        const unsigned long long value = std::wcstoull(token, &end, 0);
        if (end == token || *end != L'\0')
        {
            throw std::invalid_argument(name);
        }
        return static_cast<std::uint64_t>(value);
    }

    // parsePidListText converts a delimiter-separated PID list into unique PID values.
    // Inputs: text accepts decimal or 0x-prefixed numbers separated by comma, semicolon, pipe, or whitespace.
    // Processing: PID zero is ignored and duplicates are removed while preserving first-seen order.
    // Returns: normalized PID vector; throws when a token is malformed or the protocol maximum is exceeded.
    std::vector<std::uint32_t> parsePidListText(const std::wstring& text)
    {
        std::vector<std::uint32_t> processIds;
        std::wstring token;

        const auto flushToken = [&]()
        {
            if (token.empty())
            {
                return;
            }

            const std::uint32_t processId = parseU32(token.c_str(), "pid list");
            token.clear();
            if (processId == 0U)
            {
                return;
            }
            if (std::find(processIds.begin(), processIds.end(), processId) != processIds.end())
            {
                return;
            }
            if (processIds.size() >= KSWORD_ARK_MINIFILTER_BYPASS_PID_MAX_COUNT)
            {
                throw std::out_of_range("too many pids");
            }
            processIds.push_back(processId);
        };

        for (const wchar_t ch : text)
        {
            if (ch == L',' || ch == L';' || ch == L'|' || std::iswspace(ch) != 0)
            {
                flushToken();
                continue;
            }
            token.push_back(ch);
        }
        flushToken();
        return processIds;
    }

    // hex64 formats a 64-bit integer as a fixed-width hexadecimal value.
    // Inputs: value is the integer to render.
    // Processing: uses an isolated stringstream to avoid mutating global cout state.
    // Returns: wide string in 0xNNNN form.
    std::wstring hex64(std::uint64_t value)
    {
        std::wostringstream stream;
        stream << L"0x" << std::hex << std::uppercase << value;
        return stream.str();
    }

    // hexdump prints raw bytes in a stable offset+ASCII layout.
    // Inputs: source bytes and an optional row width.
    // Processing: emits to stdout only; suitable for debugging variable payloads.
    // Returns: no value.
    void hexdump(const std::uint8_t* data, std::size_t size, std::size_t width = 16U)
    {
        if (data == nullptr || size == 0U)
        {
            std::wcout << L"(empty)\n";
            return;
        }

        for (std::size_t offset = 0U; offset < size; offset += width)
        {
            const std::size_t lineBytes = std::min(width, size - offset);
            std::wcout << std::hex << std::setw(8) << std::setfill(L'0') << offset << L": ";
            for (std::size_t index = 0U; index < width; ++index)
            {
                if (index < lineBytes)
                {
                    std::wcout << std::setw(2) << static_cast<unsigned int>(data[offset + index]) << L' ';
                }
                else
                {
                    std::wcout << L"   ";
                }
            }
            std::wcout << L"|";
            for (std::size_t index = 0U; index < lineBytes; ++index)
            {
                const unsigned char ch = data[offset + index];
                std::wcout << (ch >= 32U && ch < 127U ? static_cast<wchar_t>(ch) : L'.');
            }
            std::wcout << L"|\n" << std::dec << std::setfill(L' ');
        }
    }

    // dumpWideText prints a fixed wide string only when it is non-empty.
    // Inputs: label and the decoded wide string.
    // Processing: emits one line in a consistent key/value format.
    // Returns: no value.
    void dumpWideText(const wchar_t* label, const std::wstring& value)
    {
        if (!value.empty())
        {
            std::wcout << L"  " << label << L"='" << value << L"'\n";
        }
    }

    // reportFixedResponse prints the common fixed-response banner.
    // Inputs: protocol version, status value, last status and bytesReturned.
    // Processing: standardizes the fields required by the plan.
    // Returns: no value.
    void reportFixedResponse(std::uint32_t version, std::uint32_t status, long lastStatus, DWORD bytesReturned)
    {
        std::wcout << L"version=" << version
                   << L" status=" << status
                   << L" lastStatus=0x" << std::hex << static_cast<unsigned long>(lastStatus)
                   << L" bytesReturned=" << std::dec << bytesReturned << L"\n";
    }

    // printResponseBanner keeps the older command implementations readable.
    // Inputs: the common version/status/last-status/byte-count tuple.
    // Processing: delegates to the normalized fixed-response printer.
    // Returns: no value; output goes to stdout.
    void printResponseBanner(std::uint32_t version, std::uint32_t status, long lastStatus, DWORD bytesReturned)
    {
        reportFixedResponse(version, status, lastStatus, bytesReturned);
    }

    // reportFixedStatus prints protocol/status fields for read-only fixed responses.
    // Inputs: protocol version, status and lastStatus fields.
    // Processing: leaves bytesReturned handling to the caller when it differs.
    // Returns: no value.
    void reportFixedStatus(std::uint32_t version, std::uint32_t status, long lastStatus)
    {
        std::wcout << L"version=" << version
                   << L" status=" << status
                   << L" lastStatus=0x" << std::hex << static_cast<unsigned long>(lastStatus)
                   << std::dec << L"\n";
    }

    // responseCountLimit bounds user-visible row printing.
    // Inputs: reported count, available count and CLI limit.
    // Processing: chooses the smallest count to keep parsing within buffer.
    // Returns: bounded count.
    std::size_t responseCountLimit(std::size_t reported, std::size_t available, std::size_t limit)
    {
        return std::min({ reported, available, limit });
    }

    // currentUtc100ns returns the current UTC FILETIME tick count.
    // Inputs: none.
    // Processing: converts GetSystemTimePreciseAsFileTime into a 64-bit scalar.
    // Returns: UTC time in 100ns units.
    std::uint64_t currentUtc100ns()
    {
        FILETIME fileTime{};
        ::GetSystemTimePreciseAsFileTime(&fileTime);
        ULARGE_INTEGER value{};
        value.HighPart = fileTime.dwHighDateTime;
        value.LowPart = fileTime.dwLowDateTime;
        return static_cast<std::uint64_t>(value.QuadPart);
    }

    // formatGuid128 renders the callback GUID packet in the same shape as UI code.
    // Inputs: fixed 16-byte GUID packet.
    // Processing: prints uppercase hexadecimal with 8-4-4-4-12 grouping.
    // Returns: formatted GUID text.
    std::wstring formatGuid128(const KSWORD_ARK_GUID128& guid)
    {
        std::wostringstream stream;
        const unsigned char* bytes = guid.bytes;
        stream << std::hex << std::uppercase << std::setfill(L'0')
               << std::setw(2) << static_cast<unsigned int>(bytes[0])
               << std::setw(2) << static_cast<unsigned int>(bytes[1])
               << std::setw(2) << static_cast<unsigned int>(bytes[2])
               << std::setw(2) << static_cast<unsigned int>(bytes[3]) << L"-"
               << std::setw(2) << static_cast<unsigned int>(bytes[4])
               << std::setw(2) << static_cast<unsigned int>(bytes[5]) << L"-"
               << std::setw(2) << static_cast<unsigned int>(bytes[6])
               << std::setw(2) << static_cast<unsigned int>(bytes[7]) << L"-"
               << std::setw(2) << static_cast<unsigned int>(bytes[8])
               << std::setw(2) << static_cast<unsigned int>(bytes[9]) << L"-"
               << std::setw(2) << static_cast<unsigned int>(bytes[10])
               << std::setw(2) << static_cast<unsigned int>(bytes[11])
               << std::setw(2) << static_cast<unsigned int>(bytes[12])
               << std::setw(2) << static_cast<unsigned int>(bytes[13])
               << std::setw(2) << static_cast<unsigned int>(bytes[14])
               << std::setw(2) << static_cast<unsigned int>(bytes[15]);
        return stream.str();
    }

    // parseGuid128 decodes a 32-hex-digit callback GUID string.
    // Inputs: GUID text with or without dashes.
    // Processing: strips separators and decodes byte pairs in order.
    // Returns: GUID packet or throws on malformed text.
    KSWORD_ARK_GUID128 parseGuid128(const std::wstring& text)
    {
        const std::vector<std::uint8_t> bytes = parseHexBytes(text);
        if (bytes.size() != 16U)
        {
            throw std::invalid_argument("guid");
        }

        KSWORD_ARK_GUID128 guid{};
        std::memcpy(guid.bytes, bytes.data(), 16U);
        return guid;
    }

    // loadBytesFromHexOrFile loads write payload bytes from --hex or --data-file.
    // Inputs: parsed args and the option names to consult.
    // Processing: enforces maxBytes and optional mandatory payload presence.
    // Returns: decoded bytes or throws on invalid/missing combinations.
    std::vector<std::uint8_t> loadBytesFromHexOrFile(
        const NamedArgs& args,
        const wchar_t* hexKey,
        const wchar_t* fileKey,
        std::size_t maxBytes,
        bool required)
    {
        const std::wstring* hexText = getOptionText(args, hexKey);
        const std::wstring* fileText = getOptionText(args, fileKey);
        if (hexText != nullptr && fileText != nullptr)
        {
            throw std::invalid_argument("mutually exclusive payload");
        }
        if (hexText == nullptr && fileText == nullptr)
        {
            if (required)
            {
                throw std::invalid_argument("payload");
            }
            return {};
        }

        std::vector<std::uint8_t> bytes =
            (hexText != nullptr) ? parseHexBytes(*hexText) : readFileBytes(*fileText, maxBytes);
        if (bytes.size() > maxBytes)
        {
            throw std::out_of_range("payload too large");
        }
        return bytes;
    }

    // openDriverOrReport opens the shared control device and emits a uniform error.
    // Inputs: desired access for the pending IOCTL.
    // Processing: caller receives a possibly-invalid handle and may stop on failure.
    // Returns: DriverHandle RAII wrapper.
    DriverHandle openDriverOrReport(DWORD desiredAccess = kDefaultDesiredAccess)
    {
        DriverHandle handle = openDriver(desiredAccess);
        if (!handle.valid())
        {
            printWin32Error(L"CreateFileW(" KSWORD_ARK_LOG_WIN32_PATH L")", ::GetLastError());
        }
        return handle;
    }

    // runNoOutputIoctl issues a control code whose success is mostly transport-level.
    // Inputs: control code and optional request buffer.
    // Processing: prints bytesReturned and Win32 completion information.
    // Returns: CLI exit code.
    int runNoOutputIoctl(
        const wchar_t* label,
        DWORD code,
        void* input,
        DWORD inputBytes,
        DWORD desiredAccess = kDefaultDesiredAccess)
    {
        DriverHandle handle = openDriverOrReport(desiredAccess);
        if (!handle.valid())
        {
            return 2;
        }

        IoctlResult io = sendIoctl(handle, code, input, inputBytes, nullptr, 0U);
        if (!io.ok)
        {
            printWin32Error(label, io.win32Error);
            return 3;
        }

        std::wcout << L"bytesReturned=" << io.bytesReturned
                   << L" win32Error=" << io.win32Error << L"\n";
        return 0;
    }

    // printSimpleProcessEntry renders one process row with the most useful stable fields.
    // Inputs: shared process entry from a variable response.
    // Processing: prints IDs, field flags, object pointers and decoded names.
    // Returns: no value.
    void printSimpleProcessEntry(const KSWORD_ARK_PROCESS_ENTRY& entry)
    {
        std::wcout << L"  pid=" << entry.processId
                   << L" ppid=" << entry.parentProcessId
                   << L" flags=0x" << std::hex << entry.flags
                   << L" fieldFlags=0x" << entry.fieldFlags
                   << L" r0Status=" << std::dec << entry.r0Status
                   << L" image='" << fixedAnsi(entry.imageName, sizeof(entry.imageName)).c_str() << L"'\n";
        const std::wstring imagePath = fixedUtf16(entry.imagePath, KSWORD_ARK_PROCESS_IMAGE_PATH_CHARS);
        dumpWideText(L"imagePath", imagePath);
    }

    // printSimpleThreadEntry renders one thread row with cross-view relevant fields.
    // Inputs: shared thread entry.
    // Processing: prints ID, flags, stack info availability and counter sources.
    // Returns: no value.
    void printSimpleThreadEntry(const KSWORD_ARK_THREAD_ENTRY& entry)
    {
        std::wcout << L"  tid=" << entry.threadId
                   << L" pid=" << entry.processId
                   << L" flags=0x" << std::hex << entry.flags
                   << L" fieldFlags=0x" << entry.fieldFlags
                   << L" r0Status=" << std::dec << entry.r0Status
                   << L" stackBase=" << hex64(entry.stackBase)
                   << L" kernelStack=" << hex64(entry.kernelStack) << L"\n";
    }

    template <typename TResponse>
    bool sendFixedNoInput(
        DWORD code,
        const wchar_t* label,
        TResponse& response,
        IoctlResult& io,
        DWORD desiredAccess = kDefaultDesiredAccess)
    {
        DriverHandle handle = openDriverOrReport(desiredAccess);
        if (!handle.valid())
        {
            io.win32Error = ::GetLastError();
            return false;
        }

        io = sendIoctl(handle, code, nullptr, 0U, &response, static_cast<DWORD>(sizeof(response)));
        if (!io.ok)
        {
            printWin32Error(label, io.win32Error);
            return false;
        }
        if (io.bytesReturned < sizeof(response))
        {
            std::wcerr << L"error: " << label << L" response too small: " << io.bytesReturned << L" bytes\n";
            io.ok = false;
            io.win32Error = ERROR_INSUFFICIENT_BUFFER;
            return false;
        }
        return true;
    }

    template <typename TRequest, typename TResponse>
    bool sendFixedRequestResponse(
        DWORD code,
        const wchar_t* label,
        const TRequest& request,
        TResponse& response,
        IoctlResult& io,
        DWORD desiredAccess = kDefaultDesiredAccess)
    {
        DriverHandle handle = openDriverOrReport(desiredAccess);
        if (!handle.valid())
        {
            io.win32Error = ::GetLastError();
            return false;
        }

        io = sendIoctl(
            handle,
            code,
            const_cast<TRequest*>(&request),
            static_cast<DWORD>(sizeof(request)),
            &response,
            static_cast<DWORD>(sizeof(response)));
        if (!io.ok)
        {
            printWin32Error(label, io.win32Error);
            return false;
        }
        if (io.bytesReturned < sizeof(response))
        {
            std::wcerr << L"error: " << label << L" response too small: " << io.bytesReturned << L" bytes\n";
            io.ok = false;
            io.win32Error = ERROR_INSUFFICIENT_BUFFER;
            return false;
        }
        return true;
    }




    // copyBytesToFixed copies a byte vector into a fixed protocol array.
    // Inputs: destination array, destination capacity, and source bytes.
    // Processing: zero-fills destination and copies at most capacity bytes.
    // Returns: no value; destination remains deterministic for METHOD_BUFFERED.
    void copyBytesToFixed(unsigned char* destination, std::size_t capacity, const std::vector<std::uint8_t>& source)
    {
        if (destination == nullptr || capacity == 0U)
        {
            return;
        }
        std::fill(destination, destination + capacity, 0U);
        const std::size_t copyBytes = std::min<std::size_t>(capacity, source.size());
        if (copyBytes != 0U)
        {
            std::copy(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(copyBytes), destination);
        }
    }

    // checkedDwordSize converts vector sizes into Win32 DWORD byte counts.
    // Inputs: byte count in size_t.
    // Processing: rejects values that DeviceIoControl cannot represent.
    // Returns: DWORD byte count.
    DWORD checkedDwordSize(std::size_t bytes)
    {
        if (bytes > std::numeric_limits<DWORD>::max())
        {
            throw std::out_of_range("buffer too large");
        }
        return static_cast<DWORD>(bytes);
    }

    // boundedPathLength validates a fixed WCHAR path protocol field.
    // Inputs: command line path and target field capacity including terminator.
    // Processing: rejects empty or over-capacity paths before copying.
    // Returns: character length excluding NUL.
    unsigned short boundedPathLength(const std::wstring& pathText, std::size_t capacity)
    {
        if (pathText.empty() || pathText.size() >= capacity)
        {
            throw std::out_of_range("path length");
        }
        return static_cast<unsigned short>(pathText.size());
    }

    // sendRawIoctl opens the driver and performs one synchronous IOCTL call.
    // Inputs: label/code, optional input buffer, output buffer vector and access.
    // Processing: handles CreateFile/DeviceIoControl transport errors uniformly.
    // Returns: CLI-style exit code; io/output receive raw completion data.
    int sendRawIoctl(
        const wchar_t* label,
        DWORD code,
        void* input,
        DWORD inputBytes,
        std::vector<std::uint8_t>& output,
        IoctlResult& io,
        DWORD desiredAccess = kDefaultDesiredAccess)
    {
        DriverHandle handle = openDriverOrReport(desiredAccess);
        if (!handle.valid())
        {
            return 2;
        }
        io = sendIoctl(
            handle,
            code,
            input,
            inputBytes,
            output.empty() ? nullptr : output.data(),
            checkedDwordSize(output.size()));
        if (!io.ok)
        {
            printWin32Error(label, io.win32Error);
            return 3;
        }
        return 0;
    }

    // printUsage prints the CLI command reference.
    // Inputs: none.
    // Processing: groups all registered IOCTLs by command family.
    // Returns: no value; output goes to stdout.
    void printUsage()
    {
        std::wcout
            << L"KswordCLI CLI\n"
            << L"usage: KswordCLI.exe <family> <subcommand> [--named-options]\n\n"
            << L"Families and subcommands:\n"
            << L"  process   terminate | suspend | set-ppl | enum | set-visibility | set-special-flags | dkom | crossview\n"
            << L"  memory    query-va | read-va | write-va | read-phys | write-phys | translate-va | query-pte | scan-kexec | scan-evidence\n"
            << L"  file      delete-path | query-info | monitor-control | monitor-drain | monitor-status\n"
            << L"  kernel    ssdt | shadow-ssdt | scan-inline-hooks | patch-inline-hook | enum-iat-eat-hooks | query-driver-object | query-driver-integrity | force-unload-driver | query-cpu | query-phys-layout\n"
            << L"  callback  set-rules | runtime-state | wait-event | answer-event | cancel-pending | remove | remove-ex | enum | set-minifilter-bypass-pids | query-minifilter-bypass-pids\n"
            << L"  dyn       status | fields | capabilities | apply-profile | apply-profile-ex\n"
            << L"  thread    enum | crossview\n"
            << L"  handle    enum | query-object\n"
            << L"  alpc      query-port\n"
            << L"  section   query-process | query-file-mappings\n"
            << L"  trust     query-image\n"
            << L"  safety    query-policy | set-policy\n"
            << L"  preflight query\n"
            << L"  registry  read-value | enum-key | set-value | delete-value | create-key | delete-key | rename-value | rename-key\n"
            << L"  redirect  set-rules | query-status\n"
            << L"  network   set-rules | query-status\n"
            << L"  keyboard  enum-hotkeys | enum-hooks\n"
            << L"  mutation  prepare | commit | rollback | query-audit\n"
            << L"  capability query-driver-capabilities\n"
            << L"  wsl       query-silo\n"
            << L"  log       [--max-frames N]\n\n"
            << L"Common options: --flags 0xN --limit N --hexdump\n"
            << L"Examples:\n"
            << L"  KswordCLI.exe capability query-driver-capabilities\n"
            << L"  KswordCLI.exe process enum --flags 0x1 --limit 32\n"
            << L"  KswordCLI.exe memory read-va --pid 1234 --address 0x7ff700000000 --bytes 64 --hexdump\n"
            << L"  KswordCLI.exe callback set-rules --blob rules.bin\n"
            << L"  KswordCLI.exe callback set-minifilter-bypass-pids --pids 1234,5678\n"
            << L"  KswordCLI.exe callback query-minifilter-bypass-pids\n";
    }

    // printCountHeader renders the common variable-response metadata.
    // Inputs: protocol count fields and returned byte count.
    // Processing: prints stable names that are easy to compare across commands.
    // Returns: no value.
    void printCountHeader(std::uint32_t version, std::uint32_t total, std::uint32_t returned, std::uint32_t entrySize, DWORD bytesReturned)
    {
        std::wcout << L"version=" << version
                   << L" totalCount=" << total
                   << L" returnedCount=" << returned
                   << L" entrySize=" << entrySize
                   << L" bytesReturned=" << bytesReturned << L"\n";
    }

    // validateVariable validates header + variable entries before parsing rows.
    // Inputs: returned bytes, header size, entry size and minimum row size.
    // Processing: checks short buffers and invalid row sizes.
    // Returns: available row count in the returned buffer.
    std::size_t validateVariable(DWORD bytesReturned, std::size_t headerSize, std::uint32_t entrySize, std::size_t minEntrySize, const wchar_t* label)
    {
        if (bytesReturned < headerSize)
        {
            std::wcerr << L"error: " << label << L" response too small: " << bytesReturned << L" bytes\n";
            throw std::runtime_error("small response");
        }
        if (entrySize < minEntrySize)
        {
            std::wcerr << L"error: " << label << L" invalid entrySize=" << entrySize << L"\n";
            throw std::runtime_error("entry size");
        }
        return (static_cast<std::size_t>(bytesReturned) - headerSize) / static_cast<std::size_t>(entrySize);
    }

    // commandLogFamily reads the non-IOCTL log ReadFile channel.
    // Inputs: argc/argv from wmain.
    // Processing: reads bounded frames until END_OF_LOG or --max-frames.
    // Returns: process exit code.
    int commandLogFamily(int argc, wchar_t* argv[])
    {
        const NamedArgs args = parseNamedArgs(argc, argv, 2);
        const std::uint32_t maxFrames = getOptionU32(args, L"--max-frames", 64U);
        DriverHandle handle = openDriver(GENERIC_READ);
        if (!handle.valid())
        {
            printWin32Error(L"CreateFileW(" KSWORD_ARK_LOG_WIN32_PATH L")", ::GetLastError());
            return 2;
        }
        std::vector<char> buffer(4096U, '\0');
        for (std::uint32_t frame = 0; frame < maxFrames; ++frame)
        {
            DWORD bytesRead = 0;
            if (::ReadFile(handle.native(), buffer.data(), static_cast<DWORD>(buffer.size() - 1U), &bytesRead, nullptr) == FALSE)
            {
                printWin32Error(L"ReadFile(log)", ::GetLastError());
                return 3;
            }
            if (bytesRead == 0U) break;
            std::string text(buffer.data(), buffer.data() + bytesRead);
            const std::size_t marker = text.find(KSWORD_ARK_LOG_END_MARKER);
            if (marker != std::string::npos)
            {
                text.resize(marker);
                std::cout << text;
                break;
            }
            std::cout << text;
        }
        return 0;
    }


    // printProcessEnumRow renders a process enumeration entry.
    // Inputs: protocol row from KSWORD_ARK_PROCESS_ENTRY.
    // Processing: prints stable identifiers and optional image path.
    // Returns: no value.
    void printProcessEnumRow(const KSWORD_ARK_PROCESS_ENTRY& entry)
    {
        std::wcout << L"  pid=" << entry.processId
                   << L" ppid=" << entry.parentProcessId
                   << L" flags=0x" << std::hex << entry.flags
                   << L" fieldFlags=0x" << entry.fieldFlags
                   << std::dec << L" r0Status=" << entry.r0Status
                   << L" session=" << entry.sessionId
                   << L" objectTable=" << hex64(entry.objectTableAddress)
                   << L" section=" << hex64(entry.sectionObjectAddress)
                   << L" image='" << fixedAnsi(entry.imageName, sizeof(entry.imageName)).c_str() << L"'";
        const std::wstring imagePath = fixedUtf16(entry.imagePath, KSWORD_ARK_PROCESS_IMAGE_PATH_CHARS);
        if (!imagePath.empty()) std::wcout << L" path='" << imagePath << L"'";
        std::wcout << L"\n";
    }

    // commandProcessFamily implements all registered process IOCTLs.
    // Inputs: argc/argv from wmain.
    // Processing: builds fixed or variable protocol requests from named args.
    // Returns: process exit code.
    int commandProcessFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: process requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        IoctlResult io{};

        if (sub == L"terminate")
        {
            KSWORD_ARK_TERMINATE_PROCESS_REQUEST request{};
            request.processId = requireOptionU32(args, L"--pid");
            request.exitStatus = static_cast<long>(getOptionU32(args, L"--exit-status", 0xC000013AUL));
            return runNoOutputIoctl(L"IOCTL_KSWORD_ARK_TERMINATE_PROCESS", IOCTL_KSWORD_ARK_TERMINATE_PROCESS, &request, sizeof(request));
        }
        if (sub == L"suspend")
        {
            KSWORD_ARK_SUSPEND_PROCESS_REQUEST request{};
            request.processId = requireOptionU32(args, L"--pid");
            return runNoOutputIoctl(L"IOCTL_KSWORD_ARK_SUSPEND_PROCESS", IOCTL_KSWORD_ARK_SUSPEND_PROCESS, &request, sizeof(request));
        }
        if (sub == L"set-ppl")
        {
            KSWORD_ARK_SET_PPL_LEVEL_REQUEST request{};
            request.processId = requireOptionU32(args, L"--pid");
            request.protectionLevel = static_cast<unsigned char>(requireOptionU32(args, L"--level") & 0xFFU);
            return runNoOutputIoctl(L"IOCTL_KSWORD_ARK_SET_PPL_LEVEL", IOCTL_KSWORD_ARK_SET_PPL_LEVEL, &request, sizeof(request));
        }
        if (sub == L"enum")
        {
            KSWORD_ARK_ENUM_PROCESS_REQUEST request{};
            request.flags = getOptionU32(args, L"--flags", 0U);
            request.startPid = getOptionU32(args, L"--start-pid", 0U);
            request.endPid = getOptionU32(args, L"--end-pid", 0U);
            const std::uint32_t limit = getOptionU32(args, L"--limit", 128U);
            std::vector<std::uint8_t> buffer(kLargeResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_ENUM_PROCESS", IOCTL_KSWORD_ARK_ENUM_PROCESS, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_ENUM_PROCESS_RESPONSE) - sizeof(KSWORD_ARK_PROCESS_ENTRY);
            if (io.bytesReturned < headerSize) { std::wcerr << L"error: process enum response too small\n"; return 4; }
            const auto* response = reinterpret_cast<const KSWORD_ARK_ENUM_PROCESS_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_PROCESS_ENTRY), L"process enum"); }
            catch (...) { return 4; }
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, limit);
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* entry = reinterpret_cast<const KSWORD_ARK_PROCESS_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
                printProcessEnumRow(*entry);
            }
            return 0;
        }
        if (sub == L"set-visibility")
        {
            KSWORD_ARK_SET_PROCESS_VISIBILITY_REQUEST request{};
            KSWORD_ARK_SET_PROCESS_VISIBILITY_RESPONSE response{};
            request.processId = getOptionU32(args, L"--pid", 0U);
            request.action = requireOptionU32(args, L"--action");
            request.flags = getOptionU32(args, L"--flags", 0U);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_SET_PROCESS_VISIBILITY, L"IOCTL_KSWORD_ARK_SET_PROCESS_VISIBILITY", request, response, io)) return 3;
            printResponseBanner(response.version, response.status, response.lastStatus, io.bytesReturned);
            std::wcout << L"pid=" << response.processId
                       << L" action=" << request.action
                       << L" requestFlags=0x" << std::hex << request.flags
                       << std::dec << L" hiddenCount=" << response.hiddenCount << L"\n";
            return 0;
        }
        if (sub == L"set-special-flags")
        {
            KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS_REQUEST request{};
            KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS_RESPONSE response{};
            request.version = KSWORD_ARK_ENUM_PROCESS_PROTOCOL_VERSION;
            request.processId = requireOptionU32(args, L"--pid");
            request.action = requireOptionU32(args, L"--action");
            request.flags = getOptionU32(args, L"--flags", 0U);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS, L"IOCTL_KSWORD_ARK_SET_PROCESS_SPECIAL_FLAGS", request, response, io)) return 3;
            printResponseBanner(response.version, response.status, response.lastStatus, io.bytesReturned);
            std::wcout << L"pid=" << response.processId << L" action=" << response.action
                       << L" appliedFlags=0x" << std::hex << response.appliedFlags
                       << std::dec << L" touchedThreadCount=" << response.touchedThreadCount << L"\n";
            return 0;
        }
        if (sub == L"dkom")
        {
            KSWORD_ARK_DKOM_PROCESS_REQUEST request{};
            KSWORD_ARK_DKOM_PROCESS_RESPONSE response{};
            request.version = KSWORD_ARK_ENUM_PROCESS_PROTOCOL_VERSION;
            request.processId = requireOptionU32(args, L"--pid");
            request.action = getOptionU32(args, L"--action", KSWORD_ARK_PROCESS_DKOM_ACTION_REMOVE_FROM_PSP_CID_TABLE);
            request.flags = getOptionU32(args, L"--flags", 0U);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_DKOM_PROCESS, L"IOCTL_KSWORD_ARK_DKOM_PROCESS", request, response, io)) return 3;
            printResponseBanner(response.version, response.status, response.lastStatus, io.bytesReturned);
            std::wcout << L"pid=" << response.processId << L" action=" << response.action
                       << L" removedEntries=" << response.removedEntries
                       << L" pspCidTable=" << hex64(response.pspCidTableAddress)
                       << L" processObject=" << hex64(response.processObjectAddress) << L"\n";
            return 0;
        }
        if (sub == L"crossview")
        {
            KSWORD_ARK_PROCESS_CROSSVIEW_REQUEST request{};
            request.version = KSWORD_ARK_CROSSVIEW_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_PROCESS_CROSSVIEW_FLAG_INCLUDE_ALL);
            request.startPid = getOptionU32(args, L"--start-pid", 0U);
            request.endPid = getOptionU32(args, L"--end-pid", 0U);
            request.maxNodes = getOptionU32(args, L"--max-nodes", KSWORD_ARK_CROSSVIEW_DEFAULT_MAX_NODES);
            const std::uint32_t limit = getOptionU32(args, L"--limit", 128U);
            std::vector<std::uint8_t> buffer(kHugeResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_QUERY_PROCESS_CROSSVIEW", IOCTL_KSWORD_ARK_QUERY_PROCESS_CROSSVIEW, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_PROCESS_CROSSVIEW_RESPONSE) - sizeof(KSWORD_ARK_PROCESS_CROSSVIEW_ROW);
            if (io.bytesReturned < headerSize) { std::wcerr << L"error: process crossview response too small\n"; return 4; }
            const auto* response = reinterpret_cast<const KSWORD_ARK_PROCESS_CROSSVIEW_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_PROCESS_CROSSVIEW_ROW), L"process crossview"); }
            catch (...) { return 4; }
            printResponseBanner(response->version, response->status, response->lastStatus, io.bytesReturned);
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            std::wcout << L"dynMask=0x" << std::hex << response->dynDataCapabilityMask
                       << L" missingMask=0x" << response->missingCapabilityMask << std::dec << L"\n";
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, limit);
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* row = reinterpret_cast<const KSWORD_ARK_PROCESS_CROSSVIEW_ROW*>(buffer.data() + headerSize + (i * response->entrySize));
                std::wcout << L"  [" << i << L"] pid=" << row->processId
                           << L" ppid=" << row->parentProcessId
                           << L" source=0x" << std::hex << row->sourceMask
                           << L" anomaly=0x" << row->anomalyFlags
                           << L" object=" << hex64(row->objectAddress)
                           << L" start=" << hex64(row->startAddress)
                           << std::dec << L" confidence=" << row->confidence
                           << L" image='" << fixedAnsi(row->imageName, sizeof(row->imageName)).c_str()
                           << L"' detail='" << fixedAnsi(row->detail, sizeof(row->detail)).c_str() << L"'\n";
            }
            return 0;
        }
        std::wcerr << L"error: unknown process subcommand '" << sub << L"'\n";
        return 1;
    }



    // printPageTableInfo renders translate/query-pte shared response data.
    // Inputs: page table info and bytesReturned.
    // Processing: prints every relevant resolved address and raw entry value.
    // Returns: no value.
    void printPageTableInfo(const KSWORD_ARK_PAGE_TABLE_ENTRY_INFO& info, DWORD bytesReturned)
    {
        printResponseBanner(info.version, info.queryStatus, info.walkStatus, bytesReturned);
        std::wcout << L"pid=" << info.processId
                   << L" fields=0x" << std::hex << info.fieldFlags
                   << L" lookup=0x" << static_cast<unsigned long>(info.lookupStatus)
                   << L" va=" << hex64(info.virtualAddress)
                   << L" pa=" << hex64(info.physicalAddress)
                   << L" cr3=" << hex64(info.cr3PhysicalAddress)
                   << std::dec << L" resolved=" << info.resolved
                   << L" pageSize=" << info.pageSize
                   << L" largePageType=" << info.largePageType << L"\n";
        std::wcout << L"indexes pml4=" << info.pml4Index
                   << L" pdpt=" << info.pdptIndex
                   << L" pd=" << info.pdIndex
                   << L" pt=" << info.ptIndex << L"\n";
        std::wcout << L"entries pml4e=" << hex64(info.pml4eValue)
                   << L" pdpte=" << hex64(info.pdpteValue)
                   << L" pde=" << hex64(info.pdeValue)
                   << L" pte=" << hex64(info.pteValue) << L"\n";
    }

    // commandMemoryFamily implements all registered memory IOCTLs.
    // Inputs: argc/argv from wmain.
    // Processing: supports scalar args and explicit --hex/--data-file writes.
    // Returns: process exit code.
    int commandMemoryFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: memory requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        const bool dump = getOptionBool(args, L"--hexdump");
        IoctlResult io{};

        if (sub == L"query-va")
        {
            KSWORD_ARK_QUERY_VIRTUAL_MEMORY_REQUEST request{};
            KSWORD_ARK_QUERY_VIRTUAL_MEMORY_RESPONSE response{};
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_MEMORY_QUERY_FLAG_INCLUDE_ALL);
            request.processId = requireOptionU32(args, L"--pid");
            request.baseAddress = requireOptionU64(args, L"--address");
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_QUERY_VIRTUAL_MEMORY, L"IOCTL_KSWORD_ARK_QUERY_VIRTUAL_MEMORY", request, response, io)) return 3;
            printResponseBanner(response.version, response.queryStatus, response.basicStatus, io.bytesReturned);
            std::wcout << L"pid=" << response.processId << L" fields=0x" << std::hex << response.fieldFlags
                       << L" requested=" << hex64(response.requestedBaseAddress)
                       << L" base=" << hex64(response.baseAddress)
                       << L" allocationBase=" << hex64(response.allocationBase)
                       << std::dec << L" regionSize=" << response.regionSize
                       << L" protect=0x" << std::hex << response.protect
                       << L" state=0x" << response.state << L" type=0x" << response.type << std::dec << L"\n";
            dumpWideText(L"mappedFile", fixedWide(response.mappedFileName, KSWORD_ARK_MEMORY_MAPPED_FILE_NAME_CHARS));
            return 0;
        }
        if (sub == L"read-va")
        {
            KSWORD_ARK_READ_VIRTUAL_MEMORY_REQUEST request{};
            request.flags = getOptionU32(args, L"--flags", 0U);
            request.processId = ((request.flags & KSWORD_ARK_MEMORY_READ_FLAG_KERNEL_ADDRESS) != 0UL)
                ? getOptionU32(args, L"--pid", 0U)
                : requireOptionU32(args, L"--pid");
            request.baseAddress = requireOptionU64(args, L"--address");
            request.bytesToRead = requireOptionU32(args, L"--bytes");
            if (request.bytesToRead > KSWORD_ARK_MEMORY_READ_MAX_BYTES) { std::wcerr << L"error: --bytes exceeds protocol max\n"; return 1; }
            constexpr std::size_t headerSize = offsetof(KSWORD_ARK_READ_VIRTUAL_MEMORY_RESPONSE, data);
            std::vector<std::uint8_t> buffer(headerSize + request.bytesToRead, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_READ_VIRTUAL_MEMORY", IOCTL_KSWORD_ARK_READ_VIRTUAL_MEMORY, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            if (io.bytesReturned < headerSize) { std::wcerr << L"error: read-va response too small\n"; return 4; }
            const auto* response = reinterpret_cast<const KSWORD_ARK_READ_VIRTUAL_MEMORY_RESPONSE*>(buffer.data());
            printResponseBanner(response->version, response->readStatus, response->copyStatus, io.bytesReturned);
            std::wcout << L"pid=" << response->processId << L" fields=0x" << std::hex << response->fieldFlags
                       << L" requestFlags=0x" << request.flags
                       << L" copyStatus=0x" << static_cast<unsigned long>(response->copyStatus)
                       << L" source=" << std::dec << response->source
                       << L" requested=" << response->requestedBytes << L" read=" << response->bytesRead
                       << L" max=" << response->maxBytesPerRequest
                       << L" address=" << hex64(response->requestedBaseAddress) << L"\n";
            const std::size_t dataBytes = std::min<std::size_t>(static_cast<std::size_t>(io.bytesReturned) - headerSize, response->bytesRead);
            if (dump) hexdump(buffer.data() + headerSize, dataBytes);
            return 0;
        }
        if (sub == L"write-va")
        {
            const std::vector<std::uint8_t> bytes = loadBytesFromHexOrFile(args, L"--hex", L"--data-file", KSWORD_ARK_MEMORY_WRITE_MAX_BYTES, true);
            if (bytes.empty()) { std::wcerr << L"error: write-va payload is empty\n"; return 1; }
            constexpr std::size_t headerSize = offsetof(KSWORD_ARK_WRITE_VIRTUAL_MEMORY_REQUEST, data);
            std::vector<std::uint8_t> input(headerSize + bytes.size(), 0U);
            auto* request = reinterpret_cast<KSWORD_ARK_WRITE_VIRTUAL_MEMORY_REQUEST*>(input.data());
            request->flags = getOptionU32(args, L"--flags", KSWORD_ARK_MEMORY_WRITE_FLAG_UI_CONFIRMED);
            request->processId = ((request->flags & KSWORD_ARK_MEMORY_WRITE_FLAG_KERNEL_ADDRESS) != 0UL)
                ? getOptionU32(args, L"--pid", 0U)
                : requireOptionU32(args, L"--pid");
            request->baseAddress = requireOptionU64(args, L"--address");
            request->bytesToWrite = static_cast<unsigned long>(bytes.size());
            std::copy(bytes.begin(), bytes.end(), request->data);
            KSWORD_ARK_WRITE_VIRTUAL_MEMORY_RESPONSE response{};
            DriverHandle handle = openDriverOrReport();
            if (!handle.valid()) return 2;
            io = sendIoctl(handle, IOCTL_KSWORD_ARK_WRITE_VIRTUAL_MEMORY, input.data(), checkedDwordSize(input.size()), &response, sizeof(response));
            if (!io.ok) { printWin32Error(L"IOCTL_KSWORD_ARK_WRITE_VIRTUAL_MEMORY", io.win32Error); return 3; }
            if (io.bytesReturned < sizeof(response)) { std::wcerr << L"error: write-va response too small\n"; return 4; }
            printResponseBanner(response.version, response.writeStatus, response.copyStatus, io.bytesReturned);
            std::wcout << L"pid=" << response.processId << L" fields=0x" << std::hex << response.fieldFlags
                       << L" requestFlags=0x" << request->flags
                       << L" copyStatus=0x" << static_cast<unsigned long>(response.copyStatus)
                       << L" source=" << std::dec << response.source
                       << L" address=" << hex64(response.requestedBaseAddress)
                       << L" requested=" << response.requestedBytes
                       << L" written=" << response.bytesWritten << L" max=" << response.maxBytesPerRequest << L"\n";
            return 0;
        }
        if (sub == L"read-phys")
        {
            KSWORD_ARK_READ_PHYSICAL_MEMORY_REQUEST request{};
            request.physicalAddress = requireOptionU64(args, L"--address");
            request.bytesToRead = requireOptionU32(args, L"--bytes");
            if (request.bytesToRead > KSWORD_ARK_MEMORY_PHYSICAL_READ_MAX_BYTES) { std::wcerr << L"error: --bytes exceeds protocol max\n"; return 1; }
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_READ_PHYSICAL_MEMORY_RESPONSE) - sizeof(((KSWORD_ARK_READ_PHYSICAL_MEMORY_RESPONSE*)nullptr)->data);
            std::vector<std::uint8_t> buffer(headerSize + request.bytesToRead, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_READ_PHYSICAL_MEMORY", IOCTL_KSWORD_ARK_READ_PHYSICAL_MEMORY, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            if (io.bytesReturned < headerSize) { std::wcerr << L"error: read-phys response too small\n"; return 4; }
            const auto* response = reinterpret_cast<const KSWORD_ARK_READ_PHYSICAL_MEMORY_RESPONSE*>(buffer.data());
            printResponseBanner(response->version, response->readStatus, response->copyStatus, io.bytesReturned);
            std::wcout << L"fields=0x" << std::hex << response->fieldFlags
                       << L" address=" << hex64(response->requestedPhysicalAddress)
                       << std::dec << L" requested=" << response->requestedBytes
                       << L" read=" << response->bytesRead << L" max=" << response->maxBytesPerRequest << L"\n";
            const std::size_t dataBytes = std::min<std::size_t>(static_cast<std::size_t>(io.bytesReturned) - headerSize, response->bytesRead);
            if (dump) hexdump(buffer.data() + headerSize, dataBytes);
            return 0;
        }
        if (sub == L"write-phys")
        {
            const std::vector<std::uint8_t> bytes = loadBytesFromHexOrFile(args, L"--hex", L"--data-file", KSWORD_ARK_MEMORY_PHYSICAL_WRITE_MAX_BYTES, true);
            if (bytes.empty()) { std::wcerr << L"error: write-phys payload is empty\n"; return 1; }
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_WRITE_PHYSICAL_MEMORY_REQUEST) - sizeof(((KSWORD_ARK_WRITE_PHYSICAL_MEMORY_REQUEST*)nullptr)->data);
            std::vector<std::uint8_t> input(headerSize + bytes.size(), 0U);
            auto* request = reinterpret_cast<KSWORD_ARK_WRITE_PHYSICAL_MEMORY_REQUEST*>(input.data());
            request->flags = getOptionU32(args, L"--flags", KSWORD_ARK_PHYSICAL_WRITE_FLAG_UI_CONFIRMED);
            request->physicalAddress = requireOptionU64(args, L"--address");
            request->bytesToWrite = static_cast<unsigned long>(bytes.size());
            std::copy(bytes.begin(), bytes.end(), request->data);
            KSWORD_ARK_WRITE_PHYSICAL_MEMORY_RESPONSE response{};
            DriverHandle handle = openDriverOrReport();
            if (!handle.valid()) return 2;
            io = sendIoctl(handle, IOCTL_KSWORD_ARK_WRITE_PHYSICAL_MEMORY, input.data(), checkedDwordSize(input.size()), &response, sizeof(response));
            if (!io.ok) { printWin32Error(L"IOCTL_KSWORD_ARK_WRITE_PHYSICAL_MEMORY", io.win32Error); return 3; }
            if (io.bytesReturned < sizeof(response)) { std::wcerr << L"error: write-phys response too small\n"; return 4; }
            printResponseBanner(response.version, response.writeStatus, response.copyStatus, io.bytesReturned);
            std::wcout << L"fields=0x" << std::hex << response.fieldFlags
                       << L" address=" << hex64(response.requestedPhysicalAddress)
                       << std::dec << L" requested=" << response.requestedBytes
                       << L" written=" << response.bytesWritten << L" max=" << response.maxBytesPerRequest << L"\n";
            return 0;
        }
        if (sub == L"translate-va" || sub == L"query-pte")
        {
            KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_REQUEST request{};
            request.flags = getOptionU32(args, L"--flags", 0U);
            request.processId = requireOptionU32(args, L"--pid");
            request.virtualAddress = requireOptionU64(args, L"--address");
            if (sub == L"translate-va")
            {
                KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_RESPONSE response{};
                if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS, L"IOCTL_KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS", request, response, io)) return 3;
                printPageTableInfo(response.info, io.bytesReturned);
            }
            else
            {
                KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY_RESPONSE response{};
                if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY, L"IOCTL_KSWORD_ARK_QUERY_PAGE_TABLE_ENTRY", request, response, io)) return 3;
                printPageTableInfo(response.info, io.bytesReturned);
            }
            return 0;
        }

        if (sub == L"scan-kexec")
        {
            KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY_REQUEST request{};
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_KERNEL_EXEC_SCAN_FLAG_INCLUDE_ALL);
            request.maxEntries = getOptionU32(args, L"--max-entries", 4096U);
            request.startAddress = getOptionU64(args, L"--start", 0ULL);
            request.endAddress = getOptionU64(args, L"--end", 0ULL);
            const std::uint32_t limit = getOptionU32(args, L"--limit", 128U);
            std::vector<std::uint8_t> buffer(kHugeResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY", IOCTL_KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY_RESPONSE) - sizeof(KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY);
            if (io.bytesReturned < headerSize) { std::wcerr << L"error: scan-kexec response too small\n"; return 4; }
            const auto* response = reinterpret_cast<const KSWORD_ARK_SCAN_KERNEL_EXECUTABLE_MEMORY_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY), L"scan-kexec"); }
            catch (...) { return 4; }
            printResponseBanner(response->version, response->status, response->lastStatus, io.bytesReturned);
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            std::wcout << L"moduleCount=" << response->moduleCount << L"\n";
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, limit);
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* entry = reinterpret_cast<const KSWORD_ARK_KERNEL_EXECUTABLE_MEMORY_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
                std::wcout << L"  [" << i << L"] va=" << hex64(entry->virtualAddress)
                           << L" pages=" << entry->pageCount << L" pageSize=" << entry->pageSize
                           << L" flags=0x" << std::hex << entry->effectiveFlags
                           << L" risk=0x" << entry->riskFlags
                           << L" moduleBase=" << hex64(entry->moduleBase)
                           << std::dec << L" owner=" << entry->ownerKind
                           << L" path='" << fixedWide(entry->modulePath, KSWORD_ARK_KERNEL_EXEC_MODULE_PATH_CHARS) << L"'\n";
            }
            return 0;
        }
        if (sub == L"scan-evidence")
        {
            KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE_REQUEST request{};
            request.flags = getOptionU32(args, L"--flags", 0U);
            request.maxRows = getOptionU32(args, L"--max-rows", KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_MAX_ROWS);
            request.startAddress = getOptionU64(args, L"--start", 0ULL);
            request.endAddress = getOptionU64(args, L"--end", 0ULL);
            request.maxBytes = getOptionU64(args, L"--max-bytes", KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_MAX_BYTES);
            request.maxBigPoolRows = getOptionU32(args, L"--max-bigpool-rows", KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_BIGPOOL_ROWS);
            request.sampleBytes = getOptionU32(args, L"--sample-bytes", KSWORD_ARK_MEMORY_EVIDENCE_DEFAULT_SAMPLE_BYTES);
            const std::uint32_t limit = getOptionU32(args, L"--limit", 128U);
            std::vector<std::uint8_t> buffer(kHugeResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE", IOCTL_KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE_RESPONSE) - sizeof(KSWORD_ARK_KERNEL_MEMORY_EVIDENCE_ROW);
            if (io.bytesReturned < headerSize) { std::wcerr << L"error: scan-evidence response too small\n"; return 4; }
            const auto* response = reinterpret_cast<const KSWORD_ARK_SCAN_KERNEL_MEMORY_EVIDENCE_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->rowSize, sizeof(KSWORD_ARK_KERNEL_MEMORY_EVIDENCE_ROW), L"scan-evidence"); }
            catch (...) { return 4; }
            printResponseBanner(response->version, response->status, response->lastStatus, io.bytesReturned);
            printCountHeader(response->version, response->totalRows, response->returnedRows, response->rowSize, io.bytesReturned);
            std::wcout << L"responseFlags=0x" << std::hex << response->responseFlags
                       << L" sourceFlags=0x" << response->sourceFlags
                       << std::dec << L" bytesScanned=" << response->bytesScanned
                       << L" modules=" << response->moduleCount << L" bigPoolSeen=" << response->bigPoolRowsSeen << L"\n";
            const std::size_t parsed = responseCountLimit(response->returnedRows, available, limit);
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* row = reinterpret_cast<const KSWORD_ARK_KERNEL_MEMORY_EVIDENCE_ROW*>(buffer.data() + headerSize + (i * response->rowSize));
                std::wcout << L"  [" << i << L"] kind=" << row->evidenceKind
                           << L" va=" << hex64(row->virtualAddress)
                           << L" size=" << row->regionSize
                           << L" pageSize=" << row->pageSize
                           << L" perm=0x" << std::hex << row->permissionFlags
                           << L" risk=0x" << row->riskFlags
                           << L" moduleBase=" << hex64(row->moduleBase)
                           << L" ownerAddress=" << hex64(row->ownerAddress)
                           << L" last=0x" << static_cast<unsigned long>(row->lastStatus)
                           << std::dec << L" owner='" << fixedWide(row->ownerName, KSWORD_ARK_MEMORY_EVIDENCE_OWNER_NAME_CHARS)
                           << L"' detail='" << fixedWide(row->detail, KSWORD_ARK_MEMORY_EVIDENCE_DETAIL_CHARS) << L"'\n";
            }
            return 0;
        }
        std::wcerr << L"error: unknown memory subcommand '" << sub << L"'\n";
        return 1;
    }



    // printFileInfoResponse renders the fixed file-info response.
    // Inputs: response struct and bytesReturned.
    // Processing: prints status, timestamps and optional names.
    // Returns: no value.
    void printFileInfoResponse(const KSWORD_ARK_QUERY_FILE_INFO_RESPONSE& response, DWORD bytesReturned)
    {
        printResponseBanner(response.version, response.queryStatus, response.basicStatus, bytesReturned);
        std::wcout << L"fields=0x" << std::hex << response.fieldFlags
                   << L" open=0x" << static_cast<unsigned long>(response.openStatus)
                   << L" object=0x" << static_cast<unsigned long>(response.objectStatus)
                   << L" name=0x" << static_cast<unsigned long>(response.nameStatus)
                   << L" attrs=0x" << response.fileAttributes
                   << L" fileObject=" << hex64(response.fileObjectAddress)
                   << L" sectionPointers=" << hex64(response.sectionObjectPointersAddress)
                   << L" dataSection=" << hex64(response.dataSectionObjectAddress)
                   << L" imageSection=" << hex64(response.imageSectionObjectAddress)
                   << std::dec << L" allocationSize=" << response.allocationSize
                   << L" endOfFile=" << response.endOfFile << L"\n";
        dumpWideText(L"ntPath", fixedWide(response.ntPath, KSWORD_ARK_FILE_INFO_PATH_MAX_CHARS));
        dumpWideText(L"objectName", fixedWide(response.objectName, KSWORD_ARK_FILE_INFO_OBJECT_NAME_MAX_CHARS));
    }

    // commandFileFamily implements file and file-monitor IOCTLs.
    // Inputs: argc/argv from wmain.
    // Processing: handles delete/query and monitor control/status/drain.
    // Returns: process exit code.
    int commandFileFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: file requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        IoctlResult io{};

        if (sub == L"delete-path")
        {
            KSWORD_ARK_DELETE_PATH_REQUEST request{};
            request.flags = getOptionU32(args, L"--flags", 0U);
            const std::wstring& pathText = requireOptionText(args, L"--path");
            request.pathLengthChars = boundedPathLength(pathText, KSWORD_ARK_DELETE_PATH_MAX_CHARS);
            copyWideToFixed(request.path, KSWORD_ARK_DELETE_PATH_MAX_CHARS, pathText);
            return runNoOutputIoctl(L"IOCTL_KSWORD_ARK_DELETE_PATH", IOCTL_KSWORD_ARK_DELETE_PATH, &request, sizeof(request));
        }
        if (sub == L"query-info")
        {
            KSWORD_ARK_QUERY_FILE_INFO_REQUEST request{};
            KSWORD_ARK_QUERY_FILE_INFO_RESPONSE response{};
            request.flags = getOptionU32(args, L"--flags", 0U);
            const std::wstring& pathText = requireOptionText(args, L"--path");
            request.pathLengthChars = boundedPathLength(pathText, KSWORD_ARK_FILE_INFO_PATH_MAX_CHARS);
            copyWideToFixed(request.path, KSWORD_ARK_FILE_INFO_PATH_MAX_CHARS, pathText);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_QUERY_FILE_INFO, L"IOCTL_KSWORD_ARK_QUERY_FILE_INFO", request, response, io)) return 3;
            printFileInfoResponse(response, io.bytesReturned);
            return 0;
        }
        if (sub == L"monitor-control")
        {
            KSWORD_ARK_FILE_MONITOR_CONTROL_REQUEST request{};
            request.action = requireOptionU32(args, L"--action");
            request.operationMask = getOptionU32(args, L"--operation-mask", 0U);
            request.processId = getOptionU32(args, L"--pid", 0U);
            request.flags = getOptionU32(args, L"--flags", 0U);
            return runNoOutputIoctl(L"IOCTL_KSWORD_ARK_FILE_MONITOR_CONTROL", IOCTL_KSWORD_ARK_FILE_MONITOR_CONTROL, &request, sizeof(request));
        }
        if (sub == L"monitor-drain")
        {
            KSWORD_ARK_FILE_MONITOR_DRAIN_REQUEST request{};
            request.maxEvents = getOptionU32(args, L"--max-events", KSWORD_ARK_FILE_MONITOR_RING_CAPACITY);
            request.flags = getOptionU32(args, L"--flags", 0U);
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_FILE_MONITOR_DRAIN_RESPONSE) - sizeof(KSWORD_ARK_FILE_MONITOR_EVENT);
            std::vector<std::uint8_t> buffer(kHugeResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_FILE_MONITOR_DRAIN", IOCTL_KSWORD_ARK_FILE_MONITOR_DRAIN, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            if (io.bytesReturned < headerSize) { std::wcerr << L"error: file-monitor drain response too small\n"; return 4; }
            const auto* response = reinterpret_cast<const KSWORD_ARK_FILE_MONITOR_DRAIN_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_FILE_MONITOR_EVENT), L"file-monitor drain"); }
            catch (...) { return 4; }
            printCountHeader(response->version, response->totalQueuedBeforeDrain, response->returnedCount, response->entrySize, io.bytesReturned);
            std::wcout << L"droppedCount=" << response->droppedCount
                       << L" runtimeFlags=0x" << std::hex << response->runtimeFlags
                       << std::dec << L" ringCapacity=" << response->ringCapacity << L"\n";
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, request.maxEvents);
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* entry = reinterpret_cast<const KSWORD_ARK_FILE_MONITOR_EVENT*>(buffer.data() + headerSize + (i * response->entrySize));
                std::wcout << L"  [" << i << L"] op=" << entry->operationType
                           << L" major=" << entry->majorFunction
                           << L" minor=" << entry->minorFunction
                           << L" pid=" << entry->processId
                           << L" tid=" << entry->threadId
                           << L" flags=0x" << std::hex << entry->fieldFlags
                           << L" result=0x" << static_cast<unsigned long>(entry->resultStatus)
                           << std::dec << L" seq=" << entry->sequence
                           << L" path='" << fixedWide(entry->path, KSWORD_ARK_FILE_MONITOR_PATH_CHARS) << L"'";
                if ((entry->fieldFlags & KSWORD_ARK_FILE_MONITOR_FIELD_FSCTL_PRESENT) != 0UL)
                {
                    const wchar_t* fsctlText = KswordARKFileMonitorFsctlCodeToText(entry->fsControlCode);
                    std::wcout << L" fsctl=0x" << std::hex << entry->fsControlCode
                               << std::dec << L" fsInput=" << entry->fsInputBufferLength
                               << L" fsOutput=" << entry->fsOutputBufferLength
                               << L" oplock=" << (KswordARKFileMonitorFsctlIsOplockRelated(entry->fsControlCode) ? L"true" : L"false");
                    if (fsctlText != nullptr)
                    {
                        std::wcout << L" fsctlName='" << fsctlText << L"'";
                    }
                }
                std::wcout << L"\n";
            }
            return 0;
        }
        if (sub == L"monitor-status")
        {
            KSWORD_ARK_FILE_MONITOR_STATUS_RESPONSE response{};
            if (!sendFixedNoInput(IOCTL_KSWORD_ARK_FILE_MONITOR_QUERY_STATUS, L"IOCTL_KSWORD_ARK_FILE_MONITOR_QUERY_STATUS", response, io)) return 3;
            printResponseBanner(response.version, response.runtimeFlags, response.lastErrorStatus, io.bytesReturned);
            std::wcout << L"size=" << response.size
                       << L" operationMask=0x" << std::hex << response.operationMask
                       << std::dec << L" pidFilter=" << response.processIdFilter
                       << L" ringCapacity=" << response.ringCapacity
                       << L" queued=" << response.queuedCount
                       << L" dropped=" << response.droppedCount
                       << L" sequence=" << response.sequence
                       << L" registerStatus=0x" << std::hex << static_cast<unsigned long>(response.registerStatus)
                       << L" startStatus=0x" << static_cast<unsigned long>(response.startStatus)
                       << std::dec << L"\n";
            return 0;
        }
        std::wcerr << L"error: unknown file subcommand '" << sub << L"'\n";
        return 1;
    }

    // fixedAnsiWide widens a fixed ANSI protocol field for wide console output.
    // Inputs: a char buffer and its protocol capacity.
    // Processing: extracts the NUL-terminated ANSI text and widens byte-for-byte.
    // Returns: a wide string suitable for std::wcout.
    std::wstring fixedAnsiWide(const char* text, std::size_t maxBytes)
    {
        const std::string value = fixedAnsi(text, maxBytes);
        return std::wstring(value.begin(), value.end());
    }

    // requireWideCapacity rejects strings that cannot fit in fixed protocol fields.
    // Inputs: source text, destination capacity, and a human-readable field name.
    // Processing: enforces room for a trailing NUL to avoid silent truncation.
    // Returns: no value; throws when the string is empty or too long.
    void requireWideCapacity(const std::wstring& text, std::size_t capacity, const char* name)
    {
        if (text.empty() || text.size() >= capacity)
        {
            throw std::out_of_range(name);
        }
    }

    // copyRequiredWideOption copies a mandatory named option into a fixed field.
    // Inputs: parsed args, option key, destination field, and field capacity.
    // Processing: validates capacity and writes a NUL-terminated copy.
    // Returns: no value; throws when the option is missing or too long.
    void copyRequiredWideOption(const NamedArgs& args, const wchar_t* key, wchar_t* destination, std::size_t capacity)
    {
        const std::wstring& value = requireOptionText(args, key);
        requireWideCapacity(value, capacity, "wide option");
        copyWideToFixed(destination, capacity, value);
    }

    // copyOptionalWideOption copies an optional named option into a fixed field.
    // Inputs: parsed args, option key, destination field, and field capacity.
    // Processing: leaves the field unchanged when the option is absent.
    // Returns: true when the option was supplied.
    bool copyOptionalWideOption(const NamedArgs& args, const wchar_t* key, wchar_t* destination, std::size_t capacity)
    {
        const std::wstring* value = getOptionText(args, key);
        if (value == nullptr)
        {
            return false;
        }
        requireWideCapacity(*value, capacity, "wide option");
        copyWideToFixed(destination, capacity, *value);
        return true;
    }

    // printBytesInline renders a short byte array summary on one line.
    // Inputs: label, source pointer, byte count, and display cap.
    // Processing: prints hexadecimal bytes and an ellipsis when truncated.
    // Returns: no value.
    void printBytesInline(const wchar_t* label, const unsigned char* data, std::size_t count, std::size_t maxDisplay = 32U)
    {
        std::wcout << label << L"=";
        if (data == nullptr || count == 0U)
        {
            std::wcout << L"(empty)\n";
            return;
        }

        const std::size_t shown = std::min<std::size_t>(count, maxDisplay);
        std::wcout << std::hex << std::setfill(L'0');
        for (std::size_t index = 0U; index < shown; ++index)
        {
            std::wcout << (index == 0U ? L"" : L" ")
                       << std::setw(2) << static_cast<unsigned int>(data[index]);
        }
        std::wcout << std::dec << std::setfill(L' ');
        if (shown < count)
        {
            std::wcout << L" ...";
        }
        std::wcout << L"\n";
    }

    // readRequiredBlobOption reads a mandatory blob-style file option.
    // Inputs: parsed args, option key, and protocol maximum length.
    // Processing: delegates to readFileBytes and rejects empty blobs.
    // Returns: the raw file bytes.
    std::vector<std::uint8_t> readRequiredBlobOption(const NamedArgs& args, const wchar_t* key, std::size_t maxBytes)
    {
        const std::wstring& path = requireOptionText(args, key);
        std::vector<std::uint8_t> bytes = readFileBytes(path, maxBytes);
        if (bytes.empty())
        {
            throw std::invalid_argument("empty blob");
        }
        return bytes;
    }

    // sendBlobFixedResponse sends a raw input blob and receives one fixed response.
    // Inputs: IOCTL label/code, blob bytes, response object, result packet, access.
    // Processing: performs METHOD_BUFFERED DeviceIoControl with size validation.
    // Returns: true on a transport-level success with a full fixed response.
    template <typename TResponse>
    bool sendBlobFixedResponse(
        DWORD code,
        const wchar_t* label,
        std::vector<std::uint8_t>& input,
        TResponse& response,
        IoctlResult& io,
        DWORD desiredAccess = kDefaultDesiredAccess)
    {
        DriverHandle handle = openDriverOrReport(desiredAccess);
        if (!handle.valid())
        {
            io.win32Error = ::GetLastError();
            return false;
        }

        io = sendIoctl(handle, code, input.data(), checkedDwordSize(input.size()), &response, static_cast<DWORD>(sizeof(response)));
        if (!io.ok)
        {
            printWin32Error(label, io.win32Error);
            return false;
        }
        if (io.bytesReturned < sizeof(response))
        {
            std::wcerr << L"error: " << label << L" response too small: " << io.bytesReturned << L" bytes\n";
            io.ok = false;
            io.win32Error = ERROR_INSUFFICIENT_BUFFER;
            return false;
        }
        return true;
    }

    // printModuleIdentity renders a DynData module identity packet.
    // Inputs: label and module packet.
    // Processing: prints stable scalar fields and the fixed module name.
    // Returns: no value.
    void printModuleIdentity(const wchar_t* label, const KSW_DYN_MODULE_IDENTITY_PACKET& module)
    {
        std::wcout << label << L": present=" << module.present
                   << L" class=" << module.classId
                   << L" machine=0x" << std::hex << module.machine
                   << L" timestamp=0x" << module.timeDateStamp
                   << L" imageBase=" << hex64(module.imageBase)
                   << std::dec << L" sizeOfImage=" << module.sizeOfImage
                   << L" name='" << fixedWide(module.moduleName, KSW_DYN_MODULE_NAME_CHARS) << L"'\n";
    }

    // commandKernelFamily implements kernel inspection and mutation IOCTLs.
    // Inputs: argc/argv from wmain.
    // Processing: routes SSDT, hook, DriverObject, CPU, memory-layout and unload commands.
    // Returns: process exit code.
    int commandKernelFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: kernel requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        IoctlResult io{};

        if (sub == L"ssdt" || sub == L"shadow-ssdt")
        {
            KSWORD_ARK_ENUM_SSDT_REQUEST request{};
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_ENUM_SSDT_FLAG_INCLUDE_UNRESOLVED);
            std::vector<std::uint8_t> buffer(kLargeResponseBytes, 0U);
            const DWORD code = (sub == L"ssdt") ? IOCTL_KSWORD_ARK_ENUM_SSDT : IOCTL_KSWORD_ARK_ENUM_SHADOW_SSDT;
            const wchar_t* label = (sub == L"ssdt") ? L"IOCTL_KSWORD_ARK_ENUM_SSDT" : L"IOCTL_KSWORD_ARK_ENUM_SHADOW_SSDT";
            const int rc = sendRawIoctl(label, code, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_ENUM_SSDT_RESPONSE) - sizeof(KSWORD_ARK_SSDT_ENTRY);
            const auto* response = reinterpret_cast<const KSWORD_ARK_ENUM_SSDT_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_SSDT_ENTRY), label); }
            catch (...) { return 4; }
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            std::wcout << L"serviceTableBase=" << hex64(response->serviceTableBase)
                       << L" serviceCountFromTable=" << response->serviceCountFromTable << L"\n";
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, getOptionU32(args, L"--limit", 128U));
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* entry = reinterpret_cast<const KSWORD_ARK_SSDT_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
                std::wcout << L"  [" << i << L"] index=" << entry->serviceIndex
                           << L" flags=0x" << std::hex << entry->flags
                           << L" zw=" << hex64(entry->zwRoutineAddress)
                           << L" service=" << hex64(entry->serviceRoutineAddress)
                           << std::dec << L" name='" << fixedAnsiWide(entry->serviceName, sizeof(entry->serviceName))
                           << L"' module='" << fixedAnsiWide(entry->moduleName, sizeof(entry->moduleName)) << L"'\n";
            }
            return 0;
        }

        if (sub == L"scan-inline-hooks" || sub == L"enum-iat-eat-hooks")
        {
            KSWORD_ARK_SCAN_KERNEL_HOOKS_REQUEST request{};
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_CLEAN | KSWORD_ARK_KERNEL_SCAN_FLAG_INCLUDE_INTERNAL);
            request.maxEntries = getOptionU32(args, L"--max-entries", KSWORD_ARK_KERNEL_HOOK_DEFAULT_MAX_ENTRIES);
            copyOptionalWideOption(args, L"--module", request.moduleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS);
            std::vector<std::uint8_t> buffer(kLargeResponseBytes, 0U);
            if (sub == L"scan-inline-hooks")
            {
                const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_SCAN_INLINE_HOOKS", IOCTL_KSWORD_ARK_SCAN_INLINE_HOOKS, &request, sizeof(request), buffer, io);
                if (rc != 0) return rc;
                constexpr std::size_t headerSize = sizeof(KSWORD_ARK_SCAN_INLINE_HOOKS_RESPONSE) - sizeof(KSWORD_ARK_INLINE_HOOK_ENTRY);
                const auto* response = reinterpret_cast<const KSWORD_ARK_SCAN_INLINE_HOOKS_RESPONSE*>(buffer.data());
                std::size_t available = 0U;
                try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_INLINE_HOOK_ENTRY), L"scan-inline-hooks"); }
                catch (...) { return 4; }
                printResponseBanner(response->version, response->status, response->lastStatus, io.bytesReturned);
                printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
                std::wcout << L"moduleCount=" << response->moduleCount << L"\n";
                const std::size_t parsed = responseCountLimit(response->returnedCount, available, getOptionU32(args, L"--limit", 128U));
                for (std::size_t i = 0; i < parsed; ++i)
                {
                    const auto* entry = reinterpret_cast<const KSWORD_ARK_INLINE_HOOK_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
                    std::wcout << L"  [" << i << L"] status=" << entry->status
                               << L" type=" << entry->hookType
                               << L" flags=0x" << std::hex << entry->flags
                               << L" function=" << hex64(entry->functionAddress)
                               << L" target=" << hex64(entry->targetAddress)
                               << L" moduleBase=" << hex64(entry->moduleBase)
                               << std::dec << L" function='" << fixedAnsiWide(entry->functionName, sizeof(entry->functionName))
                               << L"' module='" << fixedWide(entry->moduleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS)
                               << L"' targetModule='" << fixedWide(entry->targetModuleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS) << L"'\n";
                }
                return 0;
            }

            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_ENUM_IAT_EAT_HOOKS", IOCTL_KSWORD_ARK_ENUM_IAT_EAT_HOOKS, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_ENUM_IAT_EAT_HOOKS_RESPONSE) - sizeof(KSWORD_ARK_IAT_EAT_HOOK_ENTRY);
            const auto* response = reinterpret_cast<const KSWORD_ARK_ENUM_IAT_EAT_HOOKS_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_IAT_EAT_HOOK_ENTRY), L"enum-iat-eat-hooks"); }
            catch (...) { return 4; }
            printResponseBanner(response->version, response->status, response->lastStatus, io.bytesReturned);
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            std::wcout << L"moduleCount=" << response->moduleCount << L"\n";
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, getOptionU32(args, L"--limit", 128U));
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* entry = reinterpret_cast<const KSWORD_ARK_IAT_EAT_HOOK_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
                std::wcout << L"  [" << i << L"] class=" << entry->hookClass
                           << L" status=" << entry->status
                           << L" ordinal=" << entry->ordinal
                           << L" thunk=" << hex64(entry->thunkAddress)
                           << L" current=" << hex64(entry->currentTarget)
                           << L" expected=" << hex64(entry->expectedTarget)
                           << L" function='" << fixedAnsiWide(entry->functionName, sizeof(entry->functionName))
                           << L"' module='" << fixedWide(entry->moduleName, KSWORD_ARK_KERNEL_HOOK_MODULE_CHARS) << L"'\n";
            }
            return 0;
        }

        if (sub == L"patch-inline-hook")
        {
            KSWORD_ARK_PATCH_INLINE_HOOK_REQUEST request{};
            KSWORD_ARK_PATCH_INLINE_HOOK_RESPONSE response{};
            request.flags = getOptionU32(args, L"--flags", 0U);
            request.mode = requireOptionU32(args, L"--mode");
            request.functionAddress = requireOptionU64(args, L"--function");
            const std::vector<std::uint8_t> expected = loadBytesFromHexOrFile(args, L"--expected-hex", L"--expected-file", KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES, true);
            const std::vector<std::uint8_t> restore = loadBytesFromHexOrFile(args, L"--restore-hex", L"--restore-file", KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES, false);
            request.patchBytes = static_cast<unsigned long>(std::max(expected.size(), restore.size()));
            copyBytesToFixed(request.expectedCurrentBytes, KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES, expected);
            copyBytesToFixed(request.restoreBytes, KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES, restore);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_PATCH_INLINE_HOOK, L"IOCTL_KSWORD_ARK_PATCH_INLINE_HOOK", request, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printResponseBanner(response.version, response.status, response.lastStatus, io.bytesReturned);
            std::wcout << L"bytesPatched=" << response.bytesPatched
                       << L" fieldFlags=0x" << std::hex << response.fieldFlags
                       << L" function=" << hex64(response.functionAddress) << std::dec << L"\n";
            printBytesInline(L"beforeBytes", response.beforeBytes, KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES);
            printBytesInline(L"afterBytes", response.afterBytes, KSWORD_ARK_KERNEL_HOOK_PATCH_MAX_BYTES);
            return 0;
        }

        if (sub == L"query-driver-object")
        {
            KSWORD_ARK_QUERY_DRIVER_OBJECT_REQUEST request{};
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_DRIVER_OBJECT_QUERY_FLAG_INCLUDE_ALL);
            request.maxDevices = getOptionU32(args, L"--max-devices", KSWORD_ARK_DRIVER_DEVICE_LIMIT_DEFAULT);
            request.maxAttachedDevices = getOptionU32(args, L"--max-attached", KSWORD_ARK_DRIVER_ATTACHED_LIMIT_DEFAULT);
            copyRequiredWideOption(args, L"--driver", request.driverName, KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS);
            std::vector<std::uint8_t> buffer(kLargeResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_QUERY_DRIVER_OBJECT", IOCTL_KSWORD_ARK_QUERY_DRIVER_OBJECT, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE) - sizeof(KSWORD_ARK_DRIVER_DEVICE_ENTRY);
            const auto* response = reinterpret_cast<const KSWORD_ARK_QUERY_DRIVER_OBJECT_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->deviceEntrySize, sizeof(KSWORD_ARK_DRIVER_DEVICE_ENTRY), L"query-driver-object"); }
            catch (...) { return 4; }
            printResponseBanner(response->version, response->queryStatus, response->lastStatus, io.bytesReturned);
            printCountHeader(response->version, response->totalDeviceCount, response->returnedDeviceCount, response->deviceEntrySize, io.bytesReturned);
            std::wcout << L"fieldFlags=0x" << std::hex << response->fieldFlags
                       << L" driverObject=" << hex64(response->driverObjectAddress)
                       << L" driverStart=" << hex64(response->driverStart)
                       << L" driverSection=" << hex64(response->driverSection)
                       << L" driverUnload=" << hex64(response->driverUnload)
                       << std::dec << L" majorFunctionCount=" << response->majorFunctionCount
                       << L" driverFlags=0x" << std::hex << response->driverFlags
                       << std::dec << L" driverSize=" << response->driverSize << L"\n";
            dumpWideText(L"driverName", fixedWide(response->driverName, KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS));
            dumpWideText(L"serviceKeyName", fixedWide(response->serviceKeyName, KSWORD_ARK_DRIVER_SERVICE_KEY_CHARS));
            dumpWideText(L"imagePath", fixedWide(response->imagePath, KSWORD_ARK_DRIVER_IMAGE_PATH_CHARS));
            const std::size_t majorCount = std::min<std::size_t>(response->majorFunctionCount, KSWORD_ARK_DRIVER_MAJOR_FUNCTION_COUNT);
            for (std::size_t i = 0; i < majorCount; ++i)
            {
                const auto& major = response->majorFunctions[i];
                std::wcout << L"  major[" << i << L"] fn=" << major.majorFunction
                           << L" flags=0x" << std::hex << major.flags
                           << L" dispatch=" << hex64(major.dispatchAddress)
                           << L" moduleBase=" << hex64(major.moduleBase)
                           << std::dec << L" module='" << fixedWide(major.moduleName, KSWORD_ARK_DRIVER_MODULE_NAME_CHARS) << L"'\n";
            }
            const std::size_t parsed = responseCountLimit(response->returnedDeviceCount, available, getOptionU32(args, L"--limit", 64U));
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* device = reinterpret_cast<const KSWORD_ARK_DRIVER_DEVICE_ENTRY*>(buffer.data() + headerSize + (i * response->deviceEntrySize));
                std::wcout << L"  device[" << i << L"] depth=" << device->relationDepth
                           << L" type=0x" << std::hex << device->deviceType
                           << L" flags=0x" << device->flags
                           << L" object=" << hex64(device->deviceObjectAddress)
                           << L" attached=" << hex64(device->attachedDeviceObjectAddress)
                           << L" driver=" << hex64(device->driverObjectAddress)
                           << L" nameStatus=0x" << static_cast<unsigned long>(device->nameStatus)
                           << std::dec << L" name='" << fixedWide(device->deviceName, KSWORD_ARK_DRIVER_DEVICE_NAME_CHARS) << L"'\n";
            }
            return 0;
        }

        if (sub == L"query-driver-integrity")
        {
            KSWORD_ARK_QUERY_DRIVER_INTEGRITY_REQUEST request{};
            request.version = KSWORD_ARK_DRIVER_INTEGRITY_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_DRIVER_INTEGRITY_FLAG_DEFAULT);
            request.maxRows = getOptionU32(args, L"--max-rows", KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_MAX_ROWS);
            request.maxIdtVectorsPerCpu = getOptionU32(args, L"--max-idt-vectors", KSWORD_ARK_DRIVER_INTEGRITY_DEFAULT_IDT_VECTORS);
            request.maxDevices = getOptionU32(args, L"--max-devices", KSWORD_ARK_DRIVER_DEVICE_LIMIT_DEFAULT);
            request.maxAttachedDevices = getOptionU32(args, L"--max-attached", KSWORD_ARK_DRIVER_ATTACHED_LIMIT_DEFAULT);
            request.targetModuleBase = getOptionU64(args, L"--module-base", 0ULL);
            copyOptionalWideOption(args, L"--driver", request.driverName, KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS);
            std::vector<std::uint8_t> buffer(kLargeResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_QUERY_DRIVER_INTEGRITY", IOCTL_KSWORD_ARK_QUERY_DRIVER_INTEGRITY, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_QUERY_DRIVER_INTEGRITY_RESPONSE) - sizeof(KSWORD_ARK_DRIVER_INTEGRITY_EVIDENCE);
            const auto* response = reinterpret_cast<const KSWORD_ARK_QUERY_DRIVER_INTEGRITY_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_DRIVER_INTEGRITY_EVIDENCE), L"query-driver-integrity"); }
            catch (...) { return 4; }
            printResponseBanner(response->version, response->queryStatus, response->lastStatus, io.bytesReturned);
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            std::wcout << L"flags=0x" << std::hex << response->flags
                       << L" sourceMask=0x" << response->sourceMask
                       << std::dec << L" cpuCount=" << response->cpuCount
                       << L" moduleCount=" << response->moduleCount << L"\n";
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, getOptionU32(args, L"--limit", 128U));
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* entry = reinterpret_cast<const KSWORD_ARK_DRIVER_INTEGRITY_EVIDENCE*>(buffer.data() + headerSize + (i * response->entrySize));
                std::wcout << L"  [" << i << L"] class=" << entry->evidenceClass
                           << L" risk=0x" << std::hex << entry->riskFlags
                           << L" source=0x" << entry->sourceMask
                           << L" object=" << hex64(entry->objectAddress)
                           << L" target=" << hex64(entry->targetAddress)
                           << L" ownerBase=" << hex64(entry->ownerModuleBase)
                           << std::dec << L" confidence=" << entry->confidence
                           << L" owner='" << fixedWide(entry->ownerModule, KSWORD_ARK_DRIVER_INTEGRITY_OWNER_CHARS)
                           << L"' detail='" << fixedWide(entry->detail, KSWORD_ARK_DRIVER_INTEGRITY_DETAIL_CHARS) << L"'\n";
            }
            return 0;
        }

        if (sub == L"force-unload-driver")
        {
            KSWORD_ARK_FORCE_UNLOAD_DRIVER_REQUEST request{};
            KSWORD_ARK_FORCE_UNLOAD_DRIVER_RESPONSE response{};
            request.version = KSWORD_ARK_FORCE_UNLOAD_DRIVER_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", 0U);
            request.timeoutMilliseconds = getOptionU32(args, L"--timeout-ms", 5000U);
            request.targetModuleBase = getOptionU64(args, L"--module-base", 0ULL);
            copyRequiredWideOption(args, L"--driver", request.driverName, KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_FORCE_UNLOAD_DRIVER, L"IOCTL_KSWORD_ARK_FORCE_UNLOAD_DRIVER", request, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printResponseBanner(response.version, response.status, response.lastStatus, io.bytesReturned);
            std::wcout << L"flags=0x" << std::hex << response.flags
                       << L" waitStatus=0x" << static_cast<unsigned long>(response.waitStatus)
                       << L" driverObject=" << hex64(response.driverObjectAddress)
                       << L" unload=" << hex64(response.driverUnloadAddress)
                       << std::dec << L" cleanupFlagsApplied=" << response.cleanupFlagsApplied
                       << L" deletedDeviceCount=" << response.deletedDeviceCount
                       << L" callbackCandidates=" << response.callbackCandidates
                       << L" callbacksRemoved=" << response.callbacksRemoved
                       << L" callbackFailures=" << response.callbackFailures
                       << L" callbackLastStatus=0x" << std::hex << static_cast<unsigned long>(response.callbackLastStatus)
                       << std::dec << L" driverName='" << fixedWide(response.driverName, KSWORD_ARK_DRIVER_OBJECT_NAME_CHARS) << L"'\n";
            return 0;
        }

        if (sub == L"query-cpu")
        {
            KSWORD_ARK_QUERY_CPU_HARDWARE_RESPONSE response{};
            if (!sendFixedNoInput(IOCTL_KSWORD_ARK_QUERY_CPU_HARDWARE, L"IOCTL_KSWORD_ARK_QUERY_CPU_HARDWARE", response, io)) return 3;
            printResponseBanner(response.version, response.fieldFlags, response.lastStatus, io.bytesReturned);
            std::wcout << L"logical=" << response.logicalProcessorCount
                       << L" active=" << response.activeProcessorCount
                       << L" packages=" << response.packageCount
                       << L" family=" << response.family
                       << L" model=" << response.model
                       << L" stepping=" << response.stepping
                       << L" featureMask=0x" << std::hex << response.featureMask
                       << L" leaf1Ecx=0x" << response.leaf1Ecx
                       << L" leaf1Edx=0x" << response.leaf1Edx
                       << std::dec << L" vendor='" << fixedAnsiWide(response.vendor, KSWORD_ARK_CPU_HARDWARE_VENDOR_CHARS)
                       << L"' brand='" << fixedAnsiWide(response.brand, KSWORD_ARK_CPU_HARDWARE_BRAND_CHARS) << L"'\n";
            return 0;
        }

        if (sub == L"query-phys-layout")
        {
            KSWORD_ARK_QUERY_PHYSICAL_MEMORY_LAYOUT_RESPONSE response{};
            if (!sendFixedNoInput(IOCTL_KSWORD_ARK_QUERY_PHYSICAL_MEMORY_LAYOUT, L"IOCTL_KSWORD_ARK_QUERY_PHYSICAL_MEMORY_LAYOUT", response, io)) return 3;
            printResponseBanner(response.version, response.fieldFlags, response.lastStatus, io.bytesReturned);
            std::wcout << L"rangeCount=" << response.rangeCount
                       << L" zeroLengthRangeCount=" << response.zeroLengthRangeCount
                       << L" truncated=" << response.truncated
                       << L" totalPhysicalBytes=" << response.totalPhysicalBytes
                       << L" highestPhysicalAddress=" << hex64(response.highestPhysicalAddress)
                       << L" largestRangeBytes=" << response.largestRangeBytes
                       << L" smallestRangeBytes=" << response.smallestRangeBytes
                       << L" firstBaseAddress=" << hex64(response.firstBaseAddress)
                       << L" lastEndAddress=" << hex64(response.lastEndAddress)
                       << L" estimatedAddressSpaceGapBytes=" << response.estimatedAddressSpaceGapBytes << L"\n";
            return 0;
        }

        std::wcerr << L"error: unknown kernel subcommand '" << sub << L"'\n";
        return 1;
    }

    // printCallbackEventPacket renders one callback wait-event response.
    // Inputs: fixed event packet and returned byte count.
    // Processing: prints rule identity, decision metadata, paths and patterns.
    // Returns: no value.
    void printCallbackEventPacket(const KSWORD_ARK_CALLBACK_EVENT_PACKET& packet, DWORD bytesReturned)
    {
        std::wcout << L"size=" << packet.size
                   << L" version=" << packet.version
                   << L" bytesReturned=" << bytesReturned
                   << L" eventGuid=" << formatGuid128(packet.eventGuid) << L"\n";
        std::wcout << L"callbackType=" << packet.callbackType
                   << L" operationType=" << packet.operationType
                   << L" action=" << packet.action
                   << L" matchMode=" << packet.matchMode
                   << L" defaultDecision=" << packet.defaultDecision
                   << L" timeoutMs=" << packet.timeoutMs
                   << L" groupId=" << packet.groupId
                   << L" ruleId=" << packet.ruleId
                   << L" pid=" << packet.originatingPid
                   << L" tid=" << packet.originatingTid
                   << L" sessionId=" << packet.sessionId
                   << L" pathUnavailable=" << packet.pathUnavailable << L"\n";
        std::wcout << L"createdAtUtc100ns=" << packet.createdAtUtc100ns
                   << L" deadlineUtc100ns=" << packet.deadlineUtc100ns << L"\n";
        dumpWideText(L"initiatorPath", fixedWide(packet.initiatorPath, KSWORD_ARK_CALLBACK_EVENT_MAX_INITIATOR_CHARS));
        dumpWideText(L"targetPath", fixedWide(packet.targetPath, KSWORD_ARK_CALLBACK_EVENT_MAX_TARGET_CHARS));
        dumpWideText(L"ruleInitiatorPattern", fixedWide(packet.ruleInitiatorPattern, KSWORD_ARK_CALLBACK_EVENT_MAX_PATTERN_CHARS));
        dumpWideText(L"ruleTargetPattern", fixedWide(packet.ruleTargetPattern, KSWORD_ARK_CALLBACK_EVENT_MAX_PATTERN_CHARS));
        dumpWideText(L"groupName", fixedWide(packet.groupName, KSWORD_ARK_CALLBACK_EVENT_MAX_NAME_CHARS));
        dumpWideText(L"ruleName", fixedWide(packet.ruleName, KSWORD_ARK_CALLBACK_EVENT_MAX_NAME_CHARS));
    }

    // commandCallbackFamily implements callback rule/control IOCTLs.
    // Inputs: argc/argv from wmain.
    // Processing: handles blob rule loading, event wait/answer and external callback removal.
    // Returns: process exit code.
    int commandCallbackFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: callback requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        IoctlResult io{};

        if (sub == L"set-rules")
        {
            std::vector<std::uint8_t> blob = readRequiredBlobOption(args, L"--blob", kMaxCommandBytes);
            return runNoOutputIoctl(L"IOCTL_KSWORD_ARK_SET_CALLBACK_RULES", IOCTL_KSWORD_ARK_SET_CALLBACK_RULES, blob.data(), checkedDwordSize(blob.size()), GENERIC_READ | GENERIC_WRITE);
        }
        if (sub == L"runtime-state")
        {
            KSWORD_ARK_CALLBACK_RUNTIME_STATE response{};
            if (!sendFixedNoInput(IOCTL_KSWORD_ARK_GET_CALLBACK_RUNTIME_STATE, L"IOCTL_KSWORD_ARK_GET_CALLBACK_RUNTIME_STATE", response, io)) return 3;
            printResponseBanner(response.version, response.driverOnline, 0, io.bytesReturned);
            std::wcout << L"size=" << response.size
                       << L" callbacksRegisteredMask=0x" << std::hex << response.callbacksRegisteredMask
                       << std::dec << L" globalEnabled=" << response.globalEnabled
                       << L" rulesApplied=" << response.rulesApplied
                       << L" groupCount=" << response.groupCount
                       << L" ruleCount=" << response.ruleCount
                       << L" pendingDecisionCount=" << response.pendingDecisionCount
                       << L" waitingReceiverCount=" << response.waitingReceiverCount
                       << L" appliedRuleVersion=" << response.appliedRuleVersion
                       << L" appliedAtUtc100ns=" << response.appliedAtUtc100ns << L"\n";
            return 0;
        }
        if (sub == L"wait-event")
        {
            KSWORD_ARK_CALLBACK_WAIT_REQUEST request{};
            request.size = sizeof(request);
            request.version = KSWORD_ARK_CALLBACK_PROTOCOL_VERSION;
            request.waiterTag = getOptionU32(args, L"--waiter-tag", 0U);
            KSWORD_ARK_CALLBACK_EVENT_PACKET response{};
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_WAIT_CALLBACK_EVENT, L"IOCTL_KSWORD_ARK_WAIT_CALLBACK_EVENT", request, response, io)) return 3;
            printCallbackEventPacket(response, io.bytesReturned);
            return 0;
        }
        if (sub == L"answer-event")
        {
            KSWORD_ARK_CALLBACK_ANSWER_REQUEST request{};
            request.size = sizeof(request);
            request.version = KSWORD_ARK_CALLBACK_PROTOCOL_VERSION;
            request.eventGuid = parseGuid128(requireOptionText(args, L"--event-guid"));
            request.decision = requireOptionU32(args, L"--decision");
            request.sourceSessionId = requireOptionU32(args, L"--source-session-id");
            request.answeredAtUtc100ns = getOptionU64(args, L"--answered-at", currentUtc100ns());
            return runNoOutputIoctl(L"IOCTL_KSWORD_ARK_ANSWER_CALLBACK_EVENT", IOCTL_KSWORD_ARK_ANSWER_CALLBACK_EVENT, &request, sizeof(request));
        }
        if (sub == L"cancel-pending")
        {
            return runNoOutputIoctl(L"IOCTL_KSWORD_ARK_CANCEL_ALL_PENDING_DECISIONS", IOCTL_KSWORD_ARK_CANCEL_ALL_PENDING_DECISIONS, nullptr, 0U);
        }
        if (sub == L"remove")
        {
            KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST request{};
            KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE response{};
            request.size = sizeof(request);
            request.version = KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_PROTOCOL_VERSION;
            request.callbackClass = requireOptionU32(args, L"--class");
            request.flags = getOptionU32(args, L"--flags", 0U);
            request.callbackAddress = requireOptionU64(args, L"--callback");
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK, L"IOCTL_KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK", request, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printResponseBanner(response.version, response.callbackClass, response.ntstatus, io.bytesReturned);
            std::wcout << L"size=" << response.size
                       << L" callback=" << hex64(response.callbackAddress)
                       << L" moduleBase=" << hex64(response.moduleBase)
                       << L" moduleSize=" << response.moduleSize
                       << L" mappingFlags=0x" << std::hex << response.mappingFlags << std::dec << L"\n";
            dumpWideText(L"modulePath", fixedWide(response.modulePath, KSWORD_ARK_EXTERNAL_CALLBACK_MODULE_NAME_MAX_CHARS));
            dumpWideText(L"serviceName", fixedWide(response.serviceName, KSWORD_ARK_EXTERNAL_CALLBACK_SERVICE_NAME_MAX_CHARS));
            return 0;
        }
        if (sub == L"remove-ex")
        {
            KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_REQUEST request{};
            KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_RESPONSE response{};
            request.size = sizeof(request);
            request.version = KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_PROTOCOL_VERSION;
            request.callbackClass = requireOptionU32(args, L"--class");
            request.flags = getOptionU32(args, L"--flags", 0U);
            request.callbackAddress = requireOptionU64(args, L"--callback");
            request.registrationAddress = getOptionU64(args, L"--registration", 0ULL);
            request.rawStorageValue = getOptionU64(args, L"--raw-storage", 0ULL);
            request.enumerationGeneration = getOptionU64(args, L"--generation", 0ULL);
            request.identityHash = getOptionU64(args, L"--identity-hash", 0ULL);
            request.source = getOptionU32(args, L"--source", 0U);
            request.operationMask = getOptionU32(args, L"--operation-mask", 0U);
            request.objectTypeMask = getOptionU32(args, L"--object-type-mask", 0U);
            request.trustFlags = getOptionU32(args, L"--trust-flags", 0U);
            request.removeBehavior = getOptionU32(args, L"--remove-behavior", 0U);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX, L"IOCTL_KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX", request, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printResponseBanner(response.version, response.callbackClass, response.ntstatus, io.bytesReturned);
            std::wcout << L"size=" << response.size
                       << L" source=" << response.source
                       << L" callback=" << hex64(response.callbackAddress)
                       << L" registration=" << hex64(response.registrationAddress)
                       << L" rawStorage=" << hex64(response.rawStorageValue)
                       << L" generation=" << response.enumerationGeneration
                       << L" identityHash=" << hex64(response.identityHash)
                       << L" revalidationStatus=0x" << std::hex << static_cast<unsigned long>(response.revalidationStatus)
                       << L" trustFlags=0x" << response.trustFlags
                       << L" removeBehavior=0x" << response.removeBehavior
                       << L" mappingFlags=0x" << response.mappingFlags
                       << L" moduleBase=" << hex64(response.moduleBase)
                       << std::dec << L" moduleSize=" << response.moduleSize << L"\n";
            dumpWideText(L"modulePath", fixedWide(response.modulePath, KSWORD_ARK_EXTERNAL_CALLBACK_MODULE_NAME_MAX_CHARS));
            dumpWideText(L"serviceName", fixedWide(response.serviceName, KSWORD_ARK_EXTERNAL_CALLBACK_SERVICE_NAME_MAX_CHARS));
            dumpWideText(L"message", fixedWide(response.message, KSWORD_ARK_CALLBACK_ENUM_DETAIL_CHARS));
            return 0;
        }
        if (sub == L"set-minifilter-bypass-pids")
        {
            const std::vector<std::uint32_t> processIds = parsePidListText(requireOptionText(args, L"--pids"));
            KSWORD_ARK_MINIFILTER_BYPASS_PID_REQUEST request{};
            request.size = sizeof(request);
            request.version = KSWORD_ARK_CALLBACK_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", 0U);
            request.pidCount = static_cast<unsigned long>(processIds.size());
            for (std::size_t index = 0U; index < processIds.size(); ++index)
            {
                request.processIds[index] = static_cast<unsigned long>(processIds[index]);
            }
            return runNoOutputIoctl(
                L"IOCTL_KSWORD_ARK_SET_MINIFILTER_BYPASS_PIDS",
                IOCTL_KSWORD_ARK_SET_MINIFILTER_BYPASS_PIDS,
                &request,
                sizeof(request));
        }
        if (sub == L"query-minifilter-bypass-pids")
        {
            KSWORD_ARK_MINIFILTER_BYPASS_PID_RESPONSE response{};
            if (!sendFixedNoInput(
                IOCTL_KSWORD_ARK_QUERY_MINIFILTER_BYPASS_PIDS,
                L"IOCTL_KSWORD_ARK_QUERY_MINIFILTER_BYPASS_PIDS",
                response,
                io))
            {
                return 3;
            }
            if (response.size < sizeof(response) ||
                response.version != KSWORD_ARK_CALLBACK_PROTOCOL_VERSION ||
                response.pidCount > KSWORD_ARK_MINIFILTER_BYPASS_PID_MAX_COUNT)
            {
                std::wcerr << L"error: minifilter bypass PID response header invalid\n";
                return 4;
            }
            printResponseBanner(response.version, response.pidCount, 0, io.bytesReturned);
            std::wcout << L"size=" << response.size
                       << L" flags=0x" << std::hex << response.flags
                       << std::dec << L" pidCount=" << response.pidCount << L" pids=";
            for (unsigned long index = 0UL; index < response.pidCount; ++index)
            {
                if (index != 0UL)
                {
                    std::wcout << L",";
                }
                std::wcout << response.processIds[index];
            }
            std::wcout << L"\n";
            return 0;
        }
        if (sub == L"enum")
        {
            KSWORD_ARK_ENUM_CALLBACKS_REQUEST request{};
            request.size = sizeof(request);
            request.version = KSWORD_ARK_CALLBACK_ENUM_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_ENUM_CALLBACK_FLAG_INCLUDE_ALL);
            request.maxEntries = getOptionU32(args, L"--max-entries", 512U);
            std::vector<std::uint8_t> buffer(kLargeResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_ENUM_CALLBACKS", IOCTL_KSWORD_ARK_ENUM_CALLBACKS, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_ENUM_CALLBACKS_RESPONSE) - sizeof(KSWORD_ARK_CALLBACK_ENUM_ENTRY);
            const auto* response = reinterpret_cast<const KSWORD_ARK_ENUM_CALLBACKS_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_CALLBACK_ENUM_ENTRY), L"callback enum"); }
            catch (...) { return 4; }
            printResponseBanner(response->version, response->flags, response->lastStatus, io.bytesReturned);
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, getOptionU32(args, L"--limit", 128U));
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* entry = reinterpret_cast<const KSWORD_ARK_CALLBACK_ENUM_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
                std::wcout << L"  [" << i << L"] class=" << entry->callbackClass
                           << L" source=" << entry->source
                           << L" status=" << entry->status
                           << L" fields=0x" << std::hex << entry->fieldFlags
                           << L" callback=" << hex64(entry->callbackAddress)
                           << L" context=" << hex64(entry->contextAddress)
                           << L" registration=" << hex64(entry->registrationAddress)
                           << L" moduleBase=" << hex64(entry->moduleBase)
                           << std::dec << L" name='" << fixedWide(entry->name, KSWORD_ARK_CALLBACK_ENUM_NAME_CHARS)
                           << L"' altitude='" << fixedWide(entry->altitude, KSWORD_ARK_CALLBACK_ENUM_ALTITUDE_CHARS)
                           << L"' detail='" << fixedWide(entry->detail, KSWORD_ARK_CALLBACK_ENUM_DETAIL_CHARS) << L"'\n";
            }
            return 0;
        }
        std::wcerr << L"error: unknown callback subcommand '" << sub << L"'\n";
        return 1;
    }

    // commandDynFamily implements DynData status, field and profile IOCTLs.
    // Inputs: argc/argv from wmain.
    // Processing: handles fixed status/capability queries and raw profile blobs.
    // Returns: process exit code.
    int commandDynFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: dyn requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        IoctlResult io{};

        if (sub == L"status")
        {
            KSW_QUERY_DYN_STATUS_RESPONSE response{};
            if (!sendFixedNoInput(IOCTL_KSWORD_ARK_QUERY_DYN_STATUS, L"IOCTL_KSWORD_ARK_QUERY_DYN_STATUS", response, io)) return 3;
            printResponseBanner(response.version, response.statusFlags, response.lastStatus, io.bytesReturned);
            std::wcout << L"size=" << response.size
                       << L" systemInformerDataVersion=" << response.systemInformerDataVersion
                       << L" systemInformerDataLength=" << response.systemInformerDataLength
                       << L" matchedProfileClass=" << response.matchedProfileClass
                       << L" matchedProfileOffset=" << response.matchedProfileOffset
                       << L" matchedFieldsId=" << response.matchedFieldsId
                       << L" fieldCount=" << response.fieldCount
                       << L" capabilityMask=0x" << std::hex << response.capabilityMask << std::dec << L"\n";
            printModuleIdentity(L"ntoskrnl", response.ntoskrnl);
            printModuleIdentity(L"lxcore", response.lxcore);
            dumpWideText(L"unavailableReason", fixedWide(response.unavailableReason, KSW_DYN_REASON_CHARS));
            return 0;
        }
        if (sub == L"fields")
        {
            std::vector<std::uint8_t> buffer(kSmallResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_QUERY_DYN_FIELDS", IOCTL_KSWORD_ARK_QUERY_DYN_FIELDS, nullptr, 0U, buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSW_QUERY_DYN_FIELDS_RESPONSE) - sizeof(KSW_DYN_FIELD_ENTRY);
            const auto* response = reinterpret_cast<const KSW_QUERY_DYN_FIELDS_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSW_DYN_FIELD_ENTRY), L"dyn fields"); }
            catch (...) { return 4; }
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, getOptionU32(args, L"--limit", 128U));
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* entry = reinterpret_cast<const KSW_DYN_FIELD_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
                std::wcout << L"  [" << i << L"] id=" << entry->fieldId
                           << L" flags=0x" << std::hex << entry->flags
                           << L" source=" << std::dec << entry->source
                           << L" offset=0x" << std::hex << entry->offset
                           << L" capabilityMask=0x" << entry->capabilityMask
                           << std::dec << L" field='" << fixedAnsiWide(entry->fieldName, KSW_DYN_FIELD_NAME_CHARS)
                           << L"' sourceName='" << fixedAnsiWide(entry->sourceName, KSW_DYN_FIELD_SOURCE_CHARS)
                           << L"' feature='" << fixedAnsiWide(entry->featureName, KSW_DYN_FIELD_FEATURE_CHARS) << L"'\n";
            }
            return 0;
        }
        if (sub == L"capabilities")
        {
            KSW_QUERY_CAPABILITIES_RESPONSE response{};
            if (!sendFixedNoInput(IOCTL_KSWORD_ARK_QUERY_CAPABILITIES, L"IOCTL_KSWORD_ARK_QUERY_CAPABILITIES", response, io)) return 3;
            printResponseBanner(response.version, response.statusFlags, 0, io.bytesReturned);
            std::wcout << L"size=" << response.size
                       << L" capabilityMask=0x" << std::hex << response.capabilityMask
                       << std::dec << L" reserved=" << response.reserved << L"\n";
            return 0;
        }
        if (sub == L"apply-profile")
        {
            const std::size_t maxBytes = KSW_APPLY_DYN_PROFILE_REQUEST_HEADER_SIZE + (static_cast<std::size_t>(KSW_DYN_PROFILE_MAX_FIELDS) * sizeof(KSW_DYN_PROFILE_FIELD_PACKET));
            std::vector<std::uint8_t> blob = readRequiredBlobOption(args, L"--blob", maxBytes);
            KSW_APPLY_DYN_PROFILE_RESPONSE response{};
            if (!sendBlobFixedResponse(IOCTL_KSWORD_ARK_APPLY_DYN_PROFILE, L"IOCTL_KSWORD_ARK_APPLY_DYN_PROFILE", blob, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printResponseBanner(response.version, static_cast<std::uint32_t>(response.status), 0, io.bytesReturned);
            std::wcout << L"size=" << response.size
                       << L" appliedFieldCount=" << response.appliedFieldCount
                       << L" rejectedFieldCount=" << response.rejectedFieldCount
                       << L" unknownFieldCount=" << response.unknownFieldCount
                       << L" statusFlags=0x" << std::hex << response.statusFlags
                       << L" capabilityMask=0x" << response.capabilityMask << std::dec << L"\n";
            dumpWideText(L"message", fixedWide(response.message, KSW_DYN_REASON_CHARS));
            return 0;
        }
        if (sub == L"apply-profile-ex")
        {
            const std::size_t maxBytes = KSW_APPLY_DYN_PROFILE_EX_REQUEST_HEADER_SIZE + (static_cast<std::size_t>(KSW_DYN_PROFILE_EX_MAX_ITEMS) * sizeof(KSW_DYN_PROFILE_EX_ITEM_PACKET));
            std::vector<std::uint8_t> blob = readRequiredBlobOption(args, L"--blob", maxBytes);
            KSW_APPLY_DYN_PROFILE_EX_RESPONSE response{};
            if (!sendBlobFixedResponse(IOCTL_KSWORD_ARK_APPLY_DYN_PROFILE_EX, L"IOCTL_KSWORD_ARK_APPLY_DYN_PROFILE_EX", blob, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printResponseBanner(response.version, static_cast<std::uint32_t>(response.status), 0, io.bytesReturned);
            std::wcout << L"size=" << response.size
                       << L" appliedItemCount=" << response.appliedItemCount
                       << L" rejectedItemCount=" << response.rejectedItemCount
                       << L" unknownItemCount=" << response.unknownItemCount
                       << L" statusFlags=0x" << std::hex << response.statusFlags
                       << L" capabilityMask=0x" << response.capabilityMask << std::dec << L"\n";
            dumpWideText(L"message", fixedWide(response.message, KSW_DYN_REASON_CHARS));
            return 0;
        }
        std::wcerr << L"error: unknown dyn subcommand '" << sub << L"'\n";
        return 1;
    }

    // commandCapabilityFamily implements the unified driver capability query.
    // Inputs: argc/argv from wmain.
    // Processing: parses a variable response containing feature rows.
    // Returns: process exit code.
    int commandCapabilityFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: capability requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        if (sub != L"query-driver-capabilities")
        {
            std::wcerr << L"error: unknown capability subcommand '" << sub << L"'\n";
            return 1;
        }

        IoctlResult io{};
        std::vector<std::uint8_t> buffer(kSmallResponseBytes, 0U);
        const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_QUERY_DRIVER_CAPABILITIES", IOCTL_KSWORD_ARK_QUERY_DRIVER_CAPABILITIES, nullptr, 0U, buffer, io);
        if (rc != 0) return rc;
        constexpr std::size_t headerSize = sizeof(KSWORD_ARK_QUERY_DRIVER_CAPABILITIES_RESPONSE) - sizeof(KSWORD_ARK_FEATURE_CAPABILITY_ENTRY);
        const auto* response = reinterpret_cast<const KSWORD_ARK_QUERY_DRIVER_CAPABILITIES_RESPONSE*>(buffer.data());
        std::size_t available = 0U;
        try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_FEATURE_CAPABILITY_ENTRY), L"driver capabilities"); }
        catch (...) { return 4; }
        printResponseBanner(response->version, response->statusFlags, response->lastErrorStatus, io.bytesReturned);
        printCountHeader(response->version, response->totalFeatureCount, response->returnedFeatureCount, response->entrySize, io.bytesReturned);
        std::wcout << L"size=" << response->size
                   << L" driverProtocolVersion=0x" << std::hex << response->driverProtocolVersion
                   << L" securityPolicyFlags=0x" << response->securityPolicyFlags
                   << L" dynDataStatusFlags=0x" << response->dynDataStatusFlags
                   << L" dynDataCapabilityMask=0x" << response->dynDataCapabilityMask
                   << std::dec << L" lastErrorSource='" << fixedAnsiWide(response->lastErrorSource, KSWORD_ARK_CAPABILITY_ERROR_SOURCE_CHARS)
                   << L"' lastErrorSummary='" << fixedAnsiWide(response->lastErrorSummary, KSWORD_ARK_CAPABILITY_ERROR_SUMMARY_CHARS) << L"'\n";
        const std::size_t parsed = responseCountLimit(response->returnedFeatureCount, available, getOptionU32(args, L"--limit", 128U));
        for (std::size_t i = 0; i < parsed; ++i)
        {
            const auto* entry = reinterpret_cast<const KSWORD_ARK_FEATURE_CAPABILITY_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
            std::wcout << L"  [" << i << L"] featureId=" << entry->featureId
                       << L" state=" << entry->state
                       << L" flags=0x" << std::hex << entry->flags
                       << L" requiredPolicy=0x" << entry->requiredPolicyFlags
                       << L" deniedPolicy=0x" << entry->deniedPolicyFlags
                       << L" requiredDyn=0x" << entry->requiredDynDataMask
                       << L" presentDyn=0x" << entry->presentDynDataMask
                       << std::dec << L" feature='" << fixedAnsiWide(entry->featureName, KSWORD_ARK_CAPABILITY_NAME_CHARS)
                       << L"' stateName='" << fixedAnsiWide(entry->stateName, KSWORD_ARK_CAPABILITY_STATE_CHARS)
                       << L"' reason='" << fixedAnsiWide(entry->reasonText, KSWORD_ARK_CAPABILITY_REASON_CHARS) << L"'\n";
        }
        return 0;
    }

    // commandThreadFamily implements thread enumeration and cross-view IOCTLs.
    // Inputs: argc/argv from wmain.
    // Processing: issues variable response queries and prints bounded rows.
    // Returns: process exit code.
    int commandThreadFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: thread requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        IoctlResult io{};
        if (sub == L"enum")
        {
            KSWORD_ARK_ENUM_THREAD_REQUEST request{};
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_ENUM_THREAD_FLAG_INCLUDE_ALL);
            request.processId = getOptionU32(args, L"--pid", 0U);
            std::vector<std::uint8_t> buffer(kLargeResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_ENUM_THREAD", IOCTL_KSWORD_ARK_ENUM_THREAD, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_ENUM_THREAD_RESPONSE) - sizeof(KSWORD_ARK_THREAD_ENTRY);
            const auto* response = reinterpret_cast<const KSWORD_ARK_ENUM_THREAD_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_THREAD_ENTRY), L"thread enum"); }
            catch (...) { return 4; }
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, getOptionU32(args, L"--limit", 128U));
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* entry = reinterpret_cast<const KSWORD_ARK_THREAD_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
                printSimpleThreadEntry(*entry);
            }
            return 0;
        }
        if (sub == L"crossview")
        {
            KSWORD_ARK_THREAD_CROSSVIEW_REQUEST request{};
            request.version = KSWORD_ARK_CROSSVIEW_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_THREAD_CROSSVIEW_FLAG_INCLUDE_ALL);
            request.processId = getOptionU32(args, L"--pid", 0U);
            request.startTid = getOptionU32(args, L"--start-tid", 0U);
            request.endTid = getOptionU32(args, L"--end-tid", 0U);
            request.maxNodes = getOptionU32(args, L"--max-nodes", KSWORD_ARK_CROSSVIEW_DEFAULT_MAX_NODES);
            std::vector<std::uint8_t> buffer(kHugeResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_QUERY_THREAD_CROSSVIEW", IOCTL_KSWORD_ARK_QUERY_THREAD_CROSSVIEW, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_THREAD_CROSSVIEW_RESPONSE) - sizeof(KSWORD_ARK_THREAD_CROSSVIEW_ROW);
            const auto* response = reinterpret_cast<const KSWORD_ARK_THREAD_CROSSVIEW_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_THREAD_CROSSVIEW_ROW), L"thread crossview"); }
            catch (...) { return 4; }
            printResponseBanner(response->version, response->status, response->lastStatus, io.bytesReturned);
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            std::wcout << L"dynDataCapabilityMask=0x" << std::hex << response->dynDataCapabilityMask
                       << L" missingCapabilityMask=0x" << response->missingCapabilityMask << std::dec << L"\n";
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, getOptionU32(args, L"--limit", 128U));
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* row = reinterpret_cast<const KSWORD_ARK_THREAD_CROSSVIEW_ROW*>(buffer.data() + headerSize + (i * response->entrySize));
                std::wcout << L"  [" << i << L"] pid=" << row->processId
                           << L" tid=" << row->threadId
                           << L" object=" << hex64(row->objectAddress)
                           << L" processObject=" << hex64(row->processObjectAddress)
                           << L" start=" << hex64(row->startAddress)
                           << L" source=0x" << std::hex << row->sourceMask
                           << L" anomaly=0x" << row->anomalyFlags
                           << L" last=0x" << static_cast<unsigned long>(row->lastStatus)
                           << std::dec << L" confidence=" << row->confidence
                           << L" image='" << fixedAnsiWide(row->imageName, sizeof(row->imageName))
                           << L"' detail='" << fixedAnsiWide(row->detail, KSWORD_ARK_CROSSVIEW_DETAIL_CHARS) << L"'\n";
            }
            return 0;
        }
        std::wcerr << L"error: unknown thread subcommand '" << sub << L"'\n";
        return 1;
    }

    // commandHandleFamily implements handle table and object queries.
    // Inputs: argc/argv from wmain.
    // Processing: queries process handles or details for one handle value.
    // Returns: process exit code.
    int commandHandleFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: handle requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        IoctlResult io{};
        if (sub == L"enum")
        {
            KSWORD_ARK_ENUM_PROCESS_HANDLES_REQUEST request{};
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_ENUM_HANDLE_FLAG_INCLUDE_ALL);
            request.processId = requireOptionU32(args, L"--pid");
            std::vector<std::uint8_t> buffer(kLargeResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_ENUM_PROCESS_HANDLES", IOCTL_KSWORD_ARK_ENUM_PROCESS_HANDLES, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE) - sizeof(KSWORD_ARK_HANDLE_ENTRY);
            const auto* response = reinterpret_cast<const KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_HANDLE_ENTRY), L"handle enum"); }
            catch (...) { return 4; }
            printResponseBanner(response->version, response->overallStatus, response->lastStatus, io.bytesReturned);
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            std::wcout << L"processId=" << response->processId << L"\n";
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, getOptionU32(args, L"--limit", 128U));
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* entry = reinterpret_cast<const KSWORD_ARK_HANDLE_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
                std::wcout << L"  [" << i << L"] pid=" << entry->processId
                           << L" handle=0x" << std::hex << entry->handleValue
                           << L" fields=0x" << entry->fieldFlags
                           << L" access=0x" << entry->grantedAccess
                           << L" attrs=0x" << entry->attributes
                           << L" object=" << hex64(entry->objectAddress)
                           << L" dyn=0x" << entry->dynDataCapabilityMask
                           << std::dec << L" typeIndex=" << entry->objectTypeIndex
                           << L" decodeStatus=" << entry->decodeStatus << L"\n";
            }
            return 0;
        }
        if (sub == L"query-object")
        {
            KSWORD_ARK_QUERY_HANDLE_OBJECT_REQUEST request{};
            KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE response{};
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_QUERY_OBJECT_FLAG_INCLUDE_ALL);
            request.processId = requireOptionU32(args, L"--pid");
            request.handleValue = requireOptionU64(args, L"--handle");
            request.requestedAccess = getOptionU32(args, L"--access", 0U);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_QUERY_HANDLE_OBJECT, L"IOCTL_KSWORD_ARK_QUERY_HANDLE_OBJECT", request, response, io)) return 3;
            printResponseBanner(response.version, response.queryStatus, response.objectReferenceStatus, io.bytesReturned);
            std::wcout << L"size=" << response.size
                       << L" pid=" << response.processId
                       << L" fields=0x" << std::hex << response.fieldFlags
                       << L" handle=" << hex64(response.handleValue)
                       << L" object=" << hex64(response.objectAddress)
                       << L" dyn=0x" << response.dynDataCapabilityMask
                       << L" typeStatus=0x" << static_cast<unsigned long>(response.typeStatus)
                       << L" nameStatus=0x" << static_cast<unsigned long>(response.nameStatus)
                       << L" proxyNtStatus=0x" << static_cast<unsigned long>(response.proxyNtStatus)
                       << std::dec << L" typeIndex=" << response.objectTypeIndex
                       << L" proxyStatus=" << response.proxyStatus
                       << L" requestedAccess=" << response.requestedAccess
                       << L" actualGrantedAccess=" << response.actualGrantedAccess
                       << L" proxyHandle=" << hex64(response.proxyHandle) << L"\n";
            dumpWideText(L"typeName", fixedWide(response.typeName, KSWORD_ARK_OBJECT_TYPE_NAME_CHARS));
            dumpWideText(L"objectName", fixedWide(response.objectName, KSWORD_ARK_OBJECT_NAME_CHARS));
            return 0;
        }
        std::wcerr << L"error: unknown handle subcommand '" << sub << L"'\n";
        return 1;
    }

    // printAlpcPortInfo renders one ALPC related-port packet.
    // Inputs: label and protocol port info.
    // Processing: prints relation, status, object pointers and optional name.
    // Returns: no value.
    void printAlpcPortInfo(const wchar_t* label, const KSWORD_ARK_ALPC_PORT_INFO& info)
    {
        std::wcout << L"  " << label
                   << L": relation=" << info.relation
                   << L" fields=0x" << std::hex << info.fieldFlags
                   << L" flags=0x" << info.flags
                   << L" object=" << hex64(info.objectAddress)
                   << L" context=" << hex64(info.portContext)
                   << L" basicStatus=0x" << static_cast<unsigned long>(info.basicStatus)
                   << L" nameStatus=0x" << static_cast<unsigned long>(info.nameStatus)
                   << std::dec << L" ownerPid=" << info.ownerProcessId
                   << L" state=" << info.state
                   << L" sequence=" << info.sequenceNo
                   << L" name='" << fixedWide(info.portName, KSWORD_ARK_ALPC_PORT_NAME_CHARS) << L"'\n";
    }

    // commandAlpcFamily implements ALPC port diagnostics.
    // Inputs: argc/argv from wmain.
    // Processing: queries an ALPC handle in a process.
    // Returns: process exit code.
    int commandAlpcFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: alpc requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        if (sub != L"query-port")
        {
            std::wcerr << L"error: unknown alpc subcommand '" << sub << L"'\n";
            return 1;
        }
        KSWORD_ARK_QUERY_ALPC_PORT_REQUEST request{};
        KSWORD_ARK_QUERY_ALPC_PORT_RESPONSE response{};
        request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_ALPC_QUERY_FLAG_INCLUDE_ALL);
        request.processId = requireOptionU32(args, L"--pid");
        request.handleValue = requireOptionU64(args, L"--handle");
        IoctlResult io{};
        if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_QUERY_ALPC_PORT, L"IOCTL_KSWORD_ARK_QUERY_ALPC_PORT", request, response, io)) return 3;
        printResponseBanner(response.version, response.queryStatus, response.objectReferenceStatus, io.bytesReturned);
        std::wcout << L"size=" << response.size
                   << L" pid=" << response.processId
                   << L" fields=0x" << std::hex << response.fieldFlags
                   << L" handle=" << hex64(response.handleValue)
                   << L" dyn=0x" << response.dynDataCapabilityMask
                   << L" typeStatus=0x" << static_cast<unsigned long>(response.typeStatus)
                   << L" basicStatus=0x" << static_cast<unsigned long>(response.basicStatus)
                   << L" communicationStatus=0x" << static_cast<unsigned long>(response.communicationStatus)
                   << L" nameStatus=0x" << static_cast<unsigned long>(response.nameStatus)
                   << std::dec << L" typeName='" << fixedWide(response.typeName, KSWORD_ARK_ALPC_TYPE_NAME_CHARS) << L"'\n";
        printAlpcPortInfo(L"queryPort", response.queryPort);
        printAlpcPortInfo(L"connectionPort", response.connectionPort);
        printAlpcPortInfo(L"serverPort", response.serverPort);
        printAlpcPortInfo(L"clientPort", response.clientPort);
        return 0;
    }

    // commandSectionFamily implements process and file section mapping queries.
    // Inputs: argc/argv from wmain.
    // Processing: prints variable mapping rows with bounded parsing.
    // Returns: process exit code.
    int commandSectionFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: section requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        IoctlResult io{};
        if (sub == L"query-process")
        {
            KSWORD_ARK_QUERY_PROCESS_SECTION_REQUEST request{};
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_SECTION_QUERY_FLAG_INCLUDE_ALL);
            request.processId = requireOptionU32(args, L"--pid");
            request.maxMappings = getOptionU32(args, L"--max-mappings", KSWORD_ARK_SECTION_MAPPING_LIMIT_DEFAULT);
            std::vector<std::uint8_t> buffer(kLargeResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_QUERY_PROCESS_SECTION", IOCTL_KSWORD_ARK_QUERY_PROCESS_SECTION, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_QUERY_PROCESS_SECTION_RESPONSE) - sizeof(KSWORD_ARK_SECTION_MAPPING_ENTRY);
            const auto* response = reinterpret_cast<const KSWORD_ARK_QUERY_PROCESS_SECTION_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_SECTION_MAPPING_ENTRY), L"section query-process"); }
            catch (...) { return 4; }
            printResponseBanner(response->version, response->queryStatus, response->lastStatus, io.bytesReturned);
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            std::wcout << L"pid=" << response->processId
                       << L" fieldFlags=0x" << std::hex << response->fieldFlags
                       << L" sectionObject=" << hex64(response->sectionObjectAddress)
                       << L" controlArea=" << hex64(response->controlAreaAddress)
                       << L" dyn=0x" << response->dynDataCapabilityMask << std::dec << L"\n";
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, getOptionU32(args, L"--limit", 128U));
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* entry = reinterpret_cast<const KSWORD_ARK_SECTION_MAPPING_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
                std::wcout << L"  [" << i << L"] type=" << entry->viewMapType
                           << L" pid=" << entry->processId
                           << L" start=" << hex64(entry->startVa)
                           << L" end=" << hex64(entry->endVa) << L"\n";
            }
            return 0;
        }
        if (sub == L"query-file-mappings")
        {
            KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_REQUEST request{};
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_FILE_SECTION_QUERY_FLAG_INCLUDE_ALL);
            request.maxMappings = getOptionU32(args, L"--max-mappings", KSWORD_ARK_SECTION_MAPPING_LIMIT_DEFAULT);
            const std::wstring& path = requireOptionText(args, L"--path");
            request.pathLengthChars = boundedPathLength(path, KSWORD_ARK_FILE_SECTION_PATH_MAX_CHARS);
            copyWideToFixed(request.path, KSWORD_ARK_FILE_SECTION_PATH_MAX_CHARS, path);
            std::vector<std::uint8_t> buffer(kLargeResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS", IOCTL_KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_RESPONSE) - sizeof(KSWORD_ARK_FILE_SECTION_MAPPING_ENTRY);
            const auto* response = reinterpret_cast<const KSWORD_ARK_QUERY_FILE_SECTION_MAPPINGS_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_FILE_SECTION_MAPPING_ENTRY), L"section query-file-mappings"); }
            catch (...) { return 4; }
            printResponseBanner(response->version, response->queryStatus, response->lastStatus, io.bytesReturned);
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            std::wcout << L"fieldFlags=0x" << std::hex << response->fieldFlags
                       << L" fileObject=" << hex64(response->fileObjectAddress)
                       << L" sectionPointers=" << hex64(response->sectionObjectPointersAddress)
                       << L" dataControlArea=" << hex64(response->dataControlAreaAddress)
                       << L" imageControlArea=" << hex64(response->imageControlAreaAddress)
                       << L" dyn=0x" << response->dynDataCapabilityMask << std::dec << L"\n";
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, getOptionU32(args, L"--limit", 128U));
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* entry = reinterpret_cast<const KSWORD_ARK_FILE_SECTION_MAPPING_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
                std::wcout << L"  [" << i << L"] sectionKind=" << entry->sectionKind
                           << L" type=" << entry->viewMapType
                           << L" pid=" << entry->processId
                           << L" controlArea=" << hex64(entry->controlAreaAddress)
                           << L" start=" << hex64(entry->startVa)
                           << L" end=" << hex64(entry->endVa) << L"\n";
            }
            return 0;
        }
        std::wcerr << L"error: unknown section subcommand '" << sub << L"'\n";
        return 1;
    }

    // commandWslFamily implements WSL silo diagnostics.
    // Inputs: argc/argv from wmain.
    // Processing: sends pid/tid query and prints the fixed response.
    // Returns: process exit code.
    int commandWslFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: wsl requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        if (sub != L"query-silo")
        {
            std::wcerr << L"error: unknown wsl subcommand '" << sub << L"'\n";
            return 1;
        }
        KSWORD_ARK_QUERY_WSL_SILO_REQUEST request{};
        KSWORD_ARK_QUERY_WSL_SILO_RESPONSE response{};
        request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_WSL_QUERY_FLAG_INCLUDE_ALL);
        request.processId = getOptionU32(args, L"--pid", 0U);
        request.threadId = getOptionU32(args, L"--tid", 0U);
        IoctlResult io{};
        if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_QUERY_WSL_SILO, L"IOCTL_KSWORD_ARK_QUERY_WSL_SILO", request, response, io)) return 3;
        printResponseBanner(response.version, response.queryStatus, response.processLookupStatus, io.bytesReturned);
        std::wcout << L"size=" << response.size
                   << L" fieldFlags=0x" << std::hex << response.fieldFlags
                   << L" dyn=0x" << response.dynDataCapabilityMask
                   << L" processLookup=0x" << static_cast<unsigned long>(response.processLookupStatus)
                   << L" threadLookup=0x" << static_cast<unsigned long>(response.threadLookupStatus)
                   << std::dec << L" pid=" << response.processId
                   << L" tid=" << response.threadId
                   << L" processSubsystemType=" << response.processSubsystemType
                   << L" threadSubsystemType=" << response.threadSubsystemType
                   << L" linuxPid=" << response.linuxProcessId
                   << L" linuxTid=" << response.linuxThreadId
                   << L" siloRoutinesMask=0x" << std::hex << response.siloRoutinesMask << std::dec << L"\n";
        printModuleIdentity(L"lxcore", response.lxcore);
        return 0;
    }

    // commandTrustFamily implements image trust diagnostics.
    // Inputs: argc/argv from wmain.
    // Processing: sends a path-based trust query and prints CI/signing fields.
    // Returns: process exit code.
    int commandTrustFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: trust requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        if (sub != L"query-image")
        {
            std::wcerr << L"error: unknown trust subcommand '" << sub << L"'\n";
            return 1;
        }
        KSWORD_ARK_QUERY_IMAGE_TRUST_REQUEST request{};
        KSWORD_ARK_QUERY_IMAGE_TRUST_RESPONSE response{};
        request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_TRUST_QUERY_FLAG_INCLUDE_ALL);
        const std::wstring& path = requireOptionText(args, L"--path");
        request.pathLengthChars = boundedPathLength(path, KSWORD_ARK_TRUST_PATH_MAX_CHARS);
        copyWideToFixed(request.path, KSWORD_ARK_TRUST_PATH_MAX_CHARS, path);
        IoctlResult io{};
        if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_QUERY_IMAGE_TRUST, L"IOCTL_KSWORD_ARK_QUERY_IMAGE_TRUST", request, response, io)) return 3;
        printResponseBanner(response.version, response.queryStatus, response.openStatus, io.bytesReturned);
        std::wcout << L"size=" << response.size
                   << L" fieldFlags=0x" << std::hex << response.fieldFlags
                   << L" ciOptions=0x" << response.codeIntegrityOptions
                   << L" fileObject=" << hex64(response.fileObjectAddress)
                   << L" ciStatus=0x" << static_cast<unsigned long>(response.codeIntegrityStatus)
                   << L" secureBootStatus=0x" << static_cast<unsigned long>(response.secureBootStatus)
                   << L" objectStatus=0x" << static_cast<unsigned long>(response.objectStatus)
                   << L" signingStatus=0x" << static_cast<unsigned long>(response.signingLevelStatus)
                   << std::dec << L" trustSource=" << response.trustSource
                   << L" signingLevel=" << response.signingLevel
                   << L" signingLevelFlags=" << response.signingLevelFlags
                   << L" secureBootEnabled=" << response.secureBootEnabled
                   << L" secureBootCapable=" << response.secureBootCapable
                   << L" thumbprintAlgorithm=" << response.thumbprintAlgorithm
                   << L" thumbprintSize=" << response.thumbprintSize << L"\n";
        printBytesInline(L"thumbprint", response.thumbprint, std::min<std::size_t>(response.thumbprintSize, KSWORD_ARK_TRUST_THUMBPRINT_MAX_BYTES), KSWORD_ARK_TRUST_THUMBPRINT_MAX_BYTES);
        dumpWideText(L"ntPath", fixedWide(response.ntPath, KSWORD_ARK_TRUST_PATH_MAX_CHARS));
        return 0;
    }

    // commandSafetyFamily implements safety policy query/update IOCTLs.
    // Inputs: argc/argv from wmain.
    // Processing: sends fixed request/response policy packets.
    // Returns: process exit code.
    int commandSafetyFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: safety requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        IoctlResult io{};
        if (sub == L"query-policy")
        {
            KSWORD_ARK_QUERY_SAFETY_POLICY_REQUEST request{};
            KSWORD_ARK_QUERY_SAFETY_POLICY_RESPONSE response{};
            request.size = sizeof(request);
            request.version = KSWORD_ARK_SAFETY_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", 0U);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_QUERY_SAFETY_POLICY, L"IOCTL_KSWORD_ARK_QUERY_SAFETY_POLICY", request, response, io)) return 3;
            printResponseBanner(response.version, response.policyFlags, response.lastStatus, io.bytesReturned);
            std::wcout << L"size=" << response.size
                       << L" defaultPolicyFlags=0x" << std::hex << response.defaultPolicyFlags
                       << std::dec << L" generation=" << response.policyGeneration
                       << L" lastOperation=" << response.lastOperation
                       << L" lastDecision=" << response.lastDecision
                       << L" lastReason=" << response.lastReason
                       << L" lastRiskLevel=" << response.lastRiskLevel
                       << L" lastTargetPid=" << response.lastTargetProcessId
                       << L" allowed=" << response.allowedCount
                       << L" denied=" << response.deniedCount
                       << L" auditOnly=" << response.auditOnlyCount
                       << L" lastTargetText='" << fixedWide(response.lastTargetText, KSWORD_ARK_SAFETY_TEXT_MAX_CHARS) << L"'\n";
            return 0;
        }
        if (sub == L"set-policy")
        {
            KSWORD_ARK_SET_SAFETY_POLICY_REQUEST request{};
            KSWORD_ARK_SET_SAFETY_POLICY_RESPONSE response{};
            request.size = sizeof(request);
            request.version = KSWORD_ARK_SAFETY_PROTOCOL_VERSION;
            request.setFlags = getOptionU32(args, L"--set-flags", 0U);
            request.clearFlags = getOptionU32(args, L"--clear-flags", 0U);
            request.expectedGeneration = getOptionU32(args, L"--expected-generation", 0U);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_SET_SAFETY_POLICY, L"IOCTL_KSWORD_ARK_SET_SAFETY_POLICY", request, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printResponseBanner(response.version, static_cast<std::uint32_t>(response.status), response.status, io.bytesReturned);
            std::wcout << L"size=" << response.size
                       << L" oldPolicyFlags=0x" << std::hex << response.oldPolicyFlags
                       << L" newPolicyFlags=0x" << response.newPolicyFlags
                       << std::dec << L" oldGeneration=" << response.oldGeneration
                       << L" newGeneration=" << response.newGeneration << L"\n";
            return 0;
        }
        std::wcerr << L"error: unknown safety subcommand '" << sub << L"'\n";
        return 1;
    }

    // commandPreflightFamily implements the release-readiness preflight query.
    // Inputs: argc/argv from wmain.
    // Processing: sends a bounded variable response and prints check rows.
    // Returns: process exit code.
    int commandPreflightFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: preflight requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        if (sub != L"query")
        {
            std::wcerr << L"error: unknown preflight subcommand '" << sub << L"'\n";
            return 1;
        }
        KSWORD_ARK_QUERY_PREFLIGHT_REQUEST request{};
        request.size = sizeof(request);
        request.version = KSWORD_ARK_PREFLIGHT_PROTOCOL_VERSION;
        request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_PREFLIGHT_QUERY_FLAG_INCLUDE_ALL);
        std::vector<std::uint8_t> buffer(kSmallResponseBytes, 0U);
        IoctlResult io{};
        const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_QUERY_PREFLIGHT", IOCTL_KSWORD_ARK_QUERY_PREFLIGHT, &request, sizeof(request), buffer, io);
        if (rc != 0) return rc;
        constexpr std::size_t headerSize = sizeof(KSWORD_ARK_QUERY_PREFLIGHT_RESPONSE) - sizeof(KSWORD_ARK_PREFLIGHT_CHECK_ENTRY);
        const auto* response = reinterpret_cast<const KSWORD_ARK_QUERY_PREFLIGHT_RESPONSE*>(buffer.data());
        std::size_t available = 0U;
        try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_PREFLIGHT_CHECK_ENTRY), L"preflight"); }
        catch (...) { return 4; }
        printResponseBanner(response->version, response->overallStatus, response->dynDataLastStatus, io.bytesReturned);
        printCountHeader(response->version, response->totalCheckCount, response->returnedCheckCount, response->entrySize, io.bytesReturned);
        std::wcout << L"size=" << response->size
                   << L" fieldFlags=0x" << std::hex << response->fieldFlags
                   << L" dynDataStatusFlags=0x" << response->dynDataStatusFlags
                   << L" dynDataCapabilityMask=0x" << response->dynDataCapabilityMask
                   << L" safetyPolicyFlags=0x" << response->safetyPolicyFlags
                   << L" fileMonitorRuntimeFlags=0x" << response->fileMonitorRuntimeFlags
                   << L" trustFieldFlags=0x" << response->trustFieldFlags
                   << L" codeIntegrityOptions=0x" << response->codeIntegrityOptions
                   << std::dec << L" buildConfiguration=" << response->buildConfiguration
                   << L" targetArchitecture=" << response->targetArchitecture
                   << L" ioctlRegistryCount=" << response->ioctlRegistryCount
                   << L" ioctlDuplicateCount=" << response->ioctlDuplicateCount
                   << L" safetyGeneration=" << response->safetyPolicyGeneration
                   << L" fileMonitorQueued=" << response->fileMonitorQueuedCount
                   << L" fileMonitorDropped=" << response->fileMonitorDroppedCount
                   << L" secureBootEnabled=" << response->secureBootEnabled
                   << L" secureBootCapable=" << response->secureBootCapable << L"\n";
        const std::size_t parsed = responseCountLimit(response->returnedCheckCount, available, getOptionU32(args, L"--limit", 64U));
        for (std::size_t i = 0; i < parsed; ++i)
        {
            const auto* check = reinterpret_cast<const KSWORD_ARK_PREFLIGHT_CHECK_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
            std::wcout << L"  [" << i << L"] id=" << check->checkId
                       << L" status=" << check->status
                       << L" ntstatus=0x" << std::hex << static_cast<unsigned long>(check->ntstatus)
                       << std::dec << L" name='" << fixedAnsiWide(check->checkName, KSWORD_ARK_PREFLIGHT_CHECK_NAME_CHARS)
                       << L"' detail='" << fixedAnsiWide(check->detail, KSWORD_ARK_PREFLIGHT_CHECK_DETAIL_CHARS) << L"'\n";
        }
        return 0;
    }

    // printRegistryOperationResponse renders common registry mutation response fields.
    // Inputs: response and bytesReturned.
    // Processing: prints status and NTSTATUS.
    // Returns: no value.
    void printRegistryOperationResponse(const KSWORD_ARK_REGISTRY_OPERATION_RESPONSE& response, DWORD bytesReturned)
    {
        printResponseBanner(response.version, response.status, response.lastStatus, bytesReturned);
    }

    // commandRegistryFamily implements all registry read/enumeration/mutation IOCTLs.
    // Inputs: argc/argv from wmain.
    // Processing: copies fixed key/value fields and optional binary data file.
    // Returns: process exit code.
    int commandRegistryFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: registry requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        IoctlResult io{};

        if (sub == L"read-value")
        {
            KSWORD_ARK_READ_REGISTRY_VALUE_REQUEST request{};
            KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE response{};
            request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", 0U);
            request.maxDataBytes = getOptionU32(args, L"--max-data-bytes", KSWORD_ARK_REGISTRY_DATA_MAX_BYTES);
            copyRequiredWideOption(args, L"--key", request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS);
            if (copyOptionalWideOption(args, L"--value", request.valueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS))
            {
                request.flags |= KSWORD_ARK_REGISTRY_READ_FLAG_VALUE_NAME_PRESENT;
            }
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_READ_REGISTRY_VALUE, L"IOCTL_KSWORD_ARK_READ_REGISTRY_VALUE", request, response, io)) return 3;
            printResponseBanner(response.version, response.status, response.lastStatus, io.bytesReturned);
            std::wcout << L"valueType=" << response.valueType
                       << L" dataBytes=" << response.dataBytes
                       << L" requiredBytes=" << response.requiredBytes << L"\n";
            const std::size_t dataBytes = std::min<std::size_t>(response.dataBytes, KSWORD_ARK_REGISTRY_DATA_MAX_BYTES);
            if (getOptionBool(args, L"--hexdump")) hexdump(response.data, dataBytes);
            else printBytesInline(L"data", response.data, dataBytes);
            return 0;
        }
        if (sub == L"enum-key")
        {
            KSWORD_ARK_ENUM_REGISTRY_KEY_REQUEST request{};
            KSWORD_ARK_ENUM_REGISTRY_KEY_RESPONSE response{};
            request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_REGISTRY_ENUM_FLAG_INCLUDE_SUBKEYS | KSWORD_ARK_REGISTRY_ENUM_FLAG_INCLUDE_VALUES);
            request.maxSubKeys = getOptionU32(args, L"--max-subkeys", KSWORD_ARK_REGISTRY_ENUM_MAX_SUBKEYS);
            request.maxValues = getOptionU32(args, L"--max-values", KSWORD_ARK_REGISTRY_ENUM_MAX_VALUES);
            request.maxValueDataBytes = getOptionU32(args, L"--max-value-data-bytes", KSWORD_ARK_REGISTRY_ENUM_VALUE_DATA_MAX_BYTES);
            copyRequiredWideOption(args, L"--key", request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_ENUM_REGISTRY_KEY, L"IOCTL_KSWORD_ARK_ENUM_REGISTRY_KEY", request, response, io)) return 3;
            printResponseBanner(response.version, response.status, response.lastStatus, io.bytesReturned);
            std::wcout << L"subKeyCount=" << response.subKeyCount
                       << L" returnedSubKeyCount=" << response.returnedSubKeyCount
                       << L" valueCount=" << response.valueCount
                       << L" returnedValueCount=" << response.returnedValueCount << L"\n";
            const std::size_t subKeyCount = std::min<std::size_t>({ response.returnedSubKeyCount, KSWORD_ARK_REGISTRY_ENUM_MAX_SUBKEYS, getOptionU32(args, L"--limit", 128U) });
            for (std::size_t i = 0; i < subKeyCount; ++i)
            {
                std::wcout << L"  subkey[" << i << L"] '" << fixedWide(response.subKeys[i].name, KSWORD_ARK_REGISTRY_ENUM_KEY_NAME_CHARS) << L"'\n";
            }
            const std::size_t valueCount = std::min<std::size_t>({ response.returnedValueCount, KSWORD_ARK_REGISTRY_ENUM_MAX_VALUES, getOptionU32(args, L"--limit", 128U) });
            for (std::size_t i = 0; i < valueCount; ++i)
            {
                const auto& value = response.values[i];
                std::wcout << L"  value[" << i << L"] type=" << value.valueType
                           << L" dataBytes=" << value.dataBytes
                           << L" requiredBytes=" << value.requiredBytes
                           << L" flags=0x" << std::hex << value.flags << std::dec
                           << L" name='" << fixedWide(value.name, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS) << L"'\n";
                printBytesInline(L"    data", value.data, std::min<std::size_t>(value.dataBytes, KSWORD_ARK_REGISTRY_ENUM_VALUE_DATA_MAX_BYTES), 24U);
            }
            return 0;
        }
        if (sub == L"set-value")
        {
            KSWORD_ARK_SET_REGISTRY_VALUE_REQUEST request{};
            KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
            request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", 0U);
            request.valueType = requireOptionU32(args, L"--type");
            copyRequiredWideOption(args, L"--key", request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS);
            if (copyOptionalWideOption(args, L"--value", request.valueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS))
            {
                request.flags |= KSWORD_ARK_REGISTRY_SET_FLAG_VALUE_NAME_PRESENT;
            }
            const std::vector<std::uint8_t> data = readRequiredBlobOption(args, L"--data-file", KSWORD_ARK_REGISTRY_DATA_MAX_BYTES);
            request.dataBytes = static_cast<unsigned long>(data.size());
            copyBytesToFixed(request.data, KSWORD_ARK_REGISTRY_DATA_MAX_BYTES, data);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_SET_REGISTRY_VALUE, L"IOCTL_KSWORD_ARK_SET_REGISTRY_VALUE", request, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printRegistryOperationResponse(response, io.bytesReturned);
            return 0;
        }
        if (sub == L"delete-value")
        {
            KSWORD_ARK_REGISTRY_VALUE_NAME_REQUEST request{};
            KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
            request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", 0U);
            copyRequiredWideOption(args, L"--key", request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS);
            if (copyOptionalWideOption(args, L"--value", request.valueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS))
            {
                request.flags |= KSWORD_ARK_REGISTRY_DELETE_VALUE_FLAG_NAME_PRESENT;
            }
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_DELETE_REGISTRY_VALUE, L"IOCTL_KSWORD_ARK_DELETE_REGISTRY_VALUE", request, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printRegistryOperationResponse(response, io.bytesReturned);
            return 0;
        }
        if (sub == L"create-key" || sub == L"delete-key")
        {
            KSWORD_ARK_REGISTRY_KEY_PATH_REQUEST request{};
            KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
            request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", 0U);
            copyRequiredWideOption(args, L"--key", request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS);
            const DWORD code = (sub == L"create-key") ? IOCTL_KSWORD_ARK_CREATE_REGISTRY_KEY : IOCTL_KSWORD_ARK_DELETE_REGISTRY_KEY;
            const wchar_t* label = (sub == L"create-key") ? L"IOCTL_KSWORD_ARK_CREATE_REGISTRY_KEY" : L"IOCTL_KSWORD_ARK_DELETE_REGISTRY_KEY";
            if (!sendFixedRequestResponse(code, label, request, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printRegistryOperationResponse(response, io.bytesReturned);
            return 0;
        }
        if (sub == L"rename-value")
        {
            KSWORD_ARK_RENAME_REGISTRY_VALUE_REQUEST request{};
            KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
            request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", 0U);
            copyRequiredWideOption(args, L"--key", request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS);
            copyRequiredWideOption(args, L"--old-value", request.oldValueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS);
            copyRequiredWideOption(args, L"--new-value", request.newValueName, KSWORD_ARK_REGISTRY_VALUE_NAME_CHARS);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_RENAME_REGISTRY_VALUE, L"IOCTL_KSWORD_ARK_RENAME_REGISTRY_VALUE", request, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printRegistryOperationResponse(response, io.bytesReturned);
            return 0;
        }
        if (sub == L"rename-key")
        {
            KSWORD_ARK_RENAME_REGISTRY_KEY_REQUEST request{};
            KSWORD_ARK_REGISTRY_OPERATION_RESPONSE response{};
            request.version = KSWORD_ARK_REGISTRY_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", 0U);
            copyRequiredWideOption(args, L"--key", request.keyPath, KSWORD_ARK_REGISTRY_PATH_CHARS);
            copyRequiredWideOption(args, L"--new-name", request.newKeyName, KSWORD_ARK_REGISTRY_ENUM_KEY_NAME_CHARS);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_RENAME_REGISTRY_KEY, L"IOCTL_KSWORD_ARK_RENAME_REGISTRY_KEY", request, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printRegistryOperationResponse(response, io.bytesReturned);
            return 0;
        }
        std::wcerr << L"error: unknown registry subcommand '" << sub << L"'\n";
        return 1;
    }

    // commandRedirectFamily implements redirect rule load and status query.
    // Inputs: argc/argv from wmain.
    // Processing: accepts a blob for rule snapshots and prints runtime rules.
    // Returns: process exit code.
    int commandRedirectFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: redirect requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        IoctlResult io{};
        if (sub == L"set-rules")
        {
            std::vector<std::uint8_t> blob = readRequiredBlobOption(args, L"--blob", kMaxCommandBytes);
            KSWORD_ARK_REDIRECT_SET_RULES_RESPONSE response{};
            if (!sendBlobFixedResponse(IOCTL_KSWORD_ARK_REDIRECT_SET_RULES, L"IOCTL_KSWORD_ARK_REDIRECT_SET_RULES", blob, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printResponseBanner(response.version, response.status, response.lastStatus, io.bytesReturned);
            std::wcout << L"runtimeFlags=0x" << std::hex << response.runtimeFlags
                       << L" appliedCount=" << std::dec << response.appliedCount
                       << L" rejectedIndex=" << response.rejectedIndex
                       << L" fileRuleCount=" << response.fileRuleCount
                       << L" registryRuleCount=" << response.registryRuleCount
                       << L" generation=" << response.generation << L"\n";
            return 0;
        }
        if (sub == L"query-status")
        {
            KSWORD_ARK_REDIRECT_STATUS_RESPONSE response{};
            if (!sendFixedNoInput(IOCTL_KSWORD_ARK_REDIRECT_QUERY_STATUS, L"IOCTL_KSWORD_ARK_REDIRECT_QUERY_STATUS", response, io)) return 3;
            printResponseBanner(response.version, response.status, response.registryRegisterStatus, io.bytesReturned);
            std::wcout << L"runtimeFlags=0x" << std::hex << response.runtimeFlags
                       << std::dec << L" fileRuleCount=" << response.fileRuleCount
                       << L" registryRuleCount=" << response.registryRuleCount
                       << L" generation=" << response.generation
                       << L" fileHits=" << response.fileRedirectHits
                       << L" registryHits=" << response.registryRedirectHits
                       << L" registryRegisterStatus=0x" << std::hex << static_cast<unsigned long>(response.registryRegisterStatus) << std::dec << L"\n";
            const std::size_t limit = getOptionU32(args, L"--limit", 16U);
            for (std::size_t i = 0; i < std::min<std::size_t>(KSWORD_ARK_REDIRECT_MAX_RULES, limit); ++i)
            {
                const auto& rule = response.rules[i];
                if (rule.ruleId == 0U && rule.sourcePath[0] == L'\0' && rule.targetPath[0] == L'\0')
                {
                    continue;
                }
                std::wcout << L"  rule[" << i << L"] id=" << rule.ruleId
                           << L" type=" << rule.type
                           << L" action=" << rule.action
                           << L" matchMode=" << rule.matchMode
                           << L" flags=0x" << std::hex << rule.flags
                           << std::dec << L" pid=" << rule.processId
                           << L" source='" << fixedWide(rule.sourcePath, KSWORD_ARK_REDIRECT_PATH_CHARS)
                           << L"' target='" << fixedWide(rule.targetPath, KSWORD_ARK_REDIRECT_PATH_CHARS) << L"'\n";
            }
            return 0;
        }
        std::wcerr << L"error: unknown redirect subcommand '" << sub << L"'\n";
        return 1;
    }

    // commandNetworkFamily implements network rule load and status query.
    // Inputs: argc/argv from wmain.
    // Processing: accepts a blob for rules and prints runtime snapshot state.
    // Returns: process exit code.
    int commandNetworkFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: network requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        IoctlResult io{};
        if (sub == L"set-rules")
        {
            std::vector<std::uint8_t> blob = readRequiredBlobOption(args, L"--blob", kMaxCommandBytes);
            KSWORD_ARK_NETWORK_SET_RULES_RESPONSE response{};
            if (!sendBlobFixedResponse(IOCTL_KSWORD_ARK_NETWORK_SET_RULES, L"IOCTL_KSWORD_ARK_NETWORK_SET_RULES", blob, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printResponseBanner(response.version, response.status, response.lastStatus, io.bytesReturned);
            std::wcout << L"runtimeFlags=0x" << std::hex << response.runtimeFlags
                       << std::dec << L" appliedCount=" << response.appliedCount
                       << L" blockedRuleCount=" << response.blockedRuleCount
                       << L" hiddenPortRuleCount=" << response.hiddenPortRuleCount
                       << L" rejectedIndex=" << response.rejectedIndex
                       << L" generation=" << response.generation << L"\n";
            return 0;
        }
        if (sub == L"query-status")
        {
            KSWORD_ARK_NETWORK_STATUS_RESPONSE response{};
            if (!sendFixedNoInput(IOCTL_KSWORD_ARK_NETWORK_QUERY_STATUS, L"IOCTL_KSWORD_ARK_NETWORK_QUERY_STATUS", response, io)) return 3;
            printResponseBanner(response.version, response.status, response.registerStatus, io.bytesReturned);
            std::wcout << L"runtimeFlags=0x" << std::hex << response.runtimeFlags
                       << std::dec << L" ruleCount=" << response.ruleCount
                       << L" blockedRuleCount=" << response.blockedRuleCount
                       << L" hiddenPortRuleCount=" << response.hiddenPortRuleCount
                       << L" generation=" << response.generation
                       << L" classifyCount=" << response.classifyCount
                       << L" blockedCount=" << response.blockedCount
                       << L" registerStatus=0x" << std::hex << static_cast<unsigned long>(response.registerStatus)
                       << L" engineStatus=0x" << static_cast<unsigned long>(response.engineStatus) << std::dec << L"\n";
            const std::size_t limit = getOptionU32(args, L"--limit", 16U);
            for (std::size_t i = 0; i < std::min<std::size_t>(KSWORD_ARK_NETWORK_MAX_RULES, limit); ++i)
            {
                const auto& rule = response.rules[i];
                if (rule.ruleId == 0U && rule.localPort == 0U && rule.remotePort == 0U && rule.processId == 0U)
                {
                    continue;
                }
                std::wcout << L"  rule[" << i << L"] id=" << rule.ruleId
                           << L" action=" << rule.action
                           << L" dir=0x" << std::hex << rule.directionMask
                           << L" proto=" << std::dec << rule.protocol
                           << L" pid=" << rule.processId
                           << L" flags=0x" << std::hex << rule.flags
                           << std::dec << L" localPort=" << rule.localPort
                           << L" remotePort=" << rule.remotePort << L"\n";
            }
            return 0;
        }
        std::wcerr << L"error: unknown network subcommand '" << sub << L"'\n";
        return 1;
    }

    // commandKeyboardFamily implements keyboard hotkey and hook enumeration.
    // Inputs: argc/argv from wmain.
    // Processing: prints bounded row lists and diagnostic offsets.
    // Returns: process exit code.
    int commandKeyboardFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: keyboard requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        IoctlResult io{};
        if (sub == L"enum-hotkeys" || sub == L"enum-hooks")
        {
            KSWORD_ARK_ENUM_KEYBOARD_REQUEST request{};
            request.version = KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_ALL);
            request.processId = getOptionU32(args, L"--pid", 0U);
            request.maxEntries = getOptionU32(args, L"--max-entries", 256U);
            std::vector<std::uint8_t> buffer(kLargeResponseBytes, 0U);
            if (sub == L"enum-hotkeys")
            {
                const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS", IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS, &request, sizeof(request), buffer, io);
                if (rc != 0) return rc;
                constexpr std::size_t headerSize = sizeof(KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE) - sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY);
                const auto* response = reinterpret_cast<const KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE*>(buffer.data());
                std::size_t available = 0U;
                try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY), L"keyboard hotkeys"); }
                catch (...) { return 4; }
                printResponseBanner(response->version, response->status, response->lastStatus, io.bytesReturned);
                printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
                std::wcout << L"win32kBase=" << hex64(response->win32kBase)
                           << L" sessionGlobals=" << hex64(response->sessionGlobals)
                           << L" tableOffset=" << response->tableOffset
                           << L" hotkeyNextOffset=" << response->hotkeyNextOffset
                           << L" hotkeyModifiersOffset=" << response->hotkeyModifiersOffset
                           << L" hotkeyVkOffset=" << response->hotkeyVkOffset
                           << L" hotkeyIdOffset=" << response->hotkeyIdOffset << L"\n";
                const std::size_t parsed = responseCountLimit(response->returnedCount, available, getOptionU32(args, L"--limit", 128U));
                for (std::size_t i = 0; i < parsed; ++i)
                {
                    const auto* entry = reinterpret_cast<const KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
                    std::wcout << L"  [" << i << L"] source=" << entry->source
                               << L" status=" << entry->status
                               << L" flags=0x" << std::hex << entry->flags
                               << L" bucket=" << entry->bucketIndex
                               << L" depth=" << entry->depth
                               << L" modifiers=0x" << entry->modifiers
                               << L" modifiers2=0x" << entry->modifierFlags2
                               << L" vk=" << std::dec << entry->virtualKey
                               << L" id=" << entry->hotkeyId
                               << L" pid=" << entry->processId
                               << L" tid=" << entry->threadId
                               << L" last=0x" << std::hex << static_cast<unsigned long>(entry->lastStatus)
                               << std::dec << L" detail='" << fixedWide(entry->detail, KSWORD_ARK_KEYBOARD_DETAIL_CHARS) << L"'\n";
                }
                return 0;
            }
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOOKS", IOCTL_KSWORD_ARK_ENUM_KEYBOARD_HOOKS, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE) - sizeof(KSWORD_ARK_KEYBOARD_HOOK_ENTRY);
            const auto* response = reinterpret_cast<const KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_KEYBOARD_HOOK_ENTRY), L"keyboard hooks"); }
            catch (...) { return 4; }
            printResponseBanner(response->version, response->status, response->lastStatus, io.bytesReturned);
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            std::wcout << L"win32kBase=" << hex64(response->win32kBase)
                       << L" threadHookArrayOffset=" << response->threadHookArrayOffset
                       << L" desktopInfoOffset=" << response->desktopInfoOffset
                       << L" desktopHookArrayOffset=" << response->desktopHookArrayOffset
                       << L" hookNextOffset=" << response->hookNextOffset
                       << L" hookTypeOffset=" << response->hookTypeOffset
                       << L" hookProcedureOffset=" << response->hookProcedureOffset
                       << L" hookFlagsOffset=" << response->hookFlagsOffset
                       << L" hookModuleIdOffset=" << response->hookModuleIdOffset
                       << L" hookTargetThreadInfoOffset=" << response->hookTargetThreadInfoOffset << L"\n";
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, getOptionU32(args, L"--limit", 128U));
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* entry = reinterpret_cast<const KSWORD_ARK_KEYBOARD_HOOK_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
                std::wcout << L"  [" << i << L"] source=" << entry->source
                           << L" status=" << entry->status
                           << L" flags=0x" << std::hex << entry->flags
                           << L" type=" << entry->hookType
                           << L" scope=" << entry->hookScope
                           << L" pid=" << std::dec << entry->processId
                           << L" tid=" << entry->threadId
                           << L" moduleId=" << entry->moduleId
                           << L" last=0x" << std::hex << static_cast<unsigned long>(entry->lastStatus)
                           << L" hook=" << hex64(entry->hookObject)
                           << L" proc=" << hex64(entry->procedureAddress)
                           << std::dec << L" detail='" << fixedWide(entry->detail, KSWORD_ARK_KEYBOARD_DETAIL_CHARS) << L"'\n";
            }
            return 0;
        }
        std::wcerr << L"error: unknown keyboard subcommand '" << sub << L"'\n";
        return 1;
    }

    // commandMutationFamily implements prepare/commit/rollback and audit queries.
    // Inputs: argc/argv from wmain.
    // Processing: loads hex or file blobs and renders audit rows.
    // Returns: process exit code.
    int commandMutationFamily(int argc, wchar_t* argv[])
    {
        if (argc < 3) { std::wcerr << L"error: mutation requires a subcommand\n"; return 1; }
        const std::wstring sub = argv[2];
        const NamedArgs args = parseNamedArgs(argc, argv, 3);
        IoctlResult io{};
        if (sub == L"prepare")
        {
            std::vector<std::uint8_t> afterBytes = loadBytesFromHexOrFile(args, L"--after-hex", L"--after-file", KSWORD_ARK_MUTATION_MAX_BYTES, true);
            std::vector<std::uint8_t> beforeBytes = loadBytesFromHexOrFile(args, L"--before-hex", L"--before-file", KSWORD_ARK_MUTATION_MAX_BYTES, false);
            KSWORD_ARK_MUTATION_PREPARE_REQUEST request{};
            KSWORD_ARK_MUTATION_RESPONSE response{};
            request.size = sizeof(request);
            request.version = KSWORD_ARK_MUTATION_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", 0U);
            request.targetKind = requireOptionU32(args, L"--target-kind");
            request.processId = getOptionU32(args, L"--pid", 0U);
            request.bytes = static_cast<unsigned long>(afterBytes.size());
            request.targetAddress = getOptionU64(args, L"--address", 0ULL);
            request.targetContext = getOptionU64(args, L"--context", 0ULL);
            copyBytesToFixed(request.afterBytes, KSWORD_ARK_MUTATION_MAX_BYTES, afterBytes);
            copyBytesToFixed(request.expectedBeforeBytes, KSWORD_ARK_MUTATION_MAX_BYTES, beforeBytes);
            if (!sendFixedRequestResponse(IOCTL_KSWORD_ARK_MUTATION_PREPARE, L"IOCTL_KSWORD_ARK_MUTATION_PREPARE", request, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printResponseBanner(response.version, response.status, response.lastStatus, io.bytesReturned);
            std::wcout << L"size=" << response.size
                       << L" targetKind=" << response.targetKind
                       << L" pid=" << response.processId
                       << L" bytes=" << response.bytes
                       << L" riskFlags=0x" << std::hex << response.riskFlags
                       << L" transactionId=" << response.transactionId
                       << L" targetAddress=" << hex64(response.targetAddress)
                       << L" targetContext=" << hex64(response.targetContext)
                       << L" beforeHash=" << hex64(response.beforeHash)
                       << L" afterHash=" << hex64(response.afterHash)
                       << std::dec << L" timestampTick=" << response.timestampTick << L"\n";
            printBytesInline(L"beforeBytes", response.beforeBytes, response.bytes);
            printBytesInline(L"afterBytes", response.afterBytes, response.bytes);
            return 0;
        }
        if (sub == L"commit" || sub == L"rollback")
        {
            KSWORD_ARK_MUTATION_TRANSACTION_REQUEST request{};
            KSWORD_ARK_MUTATION_RESPONSE response{};
            request.size = sizeof(request);
            request.version = KSWORD_ARK_MUTATION_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", 0U);
            request.transactionId = requireOptionU64(args, L"--transaction-id");
            const DWORD code = (sub == L"commit") ? IOCTL_KSWORD_ARK_MUTATION_COMMIT : IOCTL_KSWORD_ARK_MUTATION_ROLLBACK;
            const wchar_t* label = (sub == L"commit") ? L"IOCTL_KSWORD_ARK_MUTATION_COMMIT" : L"IOCTL_KSWORD_ARK_MUTATION_ROLLBACK";
            if (!sendFixedRequestResponse(code, label, request, response, io, GENERIC_READ | GENERIC_WRITE)) return 3;
            printResponseBanner(response.version, response.status, response.lastStatus, io.bytesReturned);
            std::wcout << L"size=" << response.size
                       << L" targetKind=" << response.targetKind
                       << L" pid=" << response.processId
                       << L" bytes=" << response.bytes
                       << L" riskFlags=0x" << std::hex << response.riskFlags
                       << L" transactionId=" << response.transactionId
                       << L" targetAddress=" << hex64(response.targetAddress)
                       << L" targetContext=" << hex64(response.targetContext)
                       << L" beforeHash=" << hex64(response.beforeHash)
                       << L" afterHash=" << hex64(response.afterHash)
                       << std::dec << L" timestampTick=" << response.timestampTick << L"\n";
            printBytesInline(L"beforeBytes", response.beforeBytes, response.bytes);
            printBytesInline(L"afterBytes", response.afterBytes, response.bytes);
            return 0;
        }
        if (sub == L"query-audit")
        {
            KSWORD_ARK_MUTATION_QUERY_AUDIT_REQUEST request{};
            request.size = sizeof(request);
            request.version = KSWORD_ARK_MUTATION_PROTOCOL_VERSION;
            request.flags = getOptionU32(args, L"--flags", 0U);
            request.maxEntries = getOptionU32(args, L"--max-entries", KSWORD_ARK_MUTATION_AUDIT_RING_CAPACITY);
            request.startSequence = getOptionU64(args, L"--start-sequence", 0ULL);
            std::vector<std::uint8_t> buffer(kLargeResponseBytes, 0U);
            const int rc = sendRawIoctl(L"IOCTL_KSWORD_ARK_MUTATION_QUERY_AUDIT", IOCTL_KSWORD_ARK_MUTATION_QUERY_AUDIT, &request, sizeof(request), buffer, io);
            if (rc != 0) return rc;
            constexpr std::size_t headerSize = sizeof(KSWORD_ARK_MUTATION_QUERY_AUDIT_RESPONSE) - sizeof(KSWORD_ARK_MUTATION_AUDIT_ENTRY);
            const auto* response = reinterpret_cast<const KSWORD_ARK_MUTATION_QUERY_AUDIT_RESPONSE*>(buffer.data());
            std::size_t available = 0U;
            try { available = validateVariable(io.bytesReturned, headerSize, response->entrySize, sizeof(KSWORD_ARK_MUTATION_AUDIT_ENTRY), L"mutation audit"); }
            catch (...) { return 4; }
            printCountHeader(response->version, response->totalCount, response->returnedCount, response->entrySize, io.bytesReturned);
            std::wcout << L"size=" << response->size
                       << L" lostCount=" << response->lostCount
                       << L" oldestSequence=" << response->oldestSequence
                       << L" nextSequence=" << response->nextSequence << L"\n";
            const std::size_t parsed = responseCountLimit(response->returnedCount, available, getOptionU32(args, L"--limit", 64U));
            for (std::size_t i = 0; i < parsed; ++i)
            {
                const auto* entry = reinterpret_cast<const KSWORD_ARK_MUTATION_AUDIT_ENTRY*>(buffer.data() + headerSize + (i * response->entrySize));
                std::wcout << L"  [" << i << L"] op=" << entry->operation
                           << L" status=" << entry->status
                           << L" targetKind=" << entry->targetKind
                           << L" pid=" << entry->processId
                           << L" bytes=" << entry->bytes
                           << L" risk=0x" << std::hex << entry->riskFlags
                           << L" flags=0x" << entry->flags
                           << L" transactionId=" << entry->transactionId
                           << L" sequence=" << entry->sequence
                           << L" targetAddress=" << hex64(entry->targetAddress)
                           << L" targetContext=" << hex64(entry->targetContext)
                           << L" beforeHash=" << hex64(entry->beforeHash)
                           << L" afterHash=" << hex64(entry->afterHash)
                           << std::dec << L" timestampTick=" << entry->timestampTick << L"\n";
                if (getOptionBool(args, L"--hexdump"))
                {
                    printBytesInline(L"    byteData", entry->byteData, std::min<std::size_t>(entry->bytes, KSWORD_ARK_MUTATION_MAX_BYTES), 32U);
                }
            }
            return 0;
        }
        std::wcerr << L"error: unknown mutation subcommand '" << sub << L"'\n";
        return 1;
    }

    // dispatchCommand maps the first CLI token to an IOCTL command family.
    // Inputs: argc/argv from wmain; argv[1] is the command family.
    // Processing: keeps family routing centralized and leaves subcommand parsing
    //             to the family handlers.
    // Returns: process exit code from the selected handler.
    int dispatchCommand(int argc, wchar_t* argv[])
    {
        if (argc < 2 || argv[1] == nullptr)
        {
            printUsage();
            return 1;
        }

        const std::wstring family = argv[1];
        if (family == L"help" || family == L"--help" || family == L"-h" || family == L"/?")
        {
            printUsage();
            return 0;
        }
        if (family == L"log") return commandLogFamily(argc, argv);
        if (family == L"process") return commandProcessFamily(argc, argv);
        if (family == L"memory") return commandMemoryFamily(argc, argv);
        if (family == L"file") return commandFileFamily(argc, argv);
        if (family == L"kernel") return commandKernelFamily(argc, argv);
        if (family == L"callback") return commandCallbackFamily(argc, argv);
        if (family == L"dyn") return commandDynFamily(argc, argv);
        if (family == L"thread") return commandThreadFamily(argc, argv);
        if (family == L"handle") return commandHandleFamily(argc, argv);
        if (family == L"alpc") return commandAlpcFamily(argc, argv);
        if (family == L"section") return commandSectionFamily(argc, argv);
        if (family == L"trust") return commandTrustFamily(argc, argv);
        if (family == L"safety") return commandSafetyFamily(argc, argv);
        if (family == L"preflight") return commandPreflightFamily(argc, argv);
        if (family == L"registry") return commandRegistryFamily(argc, argv);
        if (family == L"redirect") return commandRedirectFamily(argc, argv);
        if (family == L"network") return commandNetworkFamily(argc, argv);
        if (family == L"keyboard") return commandKeyboardFamily(argc, argv);
        if (family == L"mutation") return commandMutationFamily(argc, argv);
        if (family == L"capability") return commandCapabilityFamily(argc, argv);
        if (family == L"wsl") return commandWslFamily(argc, argv);

        std::wcerr << L"error: unknown family '" << family << L"'\n";
        printUsage();
        return 1;
    }

    // configureConsole prepares stdout/stderr for Unicode-friendly diagnostics.
    // Inputs: none.
    // Processing: uses UTF-8 wide-text mode so console and redirected output do
    //             not contain UTF-16 NUL separators.
    // Returns: no value.
    void configureConsole()
    {
        (void)_setmode(_fileno(stdout), _O_U8TEXT);
        (void)_setmode(_fileno(stderr), _O_U8TEXT);
    }
}

// wmain is the process entry point for the Unicode CLI.
// Inputs: raw command-line arguments from the Windows CRT.
// Processing: configures console output, dispatches the requested family, and
//             converts parser/runtime exceptions into stable CLI errors.
// Returns: process exit code; zero means the requested operation completed.
int wmain(int argc, wchar_t* argv[])
{
    configureConsole();
    try
    {
        return dispatchCommand(argc, argv);
    }
    catch (const std::exception& ex)
    {
        const std::string message = ex.what();
        std::wcerr << L"error: " << std::wstring(message.begin(), message.end()) << L"\n";
        return 1;
    }
}
