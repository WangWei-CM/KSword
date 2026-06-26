#include "FileFeature.h"

#include "FileView.h"

namespace Ksword::Features::File {

HWND CreateFileFeaturePage(HWND parent, const RECT& bounds) {
    return CreateFileViewPage(parent, bounds);
}

} // namespace Ksword::Features::File
