@echo off
start "" industrial_hmi.exe
powershell -Command "Start-Sleep -Milliseconds 1200; Get-Process -Name industrial_hmi | Format-List ProcessName, Id, WorkingSet64, PrivateMemorySize64"
taskkill /F /IM industrial_hmi.exe >nul 2>&1
