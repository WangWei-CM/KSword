#pragma once

#include "../../Core/Win32Lean.h"

#include "DriverModel.h"

#include <string>

namespace Ksword::Features::Driver {

// CreateDriverOverviewView creates the read-only overview child page. Inputs
// are the parent HWND, placement rectangle, and a model pointer owned by the
// root Driver feature page; processing registers the window class and creates
// a report list; output is the child HWND or nullptr on failure.
HWND CreateDriverOverviewView(HWND parent, const RECT& bounds, DriverModel* model);

// ResizeDriverOverviewView moves the overview child page. Inputs are an
// existing child HWND and the new client rectangle; processing delegates to
// MoveWindow; no value is returned.
void ResizeDriverOverviewView(HWND view, const RECT& bounds);

// RefreshDriverOverviewView repopulates the visible list from the attached
// model. Input is the overview child HWND; processing rebuilds every row; no
// value is returned.
void RefreshDriverOverviewView(HWND view);

// ExportDriverOverviewViewTsv serializes the current overview rows. Input is
// the overview child HWND; processing reads the attached model and converts the
// visible snapshot to TSV; output is ready for the clipboard or file export.
std::wstring ExportDriverOverviewViewTsv(HWND view);

} // namespace Ksword::Features::Driver
