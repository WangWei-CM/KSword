#pragma once
#include "../../imgui/imgui.h"
#include "../TextEditor/TextEditor.h"
#include <D3dx9tex.h>
#pragma comment(lib, "D3dx9")
extern inline void KswordLogo5			();
extern inline void RenderBoolRow		(const char* name, bool value);
extern inline void KswordGUIShowStatus	();
extern inline void KswordGUIInit		();
extern		  void StyleColorsRedBlack  (ImGuiStyle* dst = nullptr);

extern inline void KswordGUIProcess		();
extern        void KillProcessByTaskkill(int pid);

extern inline void KswordToolBar		();

extern inline void Ksword5Title         ();

       inline void TextEditorWindow     (TextEditor& editor,
                                         bool& isOpen,
                                         bool& showSaveDialog,
                                         bool& showOpenDialog,
                                         std::string& currentFilePath);

extern inline void PointerWindow();
extern int my_image_width ;
extern int my_image_height ;
extern PDIRECT3DTEXTURE9 my_texture ;
extern bool ret;
extern ImVec4 StyleColor;

#include "process/process.h"
#include "Monitor/MonitorMain.h"

#define STYLE_COLOR ImVec4(StyleColor.w, StyleColor.x, StyleColor.y, 0.00f)