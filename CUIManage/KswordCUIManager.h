#ifdef KSWORD_WITH_COMMAND
#pragma once
#ifndef KSWORD_CUI_MANAGER_HEAD
#define KSWORD_CUI_MANAGER_HEAD

#include "KswordTotalHead.h"
#define FONT_SIZE_WIDTH 8
#define FONT_SIZE_HEIGHT 16
#define K_DEFAULT_SPACE 20
extern int LeftColumnStartLocationX;
extern int LeftColumnStartLocationY;
extern int RightColumnStartLocationX;
extern int RightColumnStartLocationY;
extern int ColumnWidth;
extern int ColumnHeight;
extern int fontWidth;
extern int fontHeight;
extern int MainWindow2StartLocationY;
extern void CalcWindowStyle();

extern void SetWindowNormal();
extern void SetWindowKStyle();


#endif // !KSWORD_CUI_MANAGER_HEAD
#endif