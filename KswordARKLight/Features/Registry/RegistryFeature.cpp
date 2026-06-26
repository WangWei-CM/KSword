#include "RegistryFeature.h"

#include "RegistryView.h"

namespace Ksword::Features::Registry {

HWND CreateRegistryFeaturePage(HWND parent, const RECT& bounds) {
    return CreateRegistryView(parent, bounds);
}

} // namespace Ksword::Features::Registry
