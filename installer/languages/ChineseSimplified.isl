; *** Inno Setup version 6.5.0+ Chinese (Simplified) messages ***
;
; To download user-contributed translations of this file, go to:
;   https://jrsoftware.org/files/istrans/
;
; Note: When translating this text, do not add periods (.) to the end of
; messages that didn't have them already, because on those messages Inno
; Setup adds the periods automatically (appending a period would result in
; two periods being displayed).

[LangOptions]
LanguageName=简体中文
LanguageID=$0804
LanguageCodePage=936

[Messages]

; *** Application titles
SetupAppTitle=安装
SetupWindowTitle=安装 - %1
UninstallAppTitle=卸载
UninstallAppFullTitle=%1 卸载

; *** Misc. common
InformationTitle=信息
ConfirmTitle=确认
ErrorTitle=错误

; *** SetupLdr messages
SetupLdrStartupMessage=现在将安装 %1。您想要继续吗？
LdrCannotCreateTemp=无法创建临时文件。安装程序已中止
LdrCannotExecTemp=无法执行临时目录中的文件。安装程序已中止
HelpTextNote=

; *** Startup error messages
LastErrorMessage=%1。%n%n错误 %2: %3
SetupFileMissing=安装目录中缺少文件 %1。请修正这个问题或者获取程序的新副本。
SetupFileCorrupt=安装文件已损坏。请获取程序的新副本。
SetupFileCorruptOrWrongVer=安装文件已损坏，或与此安装程序的版本不兼容。请修正这个问题或者获取程序的新副本。
InvalidParameter=在命令行传递了无效的参数：%n%n%1
SetupAlreadyRunning=安装程序已在运行。
WindowsVersionNotSupported=该程序不支持您的计算机运行的 Windows 版本。
WindowsServicePackRequired=该程序需要 %1 Service Pack %2 或更高版本。
NotOnThisPlatform=该程序将无法运行于 %1。
OnlyOnThisPlatform=该程序必须运行于 %1。
OnlyOnTheseArchitectures=该程序只能安装在为以下处理器体系结构设计的 Windows 版本中：%n%n%1
WinVersionTooLowError=该程序需要 %1 版本 %2 或更高。
WinVersionTooHighError=该程序不能安装在 %1 版本 %2 或更高。
AdminPrivilegesRequired=安装该程序时您必须以管理员身份登录。
PowerUserPrivilegesRequired=安装该程序时您必须以管理员或 Power Users 组成员身份登录。
SetupAppRunningError=安装程序检测到 %1 正在运行。%n%n请立即关闭它的所有实例，然后点击"确定"继续，或点击"取消"退出。
UninstallAppRunningError=卸载程序检测到 %1 正在运行。%n%n请立即关闭它的所有实例，然后点击"确定"继续，或点击"取消"退出。

; *** Startup questions
PrivilegesRequiredOverrideTitle=选择安装程序安装模式
PrivilegesRequiredOverrideInstruction=选择安装模式
PrivilegesRequiredOverrideText1=%1 可以为所有用户安装（需要管理员权限），或仅为您安装。
PrivilegesRequiredOverrideText2=%1 可以仅为您安装，或为所有用户安装（需要管理员权限）。
PrivilegesRequiredOverrideAllUsers=为所有用户安装(&A)
PrivilegesRequiredOverrideAllUsersRecommended=为所有用户安装(&A)（推荐）
PrivilegesRequiredOverrideCurrentUser=仅为我安装(&M)
PrivilegesRequiredOverrideCurrentUserRecommended=仅为我安装(&M)（推荐）

; *** Misc. errors
ErrorCreatingDir=安装程序无法创建目录"%1"
ErrorTooManyFilesInDir=无法在目录"%1"中创建文件，因为该目录包含太多文件

; *** Setup common messages
ExitSetupTitle=退出安装程序
ExitSetupMessage=安装未完成。如果您现在退出，程序将不会被安装。%n%n您可以在稍后重新运行安装程序以完成安装。%n%n退出安装程序吗？
AboutSetupMenuItem=关于安装程序(&A)...
AboutSetupTitle=关于安装程序
AboutSetupMessage=%1 版本 %2%n%3%n%n%1 主页：%n%4
AboutSetupNote=
TranslatorNote=

; *** Buttons
ButtonBack=< 上一步(&B)
ButtonNext=下一步(&N) >
ButtonInstall=安装(&I)
ButtonOK=确定
ButtonCancel=取消
ButtonYes=是(&Y)
ButtonYesToAll=全是(&A)
ButtonNo=否(&N)
ButtonNoToAll=全否(&O)
ButtonFinish=完成(&F)
ButtonBrowse=浏览(&B)...
ButtonWizardBrowse=浏览(&R)...
ButtonNewFolder=新建文件夹(&M)

