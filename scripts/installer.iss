; DeskOrganization 安装包定义，由 scripts\package.cmd 调用 Inno Setup 编译。
; 版本号从命令行 /DAppVersion 传入，不在这里重复维护。

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId=DeskOrganization
AppName=DeskOrganization
AppVersion={#AppVersion}
AppPublisher=くじょう しおり
DefaultDirName={autopf}/DeskOrganization
DefaultGroupName=DeskOrganization
DisableProgramGroupPage=yes
DisableWelcomePage=no
OutputBaseFilename=DeskOrganization-{#AppVersion}-win-x64-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 程序平时只待在托盘里，没有可关的窗口，所以交给安装向导去结束它，不要装完自动重启
CloseApplications=yes
RestartApplications=no
; 默认装到当前用户的 Programs 目录，不弹 UAC；需要给所有用户装时在向导里可以切换
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\DeskOrganization.exe
; 安装向导自己的图标也用它，和 exe 里那份是同一个文件
SetupIconFile=..\resources\DeskOrganization.ico
LicenseFile=..\LICENSE

[Files]
Source: ..\out\release\DeskOrganization.exe; DestDir: {app}; Flags: ignoreversion
Source: ..\README.md; DestDir: {app}; Flags: ignoreversion
Source: ..\LICENSE; DestDir: {app}; Flags: ignoreversion

[Icons]
Name: {group}\DeskOrganization; Filename: {app}\DeskOrganization.exe

[Tasks]
Name: autostart; Description: 开机后自动启动（程序常驻托盘）; GroupDescription: 附加任务:; Flags: unchecked

[Registry]
; 开机自启走 HKCU 的 Run 键，卸载时只删自己写的那一项
Root: HKCU; Subkey: Software\Microsoft\Windows\CurrentVersion\Run; ValueType: string; \
  ValueName: DeskOrganization; ValueData: """{app}\DeskOrganization.exe"""; Tasks: autostart; \
  Flags: uninsdeletevalue

[Run]
; 装完立刻启动，桌面整理这种东西装上就想知道效果
Filename: {app}\DeskOrganization.exe; Description: 立即启动 DeskOrganization; Flags: nowait postinstall skipifsilent
