#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace ks::ui::detail
{
    class MemoryEditHistory final
    {
    public:
        using Bytes = std::vector<std::uint8_t>;
        enum class RecordResult { Unchanged, Recorded, CapacityExceeded, SizeMismatch, InvalidInput };

        struct DifferenceSummary { std::size_t bytes = 0; std::size_t runs = 0; };

        explicit MemoryEditHistory(
            std::size_t maximumBytes = 32U * 1024U * 1024U,
            std::size_t maximumCommands = 256U);

        void reset();
        RecordResult record(const Bytes& before, const Bytes& after);
        RecordResult record(const std::uint8_t* before, const std::uint8_t* after, std::size_t length);
        // The caller guarantees bytes outside [offset, offset+count) are unchanged.
        // Commands still retain the complete snapshot length and absolute offsets.
        RecordResult recordRange(const std::uint8_t* before, const std::uint8_t* after,
            std::size_t length, std::size_t offset, std::size_t count);
        static std::optional<DifferenceSummary> updateDifferenceSummary(DifferenceSummary current,
            const std::uint8_t* baseline, const std::uint8_t* before, const std::uint8_t* after,
            std::size_t length, std::size_t offset, std::size_t count);
        std::optional<Bytes> undo(const Bytes& current);
        std::optional<Bytes> redo(const Bytes& current);
        std::optional<Bytes> undo(const std::uint8_t* current, std::size_t length);
        std::optional<Bytes> redo(const std::uint8_t* current, std::size_t length);

        bool canUndo() const;
        bool canRedo() const;
        std::size_t undoCount() const;
        std::size_t redoCount() const;
        // Includes retained before/after byte payloads and command/range storage.
        // The caller owns the current snapshot; this model keeps only differences.
        std::size_t memoryUsage() const;

    private:
        struct Range { std::size_t offset = 0; Bytes before; Bytes after; };
        struct Command { std::size_t length = 0; std::size_t cost = 0; std::vector<Range> ranges; };

        std::optional<Bytes> apply(const std::uint8_t* current, std::size_t length, bool forward);
        std::deque<Command> m_commands;
        std::size_t m_cursor = 0;
        std::size_t m_memoryUsage = 0;
        std::size_t m_maximumBytes;
        std::size_t m_maximumCommands;
    };
}
