#include "SplitterUtil.h"

#include <algorithm>

namespace Ksword::Ui {

int ClampSplitterPosition(int totalLength, int desiredPosition, int minimumPaneSize) {
    // Inputs use logical pixels. The minimum is clamped to zero so callers may
    // explicitly allow a pane to collapse without extra validation.
    const int safeTotal = std::max(0, totalLength);
    const int safeMinimum = std::max(0, minimumPaneSize);
    if (safeTotal <= safeMinimum * 2) {
        return safeTotal / 2;
    }
    return std::clamp(desiredPosition, safeMinimum, safeTotal - safeMinimum);
}

SplitterLayout CalculateSplitterRects(
    const RECT& clientRect,
    SplitterOrientation orientation,
    int splitterPosition,
    int splitterSize) {
    // The helper performs geometry math only. It does not create windows,
    // subclass controls, or change DockManager behavior.
    SplitterLayout layout{};
    const int safeSplitterSize = std::max(1, splitterSize);

    if (orientation == SplitterOrientation::Vertical) {
        const int totalWidth = std::max(0, static_cast<int>(clientRect.right - clientRect.left));
        const int split = clientRect.left + std::clamp(splitterPosition, 0, totalWidth);
        const int half = safeSplitterSize / 2;
        layout.firstPane = RECT{ clientRect.left, clientRect.top, split - half, clientRect.bottom };
        layout.splitterBar = RECT{ split - half, clientRect.top, split - half + safeSplitterSize, clientRect.bottom };
        layout.secondPane = RECT{ split - half + safeSplitterSize, clientRect.top, clientRect.right, clientRect.bottom };
    } else {
        const int totalHeight = std::max(0, static_cast<int>(clientRect.bottom - clientRect.top));
        const int split = clientRect.top + std::clamp(splitterPosition, 0, totalHeight);
        const int half = safeSplitterSize / 2;
        layout.firstPane = RECT{ clientRect.left, clientRect.top, clientRect.right, split - half };
        layout.splitterBar = RECT{ clientRect.left, split - half, clientRect.right, split - half + safeSplitterSize };
        layout.secondPane = RECT{ clientRect.left, split - half + safeSplitterSize, clientRect.right, clientRect.bottom };
    }

    return layout;
}

bool HitTestSplitter(const SplitterLayout& layout, POINT point) {
    // PtInRect follows Win32 semantics: left/top inclusive, right/bottom
    // exclusive. The caller supplies client coordinates.
    return ::PtInRect(&layout.splitterBar, point) != FALSE;
}

} // namespace Ksword::Ui
