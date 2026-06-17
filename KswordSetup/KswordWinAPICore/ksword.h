#ifndef KSWORD_HEAD_FILE
#define KSWORD_HEAD_FILE

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

#include "KLog.h"

#ifdef GetUserName
#undef GetUserName
#endif

// GetUserName returns the current Windows account name as a narrow string.
std::string GetUserName();

// GetHostName returns the current computer name as a narrow string.
std::string GetHostName();

// IsAdmin checks whether the current process token belongs to Administrators.
bool IsAdmin();

// GetSelfPath returns the absolute executable path for the current process.
std::string GetSelfPath();

// KEnsureAppUserModelID assigns the stable shell identity used by taskbar and
// Alt+Tab grouping. Input is none; output is true when the current Windows shell
// accepted the id; repeated calls return the cached one-shot result.
bool KEnsureAppUserModelID();

#endif
