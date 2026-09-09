; *** Inno Setup version 6.5.0+ Chinese (Traditional) messages ***
;
; To download user-contributed translations of this file, go to:
;   https://jrsoftware.org/files/istrans/
;
; Note: When translating this text, do not add periods (.) to the end of
; messages that didn't have them already, because on those messages Inno
; Setup adds the periods automatically (appending a period would result in
; two periods being displayed).

[LangOptions]
LanguageName=繁體中文
LanguageID=$0404
LanguageCodePage=950

[Messages]

; *** Application titles
SetupAppTitle=安裝
SetupWindowTitle=安裝 - %1
UninstallAppTitle=移除
UninstallAppFullTitle=%1 移除

; *** Misc. common
InformationTitle=資訊
ConfirmTitle=確認
ErrorTitle=錯誤

; *** SetupLdr messages
SetupLdrStartupMessage=這將會安裝 %1。您想要繼續嗎？
LdrCannotCreateTemp=無法建立暫存檔案。安裝程式已中止
LdrCannotExecTemp=無法執行暫存目錄中的檔案。安裝程式已中止
HelpTextNote=

; *** Startup error messages
LastErrorMessage=%1。%n%n錯誤 %2: %3
SetupFileMissing=安裝目錄中缺少檔案 %1。請修正這個問題，或是取得程式的新複本。
SetupFileCorrupt=安裝檔案已損毀。請取得程式的新複本。
SetupFileCorruptOrWrongVer=安裝檔案已損毀，或與此版本的安裝程式不相容。請修正這個問題，或是取得程式的新複本。
InvalidParameter=在命令列傳遞了無效的參數：%n%n%1
SetupAlreadyRunning=安裝程式已經在執行中。
WindowsVersionNotSupported=此程式不支援您電腦上執行的 Windows 版本。
WindowsServicePackRequired=此程式需要 %1 Service Pack %2 或更新版本。
NotOnThisPlatform=此程式將無法在 %1 上執行。
OnlyOnThisPlatform=此程式必須在 %1 上執行。
OnlyOnTheseArchitectures=此程式只能安裝在針對下列處理器架構設計的 Windows 版本中：%n%n%1
WinVersionTooLowError=此程式需要 %1 版本 %2 或更新版本。
WinVersionTooHighError=此程式無法安裝在 %1 版本 %2 或更新版本上。
AdminPrivilegesRequired=安裝此程式時，您必須以系統管理員身分登入。
PowerUserPrivilegesRequired=安裝此程式時，您必須以系統管理員或 Power Users 群組成員身分登入。
SetupAppRunningError=安裝程式偵測到 %1 正在執行中。%n%n請立即關閉它的所有執行個體，然後按「確定」繼續，或按「取消」結束。
UninstallAppRunningError=移除程式偵測到 %1 正在執行中。%n%n請立即關閉它的所有執行個體，然後按「確定」繼續，或按「取消」結束。

; *** Startup questions
PrivilegesRequiredOverrideTitle=選取安裝模式
PrivilegesRequiredOverrideInstruction=選取安裝模式
PrivilegesRequiredOverrideText1=%1 可以為所有使用者安裝（需要系統管理員權限），或是僅為您安裝。
PrivilegesRequiredOverrideText2=%1 可以僅為您安裝，或是為所有使用者安裝（需要系統管理員權限）。
PrivilegesRequiredOverrideAllUsers=為所有使用者安裝(&A)
PrivilegesRequiredOverrideAllUsersRecommended=為所有使用者安裝(&A)（建議）
PrivilegesRequiredOverrideCurrentUser=僅為我安裝(&M)
PrivilegesRequiredOverrideCurrentUserRecommended=僅為我安裝(&M)（建議）

; *** Misc. errors
ErrorCreatingDir=安裝程式無法建立目錄「%1」
ErrorTooManyFilesInDir=無法在目錄「%1」中建立檔案，因為該目錄包含太多檔案

