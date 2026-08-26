; PhotoFrame Installer Script for Inno Setup
; Compile with Inno Setup 6+

#define MyAppName "PhotoFrame"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "PhotoFrame"
#define MyAppExeName "PhotoFrame.UI.exe"

[Setup]
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputDir=..\publish\installer
OutputBaseFilename=PhotoFrame-Setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "Auto-start with Windows"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
Source: "..\src\PhotoFrame.UI\bin\Release\net8.0-windows\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
var
  RunKey: String;
begin
  if CurStep = ssPostInstall then
  begin
    if IsTaskSelected('autostart') then
    begin
      RunKey := 'SOFTWARE\Microsoft\Windows\CurrentVersion\Run';
      RegWriteStringValue(HKEY_CURRENT_USER, RunKey, '{#MyAppName}',
        '"' + ExpandConstant('{app}\{#MyAppExeName}') + '"');
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  RunKey: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    RunKey := 'SOFTWARE\Microsoft\Windows\CurrentVersion\Run';
    RegDeleteValue(HKEY_CURRENT_USER, RunKey, '{#MyAppName}');
  end;
end;
