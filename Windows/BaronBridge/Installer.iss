; Copyright (C) 2026 Baron Potato
; SPDX-License-Identifier: GPL-3.0-only

#ifndef BridgePayload
  #error Pass /DBridgePayload with the CMake install directory.
#endif

#define BridgeName "Baron Bridge"
#define BridgeClsid "{{7ab3f5dc-6138-43e2-831a-96c4e11d5be1}"
#define BridgeAppId "{{BB3EC509-3C76-4A1F-8697-A6E02A82F83B}"

[Setup]
AppId={#BridgeAppId}
AppName={#BridgeName}
AppVersion=1.0.2
AppPublisher=Baron Potato
VersionInfoCompany=Baron Potato
VersionInfoCopyright=Copyright (C) 2026 Baron Potato
VersionInfoVersion=1.0.2
DefaultDirName={autopf}\{#BridgeName}
UsePreviousAppDir=no
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=Baron-Bridge-x64-Setup
Compression=lzma2
SolidCompression=yes
CloseApplications=no
RestartApplications=no
WizardStyle=modern

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#BridgePayload}\BaronBridge.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BridgePayload}\COPYING.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BridgePayload}\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BridgePayload}\Steinberg-SDK-LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BridgePayload}\Inno-Setup-LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
Root: HKLM64; Subkey: "SOFTWARE\ASIO\{#BridgeName}"; ValueType: string; ValueName: "CLSID"; ValueData: "{#BridgeClsid}"; Flags: uninsdeletekey
Root: HKLM64; Subkey: "SOFTWARE\ASIO\{#BridgeName}"; ValueType: string; ValueName: "Description"; ValueData: "{#BridgeName}"
Root: HKLM64; Subkey: "SOFTWARE\Classes\CLSID\{#BridgeClsid}"; ValueType: string; ValueData: "{#BridgeName}"; Flags: uninsdeletekey
Root: HKLM64; Subkey: "SOFTWARE\Classes\CLSID\{#BridgeClsid}\InprocServer32"; ValueType: string; ValueData: "{app}\BaronBridge.dll"
Root: HKLM64; Subkey: "SOFTWARE\Classes\CLSID\{#BridgeClsid}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Both"
Root: HKLM64; Subkey: "SOFTWARE\ASIO\Vesper ASIO Bridge"; ValueType: none; Flags: deletekey; Check: IsLegacyBridge('Vesper ASIO Bridge')
Root: HKLM64; Subkey: "SOFTWARE\ASIO\Baron ASIO Bridge"; ValueType: none; Flags: deletekey; Check: IsLegacyBridge('Baron ASIO Bridge')

[Code]
var
  LegacyDirectory: String;
  LegacyUninstaller: String;

function IsLegacyBridge(const Name: String): Boolean;
var
  DriverClsid: String;
begin
  Result := RegQueryStringValue(HKLM64, 'SOFTWARE\ASIO\' + Name, 'CLSID', DriverClsid) and
    (CompareText(DriverClsid, ExpandConstant('{#BridgeClsid}')) = 0);
end;

function InitializeSetup: Boolean;
var
  OldDll: String;
  OldDirectory: String;
  UninstallCommand: String;
begin
  Result := True;
  if not (IsLegacyBridge('Vesper ASIO Bridge') or IsLegacyBridge('Baron ASIO Bridge')) then
    Exit;
  if not RegQueryStringValue(HKLM64,
    ExpandConstant('SOFTWARE\Classes\CLSID\{#BridgeClsid}\InprocServer32'), '', OldDll) then
    Exit;
  OldDirectory := ExtractFileDir(OldDll);
  if (CompareText(OldDll, ExpandConstant('{autopf}\Vesper ASIO Bridge\VesperASIOBridge.dll')) <> 0) and
     (CompareText(OldDll, ExpandConstant('{autopf}\Baron ASIO Bridge\VesperASIOBridge.dll')) <> 0) then
    Exit;
  if not RegQueryStringValue(HKLM64,
    ExpandConstant('SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{#BridgeAppId}_is1'),
    'UninstallString', UninstallCommand) then
    Exit;
  if CompareText(UninstallCommand, '"' + AddBackslash(OldDirectory) + 'unins000.exe"') <> 0 then
    Exit;
  LegacyDirectory := OldDirectory;
  LegacyUninstaller := AddBackslash(OldDirectory) + 'unins000.exe';
end;

procedure CurStepChanged(Step: TSetupStep);
var
  OldPath: String;
begin
  if (Step <> ssPostInstall) or (LegacyDirectory = '') or
    (CompareText(LegacyDirectory, ExpandConstant('{app}')) = 0) then
    Exit;
  OldPath := AddBackslash(LegacyDirectory);
  DeleteFile(LegacyUninstaller);
  DeleteFile(OldPath + 'unins000.dat');
  DeleteFile(OldPath + 'unins000.msg');
  DeleteFile(OldPath + 'VesperASIOBridge.dll');
  DeleteFile(OldPath + 'Steinberg-ASIO-LICENSE.txt');
  RemoveDir(LegacyDirectory);
end;
