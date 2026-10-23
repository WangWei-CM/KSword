#pragma once

#include <QString>

#include <cstddef>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <evntcons.h>
#include <tdh.h>

namespace ks::monitor
{
    // Per consumer thread; no GUI/model ownership and no shared metadata-I/O lock.
    class ProcessTraceEventMetadata final
    {
    public:
        using Buffer = std::shared_ptr<const std::vector<unsigned char>>;
        using Loader = std::function<Buffer(const EVENT_RECORD*)>;
        static constexpr std::size_t MaximumEntries = 1024;
        static constexpr std::size_t MaximumBytes = 32 * 1024 * 1024;

        explicit ProcessTraceEventMetadata(Loader loader = load) : m_loader(std::move(loader)) {}

        static std::string key(const EVENT_RECORD* event)
        {
            if (!event) return {};
            std::string result;
            auto append = [&result](const auto& value) {
                result.append(reinterpret_cast<const char*>(&value), sizeof(value));
            };
            append(event->EventHeader.ProviderId);
            append(event->EventHeader.EventDescriptor); // Id/version/channel/level/opcode/task/keyword
            append(event->EventHeader.Flags); // includes width/classic/TraceLogging header form
            append(event->EventHeader.EventProperty);
            if (event->ExtendedData)
                for (USHORT i = 0; i < event->ExtendedDataCount; ++i)
                {
                    const auto& extension = event->ExtendedData[i];
                    if (extension.ExtType != EVENT_HEADER_EXT_TYPE_EVENT_SCHEMA_TL) continue;
                    append(extension.ExtType); append(extension.DataSize);
                    if (extension.DataPtr && extension.DataSize)
                        result.append(reinterpret_cast<const char*>(static_cast<ULONG_PTR>(extension.DataPtr)), extension.DataSize);
                }
            return result;
        }

        Buffer get(const EVENT_RECORD* event)
        {
            if (!event) return {};
            const auto identity = key(event);
            const auto found = m_entries.find(identity);
            if (found != m_entries.end()) return found->second;
            const auto result = m_loader(event);
            // Do not cache failures: a provider/manifest may become available later.
            if (!valid(result)) return {};
            const std::size_t bytes = identity.size() * 2 + result->size();
            if (bytes > MaximumBytes) return result; // Decode safely without retaining oversized schemas.
            while (!m_order.empty() && (m_entries.size() >= MaximumEntries || m_bytes > MaximumBytes - bytes))
            {
                const auto old = m_entries.find(m_order.front());
                if (old != m_entries.end()) { m_bytes -= old->first.size() * 2 + old->second->size(); m_entries.erase(old); }
                m_order.pop_front();
            }
            m_entries.emplace(identity, result); m_order.push_back(identity); m_bytes += bytes;
            return result;
        }

        static bool valid(const Buffer& buffer)
        {
            if (!buffer || buffer->size() < offsetof(TRACE_EVENT_INFO, EventPropertyInfoArray)) return false;
            const auto* info = reinterpret_cast<const TRACE_EVENT_INFO*>(buffer->data());
            const auto available = (buffer->size() - offsetof(TRACE_EVENT_INFO, EventPropertyInfoArray)) / sizeof(EVENT_PROPERTY_INFO);
            return info->PropertyCount <= available && info->TopLevelPropertyCount <= info->PropertyCount;
        }

        static QString text(const Buffer& buffer, const ULONG offset)
        {
            if (!buffer || !offset || offset >= buffer->size() || offset % sizeof(wchar_t)) return {};
            const auto* first = reinterpret_cast<const wchar_t*>(buffer->data() + offset);
            const auto capacity = (buffer->size() - offset) / sizeof(wchar_t);
            const auto count = wcsnlen_s(first, capacity);
            if (count == capacity) return {};
            return QString::fromWCharArray(first, static_cast<int>(count));
        }

        static QString eventName(const Buffer& buffer)
        {
            if (!valid(buffer)) return {};
            const auto* info = reinterpret_cast<const TRACE_EVENT_INFO*>(buffer->data());
            for (const ULONG offset : {info->EventNameOffset, info->TaskNameOffset, info->OpcodeNameOffset})
            {
                const auto value = text(buffer, offset).trimmed();
                if (!value.isEmpty()) return value;
            }
            return {};
        }

        std::size_t entries() const { return m_entries.size(); }
        std::size_t bytes() const { return m_bytes; }

    private:
        static Buffer load(const EVENT_RECORD* record)
        {
            if (!record) return {};
            DWORD size = 0;
            const auto status = ::TdhGetEventInformation(const_cast<EVENT_RECORD*>(record), 0, nullptr, nullptr, &size);
            if (status != ERROR_INSUFFICIENT_BUFFER || size < offsetof(TRACE_EVENT_INFO, EventPropertyInfoArray)) return {};
            auto bytes = std::make_shared<std::vector<unsigned char>>(size, 0);
            if (::TdhGetEventInformation(const_cast<EVENT_RECORD*>(record), 0, nullptr,
                reinterpret_cast<PTRACE_EVENT_INFO>(bytes->data()), &size) != ERROR_SUCCESS) return {};
            if (size > bytes->size()) return {};
            bytes->resize(size);
            return bytes;
        }

        Loader m_loader;
        std::unordered_map<std::string, Buffer> m_entries;
        std::deque<std::string> m_order;
        std::size_t m_bytes = 0;
    };

    inline ProcessTraceEventMetadata& processTraceMetadataForThread()
    {
        static thread_local ProcessTraceEventMetadata cache;
        return cache;
    }
}
