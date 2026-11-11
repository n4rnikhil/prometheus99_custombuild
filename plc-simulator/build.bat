@echo off
setlocal
REM ==========================================================================
REM  PLC Simulator - Windows build (MinGW-w64 gcc). Needs nothing from the HMI runtime.
REM    build.bat          plc_simulator.exe (window) and plc_simulator_cli.exe (console / headless)
REM    build.bat test     + the device-model tests (tests\test_plc_models.exe)
REM    build.bat clean
REM ==========================================================================
cd /d "%~dp0"

set CORE=src\plc_models.c src\modbus_server.c
set WARN=-std=c99 -Wall -Wextra -Wshadow -Werror
set HARDEN=-fstack-protector-strong -static -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va

if /i "%~1"=="clean" (
    del /q plc_simulator.exe plc_simulator_cli.exe tests\*.exe 2>nul
    echo Cleaned.
    exit /b 0
)
where gcc >nul 2>nul
if errorlevel 1 (
    echo [ERROR] gcc not found. Install MinGW-w64.
    exit /b 1
)

echo [GCC] building plc_simulator.exe
REM Icon and version information (optional: skipped when windres is not installed)
set RES=
where windres >nul 2>nul
if not errorlevel 1 (
    windres res\plc_simulator.rc -O coff -o res\plc_simulator.res
    if not errorlevel 1 set RES=res\plc_simulator.res
)
gcc %WARN% -Os -s %HARDEN% -I include src\main_win32.c %CORE% %RES% -luser32 -lgdi32 -lwinmm -lws2_32 -mwindows -o plc_simulator.exe
if errorlevel 1 (
    echo [FAILED] plc_simulator compile error
    exit /b 1
)
for %%I in (plc_simulator.exe) do echo [OK] plc_simulator.exe  %%~zI bytes

echo [GCC] building plc_simulator_cli.exe
gcc %WARN% -Os -s %HARDEN% -I include src\main_console.c %CORE% -lws2_32 -o plc_simulator_cli.exe
if errorlevel 1 (
    echo [FAILED] plc_simulator_cli compile error
    exit /b 1
)
for %%I in (plc_simulator_cli.exe) do echo [OK] plc_simulator_cli.exe  %%~zI bytes
if /i not "%~1"=="test" exit /b 0

echo.
echo [TEST] building device-model tests
gcc %WARN% -O1 -g -fsanitize=undefined -fsanitize-trap=all -I include tests\test_plc_models.c src\plc_models.c -o tests\test_plc_models.exe
if errorlevel 1 (
    echo [FAILED] test build
    exit /b 1
)
.\tests\test_plc_models.exe
if errorlevel 1 (
    echo [FAILED] device-model tests
    exit /b 1
)
echo [PASSED] device-model tests
exit /b 0
