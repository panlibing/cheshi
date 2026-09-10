@echo off
rem Build portrelay.exe with MSVC (VS2022). Run from anywhere.
setlocal
cd /d "%~dp0"
set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo [ERROR] vcvars64.bat not found: %VCVARS%
  echo         Run this script from "Developer Command Prompt for VS 2022" instead.
  exit /b 1
)

call "%VCVARS%" >nul
cl /nologo /O2 /std:c++17 /EHsc /utf-8 /W3 portrelay.cpp ws2_32.lib /Fe:portrelay.exe
if errorlevel 1 (
  echo [ERROR] build failed
  exit /b 1
)
echo [OK] generated portrelay.exe
