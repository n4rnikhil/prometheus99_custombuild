; Prometheus HMI - Windows installer (NSIS 3, https://nsis.sourceforge.io)
;   makensis /DSTAGE=<folder with the built files> /DVERSION=6.1 hmi-runtime\installer\prometheus_hmi.nsi
; Per-user install: no administrator rights, and the runtime can write its log, key and vault
; next to the executable.

Target amd64-unicode          ; the programs are 64-bit: so is the installer
!include "MUI2.nsh"

!ifndef VERSION
  !define VERSION "6.1"
!endif
!ifndef STAGE
  !error "pass /DSTAGE=<folder with industrial_hmi.exe, plc_simulator.exe, ...>"
!endif

Name "Prometheus HMI"
OutFile "Prometheus-HMI-Setup-${VERSION}.exe"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\Prometheus HMI"
InstallDirRegKey HKCU "Software\Prometheus HMI" "InstallDir"
SetCompressor /SOLID lzma
!define MUI_ICON   "${STAGE}\industrial_hmi.ico"
!define MUI_UNICON "${STAGE}\industrial_hmi.ico"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\PrometheusHMI"
!define BENCH_ARGS "--add atv340 --add tank --add meter --open-hmi"

!define MUI_FINISHPAGE_RUN "$INSTDIR\plc_simulator.exe"
!define MUI_FINISHPAGE_RUN_PARAMETERS "${BENCH_ARGS}"
!define MUI_FINISHPAGE_RUN_TEXT "Start Prometheus HMI with the simulated plant now"
!define MUI_WELCOMEPAGE_TEXT "This installs the Prometheus HMI operator panel and the PLC simulator for bench testing.$\r$\n$\r$\nBench use only: the included hmi.cfg allows the factory PINs (Operator 1111). On a real plant set site PINs with industrial_hmi --set-pin and remove --allow-factory-pins.$\r$\n$\r$\nSTOP ALL is a software stop, not an emergency stop."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

VIProductVersion "${VERSION}.0.0"
VIAddVersionKey "ProductName" "Prometheus HMI"
VIAddVersionKey "FileDescription" "Prometheus HMI installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"

Section "Prometheus HMI" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${STAGE}\industrial_hmi.exe"
  File "${STAGE}\plc_simulator.exe"
  File "${STAGE}\plc_simulator_cli.exe"
  File "${STAGE}\industrial_hmi.ico"
  File "${STAGE}\README.txt"
  ; keep a site's own configuration on upgrade
  IfFileExists "$INSTDIR\hmi.cfg" +2 0
    File "${STAGE}\hmi.cfg"
  SetOutPath "$INSTDIR\examples"
  File "${STAGE}\examples\*.cfg"
  SetOutPath "$INSTDIR"

  CreateDirectory "$SMPROGRAMS\Prometheus HMI"
  CreateShortcut "$SMPROGRAMS\Prometheus HMI\Prometheus HMI (bench with simulator).lnk" "$INSTDIR\plc_simulator.exe" "${BENCH_ARGS}" "$INSTDIR\industrial_hmi.exe" 0
  CreateShortcut "$SMPROGRAMS\Prometheus HMI\Prometheus HMI.lnk" "$INSTDIR\industrial_hmi.exe" "" "$INSTDIR\industrial_hmi.exe" 0
  CreateShortcut "$SMPROGRAMS\Prometheus HMI\PLC Simulator.lnk" "$INSTDIR\plc_simulator.exe" "" "$INSTDIR\plc_simulator.exe" 0
  CreateShortcut "$SMPROGRAMS\Prometheus HMI\Uninstall Prometheus HMI.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\Prometheus HMI.lnk" "$INSTDIR\plc_simulator.exe" "${BENCH_ARGS}" "$INSTDIR\industrial_hmi.exe" 0

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\Prometheus HMI" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "Prometheus HMI"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "Prometheus HMI"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\industrial_hmi.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  ; The audit log, device key and PIN vault are records and secrets of this site: they stay.
  Delete "$INSTDIR\industrial_hmi.exe"
  Delete "$INSTDIR\plc_simulator.exe"
  Delete "$INSTDIR\plc_simulator_cli.exe"
  Delete "$INSTDIR\industrial_hmi.ico"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\examples\*.cfg"
  RMDir  "$INSTDIR\examples"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir  "$INSTDIR"
  Delete "$SMPROGRAMS\Prometheus HMI\*.lnk"
  RMDir  "$SMPROGRAMS\Prometheus HMI"
  Delete "$DESKTOP\Prometheus HMI.lnk"
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "Software\Prometheus HMI"
SectionEnd
