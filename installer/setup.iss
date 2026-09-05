; ============================================================================
; Emebalachat Installer - Inno Setup 6.x Script
; ============================================================================
; This script creates a professional Windows installer for Emebalachat.
; It downloads the AI translation model during installation and generates
; a runtime configuration file.
;
; Requirements:
;   - Inno Setup 6.1 or later (for CreateDownloadPage support)
;   - Build Emebalachat.exe with CMake before compiling this installer
; ============================================================================

; ------------------------------------------------------------------------
; [Setup] - Core installer configuration
; ------------------------------------------------------------------------
[Setup]
AppId={{E3B7A1C4-8D2F-4A6E-9C1B-5F0D3E8A7B2C}
; Display name rebranded; DefaultDirName/DefaultGroupName intentionally keep
; "Emebalachat" for upgrade-path continuity with existing installs (architect plan row #18).
AppName=Emebala Chat
AppVersion=0.10.0
AppPublisher=Team Sunplaza
DefaultDirName={autopf}\Emebalachat
DefaultGroupName=Emebalachat
OutputDir=output
OutputBaseFilename=Emebalachat_Setup_0.10.0
WizardStyle=modern
WizardSizePercent=110
Compression=lzma2/ultra64
SolidCompression=yes
PrivilegesRequired=admin
MinVersion=10.0
ExtraDiskSpaceRequired=2100000000
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Optional icon/image files - compile succeeds even if they don't exist
#ifexist "assets\icon.ico"
SetupIconFile=assets\icon.ico
#endif
#ifexist "assets\wizard_large.bmp"
WizardImageFile=assets\wizard_large.bmp
#endif
#ifexist "assets\wizard_small.bmp"
WizardSmallImageFile=assets\wizard_small.bmp
#endif

; ------------------------------------------------------------------------
; [Languages] - Installer UI languages
; ------------------------------------------------------------------------
[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"

; ------------------------------------------------------------------------
; [CustomMessages] - Localized strings for download and disk space logic
; ------------------------------------------------------------------------
[CustomMessages]
english.TaskAutoStart=Launch Emebala Chat automatically when Windows starts
korean.TaskAutoStart=Windows 시작 시 Emebala Chat 자동 실행

english.TaskDesktopIcon=Create a desktop shortcut
korean.TaskDesktopIcon=바탕화면에 바로가기 만들기

english.DownloadingModel=Downloading AI translation model...
korean.DownloadingModel=AI 번역 모델 다운로드 중...

english.DownloadingModelDesc=This may take several minutes...
korean.DownloadingModelDesc=인터넷 속도에 따라 몇 분 정도 걸릴 수 있습니다.

english.ModelAlreadyExists=AI model already exists. Skipping download.
korean.ModelAlreadyExists=AI 모델이 이미 설치되어 있습니다. 다운로드를 건너뜁니다.

english.DownloadFailed=Failed to download the AI model.
korean.DownloadFailed=AI 번역 모델 다운로드에 실패했습니다.

english.DownloadFailedDetail=You can still use Google Translate (free, online). The model can be downloaded later.
korean.DownloadFailedDetail=Google 번역(무료, 온라인)으로 계속 사용 가능합니다. 모델은 나중에 수동 다운로드 가능합니다.

english.DownloadRetry=Retry download?
korean.DownloadRetry=다운로드를 다시 시도하시겠습니까?

english.DiskSpaceWarning=At least 3 GB free space recommended. Current: %1 GB. Continue?
korean.DiskSpaceWarning=최소 3GB 여유 공간 필요. 현재: %1 GB. 계속?

; ------------------------------------------------------------------------
; [Tasks] - Optional user-selectable tasks
; ------------------------------------------------------------------------
[Tasks]
Name: "desktopicon"; Description: "{cm:TaskDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "autostart"; Description: "{cm:TaskAutoStart}"

; ------------------------------------------------------------------------
; [Files] - Files to install
; ------------------------------------------------------------------------
[Files]
Source: "..\build\Emebalachat.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; ------------------------------------------------------------------------
; [Icons] - Start Menu and Desktop shortcuts
; ------------------------------------------------------------------------
[Icons]
Name: "{group}\Emebala Chat"; Filename: "{app}\Emebalachat.exe"
Name: "{group}\Uninstall"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Emebala Chat"; Filename: "{app}\Emebalachat.exe"; Tasks: desktopicon

; ------------------------------------------------------------------------
; [Registry] - Auto-start entry (only if autostart task selected)
; ------------------------------------------------------------------------
[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Emebalachat"; ValueData: """{app}\Emebalachat.exe"""; Tasks: autostart; Flags: uninsdeletevalue

; ------------------------------------------------------------------------
; [UninstallDelete] - Clean up extra files on uninstall
; ------------------------------------------------------------------------
[UninstallDelete]
Type: filesandordirs; Name: "{app}\models"
Type: files; Name: "{app}\config.json"

; ------------------------------------------------------------------------
; [Run] - Post-install launch option
; ------------------------------------------------------------------------
[Run]
Filename: "{app}\Emebalachat.exe"; Description: "{cm:LaunchProgram,Emebala Chat}"; Flags: nowait postinstall skipifsilent

; ========================================================================
; [Code] - Pascal Script for custom installer logic
; ========================================================================
[Code]

const
  MODEL_URL = 'https://huggingface.co/tencent/Hy-MT2-1.8B-GGUF/resolve/main/Hy-MT2-1.8B-Q8_0.gguf';
  MODEL_FILENAME = 'Hy-MT2-1.8B-Q8_0.gguf';
  MIN_DISK_SPACE_MB = 3072; // 3 GB in MB

  // M2 (security): Pinned SHA-256 of the model file above. Empty string =
  // verification skipped (development builds). RELEASE PROCEDURE: before
  // shipping, compute the hash of the exact file hosted at MODEL_URL and
  // paste it here (hex only, no separators), e.g. on Windows:
  //   certutil -hashfile "Hy-MT2-1.8B-Q8_0.gguf" SHA256
  //   (or PowerShell: (Get-FileHash model.gguf -Algorithm SHA256).Hash)
  // When non-empty, the download page fails the download on mismatch and
  // VerifyDownloadedModel() re-checks the temp file before it is copied to
  // the models directory, so an attacker-influenced GGUF is never handed
  // to the llama.cpp parser.
  EXPECTED_MODEL_SHA256 = '';

var
  DownloadPage: TDownloadWizardPage;
  ModelSkipped: Boolean;

// ------------------------------------------------------------------------
// VerifyDownloadedModel - M2 integrity check of the downloaded temp file
// against EXPECTED_MODEL_SHA256. Returns True when no hash is pinned
// (empty constant = skip, documented above). GetSHA256OfFile is only part
// of the [Code] API on Inno Setup 6.3+, so on older compilers this defers
// to the RequiredSHA256OfFile check enforced natively by
// TDownloadWizardPage.Add below (supported since 6.0). May raise on file
// read errors - callers must wrap in try/except and treat it as failure.
// ------------------------------------------------------------------------
function VerifyDownloadedModel(const FilePath: String): Boolean;
var
  ActualHash: String;
begin
  Result := True;
  if EXPECTED_MODEL_SHA256 = '' then
  begin
    Log('EXPECTED_MODEL_SHA256 is empty - model integrity verification skipped (dev build).');
    Exit;
  end;
#if VER >= 0x06030000
  ActualHash := GetSHA256OfFile(FilePath);
  Result := SameText(ActualHash, EXPECTED_MODEL_SHA256);
  if Result then
    Log('Model SHA-256 verified: ' + ActualHash)
  else
    Log('MODEL SHA-256 MISMATCH - expected ' + EXPECTED_MODEL_SHA256 + ', got ' + ActualHash);
#else
  Log('Explicit post-download hash re-check not available on this Inno Setup version; relying on the download page RequiredSHA256OfFile verification.');
#endif
end;

// ------------------------------------------------------------------------
// Download progress callback - logs progress to the installer log
// ------------------------------------------------------------------------
function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  if ProgressMax <> 0 then
    Log(Format('  %s: %d of %d bytes downloaded', [FileName, Progress, ProgressMax]))
  else
    Log(Format('  %s: %d bytes downloaded', [FileName, Progress]));
  Result := True;
end;

// ------------------------------------------------------------------------
// InitializeWizard - Create the download page using built-in API
// ------------------------------------------------------------------------
procedure InitializeWizard();
begin
  DownloadPage := CreateDownloadPage(
    CustomMessage('DownloadingModel'),
    CustomMessage('DownloadingModelDesc'),
    @OnDownloadProgress
  );
  ModelSkipped := False;
end;

// ------------------------------------------------------------------------
// CheckDiskSpace - Warn the user if free space is below 3 GB
// Returns True if installation should proceed, False to stay on page
// ------------------------------------------------------------------------
function CheckDiskSpace(): Boolean;
var
  FreeSpaceMB: Cardinal;
  TotalSpaceMB: Cardinal;
  FreeSpaceGB: String;
begin
  Result := True;

  if GetSpaceOnDisk(ExtractFileDrive(WizardDirValue()), True, FreeSpaceMB, TotalSpaceMB) then
  begin
    if FreeSpaceMB < MIN_DISK_SPACE_MB then
    begin
      // Format free space as GB with one decimal place
      FreeSpaceGB := Format('%.1f', [FreeSpaceMB / 1024.0]);
      if MsgBox(FmtMessage(CustomMessage('DiskSpaceWarning'), [FreeSpaceGB]),
                mbConfirmation, MB_YESNO) = IDNO then
      begin
        Result := False;
      end;
    end;
  end;
end;

// ------------------------------------------------------------------------
// DownloadModel - Download the AI model with retry/skip/cancel logic
// ------------------------------------------------------------------------
procedure DownloadModel();
var
  ModelDestDir: String;
  ModelDestPath: String;
  ModelTmpPath: String;
  DownloadSuccess: Boolean;
  HashOk: Boolean;
  UserChoice: Integer;
begin
  ModelDestDir := ExpandConstant('{app}\models');
  ModelDestPath := ModelDestDir + '\' + MODEL_FILENAME;
  ModelTmpPath := ExpandConstant('{tmp}\') + MODEL_FILENAME;

  // Check if the model file already exists at the destination
  if FileExists(ModelDestPath) then
  begin
    Log('Model already exists at: ' + ModelDestPath);
    // M2: log-only hash check for pre-existing files. User data is never
    // deleted by the installer; a mismatch is surfaced in the setup log so
    // the user can re-download or manually verify (certutil -hashfile).
    try
      if not VerifyDownloadedModel(ModelDestPath) then
        Log('WARNING: pre-existing model file does not match EXPECTED_MODEL_SHA256. It was NOT touched by this installer.');
    except
      Log('WARNING: could not hash pre-existing model file: ' + GetExceptionMessage());
    end;
    MsgBox(CustomMessage('ModelAlreadyExists'), mbInformation, MB_OK);
    Exit;
  end;

  // Create the models directory if it does not exist
  if not DirExists(ModelDestDir) then
    ForceDirectories(ModelDestDir);

  // Download loop with retry support
  DownloadSuccess := False;
  while not DownloadSuccess do
  begin
    DownloadPage.Clear();
    // M2: pass the pinned SHA-256 as RequiredSHA256OfFile - the download page
    // aborts with an exception when the downloaded bytes do not match.
    DownloadPage.Add(MODEL_URL, MODEL_FILENAME, EXPECTED_MODEL_SHA256);
    DownloadPage.Show();
    try
      DownloadPage.Download();
      DownloadSuccess := True;
      Log('Download completed successfully.');
    except
      Log('Download failed: ' + GetExceptionMessage());
      DownloadSuccess := False;
    end;
    DownloadPage.Hide();

    if DownloadSuccess then
    begin
      // M2: defense-in-depth re-verification of the temp file BEFORE copying
      // it into {app}\models. A mismatch deletes the temp file and re-enters
      // the retry/skip/cancel flow, so an unverified model is never installed.
      try
        HashOk := VerifyDownloadedModel(ModelTmpPath);
      except
        Log('Model hash verification error: ' + GetExceptionMessage());
        HashOk := False;
      end;
      if not HashOk then
      begin
        DeleteFile(ModelTmpPath);
        DownloadSuccess := False;
      end;
    end;

    if DownloadSuccess then
    begin
      // Copy the downloaded file from temp to the models directory
      if FileCopy(ModelTmpPath, ModelDestPath, False) then
        Log('Model file copied to: ' + ModelDestPath)
      else
      begin
        Log('Failed to copy model file to destination.');
        MsgBox(CustomMessage('DownloadFailed'), mbError, MB_OK);
        ModelSkipped := True;
        Exit;
      end;
    end
    else
    begin
      // Download failed - offer Retry / Skip / Cancel
      UserChoice := MsgBox(
        CustomMessage('DownloadFailed') + #13#10#13#10 +
        CustomMessage('DownloadFailedDetail') + #13#10#13#10 +
        CustomMessage('DownloadRetry'),
        mbError,
        MB_YESNOCANCEL
      );

      case UserChoice of
        IDYES:
          begin
            // Retry - continue the while loop
            Log('User chose to retry the download.');
          end;
        IDNO:
          begin
            // Skip - proceed without the model
            Log('User chose to skip the model download.');
            ModelSkipped := True;
            Exit;
          end;
        IDCANCEL:
          begin
            // Cancel - abort the entire installation
            Log('User cancelled the installation during model download.');
            WizardForm.Close();
            Exit;
          end;
      end;
    end;
  end;
end;

// ------------------------------------------------------------------------
// CreateConfigFile - Generate config.json with proper settings
// ------------------------------------------------------------------------
procedure CreateConfigFile();
var
  ConfigPath: String;
  AppDir: String;
  ModelPath: String;
  Lines: TArrayOfString;
  EngineType: String;
begin
  ConfigPath := ExpandConstant('{app}\config.json');
  AppDir := ExpandConstant('{app}');
  ModelPath := AppDir + '\models\' + MODEL_FILENAME;

  // Choose engine type based on whether the model was downloaded
  if ModelSkipped then
    EngineType := 'google'
  else
    EngineType := 'auto';

  // Escape backslashes for JSON format
  StringChangeEx(AppDir, '\', '\\', True);
  StringChangeEx(ModelPath, '\', '\\', True);

  // Build JSON content matching Emebalachat config format
  SetArrayLength(Lines, 9);
  Lines[0] := '{';
  Lines[1] := '  "source_language": "Auto Detect",';
  Lines[2] := '  "target_language": "English",';
  Lines[3] := '  "engine_type": "' + EngineType + '",';
  Lines[4] := '  "model_path": "' + ModelPath + '",';
  Lines[5] := '  "auto_send": false,';
  Lines[6] := '  "badge_x": -1,';
  Lines[7] := '  "badge_y": -1';
  Lines[8] := '}';

  if SaveStringsToUTF8File(ConfigPath, Lines, False) then
    Log('Config file created at: ' + ConfigPath)
  else
    Log('Failed to create config file at: ' + ConfigPath);
end;

// ------------------------------------------------------------------------
// CurStepChanged - Trigger model download and config creation post-install
// ------------------------------------------------------------------------
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    DownloadModel();
    CreateConfigFile();
  end;
end;

// ------------------------------------------------------------------------
// NextButtonClick - Validate disk space when leaving the directory page
// ------------------------------------------------------------------------
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then
    Result := CheckDiskSpace();
end;

// ------------------------------------------------------------------------
// CurUninstallStepChanged - Clean up auto-start registry entry on uninstall
// ------------------------------------------------------------------------
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    RegDeleteValue(HKEY_CURRENT_USER,
      'Software\Microsoft\Windows\CurrentVersion\Run',
      'Emebalachat');
    Log('Auto-start registry entry removed.');
  end;
end;
