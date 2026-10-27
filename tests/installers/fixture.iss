; SPDX-License-Identifier: GPL-3.0-or-later
; Inno Setup fixture of the installer slice (tests/installer-slice-probe.c): installs in 64-bit mode, so files go
; to Program Files and its registry keys to the 64-bit view. ISCC.exe /DAPPDIR=... /O<dir> fixture.iss
[Setup]
AppId=PwFixtureInno
AppName=PwFixture Inno
AppVersion=1.0
AppPublisher=PS5 Proton tests
DefaultDirName={autopf}\PwFixture Inno
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=fixture-inno
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Files]
Source: "{#APPDIR}\pwapp32.exe"; DestDir: "{app}"
Source: "{#APPDIR}\pwapp64.exe"; DestDir: "{app}"
Source: "{#APPDIR}\pwapp.dat"; DestDir: "{app}"

[Registry]
Root: HKLM; Subkey: "Software\PwFixture\Inno"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\PwFixture"; Flags: uninsdeletekeyifempty

[Icons]
Name: "{autoprograms}\PwFixture Inno"; Filename: "{app}\pwapp64.exe"
Name: "{autodesktop}\PwFixture Inno"; Filename: "{app}\pwapp64.exe"
