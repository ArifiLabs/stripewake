@echo off
rem Builds the Store package build\Stripewake-<version>-x64.msix (UNSIGNED: Microsoft signs it after certification).
rem Needs: build.cmd's tools, Python + Pillow, and the Windows SDK (MakeAppx, MakePri).
setlocal
cd /d "%~dp0.."
call .\build.cmd || exit /b 1
set "SDK="
for /f "delims=" %%d in ('dir /b /ad /o-n "%ProgramFiles(x86)%\Windows Kits\10\bin\10.*" 2^>nul') do (
  if not defined SDK if exist "%ProgramFiles(x86)%\Windows Kits\10\bin\%%d\x64\makeappx.exe" set "SDK=%ProgramFiles(x86)%\Windows Kits\10\bin\%%d\x64"
)
if not defined SDK (echo Windows SDK MakeAppx not found & exit /b 1)
set "VER="
for /f "tokens=2 delims==" %%v in ('findstr /r /c:"Version=\"[0-9.]*\"" msix\AppxManifest.xml') do if not defined VER set "VER=%%~v"
set "VER=%VER:"=%"
set "VER=%VER: =%"
set "VER=%VER:/>=%"
set "L=build\msix"
if exist "%L%" rmdir /s /q "%L%"
mkdir "%L%" || exit /b 1
copy /y build\Stripewake.exe "%L%\" >nul || exit /b 1
copy /y LICENSE "%L%\LICENSE.txt" >nul || exit /b 1
copy /y msix\AppxManifest.xml "%L%\" >nul || exit /b 1
python msix\make_logos.py "%L%" || exit /b 1
"%SDK%\makepri.exe" createconfig /cf build\priconfig.xml /dq en-US /o >nul || exit /b 1
"%SDK%\makepri.exe" new /pr "%L%" /cf build\priconfig.xml /mn "%L%\AppxManifest.xml" /of "%L%\resources.pri" /o >nul || exit /b 1
"%SDK%\makeappx.exe" pack /d "%L%" /p "build\Stripewake-%VER%-x64.msix" /o || exit /b 1
echo built build\Stripewake-%VER%-x64.msix
