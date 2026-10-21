; Filo installer (Inno Setup 6). Per-user install: no administrator rights, nothing outside the user's profile.
; Build with scripts\package.ps1 (it stages the files and passes StageDir / OutDir / AppVersion).
#ifndef StageDir
  #error StageDir is not defined - run scripts\package.ps1
#endif
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif

[Setup]
AppId={{6E3B9E50-7C0F-4F5B-9D43-F110F110A001}
AppName=Filo
AppVersion={#AppVersion}
AppPublisher=Filo
DefaultDirName={autopf}\Filo
DefaultGroupName=Filo
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#OutDir}
OutputBaseFilename=Filo-Setup-{#AppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\Filo.exe
SetupIconFile={#StageDir}\Filo.ico
CloseApplications=force
RestartApplications=no
UsedUserAreasWarning=no

[Tasks]
Name: "startup"; Description: "Start Filo when I sign in"; GroupDescription: "Options:"
Name: "shellmenu"; Description: "Add ""Filo"" to the File Explorer right-click menu"; GroupDescription: "Options:"
Name: "sendto"; Description: "Add Filo to the ""Send to"" menu"; GroupDescription: "Options:"

[Files]
Source: "{#StageDir}\Filo.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\icon-packs\*.json"; DestDir: "{app}\icon-packs"; Flags: ignoreversion
Source: "{#StageDir}\pdfium.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\tools\*"; DestDir: "{app}\tools"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Filo"; Filename: "{app}\Filo.exe"
Name: "{group}\Filo Settings"; Filename: "{app}\Filo.exe"; Parameters: "--settings"
Name: "{userappdata}\Microsoft\Windows\SendTo\Filo"; Filename: "{app}\Filo.exe"; Parameters: "--wheel"; Tasks: sendto

[Registry]
; Right-click cascade "Filo >" with three entries. Verbs live in the per-user CommandStore (no admin rights needed).
; On Windows 11 the cascade appears under "Show more options" (the new compact menu only lists packaged-app extensions).
Root: HKCU; Subkey: "Software\Classes\*\shell\Filo"; ValueType: string; ValueName: "MUIVerb"; ValueData: "Filo"; Tasks: shellmenu; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\*\shell\Filo"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\Filo.exe"""; Tasks: shellmenu
Root: HKCU; Subkey: "Software\Classes\*\shell\Filo"; ValueType: string; ValueName: "SubCommands"; ValueData: "Filo.Convert;Filo.Tools;Filo.Actions"; Tasks: shellmenu
Root: HKCU; Subkey: "Software\Classes\Directory\shell\Filo"; ValueType: string; ValueName: "MUIVerb"; ValueData: "Filo"; Tasks: shellmenu; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Directory\shell\Filo"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\Filo.exe"""; Tasks: shellmenu
Root: HKCU; Subkey: "Software\Classes\Directory\shell\Filo"; ValueType: string; ValueName: "SubCommands"; ValueData: "Filo.Actions"; Tasks: shellmenu
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\CommandStore\shell\Filo.Convert"; ValueType: string; ValueName: "MUIVerb"; ValueData: "Convert..."; Tasks: shellmenu; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\CommandStore\shell\Filo.Convert\command"; ValueType: string; ValueData: """{app}\Filo.exe"" --wheel --kind convert ""%1"""; Tasks: shellmenu
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\CommandStore\shell\Filo.Tools"; ValueType: string; ValueName: "MUIVerb"; ValueData: "Tools..."; Tasks: shellmenu; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\CommandStore\shell\Filo.Tools\command"; ValueType: string; ValueData: """{app}\Filo.exe"" --wheel --kind tools ""%1"""; Tasks: shellmenu
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\CommandStore\shell\Filo.Actions"; ValueType: string; ValueName: "MUIVerb"; ValueData: "Actions..."; Tasks: shellmenu; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\CommandStore\shell\Filo.Actions\command"; ValueType: string; ValueData: """{app}\Filo.exe"" --wheel --kind actions ""%1"""; Tasks: shellmenu
; Autostart (the runtime also maintains this value from the Settings window)
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Filo"; ValueData: """{app}\Filo.exe"" --background"; Tasks: startup; Flags: uninsdeletevalue

[Run]
Filename: "{app}\Filo.exe"; Description: "Start Filo now"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\Filo.exe"; Parameters: "--quit"; Flags: runhidden; RunOnceId: "QuitFilo"

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\Filo\cache"
