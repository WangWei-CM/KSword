#pragma once

// Private, value-only backing store for evicted NIDS keys. No packet bytes,
// clocks, rules, expiry policy or QObject cross this boundary. An empty SQLite
// filename creates a private temporary database removed when it is closed.
#include "network_nids.h"
#include <winsqlite/winsqlite3.h>
#include <climits>
#include <optional>
#include <stdexcept>

namespace ks::network
{
    class NidsEngine::StateStore final
    {
    public:
        StateStore() noexcept
        {
            m_library = ::LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!m_library || !load(m_open, "sqlite3_open_v2") || !load(m_close, "sqlite3_close_v2")
                || !load(m_exec, "sqlite3_exec") || !load(m_prepare, "sqlite3_prepare_v2")
                || !load(m_bindInt, "sqlite3_bind_int") || !load(m_bindBlob, "sqlite3_bind_blob")
                || !load(m_step, "sqlite3_step") || !load(m_blob, "sqlite3_column_blob")
                || !load(m_bytes, "sqlite3_column_bytes") || !load(m_reset, "sqlite3_reset")
                || !load(m_clear, "sqlite3_clear_bindings") || !load(m_finalize, "sqlite3_finalize")) return;
            if (m_open("", &m_database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                | SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) return;
            bool memoryOnly = false;
            if (m_exec(m_database, "PRAGMA compile_options", [](void* context, int count, char** values, char**) {
                if (count > 0 && values[0] && std::strcmp(values[0], "TEMP_STORE=3") == 0)
                    *static_cast<bool*>(context) = true;
                return 0;
            }, &memoryOnly, nullptr) != SQLITE_OK || memoryOnly) return;
            // This session is not durable storage. Keep a small page cache and
            // no memory mapping; SQLite spills pages instead of keeping a RAM
            // hash/offset index for every historical key. Do not alter global
            // SQLite settings or install a provider used by another component.
            if (m_exec(m_database,
                "PRAGMA cache_size=64; PRAGMA mmap_size=0; PRAGMA temp_store=FILE;"
                "PRAGMA synchronous=OFF; PRAGMA journal_size_limit=0;"
                "CREATE TABLE states(kind INTEGER NOT NULL,k BLOB NOT NULL,v BLOB NOT NULL,"
                "PRIMARY KEY(kind,k)) WITHOUT ROWID;", nullptr, nullptr, nullptr) != SQLITE_OK) return;
            if (m_prepare(m_database, "SELECT v FROM states WHERE kind=?1 AND k=?2", -1, &m_read, nullptr) != SQLITE_OK
                || m_prepare(m_database, "INSERT OR REPLACE INTO states VALUES(?1,?2,?3)", -1, &m_write, nullptr) != SQLITE_OK) return;
            m_ready = true;
        }
        ~StateStore()
        {
            if (m_read) m_finalize(m_read);
            if (m_write) m_finalize(m_write);
            if (m_database) m_close(m_database);
            if (m_library) ::FreeLibrary(m_library);
        }
        StateStore(const StateStore&) = delete;
        StateStore& operator=(const StateStore&) = delete;

        std::optional<std::vector<std::uint8_t>> read(int kind, const std::string& key)
        {
            if (!m_ready || key.size() > INT_MAX || !historyBits(kind, key, false)) return std::nullopt;
            finish(m_read);
            try {
                if (m_bindInt(m_read, 1, kind) != SQLITE_OK ||
                    m_bindBlob(m_read, 2, key.data(), static_cast<int>(key.size()), SQLITE_STATIC) != SQLITE_OK)
                    throw std::runtime_error("NIDS state lookup binding failed");
                const int result = m_step(m_read);
                if (result == SQLITE_DONE) { finish(m_read); return std::nullopt; }
                if (result != SQLITE_ROW) throw std::runtime_error("NIDS state lookup failed");
                const int size = m_bytes(m_read, 0);
                const auto* bytes = static_cast<const std::uint8_t*>(m_blob(m_read, 0));
                if (size <= 0 || !bytes) throw std::runtime_error("NIDS state value unavailable");
                std::vector<std::uint8_t> value(bytes, bytes + size);
                finish(m_read);
                return value;
            } catch (...) { finish(m_read); throw; }
        }
        bool save(int kind, const std::string& key, const std::vector<std::uint8_t>& value) noexcept
        {
            if (!m_ready || key.size() > INT_MAX || value.empty() || value.size() > INT_MAX) return false;
            finish(m_write);
            const bool success = m_bindInt(m_write, 1, kind) == SQLITE_OK
                && m_bindBlob(m_write, 2, key.data(), static_cast<int>(key.size()), SQLITE_STATIC) == SQLITE_OK
                && m_bindBlob(m_write, 3, value.data(), static_cast<int>(value.size()), SQLITE_STATIC) == SQLITE_OK
                && m_step(m_write) == SQLITE_DONE;
            finish(m_write);
            if (success) (void)historyBits(kind, key, true);
            return success; // The hot value must stay resident if this fails.
        }

