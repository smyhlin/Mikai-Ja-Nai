; Compile only through package.ps1, which supplies an explicit validated file
; inventory. No recursive source glob, downloaded AI payload or state deletion.
#ifndef AppVersion
  #error AppVersion must be supplied by package.ps1
#endif
#ifndef StageDir
  #error StageDir must be supplied by package.ps1
#endif
#ifndef OutputDir
  #error OutputDir must be supplied by package.ps1
#endif
#ifndef PayloadFiles
  #error PayloadFiles must be supplied by package.ps1
#endif

[Setup]
AppId={{0D3BBE76-AC5C-48E3-9EBF-8563798364AA}
AppName=Mikai Ja Nai
AppVersion={#AppVersion}
AppVerName=Mikai Ja Nai {#AppVersion}
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription=Mikai Ja Nai Setup
DefaultDirName={localappdata}\Programs\Mikai
DefaultGroupName=Mikai Ja Nai
DisableProgramGroupPage=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=mikai-{#AppVersion}-windows-x64-setup
SetupIconFile=..\..\resources\app\mikai.ico
UninstallDisplayIcon={app}\mikai.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile={#StageDir}\LICENSE
CloseApplications=yes
CloseApplicationsFilter=mikai.exe
RestartApplications=no
Uninstallable=yes
UninstallLogMode=append
UsePreviousAppDir=yes
UsePreviousTasks=yes
ChangesAssociations=yes
; Deliberately unsigned; do not imply a trusted publisher or auto-elevation.

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
#include PayloadFiles

[Icons]
Name: "{group}\Mikai Ja Nai"; Filename: "{app}\mikai.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Mikai Ja Nai"; Filename: "{app}\mikai.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\mikai.exe"; Description: "Launch Mikai Ja Nai"; Flags: nowait postinstall skipifsilent unchecked

; No [Registry], [InstallDelete] or [UninstallDelete]. Inno removes only files
; installed by its log; acquired native/models, logs and settings survive.
; Integrations are opt-in within the application. Their cleanup is constrained
; below to the exact executable AND ownership marker, never by product name.
[Code]
const
  RegistrationOwner = '{7D1829D9-6F44-47BA-B79B-871D57145C86}';
  OwnerValue = 'MikaiRegistrationOwner';
  AppKey = 'Software\Mikai\DefaultVideoPlayer';
  ProgIdKey = 'Software\Classes\Mikai.Video';
  CapabilitiesKey = 'Software\Mikai\DefaultVideoPlayer\Capabilities';
  RegisteredAppsKey = 'Software\RegisteredApplications';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';

function SameRegistryString(const Key, Name, Expected: String): Boolean;
var
  Actual: String;
begin
  Result := RegQueryStringValue(HKCU, Key, Name, Actual) and
    (CompareText(Actual, Expected) = 0);
end;

procedure DeleteValue(const Key, Name: String);
begin
  if RegValueExists(HKCU, Key, Name) then
    if not RegDeleteValue(HKCU, Key, Name) then
      Log('Could not remove owned registry value HKCU\' + Key + '\' + Name);
end;

procedure PruneKey(const Key: String);
begin
  // This API removes ONLY an empty key. Never use RegDeleteKeyIncludingSubkeys.
  if not RegDeleteKeyIfEmpty(HKCU, Key) then
    Log('Retaining non-empty or inaccessible registry key HKCU\' + Key);
end;

function SameOwnedDisplayValue(const Key, Name, EnglishValue,
  UkrainianValue, LegacyEnglishValue, LegacyUkrainianValue: String): Boolean;
begin
  Result := SameRegistryString(Key, Name, EnglishValue) or
    SameRegistryString(Key, Name, UkrainianValue) or
    SameRegistryString(Key, Name, LegacyEnglishValue) or
    SameRegistryString(Key, Name, LegacyUkrainianValue);
end;

procedure RemoveContextVerb(const Extension, Command, Icon: String);
var
  Key: String;
begin
  Key := 'Software\Classes\SystemFileAssociations\' + Extension + '\shell\Mikai.OpenVideo';
  if not SameRegistryString(Key, OwnerValue, RegistrationOwner) or
     not SameRegistryString(Key + '\command', '', Command) then
    exit;
  DeleteValue(Key + '\command', '');
  // Remove only known localized values written by Mikai; preserve user edits.
  if SameOwnedDisplayValue(Key, '', 'Open with Mikai Ja Nai',
    'Відкрити за допомогою Mikai Ja Nai', 'Open with Mikai',
    'Відкрити за допомогою Mikai') then DeleteValue(Key, '');
  if SameRegistryString(Key, 'Icon', Icon) then DeleteValue(Key, 'Icon');
  DeleteValue(Key, OwnerValue);
  PruneKey(Key + '\command');
  PruneKey(Key);
end;

procedure RemoveDefaultRegistration(const Command, Icon: String);
var
  Associations: String;
  Extensions: TArrayOfString;
  I: Integer;
begin
  if not SameRegistryString(AppKey, OwnerValue, RegistrationOwner) or
     not SameRegistryString(ProgIdKey, OwnerValue, RegistrationOwner) or
     not SameRegistryString(ProgIdKey + '\shell\open\command', '', Command) then
    exit;
  // Never touch extension defaults, UserChoice, OpenWith lists or another
  // installation's capabilities published under a different command.
  if SameRegistryString(RegisteredAppsKey, 'Mikai', CapabilitiesKey) then
    DeleteValue(RegisteredAppsKey, 'Mikai');
  Extensions := ['.mkv', '.mp4', '.webm', '.avi', '.mov', '.m4v',
    '.ts', '.m2ts', '.mpeg', '.mpg', '.flv'];
  Associations := CapabilitiesKey + '\FileAssociations';
  for I := 0 to GetArrayLength(Extensions) - 1 do
    if SameRegistryString(Associations, Extensions[I], 'Mikai.Video') then
      DeleteValue(Associations, Extensions[I]);
  if SameRegistryString(CapabilitiesKey, 'ApplicationName', 'Mikai Ja Nai') or
     SameRegistryString(CapabilitiesKey, 'ApplicationName', 'Mikai') then
    DeleteValue(CapabilitiesKey, 'ApplicationName');
  if SameOwnedDisplayValue(CapabilitiesKey, 'ApplicationDescription',
    'Play local video files with Mikai Ja Nai.',
    'Відтворюйте локальні відеофайли за допомогою Mikai Ja Nai.',
    'Play local video files with Mikai.',
    'Відтворюйте локальні відеофайли за допомогою Mikai.') then
    DeleteValue(CapabilitiesKey, 'ApplicationDescription');
  if SameRegistryString(CapabilitiesKey, 'ApplicationIcon', Icon) then
    DeleteValue(CapabilitiesKey, 'ApplicationIcon');
  DeleteValue(AppKey, OwnerValue);
  DeleteValue(ProgIdKey + '\shell\open\command', '');
  if SameRegistryString(ProgIdKey + '\DefaultIcon', '', Icon) then
    DeleteValue(ProgIdKey + '\DefaultIcon', '');
  if SameOwnedDisplayValue(ProgIdKey, '', 'Mikai Ja Nai video file',
    'Відеофайл Mikai Ja Nai', 'Mikai video file',
    'Відеофайл Mikai') then DeleteValue(ProgIdKey, '');
  DeleteValue(ProgIdKey, OwnerValue);
  PruneKey(Associations);
  PruneKey(CapabilitiesKey);
  PruneKey(AppKey);
  PruneKey(ProgIdKey + '\shell\open\command');
  PruneKey(ProgIdKey + '\shell\open');
  PruneKey(ProgIdKey + '\shell');
  PruneKey(ProgIdKey + '\DefaultIcon');
  PruneKey(ProgIdKey);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Exe, Command, Icon: String;
  Extensions: TArrayOfString;
  I: Integer;
begin
  if CurUninstallStep <> usUninstall then exit;
  Exe := ExpandConstant('{app}\mikai.exe');
  Command := '"' + Exe + '" --open-video "%1"';
  Icon := '"' + Exe + '",0';
  if SameRegistryString(RunKey, 'Mikai', '"' + Exe + '"') then
    DeleteValue(RunKey, 'Mikai');
  Extensions := ['.mkv', '.mp4', '.webm', '.avi', '.mov', '.m4v',
    '.ts', '.m2ts', '.mpeg', '.mpg', '.flv'];
  for I := 0 to GetArrayLength(Extensions) - 1 do
    RemoveContextVerb(Extensions[I], Command, Icon);
  RemoveDefaultRegistration(Command, Icon);
end;
