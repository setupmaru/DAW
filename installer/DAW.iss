; Windows installer for DAW (Inno Setup 6).
; Built by installer/build-installer.ps1, which stages the files and passes:
;   /DAppVersion=1.0.0  /DStageDir=<folder with DAW.exe ...>  /DOutputDir=<output folder>
;
; User data (settings, plugin cache, unsaved recordings in %APPDATA%\DAW) is not touched
; by install or uninstall.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\dist\DAW"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
; Fixed: identifies the app across versions, so a new installer upgrades in place.
AppId={{7C1D7A2E-3B5F-4E8A-9C61-2F4D8B0E5A13}
AppName=DAW
AppVersion={#AppVersion}
AppVerName=DAW {#AppVersion}
VersionInfoVersion={#AppVersion}
AppPublisher=setupmaru
AppPublisherURL=https://github.com/setupmaru/DAW
AppSupportURL=https://github.com/setupmaru/DAW/issues
AppUpdatesURL=https://github.com/setupmaru/DAW/releases
DefaultDirName={autopf}\DAW
DefaultGroupName=DAW
DisableProgramGroupPage=yes
LicenseFile={#StageDir}\LICENSE.txt
OutputDir={#OutputDir}
OutputBaseFilename=DAW-Setup-v{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; Program Files by default; the user may choose a per-user install instead.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\DAW.exe
UninstallDisplayName=DAW
; Close a running DAW before replacing it.
CloseApplications=yes

[Languages]
#if FileExists(CompilerPath + "Languages\Korean.isl")
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
#endif
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\DAW.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\THIRD-PARTY-ASIO-LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\DAW"; Filename: "{app}\DAW.exe"
Name: "{autodesktop}\DAW"; Filename: "{app}\DAW.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\DAW.exe"; Description: "{cm:LaunchProgram,DAW}"; Flags: nowait postinstall skipifsilent
