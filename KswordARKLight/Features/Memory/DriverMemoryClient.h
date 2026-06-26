#pragma once

#include "DriverMemoryModel.h"

namespace Ksword::Features::Memory {

// DriverMemoryClient is the module-local facade for driver memory operations.
// The view depends only on this class, so UI code never performs raw driver I/O
// directly. Requests are forwarded to the shared ArkDriverClient implementation
// used by the full KswordARK project, which keeps IOCTL structure ownership in
// shared/driver and avoids duplicate protocol definitions.
class DriverMemoryClient final {
public:
    DriverMemoryClient();
    ~DriverMemoryClient();

    DriverMemoryClient(const DriverMemoryClient&) = delete;
    DriverMemoryClient& operator=(const DriverMemoryClient&) = delete;

    // ReadMemory sends a validated read request to the driver facade. Input is a
    // request produced by DriverMemoryModel; processing calls ArkDriverClient
    // readVirtualMemory without zero-fill fallback so unreadable ranges are not
    // misreported as successful all-zero buffers; output describes success,
    // status and returned bytes.
    DriverMemoryReadResult ReadMemory(const DriverMemoryReadRequest& request);

    // WriteMemory sends a validated write request to the driver facade. Input is
    // a request produced by DriverMemoryModel; processing calls ArkDriverClient
    // writeVirtualMemory with the shared UI-confirmed flag; output describes
    // success, status and the accepted byte count.
    DriverMemoryWriteResult WriteMemory(const DriverMemoryWriteRequest& request);
};

} // namespace Ksword::Features::Memory
