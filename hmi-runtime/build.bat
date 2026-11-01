@echo off
setlocal enabledelayedexpansion
REM ==========================================================================
REM  Prometheus HMI Runtime - Windows build (MinGW-w64 gcc, or MSVC cl as a fallback)
REM    build.bat            release build: industrial_hmi.exe
REM    build.bat test       release build + unit tests + MCU-profile tests + Modbus fault tests
REM    build.bat debug      unoptimised build with symbols
REM    build.bat snapshot   portable C99 tool that renders every screen to BMP files
REM    build.bat clean      remove build outputs
REM  The PLC Simulator is a separate app: ..\plc-simulator\build.bat
REM ==========================================================================
cd /d "%~dp0"

set CORE=src\core\sha256.c src\core\tags.c src\core\alarms.c src\core\alarm_log.c src\core\watchdog.c src\core\security.c src\core\crc32.c src\core\device_key.c
set RENDER=src\renderer\micro_renderer.c src\renderer\layout.c src\renderer\font_inter.c
set DEVICES=src\devices\device_registry.c src\devices\altivar_drive.c src\devices\em6400_meter.c src\devices\tank_skid.c src\devices\conveyor.c src\devices\boiler.c src\devices\pump_station.c src\devices\compressor.c src\devices\air_handler.c src\devices\abb_drive.c src\devices\generic.c
set FIELDBUS=src\fieldbus\device_manager.c src\fieldbus\discovery.c src\fieldbus\modbus_proto.c src\fieldbus\driver_host.c
set HAL=src\hal\net_ip.c src\hal\net_win32.c src\hal\timer_win32.c src\hal\gesture.c src\hal\sys_win32.c src\hal\fs_stdio.c src\hal\thread_win32.c src\hal\utc_format.c
set LIB_SRC=%CORE% %RENDER% %DEVICES% %FIELDBUS% %HAL%
set SRC=src\app\app.c src\platform\win32\main_win32.c %LIB_SRC%
set MODBUS_SRC=src\fieldbus\device_manager.c src\fieldbus\discovery.c src\fieldbus\modbus_proto.c
set NET_TEST_SRC=src\hal\net_ip.c src\hal\net_win32.c src\hal\timer_win32.c %MODBUS_SRC% %DEVICES% src\core\tags.c src\core\alarms.c src\core\crc32.c
set PORTABLE_SRC=src\core\tags.c src\core\alarms.c src\core\security.c src\core\sha256.c src\core\crc32.c src\core\watchdog.c %RENDER% %DEVICES%

set GCC_WARN=-std=c99 -Wall -Wextra -Wshadow -Werror
set GCC_HARDEN=-fstack-protector-strong -static -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va
set GCC_LIBS=-luser32 -lgdi32 -lwinmm -lpsapi -lws2_32

for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd.HHmmss"') do set BUILD_ID=%%t

if /i "%~1"=="clean" (
    del /q industrial_hmi.exe tests\*.exe tools\*.exe snapshot_*.bmp *.obj 2>nul
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
for %%I in (industrial_hmi.exe) do echo [OK] industrial_hmi.exe  %%~zI bytes  build !BUILD_ID!
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
gcc %GCC_WARN% -O1 -I include tests\test_modbus_live.c %NET_TEST_SRC% -lws2_32 -lwinmm -o tests\test_modbus_live.exe
if errorlevel 1 (
    echo [FAILED] live test build
    exit /b 1
)
gcc %GCC_WARN% -O1 -g -I include tests\test_modbus_faults.c %NET_TEST_SRC% -lws2_32 -lwinmm -o tests\test_modbus_faults.exe
if errorlevel 1 (
    echo [FAILED] fault-injection test build
    exit /b 1
)
REM Integration: the runtime against the PLC Simulator's device models (test-only link to ..\plc-simulator)
gcc %GCC_WARN% -O1 -g -fsanitize=undefined -fsanitize-trap=all -I include -I ..\plc-simulator\include tests\test_integration.c ..\plc-simulator\src\plc_models.c %NET_TEST_SRC% -lws2_32 -lwinmm -o tests\test_integration.exe
if errorlevel 1 (
    echo [FAILED] integration test build
    exit /b 1
)
.\tests\test_core.exe
if errorlevel 1 (
    echo [FAILED] unit tests - or Windows Smart App Control blocked the fresh test exe:
    echo          if no test output appeared above, run tests\test_core.exe again in a few seconds
    exit /b 1
)
echo [PASSED] unit tests
.\tests\test_integration.exe
if errorlevel 1 (
    echo [FAILED] integration tests - the runtime against the simulator models
    exit /b 1
)
echo [PASSED] integration tests
gcc %GCC_WARN% -O1 -g -fsanitize=undefined -fsanitize-trap=all -DHMI_PROFILE_MCU -I include tests\test_core.c %LIB_SRC% %GCC_LIBS% -o tests\test_core_mcu.exe
if errorlevel 1 (
    echo [FAILED] MCU-profile test build
    exit /b 1
)
.\tests\test_core_mcu.exe
if errorlevel 1 (
    echo [FAILED] unit tests with the MCU profile - HMI_PROFILE_MCU
    exit /b 1
)
echo [PASSED] unit tests, MCU profile
.\tests\test_modbus_faults.exe
if errorlevel 1 (
    echo [FAILED] Modbus fault-injection tests - or Smart App Control blocked the fresh test exe
    exit /b 1
)
echo [PASSED] Modbus fault-injection tests
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
for %%I in (industrial_hmi.exe) do echo [OK] industrial_hmi.exe  %%~zI bytes  build !BUILD_ID!
exit /b 0
