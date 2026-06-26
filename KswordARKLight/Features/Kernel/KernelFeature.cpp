#include "KernelFeature.h"

#include "KernelCatalog.h"
#include "KernelPage.h"

namespace Ksword::Features::Kernel {

std::vector<KernelFeatureDescriptor> GetKernelFeatureCatalog() {
    // Return the module-local catalog rather than reaching into the application
    // registry. This keeps Kernel ownership self-contained until the final
    // session adds these files to KswordARKLight.vcxproj/.filters.
    return GetKernelFeatureDescriptors();
}

HWND CreateKernelFeaturePage(HWND parent, int controlId, const RECT& bounds) {
    // UI creation stays behind this module facade so external code does not
    // depend on KernelPage internals. The returned HWND owns its KernelPage
    // object and destroys it during WM_NCDESTROY.
    return CreateKernelPage(parent, controlId, bounds);
}

HWND CreateKernelSingleFeaturePage(HWND parent, int controlId, const RECT& bounds, KernelFeatureId featureId) {
    // CreateKernelSingleFeaturePage forwards to KernelPage while pinning one
    // retained feature id for the initial visible tab. This keeps callers from
    // reaching into KernelPage selection internals.
    return CreateKernelPageForFeature(parent, controlId, bounds, featureId);
}

} // namespace Ksword::Features::Kernel
