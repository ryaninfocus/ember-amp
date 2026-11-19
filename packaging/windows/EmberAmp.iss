; Ember Amp installer for Windows (Inno Setup 6)
; iscc /DVersion=1.0.0 /DArt=<artefacts Release dir> /DOut=<output dir> EmberAmp.iss

#ifndef Version
  #define Version "1.0.0"
#endif

[Setup]
AppId={{256CD933-F665-46B4-A548-E4BB2B6555F8}
AppName=Ember Amp
AppVersion={#Version}
AppPublisher=Ember Audio
DefaultDirName={autopf}\Ember Amp
DefaultGroupName=Ember Amp
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir={#Out}
OutputBaseFilename=Ember-Amp-{#Version}-Windows-Setup
SetupIconFile=ember.ico
UninstallDisplayIcon={app}\Ember Amp.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
InfoBeforeFile=Read Me.txt

[Files]
Source: "{#Art}\Standalone\Ember Amp.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Art}\VST3\Ember Amp.vst3\*"; DestDir: "{commoncf64}\VST3\Ember Amp.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "Read Me.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Ember Amp"; Filename: "{app}\Ember Amp.exe"
Name: "{autodesktop}\Ember Amp"; Filename: "{app}\Ember Amp.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Run]
Filename: "{app}\Ember Amp.exe"; Description: "Open Ember Amp now"; Flags: nowait postinstall skipifsilent
