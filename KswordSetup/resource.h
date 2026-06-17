#ifndef KSWORDFRAME_RESOURCE_H
#define KSWORDFRAME_RESOURCE_H

// Centralized Windows resource identifiers for application branding.
// Inputs are replaceable project-local file names; the .rc file embeds them,
// and KTitleBar.cpp reuses the same macros when it needs a disk fallback.
// Keep the executable icon at the lowest numeric resource id. Explorer and
// shell icon extraction APIs commonly choose the first/lowest RT_GROUP_ICON
// when showing an .exe file in File Explorer.
#define IDI_KSWORD_APP_ICON        1
#define IDR_KSWORD_APP_ICON_ICO    201
#define IDR_KSWORD_APP_LOGO_PNG    202
#define IDR_KSWORD_SETUP_CHARACTER_PNG 203

#define KSWORD_APP_ICON_FILE       "..\\Ksword5.1\\Ksword5.1\\Resource\\Logo\\KswordLogo.ico"
#define KSWORD_APP_LOGO_FILE       "Resources\\app_logo.png"
#define KSWORD_SETUP_CHARACTER_FILE "F27364739B6A229C3EA888E3C9DE85F9.png"

#endif // KSWORDFRAME_RESOURCE_H