; *** Setup common messages
ExitSetupTitle=結束安裝程式
ExitSetupMessage=安裝尚未完成。如果您現在結束，程式將不會被安裝。%n%n您可以在稍後重新執行安裝程式以完成安裝。%n%n要結束安裝程式嗎？
AboutSetupMenuItem=關於安裝程式(&A)...
AboutSetupTitle=關於安裝程式
AboutSetupMessage=%1 版本 %2%n%3%n%n%1 首頁：%n%4
AboutSetupNote=
TranslatorNote=

; *** Buttons
ButtonBack=< 上一步(&B)
ButtonNext=下一步(&N) >
ButtonInstall=安裝(&I)
ButtonOK=確定
ButtonCancel=取消
ButtonYes=是(&Y)
ButtonYesToAll=全部皆是(&A)
ButtonNo=否(&N)
ButtonNoToAll=全部皆否(&O)
ButtonFinish=完成(&F)
ButtonBrowse=瀏覽(&B)...
ButtonWizardBrowse=瀏覽(&R)...
ButtonNewFolder=建立新資料夾(&M)

; *** "Select Language" dialog messages
SelectLanguageTitle=選取安裝語言
SelectLanguageLabel=選取安裝期間要使用的語言。

; *** Common wizard text
ClickNext=按「下一步」繼續，或按「取消」結束安裝程式。
BeveledLabel=
BrowseDialogTitle=瀏覽資料夾
BrowseDialogLabel=在下方清單中選取資料夾，然後按「確定」。
NewFolderName=新資料夾

; *** "Welcome" wizard page
WelcomeLabel1=歡迎使用 [name] 安裝精靈
WelcomeLabel2=這將會在您的電腦上安裝 [name/ver]。%n%n建議您在繼續之前先關閉所有其他應用程式。

; *** "Password" wizard page
WizardPassword=密碼
PasswordLabel1=此安裝受密碼保護。
PasswordLabel3=請提供密碼，然後按「下一步」繼續。密碼區分大小寫。
PasswordEditLabel=密碼(&P)：
IncorrectPassword=您輸入的密碼不正確。請再試一次。

; *** "License Agreement" wizard page
WizardLicense=授權合約
LicenseLabel=請在繼續之前閱讀下列重要資訊。
LicenseLabel3=請閱讀下列授權合約。在繼續安裝之前，您必須接受這份合約的條款。
LicenseAccepted=我同意合約條款(&A)
LicenseNotAccepted=我不同意合約條款(&D)

; *** "Information" wizard pages
WizardInfoBefore=資訊
InfoBeforeLabel=請在繼續之前閱讀下列重要資訊。
InfoBeforeClickLabel=當您準備好繼續安裝時，請按「下一步」。
WizardInfoAfter=資訊
InfoAfterLabel=請在繼續之前閱讀下列重要資訊。
InfoAfterClickLabel=當您準備好繼續安裝時，請按「下一步」。

; *** "User Information" wizard page
WizardUserInfo=使用者資訊
UserInfoDesc=請輸入您的資訊。
UserInfoName=使用者名稱(&U)：
UserInfoOrg=組織(&O)：
UserInfoSerial=序號(&S)：
UserInfoNameRequired=您必須輸入名稱。

; *** "Select Destination Location" wizard page
WizardSelectDir=選取目的地位址
SelectDirDesc=您要將 [name] 安裝在哪裡？
SelectDirLabel3=安裝程式將會把 [name] 安裝到下列資料夾中。
SelectDirBrowseLabel=若要繼續，請按「下一步」。如果您想要選取其他資料夾，請按「瀏覽」。
DiskSpaceGBLabel=至少需要 [gb] GB 的可用磁碟空間。
DiskSpaceMBLabel=至少需要 [mb] MB 的可用磁碟空間。
CannotInstallToNetworkDrive=安裝程式無法安裝到網路磁碟機。
CannotInstallToUNCPath=安裝程式無法安裝到 UNC 路徑。
InvalidPath=您必須輸入包含磁碟機代號的完整路徑；例如：%n%nC:\APP%n%n或是下列格式的 UNC 路徑：%n%n\server\share
InvalidDrive=您選取的磁碟機或 UNC 共用不存在或無法存取。請選取另一個。
DiskSpaceWarningTitle=磁碟空間不足
DiskSpaceWarning=安裝程式至少需要 %1 KB 的可用空間才能安裝，但選取的磁碟機只有 %2 KB 可用。%n%n您無論如何都要繼續嗎？
DirNameTooLong=資料夾名稱或路徑太長。
InvalidDirName=資料夾名稱無效。
BadDirName32=資料夾名稱不能包含下列任何字元：%n%n%1
DirExistsTitle=資料夾已存在
DirExists=資料夾：%n%n%1%n%n已經存在。您無論如何都要安裝到該資料夾嗎？
DirDoesntExistTitle=資料夾不存在
DirDoesntExist=資料夾：%n%n%1%n%n不存在。您要建立該資料夾嗎？