; *** "Select Language" dialog messages
SelectLanguageTitle=选择安装语言
SelectLanguageLabel=选择安装时使用的语言。

; *** Common wizard text
ClickNext=点击"下一步"继续，或点击"取消"退出安装程序。
BeveledLabel=
BrowseDialogTitle=浏览文件夹
BrowseDialogLabel=在下面的列表中选择一个文件夹，然后点击"确定"。
NewFolderName=新建文件夹

; *** "Welcome" wizard page
WelcomeLabel1=欢迎使用 [name] 安装向导
WelcomeLabel2=即将在您的计算机上安装 [name/ver]。%n%n建议您在继续安装前关闭所有其他应用程序。

; *** "Password" wizard page
WizardPassword=密码
PasswordLabel1=此安装程序受密码保护。
PasswordLabel3=请提供密码，然后点击"下一步"继续。密码区分大小写。
PasswordEditLabel=密码(&P)：
IncorrectPassword=您输入的密码不正确。请重试。

; *** "License Agreement" wizard page
WizardLicense=许可协议
LicenseLabel=请在继续安装前阅读以下重要信息。
LicenseLabel3=请阅读下列许可协议。在继续安装前您必须接受此协议的条款。
LicenseAccepted=我接受协议(&A)
LicenseNotAccepted=我不接受协议(&D)

; *** "Information" wizard pages
WizardInfoBefore=信息
InfoBeforeLabel=请在继续安装前阅读以下重要信息。
InfoBeforeClickLabel=准备好继续安装后，点击"下一步"。
WizardInfoAfter=信息
InfoAfterLabel=请在继续安装前阅读以下重要信息。
InfoAfterClickLabel=准备好继续安装后，点击"下一步"。

; *** "User Information" wizard page
WizardUserInfo=用户信息
UserInfoDesc=请输入您的信息。
UserInfoName=用户名(&U)：
UserInfoOrg=组织(&O)：
UserInfoSerial=序列号(&S)：
UserInfoNameRequired=您必须输入一个名称。

; *** "Select Destination Location" wizard page
WizardSelectDir=选择目标位置
SelectDirDesc=您想将 [name] 安装在哪里？
SelectDirLabel3=安装程序将安装 [name] 到下列文件夹中。
SelectDirBrowseLabel=点击"下一步"继续。如果您想选择不同的文件夹，请点击"浏览"。
DiskSpaceGBLabel=至少需要 [gb] GB 的可用磁盘空间。
DiskSpaceMBLabel=至少需要 [mb] MB 的可用磁盘空间。
CannotInstallToNetworkDrive=安装程序不能安装到网络驱动器。
CannotInstallToUNCPath=安装程序不能安装到 UNC 路径。
InvalidPath=您必须输入带有驱动器号的完整路径；例如：%n%nC:\APP%n%n或下列格式的 UNC 路径：%n%n\server\share
InvalidDrive=您选择的驱动器或 UNC 共享不存在或无法访问。请重新选择。
DiskSpaceWarningTitle=磁盘空间不足
DiskSpaceWarning=安装程序至少需要 %1 KB 可用空间才能安装，但所选驱动器只有 %2 KB 可用。%n%n您仍要继续吗？
DirNameTooLong=文件夹名或路径太长。
InvalidDirName=文件夹名无效。
BadDirName32=文件夹名不能包含下列任何字符：%n%n%1
DirExistsTitle=文件夹已存在
DirExists=文件夹：%n%n%1%n%n已经存在。您仍想要安装到该文件夹吗？
DirDoesntExistTitle=文件夹不存在
DirDoesntExist=文件夹：%n%n%1%n%n不存在。您想要创建该文件夹吗？

; *** "Select Components" wizard page
WizardSelectComponents=选择组件
SelectComponentsDesc=您想安装哪些程序组件？
SelectComponentsLabel2=选择您想要安装的组件；清除您不想安装的组件。准备好后点击"下一步"继续。
FullInstallation=完全安装
CompactInstallation=精简安装
CustomInstallation=自定义安装
NoUninstallWarningTitle=组件已存在
NoUninstallWarning=安装程序检测到下列组件已安装在您的计算机中：%n%n%1%n%n取消选择这些组件将不会卸载它们。%n%n您仍要继续吗？
ComponentSize1=%1 KB
ComponentSize2=%1 MB
ComponentsDiskSpaceGBLabel=当前选择至少需要 [gb] GB 的磁盘空间。
ComponentsDiskSpaceMBLabel=当前选择至少需要 [mb] MB 的磁盘空间。

