@echo off
rem facet build - MSVC (VS2022), static CRT, /W4.
rem Produces facet.exe (console: every mode) and facetw.exe (windows subsystem: the same
rem binary without a console flash - double-click or pin it to the taskbar).
setlocal enabledelayedexpansion
where cl >nul 2>nul
if errorlevel 1 (
  set "VCV=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  if not exist "!VCV!" (
    set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    for /f "usebackq tokens=*" %%i in (`""!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`) do set "VSDIR=%%i"
    set "VCV=!VSDIR!\VC\Auxiliary\Build\vcvars64.bat"
  )
  if not exist "!VCV!" ( echo build: vcvars64.bat not found & exit /b 1 )
  call "!VCV!" >nul
)
set CXXFLAGS=/nologo /c /std:c++20 /O2 /W4 /permissive- /EHsc /utf-8 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS
set LIBS=user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib uuid.lib advapi32.lib
cl %CXXFLAGS% facet.cpp facet_gui.cpp es_client.cpp facets.cpp || exit /b 1
rc /nologo /fo facet.res facet.rc || exit /b 1
link /nologo /SUBSYSTEM:CONSOLE /OUT:facet.exe facet.obj facet_gui.obj es_client.obj facets.obj facet.res %LIBS% || exit /b 1
link /nologo /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup /OUT:facetw.exe facet.obj facet_gui.obj es_client.obj facets.obj facet.res %LIBS% || exit /b 1
echo OK: facet.exe facetw.exe
