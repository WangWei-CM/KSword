#pragma once

#include "../Core/Win32Lean.h"

#include <commctrl.h>
#include <string>

namespace Ksword::Ui {

// CreateIconImageList creates a color image list for small control icons. Inputs
// are icon size, initial capacity and grow count; output is HIMAGELIST or null.
HIMAGELIST CreateIconImageList(int iconWidth = 16, int iconHeight = 16, int initialCount = 8, int growCount = 8);

// LoadResourceIcon loads an icon from the module resources. Inputs are module,
// resource id and size; output is HICON or null. The caller owns the icon.
HICON LoadResourceIcon(HINSTANCE instance, int resourceId, int width = 16, int height = 16);

// LoadFileIcon extracts the shell icon for a path. Inputs are a filesystem path
// and large/small choice; output is HICON or null. The caller owns the icon.
HICON LoadFileIcon(const std::wstring& path, bool largeIcon = false);

// AddIconToImageList appends an icon and optionally destroys it afterwards.
// Inputs are image list and icon; output is the image index or -1 on failure.
int AddIconToImageList(HIMAGELIST imageList, HICON icon, bool destroyIcon = false);

// DestroyIconIfNeeded destroys a non-null icon. Input is HICON; no return.
void DestroyIconIfNeeded(HICON icon);

} // namespace Ksword::Ui
