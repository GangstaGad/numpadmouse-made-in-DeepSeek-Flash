@echo off
setlocal
cd /d "%~dp0"

where cl >nul 2>nul && goto build

set "VS="
for /d %%d in ("C:\Program Files\Microsoft Visual Studio\*") do (
    for /d %%e in ("%%~d\*") do (
        if exist "%%~e\VC\Auxiliary\Build\vcvars64.bat" set "VS=%%~e"
    )
)

if not defined VS (
    echo [!] MSVC не найден. Нужен Visual Studio / Build Tools с компонентом "C++".
    exit /b 1
)

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [!] Не удалось инициализировать окружение MSVC.
    exit /b 1
)

:build
cl /nologo /utf-8 /O2 /MT /W3 /DUNICODE /D_UNICODE numpadmouse.c ^
   /link /SUBSYSTEM:WINDOWS /OUT:numpadmouse.exe ^
   user32.lib gdi32.lib advapi32.lib shell32.lib
if errorlevel 1 (
    echo [!] Сборка не удалась.
    exit /b 1
)
echo.
echo OK: %~dp0numpadmouse.exe
