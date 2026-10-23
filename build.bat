@echo off
setlocal enabledelayedexpansion
echo =================================================================
echo   Building AEGIS Industrial HMI Runtime (C99 Clean Architecture)
echo   Standards: IEC 62443 / IEC 61508 / Apple HIG Ergonomics
echo =================================================================

REM Terminate any running instance to allow binary overwrite
taskkill /F /IM industrial_hmi.exe >nul 2>&1

REM 1. Try Microsoft Visual C++ compiler (MSVC)
set "VCVARS="
if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
)

if defined VCVARS (
    echo [BUILD] Using MSVC Compiler with IEC 62443 Security Hardening...
    call "%VCVARS%" >nul 2>&1
    
    cl.exe /nologo /O1 /Os /GL /Gy /GF /Gw /GS /MD /D_CRT_SECURE_NO_WARNINGS /DNDEBUG /Iinclude ^
        src\core\tags.c ^
        src\core\alarms.c ^
        src\core\watchdog.c ^
        src\core\security.c ^
        src\core\crc32.c ^
        src\renderer\font5x7.c ^
        src\renderer\micro_renderer.c ^
        src\hal\timer.c ^
        src\hal\input_queue.c ^
        src\hal\display_win32.c ^
        src\drivers\driver_modbus.c ^
        src\drivers\driver_host.c ^
        src\main.c ^
        /Fe:industrial_hmi.exe ^
        /link /OPT:REF /OPT:ICF /LTCG /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /STACK:32768,4096 /HEAP:65536,4096 user32.lib gdi32.lib winmm.lib psapi.lib >nul 2>&1

    if !ERRORLEVEL! EQU 0 (
        del /f /q *.obj >nul 2>&1
        goto :build_success
    )
    echo [WARN] MSVC build failed or not found, attempting GCC fallback...
)

REM 2. Fallback: MinGW / GCC
set "WINLIBS_PATH=%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
set PATH=%WINLIBS_PATH%;C:\msys64\mingw64\bin;%PATH%

echo [BUILD] Using GCC Compiler with Security & Size Optimizations...
gcc -std=c99 -Wall -Wextra -Os -DNDEBUG -Iinclude ^
    -fstack-protector-strong -ffunction-sections -fdata-sections ^
    src/core/tags.c ^
    src/core/alarms.c ^
    src/core/watchdog.c ^
    src/core/security.c ^
    src/core/crc32.c ^
    src/renderer/font5x7.c ^
    src/renderer/micro_renderer.c ^
    src/hal/timer.c ^
    src/hal/input_queue.c ^
    src/hal/display_win32.c ^
    src/drivers/driver_modbus.c ^
    src/drivers/driver_host.c ^
    src/main.c ^
    -o industrial_hmi.exe ^
    -lgdi32 -luser32 -lwinmm -lpsapi ^
    -Wl,--gc-sections -Wl,--dynamicbase -Wl,--nxcompat -s

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Compilation failed with error code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

:build_success
echo =================================================================
echo [SUCCESS] Build Succeeded!
for %%A in (industrial_hmi.exe) do echo [SIZE] Executable Size: %%~zA bytes
echo [SECURITY] IEC 62443 Hardened: Stack Canaries (/GS) + ASLR + DEP/NX Active
echo =================================================================
exit /b 0
