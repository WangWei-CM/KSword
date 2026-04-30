#pragma once

#include <cstdint>

namespace ksword::ark
{
    // Capability bit placeholders keep the client API ready for Phase 1 without
    // forcing Docks to learn future R0 capability packet details.
    enum class DriverCapability : std::uint32_t
    {
        None = 0,
        ProcessActions = 1U << 0,
        FileDelete = 1U << 1,
        SsdtSnapshot = 1U << 2,
        CallbackControl = 1U << 3
    };

    // DriverCapabilities is intentionally lightweight in Phase -1. Later phases
    // can replace cachedBits with a real IOCTL query while keeping UI call sites.
    class DriverCapabilities
    {
    public:
        DriverCapabilities() noexcept = default;
        explicit DriverCapabilities(std::uint32_t cachedBits) noexcept;

        bool has(DriverCapability capability) const noexcept;
        std::uint32_t bits() const noexcept;
        void setBits(std::uint32_t cachedBits) noexcept;

    private:
        std::uint32_t m_cachedBits = 0;
    };
}
