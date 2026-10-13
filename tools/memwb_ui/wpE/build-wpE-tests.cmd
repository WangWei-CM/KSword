@echo off
chcp 65001 >nul
rem ============================================================
rem build-wpE-tests.cmd
rem 作用：构建并运行 WP-E（地址簿：AddressBookStore/Model/Panel）离屏验证夹具。
rem 仿 tools\memwb_ui\build-memwb-ui-tests.cmd 的形状，但只链接本包需要的源文件：
rem 不含 HexCanvas 工具链（地址簿面板不依赖它），不含任何 qrc/SVG（类型图标与
rem A/B 按钮均为自绘/纯文字，不需要图标资源）。HexViewWidgets.cpp（HexViewBarFrame/
rem HexViewGlyphButton）与 HexViewWidgets.Text.cpp 的 HexViewSegmented/HexViewMessageLabel
rem 不是一份文件，但 HexViewStatusBar 继承 HexViewBarFrame——即便本包用不到
rem HexViewStatusBar，moc 仍会为整份头文件生成代码，链接期仍要求基类构造函数有定义，
rem 所以两份 .cpp 都要链接。
rem 修复波新增：C12（i18n）要求模型/面板经 ks::i18n::sourceText/displayText 真正查语言包，
rem 本夹具因此要链接 Internationalization\LanguageManager.cpp 并断言 en-US 下表头是英文
rem （见 wpE_tests.Model.cpp 的 TestHeaderDataTranslatesInEnglish）。LanguageManager 不是
rem 本包新写的代码、没有 Q_OBJECT（不需要额外 moc），但主程序用 WarningLevel=Level3
rem （非 Level4/WX）编译它，这里仿 wpH 的做法单独一次 /W3 编译成 .obj，不让它既有的警告
rem 挡住本包自己代码的 /W4 /WX 强校验。
rem 第二轮修复（wave2）新增：wpE_tests.Wave2.*.cpp 五个文件——审核报告 review2-wpE.md 的
rem 补测（并入默认运行，不再靠环境变量开关）+ 本轮主会话自己补的新变异回归（D1/D4/D6/D7/
rem D8/D9/D10/D12）。不新增任何 moc/链接依赖（复用既有的三个 Q_OBJECT 类与 HexViewWidgets）。
rem 用法：在任意目录执行 tools\memwb_ui\wpE\build-wpE-tests.cmd
rem       可用环境变量 MEMWB_OUT 覆盖产物目录（默认 .codex-tmp\memwb-wpE），
rem       便于与其它工作包的夹具并行跑而不互相覆盖。
rem ============================================================
setlocal
set "REPO=%~dp0..\..\.."
pushd "%REPO%"
if errorlevel 1 exit /b 2

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" exit /b 2
call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%

set "QT=%CD%\.deps\Qt\6.9.3\msvc2022_64"
if not exist "%QT%\lib\Qt6Widgets.lib" set "QT=D:\Software\Qt\6.9.3\msvc2022_64"
if not exist "%QT%\lib\Qt6Widgets.lib" exit /b 2

set "APP=Ksword5.1\Ksword5.1"
set "CORE=shared\evidence\memory_workbench"
set "UI=%APP%\UI\MemoryWorkbench"
set "FLOW=%APP%\UI"
set "FIX=tools\memwb_ui\wpE"
rem MEMWB_OUT 可由调用方预先设置，让多个验证互不覆盖产物目录；默认 .codex-tmp\memwb-wpE。
if not defined MEMWB_OUT set "MEMWB_OUT=.codex-tmp\memwb-wpE"
set "OUT=%MEMWB_OUT%"
set "OBJ=%OUT%\obj"
set "OBJ2=%OUT%\obj2"
set "MOC=%OUT%\moc"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%OBJ2%" mkdir "%OBJ2%"
if not exist "%MOC%" mkdir "%MOC%"
if not exist "%OUT%\shots" mkdir "%OUT%\shots"

rem ---- moc：三个新类（均 Q_OBJECT）+ HexViewWidgets（HexViewSegmented 需要 moc）----
"%QT%\bin\moc.exe" "%UI%\AddressBookStore.h" -o "%MOC%\moc_AddressBookStore.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\AddressBookModel.h" -o "%MOC%\moc_AddressBookModel.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\AddressBookPanel.h" -o "%MOC%\moc_AddressBookPanel.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\HexViewWidgets.h" -o "%MOC%\moc_HexViewWidgets.cpp"
if errorlevel 1 exit /b %errorlevel%

