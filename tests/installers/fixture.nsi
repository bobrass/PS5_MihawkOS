; SPDX-License-Identifier: GPL-3.0-or-later
; NSIS fixture of the installer slice (tests/installer-slice-probe.c): a 32-bit installer, so files go to
; Program Files (x86) and its registry keys to the 32-bit view. makensis -DAPPDIR=... -DOUT=... fixture.nsi
Unicode true
Name "PwFixture NSIS"
OutFile "${OUT}"
RequestExecutionLevel admin
InstallDir "$PROGRAMFILES\PwFixture NSIS"
!define UNINSTALL "Software\Microsoft\Windows\CurrentVersion\Uninstall\PwFixtureNSIS"

Section
  SetOutPath "$INSTDIR"
  File "${APPDIR}\pwapp32.exe"
  File "${APPDIR}\pwapp64.exe"
  File "${APPDIR}\pwapp.dat"
  WriteRegStr HKLM "Software\PwFixture\NSIS" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL}" "DisplayName" "PwFixture NSIS"
  WriteRegStr HKLM "${UNINSTALL}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  SetShellVarContext all
  CreateShortcut "$SMPROGRAMS\PwFixture NSIS.lnk" "$INSTDIR\pwapp32.exe"
  CreateShortcut "$DESKTOP\PwFixture NSIS.lnk" "$INSTDIR\pwapp32.exe"
  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  Delete "$SMPROGRAMS\PwFixture NSIS.lnk"
  Delete "$DESKTOP\PwFixture NSIS.lnk"
  Delete "$INSTDIR\pwapp32.exe"
  Delete "$INSTDIR\pwapp64.exe"
  Delete "$INSTDIR\pwapp.dat"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "Software\PwFixture\NSIS"
  DeleteRegKey /ifempty HKLM "Software\PwFixture"
  DeleteRegKey HKLM "${UNINSTALL}"
SectionEnd