; *** "Select Additional Tasks" wizard page
WizardSelectTasks=选择附加任务
SelectTasksDesc=您想要安装程序执行哪些附加任务？
SelectTasksLabel2=选择您想要安装程序在安装 [name] 时执行的附加任务，然后点击"下一步"。

; *** "Select Start Menu Folder" wizard page
WizardSelectProgramGroup=选择开始菜单文件夹
SelectStartMenuFolderDesc=安装程序应该在哪里放置程序的快捷方式？
SelectStartMenuFolderLabel3=安装程序将在下列开始菜单文件夹中创建程序的快捷方式。
SelectStartMenuFolderBrowseLabel=点击"下一步"继续。如果您想选择不同的文件夹，请点击"浏览"。
MustEnterGroupName=您必须输入一个文件夹名。
GroupNameTooLong=文件夹名或路径太长。
InvalidGroupName=文件夹名无效。
BadGroupName=文件夹名不能包含下列任何字符：%n%n%1
NoProgramGroupCheck2=不创建开始菜单文件夹(&D)

; *** "Ready to Install" wizard page
WizardReady=准备安装
ReadyLabel1=安装程序现在准备开始在您的计算机上安装 [name]。
ReadyLabel2a=点击"安装"继续安装，如果您想重新查看或修改任何设置，请点击"上一步"。
ReadyLabel2b=点击"安装"继续安装。
ReadyMemoUserInfo=用户信息：
ReadyMemoDir=目标位置：
ReadyMemoType=安装类型：
ReadyMemoComponents=选定组件：
ReadyMemoGroup=开始菜单文件夹：
ReadyMemoTasks=附加任务：

; *** TDownloadWizardPage wizard page and DownloadTemporaryFile
DownloadingLabel2=正在下载文件...
ButtonStopDownload=停止下载(&S)
StopDownload=您确定要停止下载吗？
ErrorDownloadAborted=下载已中止
ErrorDownloadFailed=下载失败：%1 %2
ErrorDownloadSizeFailed=获取大小失败：%1 %2
ErrorProgress=无效的进度：%1 / %2
ErrorFileSize=文件大小无效：预期 %1，实际 %2

; *** TExtractionWizardPage wizard page and ExtractArchive
ExtractingLabel=正在提取文件...
ButtonStopExtraction=停止提取(&S)
StopExtraction=您确定要停止提取吗？
ErrorExtractionAborted=提取已中止
ErrorExtractionFailed=提取失败：%1

; *** Archive extraction failure details
ArchiveIncorrectPassword=密码不正确
ArchiveIsCorrupted=压缩包已损坏
ArchiveUnsupportedFormat=不支持的压缩包格式

; *** "Preparing to Install" wizard page
WizardPreparing=正在准备安装
PreparingDesc=安装程序正在准备在您的计算机上安装 [name]。
PreviousInstallNotCompleted=先前的程序安装或卸载未完成，需要您重启计算机以完成该安装。%n%n在重启计算机后，再次运行安装程序以完成 [name] 的安装。
CannotContinue=安装程序不能继续。请点击"取消"退出。
ApplicationsFound=以下应用程序正在使用将由安装程序更新的文件。建议您允许安装程序自动关闭这些应用程序。
ApplicationsFound2=以下应用程序正在使用将由安装程序更新的文件。建议您允许安装程序自动关闭这些应用程序。安装完成后，安装程序将尝试重新启动这些应用程序。
CloseApplications=自动关闭应用程序(&A)
DontCloseApplications=不关闭应用程序(&D)
ErrorCloseApplications=安装程序无法自动关闭所有应用程序。建议您在继续之前关闭使用需要由安装程序更新的文件的所有应用程序。
PrepareToInstallNeedsRestart=安装程序必须重新启动您的计算机。重新启动计算机后，再次运行安装程序以完成 [name] 的安装。%n%n您想要现在重新启动吗？

; *** "Installing" wizard page
WizardInstalling=正在安装
InstallingLabel=安装程序正在安装 [name] 到您的计算机，请稍候。

; *** "Setup Completed" wizard page
FinishedHeadingLabel=完成 [name] 安装向导
FinishedLabelNoIcons=安装程序已在您的计算机中安装了 [name]。
FinishedLabel=安装程序已在您的计算机中安装了 [name]。您可以通过已安装的快捷方式运行此应用程序。
ClickFinish=点击"完成"退出安装程序。
FinishedRestartLabel=为完成 [name] 的安装，安装程序必须重新启动您的计算机。要立即重启吗？
FinishedRestartMessage=为完成 [name] 的安装，安装程序必须重新启动您的计算机。%n%n要立即重启吗？
ShowReadmeCheck=是，我想查看 README 文件
YesRadio=是，现在重新启动计算机(&Y)
NoRadio=否，我稍后重新启动计算机(&N)
RunEntryExec=运行 %1
RunEntryShellExec=查看 %1