rem ---- 第二遍：LanguageManager（不是本包新写的代码，没有 Q_OBJECT 不需要 moc；主程序
rem      用 Level3 编译它，这里单独 /W3 不开 /WX，避免它既有的警告挡住本包自己代码的强校验）----
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W3 /O2 /DWIN32_LEAN_AND_MEAN /external:W0 /DNOMINMAX /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB ^
  /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" ^
  /c "%APP%\Internationalization\LanguageManager.cpp" ^
  /Fo"%OBJ2%\\"
if errorlevel 1 exit /b %errorlevel%

rem ---- 第一遍：本包新代码 + 复用的既测文件，/W4 /WX 干净 ----
if not defined CL set "CL=/MP"
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /Gy /DWIN32_LEAN_AND_MEAN /external:W0 /DNOMINMAX /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB /DQT_TESTLIB_LIB /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" /external:I"%QT%\include\QtTest" ^
  "%FIX%\wpE_main.cpp" "%FIX%\wpE_common.cpp" "%FIX%\wpE_tests.Store.cpp" "%FIX%\wpE_tests.Model.cpp" "%FIX%\wpE_tests.Panel.cpp" ^
  "%FIX%\wpE_tests.Panel.Survivors.cpp" "%FIX%\wpE_tests.Panel.Defects.cpp" "%FIX%\wpE_tests.Shots.cpp" ^
  "%FIX%\wpE_tests.Wave2.Store.cpp" "%FIX%\wpE_tests.Wave2.Model.cpp" "%FIX%\wpE_tests.Wave2.Panel.cpp" ^
  "%FIX%\wpE_tests.Wave2.PanelMore.cpp" "%FIX%\wpE_tests.Wave2.NewMutations.cpp" "%FIX%\wpE_tests.Replay3.cpp" ^
  "%UI%\AddressBookStore.cpp" "%UI%\AddressBookModel.cpp" "%UI%\AddressBookModel.StoreSync.cpp" "%UI%\AddressBookPanel.cpp" "%UI%\AddressBookPanel.RowActions.cpp" "%UI%\AddressBookPanel.Menu.cpp" ^
  "%APP%\UI\TableContextMenu.cpp" ^
  "%UI%\HexViewWidgets.cpp" "%UI%\HexViewWidgets.Text.cpp" "%UI%\HexViewFormat.cpp" "%FLOW%\FlowLayout.cpp" ^
  "%MOC%\moc_AddressBookStore.cpp" "%MOC%\moc_AddressBookModel.cpp" "%MOC%\moc_AddressBookPanel.cpp" "%MOC%\moc_HexViewWidgets.cpp" ^
  "%CORE%\MemoryAddressBook.cpp" "%CORE%\MemoryAddressBook.Serialize.cpp" ^
  "shared\evidence\PointerChain.cpp" ^
  /Fo"%OBJ%\\" /Fe"%OUT%\wpE_tests.exe" ^
  /link /OPT:REF /LIBPATH:"%QT%\lib" Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib Qt6Test.lib user32.lib ^
  "%OBJ2%\LanguageManager.obj"
if errorlevel 1 exit /b %errorlevel%

rem ---- 部署 DLL 与插件（离屏平台；本包不用 SVG，不需要 Qt6Svg/qsvgicon/qsvg）----
for %%D in (Qt6Core Qt6Gui Qt6Widgets Qt6Test) do copy /y "%QT%\bin\%%D.dll" "%OUT%\" >nul
if not exist "%OUT%\plugins\platforms" mkdir "%OUT%\plugins\platforms"
if not exist "%OUT%\plugins\styles" mkdir "%OUT%\plugins\styles"
copy /y "%QT%\plugins\platforms\qoffscreen.dll" "%OUT%\plugins\platforms\" >nul
copy /y "%QT%\plugins\platforms\qwindows.dll" "%OUT%\plugins\platforms\" >nul
copy /y "%QT%\plugins\styles\*.dll" "%OUT%\plugins\styles\" >nul 2>nul

rem ---- 运行 ----
set "PATH=%OUT%;%QT%\bin;%PATH%"
set "QT_PLUGIN_PATH=%OUT%\plugins"
set "QT_QPA_PLATFORM=offscreen"
"%OUT%\wpE_tests.exe" --shots "%OUT%\shots" %*
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
