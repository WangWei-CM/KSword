@echo off
setlocal
set "KSWORD_CHART_TEST_ROOT=%~dp0..\.."
set "KSWORD_CHART_TEST_OUT=%KSWORD_CHART_TEST_ROOT%\.codex-build-logs\metric-chart-binding-tests"
if not "%~1"=="" set "KSWORD_CHART_TEST_OUT=%~f1"
if not exist "%KSWORD_CHART_TEST_OUT%" mkdir "%KSWORD_CHART_TEST_OUT%"
set "KSWORD_CHART_TEST_VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%KSWORD_CHART_TEST_VCVARS%" set "KSWORD_CHART_TEST_VCVARS=D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%KSWORD_CHART_TEST_VCVARS%" exit /b 2
set "KSWORD_CHART_TEST_QT=%KSWORD_QT_DIR%"
if not exist "%KSWORD_CHART_TEST_QT%\lib\Qt6Widgets.lib" set "KSWORD_CHART_TEST_QT=%KSWORD_CHART_TEST_ROOT%\.deps\Qt\6.9.3\msvc2022_64"
if not exist "%KSWORD_CHART_TEST_QT%\lib\Qt6Widgets.lib" set "KSWORD_CHART_TEST_QT=D:\Software\Qt\6.9.3\msvc2022_64"
if not exist "%KSWORD_CHART_TEST_QT%\lib\Qt6Widgets.lib" exit /b 2
call "%KSWORD_CHART_TEST_VCVARS%"
if errorlevel 1 exit /b %errorlevel%
pushd "%KSWORD_CHART_TEST_OUT%"
cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /MD /Zc:__cplusplus /permissive- /DNOMINMAX /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB /external:W0 /external:I"%KSWORD_CHART_TEST_QT%\include" /external:I"%KSWORD_CHART_TEST_QT%\include\QtCore" /external:I"%KSWORD_CHART_TEST_QT%\include\QtGui" /external:I"%KSWORD_CHART_TEST_QT%\include\QtWidgets" "%KSWORD_CHART_TEST_ROOT%\tools\performance\metric_chart_binding_tests.cpp" "%KSWORD_CHART_TEST_ROOT%\shared\ui\MetricHistory.cpp" "%KSWORD_CHART_TEST_ROOT%\shared\ui\MetricChartBinding.cpp" "%KSWORD_CHART_TEST_ROOT%\shared\ui\KsPainterChart.cpp" /Fe:metric_chart_binding_tests.exe /link /LIBPATH:"%KSWORD_CHART_TEST_QT%\lib" Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib
set "KSWORD_CHART_TEST_RESULT=%errorlevel%"
if not "%KSWORD_CHART_TEST_RESULT%"=="0" goto finish
set "PATH=%KSWORD_CHART_TEST_QT%\bin;%PATH%"
metric_chart_binding_tests.exe
set "KSWORD_CHART_TEST_RESULT=%errorlevel%"
:finish
popd
exit /b %KSWORD_CHART_TEST_RESULT%
