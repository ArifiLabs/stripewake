@echo off
rem Builds build\Stripewake.exe (x64, no C runtime) and, if Inno Setup is present, the installer.
rem Run from any prompt: it finds MSVC through vswhere when cl is not on PATH.
setlocal
cd /d "%~dp0"
where cl >nul 2>nul && goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo MSVC not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:build
if not exist build mkdir build
cl /nologo /c /O1 /Os /Oi /GS- /Gs1000000 /W4 /WX /Zl /Fobuild\awake.obj src\awake.c || exit /b 1
rc /nologo /fo build\awake.res src\awake.rc || exit /b 1
link /nologo /NODEFAULTLIB /ENTRY:WinMainCRTStartup /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:NO /RELEASE ^
  /DYNAMICBASE /NXCOMPAT /OUT:build\Stripewake.exe build\awake.obj build\awake.res ^
  kernel32.lib user32.lib gdi32.lib shell32.lib advapi32.lib comctl32.lib powrprof.lib || exit /b 1
echo built build\Stripewake.exe
set "ISCC="
for %%p in ("%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe") do if exist %%p set "ISCC=%%~p"
if defined ISCC ("%ISCC%" /Q installer\Stripewake.iss || exit /b 1) else echo Inno Setup not found: installer skipped.
