@echo off
setlocal
set "KSWORD_LOG_TEST_ROOT=%~dp0..\.."
set "KSWORD_LOG_TEST_OUT=%KSWORD_LOG_TEST_ROOT%\.codex-build-logs\log-archive-tests"
if not "%~1"=="" set "KSWORD_LOG_TEST_OUT=%~f1"
if not exist "%KSWORD_LOG_TEST_OUT%" mkdir "%KSWORD_LOG_TEST_OUT%"
set "KSWORD_LOG_VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%KSWORD_LOG_VCVARS%" set "KSWORD_LOG_VCVARS=D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%KSWORD_LOG_VCVARS%" exit /b 2
call "%KSWORD_LOG_VCVARS%"
if errorlevel 1 exit /b %errorlevel%
pushd "%KSWORD_LOG_TEST_OUT%"
cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /MD /Zc:__cplusplus "%KSWORD_LOG_TEST_ROOT%\tools\performance\log_archive_tests.cpp" "%KSWORD_LOG_TEST_ROOT%\Ksword5.1\Ksword5.1\ksword\log\log.cpp" /Fe:log_archive_tests.exe /link Ole32.lib
set "KSWORD_LOG_TEST_RESULT=%errorlevel%"
if not "%KSWORD_LOG_TEST_RESULT%"=="0" goto finish
log_archive_tests.exe
set "KSWORD_LOG_TEST_RESULT=%errorlevel%"
:finish
popd
exit /b %KSWORD_LOG_TEST_RESULT%
