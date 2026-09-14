@echo off
rem Build tests\echo.exe with MSVC. Run from anywhere.
rem MSVC environment is located automatically (cl.exe on PATH -> vswhere -> usual
rem install paths), so this script works on any machine and directly in CI.
setlocal EnableDelayedExpansion
cd /d "%~dp0"
set "PF=%ProgramFiles%"
set "PF86=%ProgramFiles(x86)%"
set "VCVARS="

where cl.exe >nul 2>nul
if not errorlevel 1 goto :compile

set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=%PF%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "!VSWHERE!" (
  for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
  )
)

if not defined VCVARS (
  for %%d in ("!PF!" "!PF86!") do (
    for %%y in (2022 2019) do (
      for %%e in (Community Professional Enterprise BuildTools) do (
        if exist "%%~d\Microsoft Visual Studio\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat" (
          set "VCVARS=%%~d\Microsoft Visual Studio\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat"
        )
      )
    )
  )
)

if not defined VCVARS (
  echo [ERROR] MSVC environment not found ^(vcvars64.bat^).
  echo         Install Visual Studio 2022 with the "Desktop development with C++"
  echo         workload, or run this script from a "Developer Command Prompt for VS".
  exit /b 1
)

call "!VCVARS!" >nul

:compile
cl /nologo /O2 /std:c++17 /EHsc /utf-8 /W3 echo.cpp ws2_32.lib /Fe:echo.exe
if errorlevel 1 (
  echo [ERROR] build echo.exe failed
  exit /b 1
)
echo [OK] generated echo.exe

cl /nologo /O2 /std:c++17 /EHsc /utf-8 /W3 probe.cpp ws2_32.lib /Fe:probe.exe
if errorlevel 1 (
  echo [ERROR] build probe.exe failed
  exit /b 1
)
echo [OK] generated probe.exe

rem unit_tests.cpp 通过 #include 复用主源码(PORTRELAY_NO_MAIN), MSVC 下无需额外豁免
cl /nologo /O2 /std:c++17 /EHsc /utf-8 /W3 unit_tests.cpp ws2_32.lib /Fe:unit_tests.exe
if errorlevel 1 (
  echo [ERROR] build unit_tests.exe failed
  exit /b 1
)
echo [OK] generated unit_tests.exe