; *** "Select Components" wizard page
WizardSelectComponents=選取元件
SelectComponentsDesc=應該安裝哪些元件？
SelectComponentsLabel2=選取您想要安裝的元件；清除您不想安裝的元件。準備好繼續時，請按「下一步」。
FullInstallation=完整安裝
CompactInstallation=精簡安裝
CustomInstallation=自訂安裝
NoUninstallWarningTitle=元件已存在
NoUninstallWarning=安裝程式偵測到下列元件已經安裝在您的電腦上：%n%n%1%n%n取消選取這些元件將不會將它們移除。%n%n您無論如何都要繼續嗎？
ComponentSize1=%1 KB
ComponentSize2=%1 MB
ComponentsDiskSpaceGBLabel=目前的選取項目至少需要 [gb] GB 的磁碟空間。
ComponentsDiskSpaceMBLabel=目前的選取項目至少需要 [mb] MB 的磁碟空間。

; *** "Select Additional Tasks" wizard page
WizardSelectTasks=選取附加工作
SelectTasksDesc=應該執行哪些附加工作？
SelectTasksLabel2=選取您想要安裝程式在安裝 [name] 時執行的附加工作，然後按「下一步」。

; *** "Select Start Menu Folder" wizard page
WizardSelectProgramGroup=選取開始功能表資料夾
SelectStartMenuFolderDesc=安裝程式應該將程式的捷徑放置在哪裡？
SelectStartMenuFolderLabel3=安裝程式將在下列開始功能表資料夾中建立程式捷徑。
SelectStartMenuFolderBrowseLabel=若要繼續，請按「下一步」。如果您想要選取其他資料夾，請按「瀏覽」。
MustEnterGroupName=您必須輸入資料夾名稱。
GroupNameTooLong=資料夾名稱或路徑太長。
InvalidGroupName=資料夾名稱無效。
BadGroupName=資料夾名稱不能包含下列任何字元：%n%n%1
NoProgramGroupCheck2=不要建立開始功能表資料夾(&D)

; *** "Ready to Install" wizard page
WizardReady=準備安裝
ReadyLabel1=安裝程式現在已準備好開始在您的電腦上安裝 [name]。
ReadyLabel2a=按「安裝」繼續進行安裝，如果您想檢閱或變更任何設定，請按「上一步」。
ReadyLabel2b=按「安裝」繼續進行安裝。
ReadyMemoUserInfo=使用者資訊：
ReadyMemoDir=目的地位址：
ReadyMemoType=安裝類型：
ReadyMemoComponents=選取的元件：
ReadyMemoGroup=開始功能表資料夾：
ReadyMemoTasks=附加工作：

; *** TDownloadWizardPage wizard page and DownloadTemporaryFile
DownloadingLabel2=正在下載檔案...
ButtonStopDownload=停止下載(&S)
StopDownload=您確定要停止下載嗎？
ErrorDownloadAborted=下載已中止
ErrorDownloadFailed=下載失敗：%1 %2
ErrorDownloadSizeFailed=取得大小失敗：%1 %2
ErrorProgress=進度無效：%1 / %2
ErrorFileSize=檔案大小無效：預期為 %1，實際為 %2

; *** TExtractionWizardPage wizard page and ExtractArchive
ExtractingLabel=正在解壓縮檔案...
ButtonStopExtraction=停止解壓縮(&S)
StopExtraction=您確定要停止解壓縮嗎？
ErrorExtractionAborted=解壓縮已中止
ErrorExtractionFailed=解壓縮失敗：%1

