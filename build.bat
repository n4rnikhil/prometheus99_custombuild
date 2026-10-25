@echo off
setlocal enabledelayedexpansion
REM ==========================================================================
REM  Prometheus Industrial HMI - build script
REM    build.bat            release build (GCC/MinGW preferred, MSVC fallback)
REM    build.bat test       release build + unit tests (tests\test_core.exe)
REM    build.bat debug      unoptimised build with symbols
REM    build.bat snapshot   portable C99 tool that renders all screens to BMP files
REM    build.bat clean      remove build outputs
REM ==========================================================================
cd /d "%~dp0"

set SRC=src\main.c src\core\sha256.c src\core\tags.c src\core\alarms.c src\core\alarm_log.c src\core\watchdog.c src\core\security.c src\core\crc32.c src\renderer\micro_renderer.c src\renderer\layout.c src\renderer\font_inter.c src\drivers\schneider_profile.c src\drivers\driver_modbus.c src\drivers\modbus_proto.c src\drivers\driver_host.c src\hal\timer.c src\hal\gesture.c src\hal\display_win32.c
set LIB_SRC=%SRC:src\main.c =%
set PORTABLE_SRC=src\core\tags.c src\core\alarms.c src\core\security.c src\core\sha256.c src\core\crc32.c src\core\watchdog.c src\renderer\layout.c src\renderer\micro_renderer.c src\renderer\font_inter.c src\drivers\schneider_profile.c
set GCC_WARN=-std=c99 -Wall -Wextra -Wshadow -Werror
set GCC_HARDEN=-fstack-protector-strong -static -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va
set GCC_LIBS=-luser32 -lgdi32 -lwinmm -lpsapi -lws2_32

for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd.HHmmss"') do set BUILD_ID=%%t

if /i "%~1"=="clean" (
    del /q industrial_hmi.exe tests\test_core.exe tests\test_modbus_live.exe tools\render_snapshot.exe snapshot_*.bmp *.obj 2>nul
    echo Cleaned.
    exit /b 0
)

where gcc >nul 2>nul
if errorlevel 1 goto :msvc
if /i "%~1"=="snapshot" goto :snapshot

echo [GCC] building industrial_hmi.exe
if /i "%~1"=="debug" (
    gcc %GCC_WARN% -O0 -g -DHMI_BUILD_ID=\"!BUILD_ID!-debug\" -I include %SRC% %GCC_LIBS% -mwindows -o industrial_hmi.exe
) else (
    gcc %GCC_WARN% -Os -s -ffunction-sections -fdata-sections -Wl,--gc-sections %GCC_HARDEN% -DHMI_BUILD_ID=\"!BUILD_ID!\" -I include %SRC% %GCC_LIBS% -mwindows -o industrial_hmi.exe
)
if errorlevel 1 (
    echo [FAILED] compile error
    exit /b 1
)
call :report
if /i "%~1"=="test" goto :test
exit /b 0

:test
echo.
echo [TEST] building unit tests
gcc %GCC_WARN% -O1 -g -fsanitize=undefined -fsanitize-trap=all -I include tests\test_core.c %LIB_SRC% %GCC_LIBS% -o tests\test_core.exe
if errorlevel 1 (
    echo [FAILED] test build
    exit /b 1
)
gcc %GCC_WARN% -O1 -I include tests\test_modbus_live.c src\drivers\driver_modbus.c src\drivers\schneider_profile.c src\drivers\modbus_proto.c src\core\tags.c src\hal\timer.c -lws2_32 -lwinmm -o tests\test_modbus_live.exe
if errorlevel 1 (
    echo [FAILED] live test build
    exit /b 1
)
.\tests\test_core.exe
if errorlevel 1 (
    echo [FAILED] unit tests
    exit /b 1
)
echo [PASSED] unit tests
exit /b 0

:snapshot
echo [GCC] portable snapshot tool (pure C99, no OS headers)
gcc %GCC_WARN% -pedantic -O2 -I include tools\render_snapshot.c %PORTABLE_SRC% -o tools\render_snapshot.exe
if errorlevel 1 (
    echo [FAILED] snapshot build
    exit /b 1
)
.\tools\render_snapshot.exe 1024 600 snapshot
exit /b %errorlevel%

:msvc
where cl >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Neither gcc nor cl.exe found. Install MinGW-w64 or run from a VS Developer Prompt.
    exit /b 1
)
echo [MSVC] building industrial_hmi.exe
cl /nologo /O1 /W3 /GS /guard:cf /DHMI_BUILD_ID=\"!BUILD_ID!\" /I include %SRC% /Fe:industrial_hmi.exe /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /GUARD:CF user32.lib gdi32.lib winmm.lib psapi.lib ws2_32.lib
if errorlevel 1 (
    echo [FAILED] compile error
    exit /b 1
)
del /q *.obj 2>nul
call :report
exit /b 0

:report
for %%I in (industrial_hmi.exe) do echo [OK] industrial_hmi.exe  %%~zI bytes  build !BUILD_ID!
exit /b 0
