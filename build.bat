@echo off
setlocal enabledelayedexpansion
REM ==========================================================================
REM  Prometheus Industrial HMI - build script
REM    build.bat            release build of both apps: industrial_hmi.exe (HMI runtime)
REM                         and plc_lab.exe (simulated PLCs to test it with)
REM    build.bat test       release build + unit tests (tests\test_core.exe)
REM    build.bat debug      unoptimised build with symbols
REM    build.bat snapshot   portable C99 tool that renders all screens to BMP files
REM    build.bat clean      remove build outputs
REM ==========================================================================
cd /d "%~dp0"

set LIB_SRC=src\core\sha256.c src\core\tags.c src\core\alarms.c src\core\alarm_log.c src\core\watchdog.c src\core\security.c src\core\crc32.c src\renderer\micro_renderer.c src\renderer\layout.c src\renderer\font_inter.c src\drivers\device_profiles.c src\drivers\profiles\profile_tank_skid.c src\drivers\profiles\profile_drive.c src\drivers\profiles\profile_conveyor.c src\drivers\profiles\profile_boiler.c src\drivers\profiles\profile_pump_station.c src\drivers\profiles\profile_compressor.c src\drivers\profiles\profile_ahu.c src\drivers\profiles\profile_abb_drive.c src\drivers\profiles\profile_generic.c src\drivers\plc_sim.c src\hal\net_ip.c src\hal\net_win32.c src\drivers\device_manager.c src\drivers\discovery.c src\drivers\modbus_proto.c src\drivers\driver_host.c src\hal\timer_win32.c src\hal\gesture.c src\hal\sys_win32.c src\hal\fs_stdio.c src\hal\thread_win32.c
set SRC=src\app.c src\platform\win32\main_win32.c %LIB_SRC%
set PORTABLE_SRC=src\core\tags.c src\core\alarms.c src\core\security.c src\core\sha256.c src\core\crc32.c src\core\watchdog.c src\renderer\layout.c src\renderer\micro_renderer.c src\renderer\font_inter.c src\drivers\device_profiles.c src\drivers\profiles\profile_tank_skid.c src\drivers\profiles\profile_drive.c src\drivers\profiles\profile_conveyor.c src\drivers\profiles\profile_boiler.c src\drivers\profiles\profile_pump_station.c src\drivers\profiles\profile_compressor.c src\drivers\profiles\profile_ahu.c src\drivers\profiles\profile_abb_drive.c src\drivers\profiles\profile_generic.c src\drivers\plc_sim.c
set GCC_WARN=-std=c99 -Wall -Wextra -Wshadow -Werror
set GCC_HARDEN=-fstack-protector-strong -static -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va
set GCC_LIBS=-luser32 -lgdi32 -lwinmm -lpsapi -lws2_32

for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd.HHmmss"') do set BUILD_ID=%%t

if /i "%~1"=="clean" (
    del /q industrial_hmi.exe plc_lab.exe tests\test_core.exe tests\test_core_mcu.exe tests\test_modbus_faults.exe tests\test_modbus_live.exe tools\render_snapshot.exe snapshot_*.bmp *.obj 2>nul
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
echo [GCC] building plc_lab.exe
gcc %GCC_WARN% -Os -s %GCC_HARDEN% lab\plc_lab_win32.c src\drivers\plc_sim.c -I include -luser32 -lgdi32 -lwinmm -lws2_32 -mwindows -o plc_lab.exe
if errorlevel 1 (
    echo [FAILED] plc_lab compile error
    exit /b 1
)
for %%I in (plc_lab.exe) do echo [OK] plc_lab.exe  %%~zI bytes
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
gcc %GCC_WARN% -O1 -I include tests\test_modbus_live.c src\hal\net_ip.c src\hal\net_win32.c src\drivers\device_manager.c src\drivers\discovery.c src\drivers\device_profiles.c src\drivers\profiles\profile_tank_skid.c src\drivers\profiles\profile_drive.c src\drivers\profiles\profile_conveyor.c src\drivers\profiles\profile_boiler.c src\drivers\profiles\profile_pump_station.c src\drivers\profiles\profile_compressor.c src\drivers\profiles\profile_ahu.c src\drivers\profiles\profile_abb_drive.c src\drivers\profiles\profile_generic.c src\drivers\plc_sim.c src\drivers\modbus_proto.c src\core\tags.c src\core\alarms.c src\core\crc32.c src\hal\timer_win32.c -lws2_32 -lwinmm -o tests\test_modbus_live.exe
if errorlevel 1 (
    echo [FAILED] live test build
    exit /b 1
)
gcc %GCC_WARN% -O1 -g -I include tests\test_modbus_faults.c src\hal\net_ip.c src\hal\net_win32.c src\drivers\device_manager.c src\drivers\device_profiles.c src\drivers\profiles\profile_tank_skid.c src\drivers\profiles\profile_drive.c src\drivers\profiles\profile_conveyor.c src\drivers\profiles\profile_boiler.c src\drivers\profiles\profile_pump_station.c src\drivers\profiles\profile_compressor.c src\drivers\profiles\profile_ahu.c src\drivers\profiles\profile_abb_drive.c src\drivers\profiles\profile_generic.c src\drivers\plc_sim.c src\drivers\modbus_proto.c src\core\tags.c src\core\alarms.c src\core\crc32.c src\hal\timer_win32.c -lws2_32 -lwinmm -o tests\test_modbus_faults.exe
if errorlevel 1 (
    echo [FAILED] fault-injection test build
    exit /b 1
)
.\tests\test_core.exe
if errorlevel 1 (
    echo [FAILED] unit tests - or Windows Smart App Control blocked the fresh test exe:
    echo          if no test output appeared above, run tests\test_core.exe again in a few seconds
    exit /b 1
)
echo [PASSED] unit tests
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
call :report
exit /b 0

:report
for %%I in (industrial_hmi.exe) do echo [OK] industrial_hmi.exe  %%~zI bytes  build !BUILD_ID!
exit /b 0
