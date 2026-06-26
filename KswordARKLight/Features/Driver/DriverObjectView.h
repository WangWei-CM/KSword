#pragma once

#include "../../Core/Win32Lean.h"

#include "DriverModel.h"

#include <string>

namespace Ksword::Features::Driver {

// CreateDriverObjectView creates the read-only object-information child page.
// Inputs are the parent HWND, placement rectangle, and a model pointer owned by
// the root Driver feature page; processing registers the window class and
// creates a report list; output is the child HWND or nullptr on failure.
HWND CreateDriverObjectView(HWND parent, const RECT& bounds, DriverModel* model);

// ResizeDriverObjectView moves the object-info child page. Inputs are an
// existing child HWND and the new client rectangle; processing delegates to
// MoveWindow; no value is returned.
void ResizeDriverObjectView(HWND view, const RECT& bounds);

// RefreshDriverObjectView repopulates the visible list from the attached model.
// Input is the object-info child HWND; processing rebuilds every row; no value
// is returned.
void RefreshDriverObjectView(HWND view);

// ExportDriverObjectViewTsv serializes the current object rows. Input is the
// object-info child HWND; processing reads the attached model and converts the
// visible snapshot to TSV; output is ready for the clipboard or file export.
std::wstring ExportDriverObjectViewTsv(HWND view);

} // namespace Ksword::Features::Driver
