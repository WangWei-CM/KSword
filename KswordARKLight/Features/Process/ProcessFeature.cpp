#include "ProcessFeature.h"

#include "ProcessView.h"

namespace Ksword::Features::Process {

HWND CreateProcessFeaturePage(HWND parent, const RECT& bounds) {
    // Input is the host HWND and desired child bounds. Processing is intentionally
    // thin: this facade preserves a stable integration symbol while ProcessView
    // owns child controls, enumeration, model binding, and menu interactions.
    // Return value is the child page HWND or null if the view cannot be created.
    return CreateProcessView(parent, bounds);
}

void ResizeProcessFeaturePage(HWND page, const RECT& bounds) {
    // Input is an existing process feature page and a new host layout rectangle.
    // Processing delegates to ProcessView, which uses MoveWindow only; there is
    // no return value because invalid HWNDs are safely ignored.
    ResizeProcessView(page, bounds);
}

} // namespace Ksword::Features::Process
