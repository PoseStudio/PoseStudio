; ==============================================================================
; PoseStudio Open Source Project
; File: packaging/PoseStudio.iss
; Description: Inno Setup script that produces the Windows installer
;              (PoseStudio-<version>-Windows-Setup.exe).
;
; This script is normally compiled by the release workflow
; (.github/workflows/release.yml), which passes the defines below on the
; ISCC command line. It can also be compiled locally — see docs/RELEASING.md.
;
; Requires Inno Setup 6.3+ (for the x64compatible architecture identifier).
; ==============================================================================

; ---- Build-time parameters (overridable via ISCC /D switches) ----------------
; AppVersion : the human version, e.g. 0.3.9 (from the git tag in CI)
; AppDir     : the fully-deployed application folder (exe + Qt DLLs + shaders + Maquettes)
; HdriDir    : the stock HDRI content pack (category folders of .hdr + .jpg previews)
; OutDir     : where the compiled Setup.exe is written
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef AppDir
  #define AppDir "..\dist\app"
#endif
#ifndef HdriDir
  #define HdriDir "..\dist\hdri"
#endif
#ifndef OutDir
  #define OutDir "..\dist\out"
#endif

[Setup]
; Fixed GUID identifying PoseStudio to Windows' installed-programs machinery.
; NEVER change this — it is how upgrades find and replace an existing install.
AppId={{48F1B66A-96FA-4C59-9603-E4C71AB121B4}
AppName=PoseStudio
AppVersion={#AppVersion}
AppVerName=PoseStudio {#AppVersion}
AppPublisher=The PoseStudio Open-Source Project
AppPublisherURL=https://posestudio.org/
AppSupportURL=https://github.com/PoseStudio/PoseStudio/discussions
AppUpdatesURL=https://github.com/PoseStudio/PoseStudio/releases

; Per-user install: no administrator prompt, lands in the user's own Programs
; folder ({localappdata}\Programs\PoseStudio). This is the friendliest mode for
; testers — double-click, next, next, done — and sidesteps UAC entirely.
PrivilegesRequired=lowest
DefaultDirName={autopf}\PoseStudio
DisableProgramGroupPage=yes
DefaultGroupName=PoseStudio

; 64-bit only (the app ships 64-bit Qt + Vulkan binaries).
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

OutputDir={#OutDir}
OutputBaseFilename=PoseStudio-{#AppVersion}-Windows-Setup
SetupIconFile=..\resources\icon.ico
UninstallDisplayIcon={app}\PoseStudio.exe
UninstallDisplayName=PoseStudio

LicenseFile=..\LICENSE
WizardStyle=modern
; The app binaries and the HDR panoramas are large: the strongest compression, solid.
Compression=lzma2/max
SolidCompression=yes
; One language (English, below): nothing to ask.
ShowLanguageDialog=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; The fully-deployed application: PoseStudio.exe, Qt runtime + plugins, the
; compiled SPIR-V shaders, and the built-in Maquettes asset library.
Source: "{#AppDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

; Stock HDRI lighting environments (CC0, from Poly Haven) go into the user's
; own content library in Documents — that is where the app looks for them, and
; it is user content the person is free to reorganize or delete.
;   onlyifdoesntexist  : never overwrite a file the user replaced or edited
;   uninsneveruninstall: uninstalling the app never deletes the user's library
Source: "{#HdriDir}\*"; DestDir: "{userdocs}\My PoseStudio Library\hdri"; Flags: recursesubdirs onlyifdoesntexist uninsneveruninstall

[Icons]
Name: "{autoprograms}\PoseStudio"; Filename: "{app}\PoseStudio.exe"
Name: "{autodesktop}\PoseStudio"; Filename: "{app}\PoseStudio.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\PoseStudio.exe"; Description: "{cm:LaunchProgram,PoseStudio}"; Flags: nowait postinstall skipifsilent
