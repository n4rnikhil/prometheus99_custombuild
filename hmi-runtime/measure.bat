@echo off
setlocal
REM ==========================================================================
REM  Runtime audit: starts the HMI for 8 s, samples RAM / threads while it
REM  runs, then prints the HMI's own measured report (loop rate, jitter,
REM  integrity counters) and verifies the alarm log hash chain.
REM    measure.bat                 simulator
REM    measure.bat 127.0.0.1:1502  against a Modbus TCP PLC
REM ==========================================================================
cd /d "%~dp0"
if not exist industrial_hmi.exe (
    echo [ERROR] industrial_hmi.exe not found - run build.bat first
    exit /b 1
)
set PLC_ARG=
if not "%~1"=="" set PLC_ARG=--plc %~1
set REPORT=%TEMP%\hmi_measure_report.txt
set LOG=%TEMP%\hmi_measure_alarms.log
del /q "%REPORT%" "%LOG%" 2>nul

start "" .\industrial_hmi.exe %PLC_ARG% --exit-after-ms 8000 --report "%REPORT%" --log "%LOG%"

powershell -NoProfile -Command ^
  "Start-Sleep -Seconds 5;" ^
  "$p = Get-Process -Name industrial_hmi -ErrorAction SilentlyContinue | Select-Object -First 1;" ^
  "if (-not $p) { Write-Host '[ERROR] HMI is not running (blocked by Smart App Control or crashed?)'; exit 1 }" ^
  "Write-Host ('[RUN ] PID ' + $p.Id + '  working set ' + [math]::Round($p.WorkingSet64/1MB,2) + ' MB  private ' + [math]::Round($p.PrivateMemorySize64/1MB,2) + ' MB  threads ' + $p.Threads.Count);" ^
  "$null = $p.WaitForExit(10000);" ^
  "Write-Host '[EXIT] HMI closed'"
if errorlevel 1 exit /b 1

if not exist "%REPORT%" (
    echo [ERROR] no runtime report written
    exit /b 1
)
echo.
echo ---- measured by the HMI ----
type "%REPORT%"
echo -----------------------------
.\industrial_hmi.exe --verify-log "%LOG%"
if errorlevel 1 (
    echo [FAILED] alarm log verification
    exit /b 1
)
echo [OK] runtime audit complete
exit /b 0
