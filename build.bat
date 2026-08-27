@echo off
setlocal

where cl.exe >nul 2>nul
if errorlevel 1 (
    echo Microsoft C++ Build Tools were not found.
    echo Run this file from an "x64 Native Tools Command Prompt for VS 2022".
    exit /b 1
)

if not exist build mkdir build
pushd build

rc.exe /nologo /I ..\src /fo GammaChanger.res ..\src\GammaChanger.rc
if errorlevel 1 goto :failed

cl.exe /nologo /std:c++17 /O2 /MT /EHsc /W4 /permissive- /utf-8 ^
    /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /I ..\src ^
    ..\src\main.cpp ..\src\gamma_math.cpp GammaChanger.res ^
    /Fe:GammaChanger.exe ^
    /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT ^
    advapi32.lib comctl32.lib gdi32.lib shell32.lib user32.lib wtsapi32.lib
if errorlevel 1 goto :failed

echo.
echo Built: %CD%\GammaChanger.exe
popd
exit /b 0

:failed
popd
echo Build failed.
exit /b 1
