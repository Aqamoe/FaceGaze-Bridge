; FaceGaze Bridge 安装包（Inno Setup 6）
;
; 构建前提：已经用 cmake --install 生成 dist/（含 runtime/vrcft）。
; 推荐用 packaging/build_installer.ps1 调用，自动带入版本与目录。
;
; 用户级安装（PrivilegesRequired=lowest）：装到 %LOCALAPPDATA%\Programs，
; 不需要管理员。卸载时会移除 HKCU 卸载项、开始菜单/桌面快捷方式、
; SteamVR app 注册，以及 %LOCALAPPDATA%\FaceGazeBridge 下的配置/模块/日志。

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef DistDir
  #define DistDir "..\dist"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist-installer"
#endif

[Setup]
AppId={{7B2F3C64-9E0A-4C1B-A0D8-4F6D2B7E5A91}
AppName=FaceGaze Bridge
AppVersion={#AppVersion}
AppVerName=FaceGaze Bridge {#AppVersion}
AppPublisher=Aqamoe
AppPublisherURL=https://github.com/Aqamoe
AppSupportURL=https://github.com/Aqamoe/FaceGaze-Bridge/issues
AppCopyright=Copyright (C) 2026 Aqamoe (光水)
DefaultDirName={localappdata}\Programs\FaceGazeBridge
DefaultGroupName=FaceGaze Bridge
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#OutputDir}
OutputBaseFilename=FaceGazeBridge-Setup-{#AppVersion}
SetupIconFile=icon.ico
UninstallDisplayIcon={app}\FaceGaze Bridge.exe
UninstallDisplayName=FaceGaze Bridge
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=no
MinVersion=10.0.17763

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#DistDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\FaceGaze Bridge"; Filename: "{app}\FaceGaze Bridge.exe"
Name: "{autodesktop}\FaceGaze Bridge"; Filename: "{app}\FaceGaze Bridge.exe"; Tasks: desktopicon

[Run]
; 自动更新（引擎用 /RELAUNCH=1 调起，ADR-0036）：静默安装完把新版本拉起来。
; 单独一条而不是给下面那条去掉 skipifsilent：交互式安装的"启动"复选框在静默模式下没有
; 意义，CI 里的 /VERYSILENT 也不该顺手弹一个窗口。
Filename: "{app}\FaceGaze Bridge.exe"; Flags: nowait; Check: UpdateRelaunch
Filename: "{app}\FaceGaze Bridge.exe"; Description: "{cm:LaunchProgram,FaceGaze Bridge}"; Flags: nowait postinstall skipifsilent; Check: not UpdateRelaunch

[UninstallRun]
; 先解掉 SteamVR 启动项；SteamVR 没运行时这一步会失败，但不阻塞卸载
Filename: "{app}\fgb-cli.exe"; Parameters: "steamvr remove"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveSteamVR"

[Code]
// 自动更新走 /SILENT /RELAUNCH=1：装完必须把程序重新拉起来，
// 否则用户看到的是"更新安装完成后什么都没发生"（ADR-0036）。
function UpdateRelaunch: Boolean;
begin
  Result := ExpandConstant('{param:RELAUNCH|0}') = '1';
end;

// 用户数据（校准/预设/模块/日志/崩溃转储）不自动删除：
//  - 卸载后重装可以继续用；
//  - 静默卸载（/VERYSILENT）不会误删自动化环境里的数据。
// 交互式卸载会询问一次。
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    DataDir := ExpandConstant('{localappdata}\FaceGazeBridge');
    if DirExists(DataDir) and (not UninstallSilent) then
    begin
      if MsgBox('是否同时删除校准、输出预设、模块和日志？' + #13#10 + DataDir,
                mbConfirmation, MB_YESNO) = IDYES then
        DelTree(DataDir, True, True, True);
    end;
  end;
end;
