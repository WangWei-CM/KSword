#include "ArkDriverCapabilities.h"

namespace ksword::ark
{
    DriverCapabilities::DriverCapabilities(const std::uint32_t cachedBits) noexcept
        : m_cachedBits(cachedBits)
    {
    }

    bool DriverCapabilities::has(const DriverCapability capability) const noexcept
    {
        const auto requestedBit = static_cast<std::uint32_t>(capability);
        return requestedBit == 0 || (m_cachedBits & requestedBit) == requestedBit;
    }

    std::uint32_t DriverCapabilities::bits() const noexcept
    {
        return m_cachedBits;
    }

    void DriverCapabilities::setBits(const std::uint32_t cachedBits) noexcept
    {
        m_cachedBits = cachedBits;
    }
}