; *** Archive extraction failure details
ArchiveIncorrectPassword=密碼不正確
ArchiveIsCorrupted=壓縮檔已損毀
ArchiveUnsupportedFormat=不支援的壓縮檔格式

; *** "Preparing to Install" wizard page
WizardPreparing=正在準備安裝
PreparingDesc=安裝程式正在準備在您的電腦上安裝 [name]。
PreviousInstallNotCompleted=先前程式的安裝/移除尚未完成。您需要重新啟動電腦才能完成該安裝。%n%n重新啟動電腦後，請再次執行安裝程式以完成 [name] 的安裝。
CannotContinue=安裝程式無法繼續。請按「取消」結束。
ApplicationsFound=下列應用程式正在使用需要由安裝程式更新的檔案。建議您允許安裝程式自動關閉這些應用程式。
ApplicationsFound2=下列應用程式正在使用需要由安裝程式更新的檔案。建議您允許安裝程式自動關閉這些應用程式。安裝完成後，安裝程式將會嘗試重新啟動這些應用程式。
CloseApplications=自動關閉應用程式(&A)
DontCloseApplications=不要關閉應用程式(&D)
ErrorCloseApplications=安裝程式無法自動關閉所有的應用程式。建議您在繼續之前關閉所有正在使用需要由安裝程式更新的檔案之應用程式。
PrepareToInstallNeedsRestart=安裝程式必須重新啟動您的電腦。重新啟動電腦後，請再次執行安裝程式以完成 [name] 的安裝。%n%n您現在要重新啟動嗎？

; *** "Installing" wizard page
WizardInstalling=正在安裝
InstallingLabel=安裝程式正在您的電腦上安裝 [name]，請稍候。

; *** "Setup Completed" wizard page
FinishedHeadingLabel=正在完成 [name] 安裝精靈
FinishedLabelNoIcons=安裝程式已在您的電腦上完成 [name] 的安裝。
FinishedLabel=安裝程式已在您的電腦上完成 [name] 的安裝。可透過選取已安裝的捷徑來啟動應用程式。
ClickFinish=按「完成」結束安裝程式。
FinishedRestartLabel=為完成 [name] 的安裝，安裝程式必須重新啟動您的電腦。您現在要重新啟動嗎？
FinishedRestartMessage=為完成 [name] 的安裝，安裝程式必須重新啟動您的電腦。%n%n您現在要重新啟動嗎？
ShowReadmeCheck=是，我想檢視讀我檔案 (README)
YesRadio=是，現在重新啟動電腦(&Y)
NoRadio=否，我稍後再重新啟動電腦(&N)
RunEntryExec=執行 %1
RunEntryShellExec=檢視 %1

; *** "Setup Needs the Next Disk" stuff
ChangeDiskTitle=安裝程式需要下一張磁片
SelectDiskLabel2=請插入磁片 %1 並按「確定」。%n%n如果此磁片上的檔案可以在下方顯示的資料夾以外的資料夾中找到，請輸入正確的路徑或按「瀏覽」。
PathLabel=路徑(&P)：
FileNotInDir2=在「%2」中找不到檔案「%1」。請插入正確的磁片或選取其他資料夾。
SelectDirectoryLabel=請指定下一張磁片的位置。

; *** Installation phase messages
SetupAborted=安裝尚未完成。%n%n請修正問題並再次執行安裝程式。
AbortRetryIgnoreSelectAction=選取動作
AbortRetryIgnoreRetry=再試一次(&T)
AbortRetryIgnoreIgnore=忽略錯誤並繼續(&I)
AbortRetryIgnoreCancel=取消安裝
RetryCancelSelectAction=選取動作
RetryCancelRetry=再試一次(&T)
RetryCancelCancel=取消

