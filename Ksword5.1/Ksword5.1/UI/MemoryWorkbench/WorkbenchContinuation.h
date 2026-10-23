#pragma once
#include <functional>

namespace ks::ui
{
    // Completed is final success. Pending retains the original request and invokes
    // its completion exactly once. Rejected/Busy never invoke that completion.
    enum class LeaveEntryStatus { Completed, Pending, Rejected, Busy };
    using LeaveCompletion = std::function<void(bool)>;
}
