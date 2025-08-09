#pragma once
//#include <d3d9.h>

#include "../support/ksword.h"
#ifdef KSWORD_WITH_COMMAND
#include "../main/Security/security.h"
#endif

#include "../environment/Environment.h"
#ifdef KSWORD_WITH_COMMAND
#include "../main/Socket/KswordSelfSocket.h"
#include "../CUImanage/KswordCUIManager.h"
#include "../Main/SupportFunction/MainSupportFunction.h"
#include "../Main/ShellFunction/Command-apt/Command-apt.h"
#include "../Main/ShellFunction/Command-cd/Command-cd.h"
#include "../Main/ShellFunction/Command-guimgr/Command-guimgr.h"
#include "../Main/ShellFunction/Command-help/Command-help.h"
#include "../Main/ShellFunction/Command-procmgr/Command-procmgr.h"
#include "../Main/ShellFunction/Command-drivermgr/Command-drivermgr.h"
#include "../Main/ShellFunction/Command-handlemgr/Command-handlemgr.h"
#include "../KswordSelfManager.h"
#include "../Main/ShellFunction/Command-avkill/Command-avkill.h"
#include "../Main/ShellFunction/Command-netmgr/Command-netmgr.h"
#include "../Main/ShellFunction/Command-threadmgr/Command-threadmgr.h"
#include "../KswordDriver/KswordDriver.h"
#include "../AI/AiAPI.h"
#include "../resource.h"
#endif
#include "GUIFunction/GUIFunction.h"

#include "../imgui/imconfig.h"
#include "../imgui/imgui.h"
#include "../imgui/imgui_impl_dx9.h"
#include "../imgui/imgui_impl_win32.h"
#include "../imgui/imgui_internal.h"
#include "../imgui/imstb_rectpack.h"
#include "../imgui/imstb_textedit.h"
#include "../imgui/imstb_truetype.h"

#include "global_module_helper.hpp"
#include "Ksword5core.h"

#include <d3d9.h>
#include "Ksword5core.h"
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx9.lib")  // 链接静态库而非动态库
#define C(str) GBKtoUTF8(str).c_str()

#include "../resource.h"

extern bool KswordShowLogWindow;
extern bool KswordShowPointerWindow;
extern bool KswordShowNotpadWindow;
extern bool KswordShowToolBar;

extern bool Ksword_main_should_exit;

#include "SubProc/SubProc.h"