; *** Installation status messages
StatusClosingApplications=正在關閉應用程式...
StatusCreateDirs=正在建立目錄...
StatusExtractFiles=正在解壓縮檔案...
StatusDownloadFiles=正在下載檔案...
StatusCreateIcons=正在建立捷徑...
StatusCreateIniEntries=正在建立 INI 項目...
StatusCreateRegistryEntries=正在建立登錄項目...
StatusRegisterFiles=正在註冊檔案...
StatusSavingUninstall=正在儲存移除資訊...
StatusRunProgram=正在完成安裝...
StatusRestartingApplications=正在重新啟動應用程式...
StatusRollback=正在回復變更...

; *** Misc. errors
ErrorInternal2=內部錯誤：%1
ErrorFunctionFailedNoCode=%1 失敗
ErrorFunctionFailed=%1 失敗；代碼 %2
ErrorFunctionFailedWithMessage=%1 失敗；代碼 %2。%n%3
ErrorExecutingProgram=無法執行檔案：%n%1

; *** Registry errors
ErrorRegOpenKey=開啟登錄機碼時發生錯誤：%n%1\%2
ErrorRegCreateKey=建立登錄機碼時發生錯誤：%n%1\%2
ErrorRegWriteKey=寫入登錄機碼時發生錯誤：%n%1\%2

; *** INI errors
ErrorIniEntry=在檔案「%1」中建立 INI 項目時發生錯誤。

; *** File copying errors
FileAbortRetryIgnoreSkipNotRecommended=略過此檔案(&S)（不建議）
FileAbortRetryIgnoreIgnoreNotRecommended=忽略錯誤並繼續(&I)（不建議）
SourceIsCorrupted=來源檔案已損毀
SourceDoesntExist=來源檔案「%1」不存在
SourceVerificationFailed=來源檔案驗證失敗：%1
VerificationSignatureDoesntExist=簽章檔案「%1」不存在
VerificationSignatureInvalid=簽章檔案「%1」無效
VerificationKeyNotFound=簽章檔案「%1」使用未知的金鑰
VerificationFileNameIncorrect=檔案名稱不正確
VerificationFileTagIncorrect=檔案標籤不正確
VerificationFileSizeIncorrect=檔案大小不正確
VerificationFileHashIncorrect=檔案雜湊值不正確
ExistingFileReadOnly2=無法取代現有檔案，因為它已標記為唯讀。
ExistingFileReadOnlyRetry=移除唯讀屬性並重試(&R)
ExistingFileReadOnlyKeepExisting=保留現有檔案(&K)
ErrorReadingExistingDest=嘗試讀取現有檔案時發生錯誤：
FileExistsSelectAction=選取動作
FileExists2=檔案已經存在。
FileExistsOverwriteExisting=覆寫現有檔案(&O)
FileExistsKeepExisting=保留現有檔案(&K)
FileExistsOverwriteOrKeepAll=對後續衝突套用此動作(&D)
ExistingFileNewerSelectAction=選取動作
ExistingFileNewer2=現有檔案比安裝程式嘗試安裝的檔案更新。
ExistingFileNewerOverwriteExisting=覆寫現有檔案(&O)
ExistingFileNewerKeepExisting=保留現有檔案(&K)（建議）
ExistingFileNewerOverwriteOrKeepAll=對後續衝突套用此動作(&D)
ErrorChangingAttr=嘗試變更現有檔案屬性時發生錯誤：
ErrorCreatingTemp=嘗試在目的地目錄中建立檔案時發生錯誤：
ErrorReadingSource=嘗試讀取來源檔案時發生錯誤：
ErrorCopying=嘗試複製檔案時發生錯誤：
ErrorDownloading=嘗試下載檔案時發生錯誤：
ErrorExtracting=嘗試解壓縮壓縮檔時發生錯誤：
ErrorReplacingExistingFile=嘗試取代現有檔案時發生錯誤：
ErrorRestartReplace=RestartReplace 失敗：
ErrorRenamingTemp=嘗試重新命名目的地目錄中的檔案時發生錯誤：
ErrorRegisterServer=無法註冊 DLL/OCX：%1
ErrorRegSvr32Failed=RegSvr32 失敗，結束代碼 %1
ErrorRegisterTypeLib=無法註冊型別程式庫：%1