        static std::vector<std::uint8_t> pack(const PortScanWindow& value)
        {
            if (value.eventList.size() > (INT_MAX - 16u) / 10u) return {};
            std::vector<std::uint8_t> bytes;
            bytes.reserve(16 + value.eventList.size() * 10);
            append(bytes, value.lastAlertTimestampMs, 8);
            append(bytes, value.eventList.size(), 8);
            for (const auto& event : value.eventList) {
                append(bytes, event.timestampMs, 8); append(bytes, event.port, 2);
            }
            return bytes;
        }
        static std::vector<std::uint8_t> pack(const ByteWindow& value)
        {
            if (value.eventList.size() > (INT_MAX - 24u) / 16u) return {};
            std::vector<std::uint8_t> bytes;
            bytes.reserve(24 + value.eventList.size() * 16);
            append(bytes, value.totalBytes, 8);
            append(bytes, value.lastAlertTimestampMs, 8);
            append(bytes, value.eventList.size(), 8);
            for (const auto& event : value.eventList) {
                append(bytes, event.timestampMs, 8); append(bytes, event.byteCount, 8);
            }
            return bytes;
        }
        static std::vector<std::uint8_t> pack(std::uint64_t timestamp)
        {
            std::vector<std::uint8_t> bytes; bytes.reserve(8); append(bytes, timestamp, 8); return bytes;
        }
        static void restore(const std::vector<std::uint8_t>& bytes, PortScanWindow& value)
        {
            require(bytes.size() >= 16 && (bytes.size() - 16) % 10 == 0);
            std::size_t offset = 0;
            value.lastAlertTimestampMs = take(bytes, offset, 8);
            const auto count = take(bytes, offset, 8);
            require(count == (bytes.size() - 16) / 10);
            for (std::uint64_t i = 0; i < count; ++i) {
                PortEvent event; event.timestampMs = take(bytes, offset, 8);
                event.port = static_cast<std::uint16_t>(take(bytes, offset, 2));
                value.eventList.push_back(event);
            }
        }
        static void restore(const std::vector<std::uint8_t>& bytes, ByteWindow& value)
        {
            require(bytes.size() >= 24 && (bytes.size() - 24) % 16 == 0);
            std::size_t offset = 0;
            value.totalBytes = take(bytes, offset, 8); // Preserve the original uint64 accumulation exactly.
            value.lastAlertTimestampMs = take(bytes, offset, 8);
            const auto count = take(bytes, offset, 8);
            require(count == (bytes.size() - 24) / 16);
            for (std::uint64_t i = 0; i < count; ++i) {
                ByteEvent event; event.timestampMs = take(bytes, offset, 8);
                event.byteCount = take(bytes, offset, 8); value.eventList.push_back(event);
            }
        }
        static void restore(const std::vector<std::uint8_t>& bytes, std::uint64_t& timestamp)
        {
            require(bytes.size() == 8); std::size_t offset = 0; timestamp = take(bytes, offset, 8);
        }

    private:
        // A fixed 128KiB negative filter avoids disk lookups for never-stored
        // keys. Bits are set only after successful writes and never cleared.
        // Collisions can cause extra lookups, never a missing historical key.
        bool historyBits(int kind, const std::string& key, bool mark) noexcept
        {
            const std::uint64_t hash = std::hash<std::string>{}(key)
                ^ (0x9e3779b97f4a7c15ULL * (static_cast<std::uint64_t>(kind) + 1));
            const std::uint64_t step = (hash >> 32) | 1;
            bool present = true;
            for (std::uint64_t i = 0; i < 3; ++i) {
                const auto bit = static_cast<std::size_t>((hash + i * step) & 1048575u);
                auto& word = m_writtenBits[bit >> 6];
                const auto mask = std::uint64_t{1} << (bit & 63);
                present = present && (word & mask) != 0;
                if (mark) word |= mask;
            }
            return present;
        }
        template<class Function> bool load(Function& function, const char* name) noexcept
        {
            const FARPROC address = ::GetProcAddress(m_library, name);
            static_assert(sizeof(function) == sizeof(address));
            std::memcpy(&function, &address, sizeof(function));
            return function != nullptr;
        }
        void finish(sqlite3_stmt* statement) noexcept { m_reset(statement); m_clear(statement); }
        static void append(std::vector<std::uint8_t>& bytes, std::uint64_t value, int count)
        {
            for (int i = 0; i < count; ++i) { bytes.push_back(static_cast<std::uint8_t>(value)); value >>= 8; }
        }
        static std::uint64_t take(const std::vector<std::uint8_t>& bytes, std::size_t& offset, int count)
        {
            std::uint64_t value = 0;
            for (int i = 0; i < count; ++i) value |= static_cast<std::uint64_t>(bytes[offset++]) << (i * 8);
            return value;
        }
        static void require(bool value) { if (!value) throw std::runtime_error("NIDS state value invalid"); }
        HMODULE m_library = nullptr;
        sqlite3* m_database = nullptr;
        sqlite3_stmt* m_read = nullptr;
        sqlite3_stmt* m_write = nullptr;
        bool m_ready = false;
        std::array<std::uint64_t, 16384> m_writtenBits{};
        decltype(&sqlite3_open_v2) m_open = nullptr;
        decltype(&sqlite3_close_v2) m_close = nullptr;
        decltype(&sqlite3_exec) m_exec = nullptr;
        decltype(&sqlite3_prepare_v2) m_prepare = nullptr;
        decltype(&sqlite3_bind_int) m_bindInt = nullptr;
        decltype(&sqlite3_bind_blob) m_bindBlob = nullptr;
        decltype(&sqlite3_step) m_step = nullptr;
        decltype(&sqlite3_column_blob) m_blob = nullptr;
        decltype(&sqlite3_column_bytes) m_bytes = nullptr;
        decltype(&sqlite3_reset) m_reset = nullptr;
        decltype(&sqlite3_clear_bindings) m_clear = nullptr;
        decltype(&sqlite3_finalize) m_finalize = nullptr;
    };
}