; *** "Setup Needs the Next Disk" stuff
ChangeDiskTitle=安装程序需要下一张磁盘
SelectDiskLabel2=请插入磁盘 %1 并点击"确定"。%n%n如果此磁盘上的文件可以在与下面显示的不同的文件夹中找到，请输入正确的路径或点击"浏览"。
PathLabel=路径(&P)：
FileNotInDir2=文件"%1"无法在"%2"中找到。请插入正确的磁盘或选择另一个文件夹。
SelectDirectoryLabel=请指定下一张磁盘的位置。

; *** Installation phase messages
SetupAborted=安装程序未完成安装。%n%n请修正这个问题并重新运行安装程序。
AbortRetryIgnoreSelectAction=选择操作
AbortRetryIgnoreRetry=重试(&T)
AbortRetryIgnoreIgnore=忽略错误并继续(&I)
AbortRetryIgnoreCancel=取消安装
RetryCancelSelectAction=选择操作
RetryCancelRetry=重试(&T)
RetryCancelCancel=取消

; *** Installation status messages
StatusClosingApplications=正在关闭应用程序...
StatusCreateDirs=正在创建目录...
StatusExtractFiles=正在提取文件...
StatusDownloadFiles=正在下载文件...
StatusCreateIcons=正在创建快捷方式...
StatusCreateIniEntries=正在创建 INI 条目...
StatusCreateRegistryEntries=正在创建注册表条目...
StatusRegisterFiles=正在注册文件...
StatusSavingUninstall=正在保存卸载信息...
StatusRunProgram=正在完成安装...
StatusRestartingApplications=正在重新启动应用程序...
StatusRollback=正在撤销更改...

; *** Misc. errors
ErrorInternal2=内部错误：%1
ErrorFunctionFailedNoCode=%1 失败
ErrorFunctionFailed=%1 失败；代码 %2
ErrorFunctionFailedWithMessage=%1 失败；代码 %2。%n%3
ErrorExecutingProgram=无法执行文件：%n%1

; *** Registry errors
ErrorRegOpenKey=打开注册表键时出错：%n%1\%2
ErrorRegCreateKey=创建注册表键时出错：%n%1\%2
ErrorRegWriteKey=写入注册表键时出错：%n%1\%2

; *** INI errors
ErrorIniEntry=在文件"%1"中创建 INI 条目时出错。

; *** File copying errors
FileAbortRetryIgnoreSkipNotRecommended=跳过此文件(&S)（不推荐）
FileAbortRetryIgnoreIgnoreNotRecommended=忽略错误并继续(&I)（不推荐）
SourceIsCorrupted=源文件已损坏
SourceDoesntExist=源文件"%1"不存在
SourceVerificationFailed=源文件验证失败：%1
VerificationSignatureDoesntExist=签名文件"%1"不存在
VerificationSignatureInvalid=签名文件"%1"无效
VerificationKeyNotFound=签名文件"%1"使用未知的密钥
VerificationFileNameIncorrect=文件名不正确
VerificationFileTagIncorrect=文件标记不正确
VerificationFileSizeIncorrect=文件大小不正确
VerificationFileHashIncorrect=文件哈希值不正确
ExistingFileReadOnly2=无法替换现有文件，因为它被标记为只读。
ExistingFileReadOnlyRetry=移除只读属性并重试(&R)
ExistingFileReadOnlyKeepExisting=保留现有文件(&K)
ErrorReadingExistingDest=尝试读取现有文件时出错：
FileExistsSelectAction=选择操作
FileExists2=文件已存在。
FileExistsOverwriteExisting=覆盖现有文件(&O)
FileExistsKeepExisting=保留现有文件(&K)
FileExistsOverwriteOrKeepAll=对后续冲突执行此操作(&D)
ExistingFileNewerSelectAction=选择操作
ExistingFileNewer2=现有文件比安装程序尝试安装的文件更新。
ExistingFileNewerOverwriteExisting=覆盖现有文件(&O)
ExistingFileNewerKeepExisting=保留现有文件(&K)（推荐）
ExistingFileNewerOverwriteOrKeepAll=对后续冲突执行此操作(&D)
ErrorChangingAttr=尝试更改现有文件属性时出错：
ErrorCreatingTemp=尝试在目标目录中创建文件时出错：
ErrorReadingSource=尝试读取源文件时出错：
ErrorCopying=尝试复制文件时出错：
ErrorDownloading=尝试下载文件时出错：
ErrorExtracting=尝试解压压缩包时出错：
ErrorReplacingExistingFile=尝试替换现有文件时出错：
ErrorRestartReplace=RestartReplace 失败：
ErrorRenamingTemp=尝试重命名目标目录中的文件时出错：
ErrorRegisterServer=无法注册 DLL/OCX：%1
ErrorRegSvr32Failed=RegSvr32 失败，退出代码 %1
ErrorRegisterTypeLib=无法注册类型库：%1