; *** Uninstall display name markings
UninstallDisplayNameMark=%1 (%2)
UninstallDisplayNameMarks=%1 (%2, %3)
UninstallDisplayNameMark32Bit=32 位元
UninstallDisplayNameMark64Bit=64 位元
UninstallDisplayNameMarkAllUsers=所有使用者
UninstallDisplayNameMarkCurrentUser=目前使用者

; *** Post-installation errors
ErrorOpeningReadme=嘗試開啟讀我檔案 (README) 時發生錯誤。
ErrorRestartingComputer=安裝程式無法重新啟動電腦。請手動執行此操作。

; *** Uninstaller messages
UninstallNotFound=檔案「%1」不存在。無法移除。
UninstallOpenError=無法開啟檔案「%1」。無法移除
UninstallUnsupportedVer=此版本的移除程式無法辨識移除記錄檔「%1」的格式。無法移除
UninstallUnknownEntry=在移除記錄檔中遇到不明的項目 (%1)
ConfirmUninstall=您確定要完全移除 %1 及其所有元件嗎？
UninstallOnlyOnWin64=此安裝只能在 64 位元 Windows 上移除。
OnlyAdminCanUninstall=此安裝只能由具有系統管理員權限的使用者移除。
UninstallStatusLabel=正在從您的電腦中移除 %1，請稍候。
UninstalledAll=已成功從您的電腦中移除 %1。
UninstalledMost=%1 移除完成。%n%n某些元素無法移除。這些可以手動移除。
UninstalledAndNeedsRestart=為完成 %1 的移除，必須重新啟動您的電腦。%n%n您現在要重新啟動嗎？
UninstallDataCorrupted=「%1」檔案已損毀。無法移除

; *** Uninstallation phase messages
ConfirmDeleteSharedFileTitle=要移除共用檔案嗎？
ConfirmDeleteSharedFile2=系統表示下列共用檔案已不再被任何程式使用。您要讓移除程式移除此共用檔案嗎？%n%n如果有任何程式仍在使用此檔案且將其移除，那些程式可能無法正常運作。如果您不確定，請選擇「否」。將檔案保留在您的系統中不會造成任何危害。
SharedFileNameLabel=檔案名稱：
SharedFileLocationLabel=位置：
WizardUninstalling=移除狀態
StatusUninstalling=正在移除 %1...

; *** Shutdown block reasons
ShutdownBlockReasonInstallingApp=正在安裝 %1。
ShutdownBlockReasonUninstallingApp=正在移除 %1。

[CustomMessages]

NameAndVersion=%1 版本 %2
AdditionalIcons=附加捷徑：
CreateDesktopIcon=建立桌面捷徑(&D)
CreateQuickLaunchIcon=建立快速啟動捷徑(&Q)
ProgramOnTheWeb=%1 網站
UninstallProgram=移除 %1
LaunchProgram=啟動 %1
AssocFileExtension=將 %1 與 %2 檔案副檔名建立關聯(&A)
AssocingFileExtension=正在將 %1 與 %2 檔案副檔名建立關聯...
AutoStartProgramGroupDescription=啟動：
AutoStartProgram=自動啟動 %1
AddonHostProgramNotFound=在您選取的資料夾中找不到 %1。%n%n您無論如何都要繼續嗎？

; F2 (security, session 260909_0002) model integrity prompts
ModelHashMismatchQuestion=現有模型檔案未通過完整性（SHA-256）驗證，可能已損壞或被竄改。%n%n選擇「是」將刪除該檔案並重新下載經過驗證的模型；選擇「否」將保留現有檔案並略過下載。%n%n是否立即刪除並重新下載？
ModelHashUnverifiableQuestion=無法對現有模型檔案進行雜湊校驗以確認其完整性（讀取錯誤）。檔案可能已損壞或被其他程式鎖定。%n%n選擇「是」將刪除該檔案並重新下載經過驗證的模型；選擇「否」將保留現有檔案並略過下載。%n%n是否立即刪除並重新下載？
ModelHashDeleteFailed=無法刪除未經驗證的模型檔案（可能正被其他程式佔用）。安裝程式將保留該檔案並繼續；在通過驗證之前，應用程式將拒絕載入該檔案。
