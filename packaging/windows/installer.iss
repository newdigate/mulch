; Inno Setup script for Shader Streamer.
; Invoke: iscc /DMyVersion=<ver> /DSourceDir=<abs path to build> packaging\windows\installer.iss
#ifndef MyVersion
  #define MyVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build"
#endif

[Setup]
AppName=Shader Streamer
AppVersion={#MyVersion}
AppPublisher=newdigate
DefaultDirName={autopf}\Shader Streamer
DefaultGroupName=Shader Streamer
UninstallDisplayIcon={app}\shader_streamer.exe
OutputDir=dist
OutputBaseFilename=Shader_Streamer-{#MyVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Files]
Source: "{#SourceDir}\shader_streamer.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\*.dll";               DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\shaders\*";           DestDir: "{app}\shaders"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
; WorkingDir: {app} so the app's CWD contains shaders\ at launch.
Name: "{group}\Shader Streamer";           Filename: "{app}\shader_streamer.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall Shader Streamer"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\shader_streamer.exe"; WorkingDir: "{app}"; Description: "Launch Shader Streamer"; Flags: nowait postinstall skipifsilent
