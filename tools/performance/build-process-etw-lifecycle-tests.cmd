@echo off
setlocal
set "KSWORD_ETW_TEST_ROOT=%~dp0..\.."
set "KSWORD_ETW_TEST_VS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
if not exist "%KSWORD_ETW_TEST_VS%" set "KSWORD_ETW_TEST_VS=D:\Software\VS\Common7\Tools\VsDevCmd.bat"
if not exist "%KSWORD_ETW_TEST_VS%" (
    echo x64 MSVC build tools are required.
    exit /b 1
)
call "%KSWORD_ETW_TEST_VS%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
set "KSWORD_ETW_TEST_OUT=%KSWORD_ETW_TEST_ROOT%\.codex-build-logs\process-etw-lifecycle-tests"
if not exist "%KSWORD_ETW_TEST_OUT%" mkdir "%KSWORD_ETW_TEST_OUT%"
cl.exe /nologo /std:c++17 /Zc:__cplusplus /permissive- /EHsc /W4 /WX /O2 /MD /utf-8 "%~dp0process_etw_lifecycle_tests.cpp" /Fo"%KSWORD_ETW_TEST_OUT%\tests.obj" /Fe"%KSWORD_ETW_TEST_OUT%\tests.exe"
if errorlevel 1 exit /b %errorlevel%
"%KSWORD_ETW_TEST_OUT%\tests.exe"
exit /b %errorlevel%
