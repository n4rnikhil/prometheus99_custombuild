@echo off
setlocal
REM ==========================================================================
REM  Plug-and-play demo: simulated PLCs are "plugged in" one by one and the
REM  HMI discovers them, picks the right driver module and lays out the screen.
REM  Needs Python 3 (standard library only) for tests\modbus_plc_sim.py.
REM ==========================================================================
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Python 3 is needed for the PLC simulators. Without it, run: industrial_hmi.exe --sim
    exit /b 1
)
if not exist industrial_hmi.exe (
    echo [ERROR] industrial_hmi.exe not found - run build.bat first
    exit /b 1
)

start "" .\industrial_hmi.exe
echo The HMI is open and searching for PLCs.
echo.
echo Press a key to PLUG IN a Modicon M340 tank-skid PLC ...
pause >nul
start "PLC 1 - Modicon M340 tank skid (close this window to unplug)" python tests\modbus_plc_sim.py --profile tank --port 1502
echo   -^> it appears on the dashboard within ~3 s.
echo.
echo Press a key to PLUG IN an Altivar ATV320 drive ...
pause >nul
start "PLC 2 - Altivar drive (close this window to unplug)" python tests\modbus_plc_sim.py --profile drive --port 1503
echo   -^> the dashboard splits into two panels.
echo.
echo Press a key to plug in a PM5560 power meter (NOT a PLC) ...
pause >nul
start "Power meter - not a PLC" python tests\modbus_plc_sim.py --profile meter --port 1504
echo   -^> ignored: only PLCs with a driver module are attached.
echo.
echo To UNPLUG a PLC, close its window: it goes Offline with an alarm,
echo then leaves the dashboard after 10 s.
endlocal
