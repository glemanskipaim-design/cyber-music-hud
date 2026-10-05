@echo off
rem Uso:
rem   build.bat         compila build\CyberMusicHUD.exe
rem   build.bat test    compila e roda os testes de unidade (logic.h)
rem   build.bat e2e     compila e roda o teste ponta a ponta (mostra o HUD; nao injeta teclas)
rem   build.bat fake    compila o player de teste build\fake\Spotify.exe
rem   build.bat probe   compila e roda o diagnostico (sessoes de midia + 2 s de captura)
setlocal
cd /d "%~dp0"

rem Sem blocos ( ): o ")" de "Program Files (x86)" fecharia o bloco.
if defined VSCMD_VER goto :have_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :no_msvc
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
:have_msvc

if not exist build mkdir build
set "CFLAGS=/nologo /std:c++20 /O2 /MT /W4 /WX /EHsc /utf-8 /permissive- /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN"
set "LIBS=user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib d2d1.lib dwrite.lib shcore.lib mmdevapi.lib windowsapp.lib"
set "SRCS=src\main.cpp src\hud.cpp src\media.cpp src\audio.cpp src\procutil.cpp"

if /i "%~1"=="test" goto :test
if /i "%~1"=="fake" goto :fake

rc /nologo /fo build\app.res src\app.rc || exit /b 1
cl %CFLAGS% /Fo:build\ /Fe:build\CyberMusicHUD.exe %SRCS% build\app.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /DEPENDENTLOADFLAG:0x800 %LIBS% || exit /b 1
echo [build] ok: build\CyberMusicHUD.exe
if /i "%~1"=="e2e" goto :e2e
if /i not "%~1"=="probe" exit /b 0
build\CyberMusicHUD.exe --probe
exit /b %ERRORLEVEL%

:e2e
powershell -NoProfile -ExecutionPolicy Bypass -File tests\e2e.ps1
exit /b %ERRORLEVEL%

:test
cl %CFLAGS% /Fo:build\ /Fe:build\unit_tests.exe tests\unit_tests.cpp /link /SUBSYSTEM:CONSOLE || exit /b 1
build\unit_tests.exe
exit /b %ERRORLEVEL%

:fake
rem Player de teste (tests\fakeplayer.cpp). O nome Spotify.exe e proposital:
rem e assim que o HUD o reconhece. Fica isolado em build\fake.
if not exist build\fake mkdir build\fake
cl %CFLAGS% /Fo:build\ /Fe:build\fake\Spotify.exe tests\fakeplayer.cpp /link /SUBSYSTEM:CONSOLE windowsapp.lib || exit /b 1
echo [build] ok: build\fake\Spotify.exe
exit /b 0

:no_msvc
echo [build] MSVC nao encontrado. Instale o "Build Tools for Visual Studio" com C++.
exit /b 1