; *** Uninstall display name markings
UninstallDisplayNameMark=%1 (%2)
UninstallDisplayNameMarks=%1 (%2, %3)
UninstallDisplayNameMark32Bit=32 位
UninstallDisplayNameMark64Bit=64 位
UninstallDisplayNameMarkAllUsers=所有用户
UninstallDisplayNameMarkCurrentUser=当前用户

; *** Post-installation errors
ErrorOpeningReadme=尝试打开 README 文件时出错。
ErrorRestartingComputer=安装程序无法重新启动计算机。请手动执行。

; *** Uninstaller messages
UninstallNotFound=文件"%1"不存在。无法卸载。
UninstallOpenError=文件"%1"无法打开。无法卸载
UninstallUnsupportedVer=此版本的卸载程序无法识别卸载日志文件"%1"的格式。无法卸载
UninstallUnknownEntry=在卸载日志中遇到未知条目 (%1)
ConfirmUninstall=您确认要完全移除 %1 及其所有组件吗？
UninstallOnlyOnWin64=此安装只能在 64 位 Windows 上卸载。
OnlyAdminCanUninstall=此安装只能由具有管理员权限的用户卸载。
UninstallStatusLabel=正在从您的计算机中移除 %1，请稍候。
UninstalledAll=已顺利从您的计算机中移除 %1。
UninstalledMost=%1 卸载完成。%n%n某些元素无法移除。这些可以手动移除。
UninstalledAndNeedsRestart=为完成 %1 的卸载，必须重新启动您的计算机。%n%n您想要现在重新启动吗？
UninstallDataCorrupted="%1"文件已损坏。无法卸载

; *** Uninstallation phase messages
ConfirmDeleteSharedFileTitle=移除共享文件吗？
ConfirmDeleteSharedFile2=系统指示下列共享文件不再被任何程序使用。您想要卸载程序移除此共享文件吗？%n%n如果任何程序仍在使用此文件并且它被移除，这些程序可能无法正常运行。如果您不确定，请选择"否"。将文件保留在您的系统中不会造成任何损害。
SharedFileNameLabel=文件名：
SharedFileLocationLabel=位置：
WizardUninstalling=卸载状态
StatusUninstalling=正在卸载 %1...

; *** Shutdown block reasons
ShutdownBlockReasonInstallingApp=正在安装 %1。
ShutdownBlockReasonUninstallingApp=正在卸载 %1。

[CustomMessages]

NameAndVersion=%1 版本 %2
AdditionalIcons=附加快捷方式：
CreateDesktopIcon=创建桌面快捷方式(&D)
CreateQuickLaunchIcon=创建快速启动快捷方式(&Q)
ProgramOnTheWeb=%1 网站
UninstallProgram=卸载 %1
LaunchProgram=启动 %1
AssocFileExtension=将 %1 与 %2 文件扩展名建立关联(&A)
AssocingFileExtension=正在将 %1 与 %2 文件扩展名建立关联...
AutoStartProgramGroupDescription=启动：
AutoStartProgram=自动启动 %1
AddonHostProgramNotFound=您选择的文件夹中无法找到 %1。%n%n您确定要继续吗？

; F2 (security, session 260909_0002) model integrity prompts
ModelHashMismatchQuestion=现有模型文件未通过完整性（SHA-256）验证，可能已损坏或被篡改。%n%n选择“是”将删除该文件并重新下载经过验证的模型；选择“否”将保留现有文件并跳过下载。%n%n是否立即删除并重新下载？
ModelHashUnverifiableQuestion=无法对现有模型文件进行哈希校验以确认其完整性（读取错误）。文件可能已损坏或被其他程序锁定。%n%n选择“是”将删除该文件并重新下载经过验证的模型；选择“否”将保留现有文件并跳过下载。%n%n是否立即删除并重新下载？
ModelHashDeleteFailed=无法删除未经验证的模型文件（可能正被其他程序占用）。安装程序将保留该文件并继续；在通过验证之前，应用将拒绝加载该文件。
