#include "WindowFeature.h"

#include "WindowView.h"

namespace Ksword::Features::Window {

HWND CreateWindowFeaturePage(HWND parent, const RECT& bounds) {
    return CreateWindowFeatureView(parent, bounds);
}

} // namespace Ksword::Features::Window
