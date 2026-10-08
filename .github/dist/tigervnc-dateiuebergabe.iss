; Fork only: installer for the self-contained Windows build of the
; dist-windows job (folder dist\TigerVNC-Dateiuebergabe).  Installs for
; the current user, without administrator rights.
;
;   iscc /DAppVersion=<version> /DBuild=<number> /DSourceDir=<folder> \
;        tigervnc-dateiuebergabe.iss

#ifndef AppVersion
  #define AppVersion "0"
#endif
#ifndef Build
  #define Build "0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\TigerVNC-Dateiuebergabe"
#endif

[Setup]
AppId={{6F3C2B7E-4D1A-4E8B-9A5C-2E7F1D0B8C43}
AppName=TigerVNC mit Dateiübergabe
AppVersion={#AppVersion}
AppVerName=TigerVNC mit Dateiübergabe {#AppVersion}
VersionInfoVersion=1.0.0.{#Build}
AppPublisher=Musikhaus Schlaile GmbH
AppPublisherURL=https://ps.schlaile.de/tigervnc/download/
AppComments=Build von Schlaile aus github.com/schlaile/tigervnc, kein offizielles TigerVNC
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\TigerVNC-Dateiuebergabe
DefaultGroupName=TigerVNC mit Dateiübergabe
DisableProgramGroupPage=yes
LicenseFile={#SourceDir}\LICENCE.TXT
OutputBaseFilename=TigerVNC-Dateiuebergabe-Windows-Setup
OutputDir=.
SetupIconFile=wws-vnc.ico
UninstallDisplayIcon={app}\vncviewer.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "de"; MessagesFile: "compiler:Languages\German.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "wwsicon"; Description: "Desktop-Verknüpfung „TigerVNC für die WWS“ (öffnet Dokumente der WWS ohne Rückfrage)"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs
Source: "wws-vnc.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\TigerVNC Viewer"; Filename: "{app}\vncviewer.exe"
Name: "{group}\TigerVNC für die WWS"; Filename: "{app}\vncviewer.exe"; Parameters: "-FileDropOpen=Always"; IconFilename: "{app}\wws-vnc.ico"
Name: "{group}\TigerVNC entfernen"; Filename: "{uninstallexe}"
Name: "{userdesktop}\TigerVNC für die WWS"; Filename: "{app}\vncviewer.exe"; Parameters: "-FileDropOpen=Always"; IconFilename: "{app}\wws-vnc.ico"; Tasks: wwsicon

[Run]
Filename: "{app}\vncviewer.exe"; Parameters: "-FileDropOpen=Always"; Description: "TigerVNC jetzt starten"; Flags: nowait postinstall skipifsilent
