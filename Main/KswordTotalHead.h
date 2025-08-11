#pragma once
//#include <d3d9.h>

#include "../support/ksword.h"


#include "../environment/Environment.h"

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
