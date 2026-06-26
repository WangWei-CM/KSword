#include "MonitorFeature.h"

#include "EtwMonitorView.h"

namespace Ksword::Features::Monitor {

HWND CreateMonitorFeaturePage(HWND parent, const RECT& bounds) {
    return CreateEtwMonitorPage(parent, bounds);
}

} // namespace Ksword::Features::Monitor
