#pragma once

#include "../Core/Win32Lean.h"

namespace Ksword::Ui {

// SplitterOrientation identifies how a splitter divides a rectangle. It owns no
// state and only guides CalculateSplitterRects.
enum class SplitterOrientation {
    Vertical,   // Left/right panes with a vertical splitter bar.
    Horizontal  // Top/bottom panes with a horizontal splitter bar.
};

// SplitterLayout stores the three rectangles produced by a split operation.
// firstPane and secondPane are child placement areas; splitterBar is the hit and
// paint area for the divider.
struct SplitterLayout {
    RECT firstPane{};
    RECT splitterBar{};
    RECT secondPane{};
};

// CalculateSplitterRects divides a client rectangle. Inputs are the client
// bounds, orientation, splitter position and bar size; processing clamps the
// splitter into the rectangle; output is the three resulting rectangles.
SplitterLayout CalculateSplitterRects(const RECT& clientRect, SplitterOrientation orientation, int splitterPosition, int splitterSize);

// HitTestSplitter checks whether a point is inside the splitter bar. Inputs are
// a layout and client point; output is true when the point hits the bar.
bool HitTestSplitter(const SplitterLayout& layout, POINT point);

// ClampSplitterPosition constrains a splitter position to keep both panes at or
// above a requested minimum size. Inputs are total length, desired position and
// minimum pane size; output is the clamped position.
int ClampSplitterPosition(int totalLength, int desiredPosition, int minimumPaneSize);

} // namespace Ksword::Ui
