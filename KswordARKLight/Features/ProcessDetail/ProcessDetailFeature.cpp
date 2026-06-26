#include "ProcessDetailFeature.h"

#include "ProcessDetailPage.h"

namespace Ksword::Features::ProcessDetail {

HWND CreateProcessDetailPage(HWND parent, DWORD processId, const RECT& bounds) {
    return ProcessDetailPage::Create(parent, processId, bounds);
}

} // namespace Ksword::Features::ProcessDetail
