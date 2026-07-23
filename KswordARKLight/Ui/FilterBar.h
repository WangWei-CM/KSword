#pragma once

#include "../Core/Win32Lean.h"

#include <string>

namespace Ksword::Ui {

// CreateFilterBar creates a self-contained label/edit/clear toolbar. The
// parent receives WM_COMMAND with EN_CHANGE after 200 ms of quiet input, using
// the supplied id. It owns its child controls and needs no explicit destroy.
HWND CreateFilterBar(HWND parent, int id, const std::wstring& cueText, int x, int y, int width, int height);

// GetFilterBarText returns the current filter query without leading/trailing
// whitespace. It is safe to call from the parent command handler.
std::wstring GetFilterBarText(HWND filterBar);

// SetFilterBarText replaces the text. notifyParent controls whether a debounced
// EN_CHANGE notification is generated.
void SetFilterBarText(HWND filterBar, const std::wstring& text, bool notifyParent = true);

// FocusFilterBar moves keyboard focus to its edit box.
void FocusFilterBar(HWND filterBar);

} // namespace Ksword::Ui
