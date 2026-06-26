#include "StartupFeature.h"

#include "StartupView.h"

namespace Ksword::Features::Startup {

HWND CreateStartupFeaturePage(HWND parent, const RECT& bounds) {
    return CreateStartupFeatureView(parent, bounds);
}

} // namespace Ksword::Features::Startup
