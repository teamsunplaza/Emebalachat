; ============================================================================
; Emebalachat Installer - Inno Setup 6.x Script
; ============================================================================
; This script creates a professional Windows installer for Emebalachat.
; It downloads the AI translation model during installation and generates
; a runtime configuration file.
;
; Requirements:
;   - Inno Setup 6.3 or later (for CreateDownloadPage support AND for correct
;     UTF-8 decoding of the BOM-less official .isl translation files - see the
;     compiler-version gate below and tools/check_installer_encoding.py)
;   - Build Emebalachat.exe with CMake before compiling this installer
; ============================================================================

; ------------------------------------------------------------------------
; Encoding safety gate (session 260910_0005)
; ------------------------------------------------------------------------
; Every text input this compiler consumes is UTF-8:
;   * this .iss and the two project .isl files carry a UTF-8 BOM, so they are
;     decoded as UTF-8 by every Inno Setup 6.x compiler;
;   * the 31 official translation .isl files bundled with Inno Setup ship
;     WITHOUT a BOM by design (their BOMs were deliberately removed in 6.5).
;     BOM-less UTF-8 decoding for .iss/.isl was only introduced in 6.3;
;     older compilers silently fall back to the system ANSI code page, which
;     garbles every non-Latin language (Korean/Japanese/Chinese/Russian/
;     Arabic/Hebrew/Thai/Ukrainian/Bulgarian/Armenian/Tamil ...).
; End-user locale is irrelevant after compilation (strings become UTF-16 in
; the setup binary), so compile-time decoding is the only exposure. Refuse
; to build on an old compiler instead of shipping mojibake:
#if VER < 0x06030000
  #error This script requires Inno Setup 6.3 or later: BOM-less official .isl files are decoded as UTF-8 only since 6.3 (older compilers mis-decode them as system ANSI and garble all non-Latin languages).
#endif

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

; Mutex namespace must stay in sync with src/main.cpp CreateMutexW (audit
; Blocker 4, CWE-284: Local\ = per-session single instance; Global\ let any
; other user's session deny launches on multi-user/RDP machines).
AppMutex=Local\Emebalachat_SingleInstance
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\Emebala_chat.exe

; Optional icon/image files - compile succeeds even if they don't exist
SetupIconFile=..\assets\Emebala_Chat_Appicon.ico
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
; B-1 (session 260910_0002): expanded from 5 to 32 registered languages.
; Architect plan counted "31" but its own explicit order lists 32 entries
; (27 additional official ISL + the original 5); every file below was
; verified present in the local Inno Setup 6.5+ compiler:Languages\ folder.
; chinesesimplified/chinesetraditional intentionally keep the local
; installer\languages\ ISL files (unchanged, per plan constraint).
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "chinesesimplified"; MessagesFile: "languages\ChineseSimplified.isl"
Name: "chinesetraditional"; MessagesFile: "languages\ChineseTraditional.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "portuguese"; MessagesFile: "compiler:Languages\Portuguese.isl"
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "dutch"; MessagesFile: "compiler:Languages\Dutch.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"
Name: "polish"; MessagesFile: "compiler:Languages\Polish.isl"
Name: "ukrainian"; MessagesFile: "compiler:Languages\Ukrainian.isl"
Name: "arabic"; MessagesFile: "compiler:Languages\Arabic.isl"
Name: "hebrew"; MessagesFile: "compiler:Languages\Hebrew.isl"
Name: "swedish"; MessagesFile: "compiler:Languages\Swedish.isl"
Name: "norwegian"; MessagesFile: "compiler:Languages\Norwegian.isl"
Name: "danish"; MessagesFile: "compiler:Languages\Danish.isl"
Name: "finnish"; MessagesFile: "compiler:Languages\Finnish.isl"
Name: "thai"; MessagesFile: "compiler:Languages\Thai.isl"
Name: "czech"; MessagesFile: "compiler:Languages\Czech.isl"
Name: "hungarian"; MessagesFile: "compiler:Languages\Hungarian.isl"
Name: "catalan"; MessagesFile: "compiler:Languages\Catalan.isl"
Name: "bulgarian"; MessagesFile: "compiler:Languages\Bulgarian.isl"
Name: "slovak"; MessagesFile: "compiler:Languages\Slovak.isl"
Name: "slovenian"; MessagesFile: "compiler:Languages\Slovenian.isl"
Name: "corsican"; MessagesFile: "compiler:Languages\Corsican.isl"
Name: "armenian"; MessagesFile: "compiler:Languages\Armenian.isl"
Name: "tamil"; MessagesFile: "compiler:Languages\Tamil.isl"

; ------------------------------------------------------------------------
; [CustomMessages] - Localized strings for download and disk space logic
; ------------------------------------------------------------------------
; B-1 (session 260910_0002, architect R-1): keys are translated for the latin-
; alphabet languages (spanish/portuguese/brazilianportuguese/french/german/
; italian) in addition to the original 5. All other registered languages
; intentionally define NO custom entries here: Inno Setup automatically falls
; back to the english definition of a {cm:...} key missing in the active
; language, which avoids the ANSI-codepage mojibake risk of non-latin custom
; strings. The built-in keys used below ({cm:AdditionalIcons}, {cm:UninstallProgram},
; {cm:LaunchProgram}) are standard messages translated by every official ISL.
[CustomMessages]
english.TaskAutoStart=Launch Emebala Chat automatically when Windows starts
korean.TaskAutoStart=Windows 시작 시 Emebala Chat 자동 실행
japanese.TaskAutoStart=Windows 起動時にエメバラチャットを自動起動する
chinesesimplified.TaskAutoStart=Windows 启动时自动运行埃梅巴拉 翻译
chinesetraditional.TaskAutoStart=Windows 啟動時自動執行埃梅巴拉 翻譯

spanish.TaskAutoStart=Iniciar Emebala Chat automáticamente al iniciar Windows
portuguese.TaskAutoStart=Iniciar o Emebala Chat automaticamente ao iniciar o Windows
brazilianportuguese.TaskAutoStart=Iniciar o Emebala Chat automaticamente quando o Windows for iniciado
french.TaskAutoStart=Lancer Emebala Chat automatiquement au démarrage de Windows
german.TaskAutoStart=Emebala Chat automatisch mit Windows starten
italian.TaskAutoStart=Avvia Emebala Chat automaticamente all'avvio di Windows

english.TaskDesktopIcon=Create a desktop shortcut
korean.TaskDesktopIcon=바탕화면에 바로가기 만들기
japanese.TaskDesktopIcon=デスクトップにショートカットを作成する
chinesesimplified.TaskDesktopIcon=创建桌面快捷方式
chinesetraditional.TaskDesktopIcon=建立桌面捷徑

spanish.TaskDesktopIcon=Crear un acceso directo en el escritorio
portuguese.TaskDesktopIcon=Criar um atalho no ambiente de trabalho
brazilianportuguese.TaskDesktopIcon=Criar um atalho na área de trabalho
french.TaskDesktopIcon=Créer un raccourci sur le Bureau
german.TaskDesktopIcon=Verknüpfung auf dem Desktop erstellen
italian.TaskDesktopIcon=Crea una scorciatoia sul desktop

english.DownloadingModel=Downloading AI translation model...
korean.DownloadingModel=AI 번역 모델 다운로드 중...
japanese.DownloadingModel=AI翻訳モデルをダウンロード中...
chinesesimplified.DownloadingModel=正在下载AI翻译模型...
chinesetraditional.DownloadingModel=正在下載AI翻譯模型...

spanish.DownloadingModel=Descargando el modelo de traducción de IA...
portuguese.DownloadingModel=A transferir o modelo de tradução por IA...
brazilianportuguese.DownloadingModel=Baixando o modelo de tradução de IA...
french.DownloadingModel=Téléchargement du modèle de traduction IA en cours...
german.DownloadingModel=KI-Übersetzungsmodell wird heruntergeladen...
italian.DownloadingModel=Download del modello di traduzione IA in corso...

english.DownloadingModelDesc=This may take several minutes...
korean.DownloadingModelDesc=인터넷 속도에 따라 몇 분 정도 걸릴 수 있습니다.
japanese.DownloadingModelDesc=インターネット速度によって数分かかる場合があります...
chinesesimplified.DownloadingModelDesc=根据网速可能需要几分钟...
chinesetraditional.DownloadingModelDesc=根據網速可能需要幾分鐘...

spanish.DownloadingModelDesc=Esto puede tardar varios minutos...
portuguese.DownloadingModelDesc=Esta operação pode demorar alguns minutos...
brazilianportuguese.DownloadingModelDesc=Isso pode levar vários minutos...
french.DownloadingModelDesc=Cette opération peut durer plusieurs minutes...
german.DownloadingModelDesc=Dies kann einige Minuten dauern...
italian.DownloadingModelDesc=L'operazione potrebbe richiedere diversi minuti...

english.ModelAlreadyExists=AI model already exists. Skipping download.
korean.ModelAlreadyExists=AI 모델이 이미 설치되어 있습니다. 다운로드를 건너뜁니다.
japanese.ModelAlreadyExists=AIモデルは既にインストールされています。ダウンロードをスキップします。
chinesesimplified.ModelAlreadyExists=AI模型已安装。跳过下载。
chinesetraditional.ModelAlreadyExists=AI模型已安裝。跳過下載。

spanish.ModelAlreadyExists=El modelo de IA ya existe. Omitiendo la descarga.
portuguese.ModelAlreadyExists=O modelo de IA já existe. A transferência será ignorada.
brazilianportuguese.ModelAlreadyExists=O modelo de IA já existe. O download será ignorado.
french.ModelAlreadyExists=Le modèle IA existe déjà. Téléchargement ignoré.
german.ModelAlreadyExists=Das KI-Modell ist bereits vorhanden. Der Download wird übersprungen.
italian.ModelAlreadyExists=Il modello IA è già presente. Download ignorato.

; F2 (security, session 260909_0002): a pre-existing model file whose SHA-256
; does NOT match the pin (or cannot be hashed at all) is now an EXPLICIT user
; decision instead of a log-only warning. YES = delete + re-download the
; verified model; NO = keep the file untouched and continue (historical).
english.ModelHashMismatchQuestion=The existing model file failed integrity (SHA-256) verification. It may be corrupted or tampered with.%n%nYes deletes the file and re-downloads the verified model. No keeps the existing file untouched and skips the download.%n%nDelete and re-download now?
korean.ModelHashMismatchQuestion=기존 모델 파일의 무결성(SHA-256) 검증에 실패했습니다. 파일이 손상되었거나 변조되었을 수 있습니다.%n%n예를 선택하면 파일을 삭제하고 검증된 모델을 다시 다운로드합니다. 아니요를 선택하면 기존 파일을 그대로 두고 다운로드를 건너뜁니다.%n%n지금 삭제하고 재다운로드할까요?
japanese.ModelHashMismatchQuestion=既存のモデルファイルの整合性（SHA-256）検証に失敗しました。ファイルが破損しているか改ざんされている可能性があります。%n%n「はい」を選ぶとファイルを削除し、検証済みモデルを再ダウンロードします。「いいえ」を選ぶと既存のファイルをそのままにし、ダウンロードをスキップします。%n%n今すぐ削除して再ダウンロードしますか？
chinesesimplified.ModelHashMismatchQuestion=现有模型文件未通过完整性（SHA-256）验证，可能已损坏或被篡改。%n%n选择“是”将删除该文件并重新下载经过验证的模型；选择“否”将保留现有文件并跳过下载。%n%n是否立即删除并重新下载？
chinesetraditional.ModelHashMismatchQuestion=現有模型檔案未通過完整性（SHA-256）驗證，可能已損壞或被竄改。%n%n選擇「是」將刪除該檔案並重新下載經過驗證的模型；選擇「否」將保留現有檔案並略過下載。%n%n是否立即刪除並重新下載？

spanish.ModelHashMismatchQuestion=El archivo de modelo existente no superó la verificación de integridad (SHA-256). Puede estar dañado o haber sido manipulado.%n%n«Sí» elimina el archivo y descarga de nuevo el modelo verificado. «No» conserva el archivo existente intacto y omite la descarga.%n%n¿Eliminar y volver a descargar ahora?
portuguese.ModelHashMismatchQuestion=O ficheiro de modelo existente não passou na verificação de integridade (SHA-256). Poderá estar danificado ou manipulado.%n%n«Sim» elimina o ficheiro e transfere novamente o modelo verificado. «Não» mantém o ficheiro existente intacto e ignora a transferência.%n%nEliminar e transferir novamente agora?
brazilianportuguese.ModelHashMismatchQuestion=O arquivo de modelo existente não passou na verificação de integridade (SHA-256). Ele pode estar corrompido ou ter sido manipulado.%n%n"Sim" exclui o arquivo e baixa novamente o modelo verificado. "Não" mantém o arquivo existente intacto e ignora o download.%n%nExcluir e baixar novamente agora?
french.ModelHashMismatchQuestion=Le fichier de modèle existant a échoué à la vérification d'intégrité (SHA-256). Il est peut-être corrompu ou falsifié.%n%n« Oui » supprime le fichier et télécharge à nouveau le modèle vérifié. « Non » conserve le fichier existant intact et ignore le téléchargement.%n%nSupprimer et retélécharger maintenant ?
german.ModelHashMismatchQuestion=Die vorhandene Modelldatei hat die Integritätsprüfung (SHA-256) nicht bestanden. Sie ist möglicherweise beschädigt oder manipuliert.%n%n"Ja" löscht die Datei und lädt das verifizierte Modell erneut herunter. "Nein" behält die vorhandene Datei unverändert bei und überspringt den Download.%n%nJetzt löschen und erneut herunterladen?
italian.ModelHashMismatchQuestion=Il file del modello esistente non ha superato la verifica di integrità (SHA-256). Potrebbe essere danneggiato o manomesso.%n%n"Sì" elimina il file e scarica nuovamente il modello verificato. "No" conserva il file esistente intatto e ignora il download.%n%nEliminare e scaricare nuovamente ora?

english.ModelHashUnverifiableQuestion=The existing model file could not be hashed to verify its integrity (read error). It may be corrupted or locked by another program.%n%nYes deletes the file and re-downloads the verified model. No keeps the existing file untouched and skips the download.%n%nDelete and re-download now?
korean.ModelHashUnverifiableQuestion=기존 모델 파일을 해싱하여 무결성을 확인할 수 없습니다(읽기 오류). 파일이 손상되었거나 다른 프로그램에 의해 잠겨 있을 수 있습니다.%n%n예를 선택하면 파일을 삭제하고 검증된 모델을 다시 다운로드합니다. 아니요를 선택하면 기존 파일을 그대로 두고 다운로드를 건너뜁니다.%n%n지금 삭제하고 재다운로드할까요?
japanese.ModelHashUnverifiableQuestion=既存のモデルファイルをハッシュ化して整合性を確認できませんでした（読み取りエラー）。ファイルが破損しているか、他のプログラムによってロックされている可能性があります。%n%n「はい」を選ぶとファイルを削除し、検証済みモデルを再ダウンロードします。「いいえ」を選ぶと既存のファイルをそのままにし、ダウンロードをスキップします。%n%n今すぐ削除して再ダウンロードしますか？
chinesesimplified.ModelHashUnverifiableQuestion=无法对现有模型文件进行哈希校验以确认其完整性（读取错误）。文件可能已损坏或被其他程序锁定。%n%n选择“是”将删除该文件并重新下载经过验证的模型；选择“否”将保留现有文件并跳过下载。%n%n是否立即删除并重新下载？
chinesetraditional.ModelHashUnverifiableQuestion=無法對現有模型檔案進行雜湊校驗以確認其完整性（讀取錯誤）。檔案可能已損壞或被其他程式鎖定。%n%n選擇「是」將刪除該檔案並重新下載經過驗證的模型；選擇「否」將保留現有檔案並略過下載。%n%n是否立即刪除並重新下載？

spanish.ModelHashUnverifiableQuestion=No se pudo calcular el hash del archivo de modelo existente para verificar su integridad (error de lectura). Puede estar dañado o bloqueado por otro programa.%n%n«Sí» elimina el archivo y descarga de nuevo el modelo verificado. «No» conserva el archivo existente intacto y omite la descarga.%n%n¿Eliminar y volver a descargar ahora?
portuguese.ModelHashUnverifiableQuestion=Não foi possível calcular o hash do ficheiro de modelo existente para verificar a sua integridade (erro de leitura). Poderá estar danificado ou bloqueado por outro programa.%n%n«Sim» elimina o ficheiro e transfere novamente o modelo verificado. «Não» mantém o ficheiro existente intacto e ignora a transferência.%n%nEliminar e transferir novamente agora?
brazilianportuguese.ModelHashUnverifiableQuestion=Não foi possível calcular o hash do arquivo de modelo existente para verificar sua integridade (erro de leitura). Ele pode estar corrompido ou bloqueado por outro programa.%n%n"Sim" exclui o arquivo e baixa novamente o modelo verificado. "Não" mantém o arquivo existente intacto e ignora o download.%n%nExcluir e baixar novamente agora?
french.ModelHashUnverifiableQuestion=Le fichier de modèle existant n'a pas pu être haché pour vérifier son intégrité (erreur de lecture). Il est peut-être corrompu ou verrouillé par un autre programme.%n%n« Oui » supprime le fichier et télécharge à nouveau le modèle vérifié. « Non » conserve le fichier existant intact et ignore le téléchargement.%n%nSupprimer et retélécharger maintenant ?
german.ModelHashUnverifiableQuestion=Die vorhandene Modelldatei konnte für die Integritätsprüfung nicht gehasht werden (Lesefehler). Sie ist möglicherweise beschädigt oder durch ein anderes Programm gesperrt.%n%n"Ja" löscht die Datei und lädt das verifizierte Modell erneut herunter. "Nein" behält die vorhandene Datei unverändert bei und überspringt den Download.%n%nJetzt löschen und erneut herunterladen?
italian.ModelHashUnverifiableQuestion=Non è stato possibile calcolare l'hash del file del modello esistente per verificarne l'integrità (errore di lettura). Potrebbe essere danneggiato o bloccato da un altro programma.%n%n"Sì" elimina il file e scarica nuovamente il modello verificato. "No" conserva il file esistente intatto e ignora il download.%n%nEliminare e scaricare nuovamente ora?

english.ModelHashDeleteFailed=The unverified model file could not be deleted (it may be locked by another program). The installer will keep it unchanged and continue; the app will refuse to load it until it passes verification.
korean.ModelHashDeleteFailed=검증되지 않은 모델 파일을 삭제할 수 없습니다(다른 프로그램이 사용 중일 수 있음). 설치 마법사가 기존 파일을 그대로 두고 계속 진행하며, 앱은 검증을 통과할 때까지 이 파일의 로드를 거부합니다.
japanese.ModelHashDeleteFailed=検証されていないモデルファイルを削除できませんでした（他のプログラムによって使用中の可能性があります）。インストーラーは既存のファイルを変更せずに続行します。アプリは検証に合格するまでこのファイルの読み込みを拒否します。
chinesesimplified.ModelHashDeleteFailed=无法删除未经验证的模型文件（可能正被其他程序占用）。安装程序将保留该文件并继续；在通过验证之前，应用将拒绝加载该文件。
chinesetraditional.ModelHashDeleteFailed=無法刪除未經驗證的模型檔案（可能正被其他程式佔用）。安裝程式將保留該檔案並繼續；在通過驗證之前，應用程式將拒絕載入該檔案。

spanish.ModelHashDeleteFailed=No se pudo eliminar el archivo de modelo no verificado (puede estar bloqueado por otro programa). El instalador lo conservará sin cambios y continuará; la aplicación se negará a cargarlo hasta que supere la verificación.
portuguese.ModelHashDeleteFailed=Não foi possível eliminar o ficheiro de modelo não verificado (poderá estar bloqueado por outro programa). O instalador mantê-lo-á sem alterações e continuará; a aplicação recusará carregá-lo até que passe na verificação.
brazilianportuguese.ModelHashDeleteFailed=O arquivo de modelo não verificado não pôde ser excluído (ele pode estar bloqueado por outro programa). O instalador o manterá sem alterações e continuará; o aplicativo se recusará a carregá-lo até que ele passe na verificação.
french.ModelHashDeleteFailed=Le fichier de modèle non vérifié n'a pas pu être supprimé (il est peut-être verrouillé par un autre programme). L'installateur le conservera sans modification et continuera ; l'application refusera de le charger tant qu'il n'aura pas passé la vérification.
german.ModelHashDeleteFailed=Die nicht verifizierte Modelldatei konnte nicht gelöscht werden (möglicherweise wird sie von einem anderen Programm gesperrt). Das Installationsprogramm behält sie unverändert bei und fährt fort; die App verweigert das Laden, bis die Überprüfung bestanden ist.
italian.ModelHashDeleteFailed=Non è stato possibile eliminare il file del modello non verificato (potrebbe essere bloccato da un altro programma). Il programma di installazione lo manterrà invariato e continuerà; l'applicazione rifiuterà di caricarlo finché non avrà superato la verifica.

english.DownloadFailed=Failed to download the AI model.
korean.DownloadFailed=AI 번역 모델 다운로드에 실패했습니다.
japanese.DownloadFailed=AI翻訳モデルのダウンロードに失敗しました。
chinesesimplified.DownloadFailed=AI翻译模型下载失败。
chinesetraditional.DownloadFailed=AI翻譯模型下載失敗。

spanish.DownloadFailed=No se pudo descargar el modelo de IA.
portuguese.DownloadFailed=Falha ao transferir o modelo de IA.
brazilianportuguese.DownloadFailed=Falha ao baixar o modelo de IA.
french.DownloadFailed=Échec du téléchargement du modèle IA.
german.DownloadFailed=Der Download des KI-Modells ist fehlgeschlagen.
italian.DownloadFailed=Download del modello IA non riuscito.

english.DownloadFailedDetail=You can still use Google Translate (free, online). The model can be downloaded later.
korean.DownloadFailedDetail=Google 번역(무료, 온라인)으로 계속 사용 가능합니다. 모델은 나중에 수동 다운로드 가능합니다.
japanese.DownloadFailedDetail=Google翻訳（無料、オンライン）で引き続き利用可能です。モデルは後で手動ダウンロードできます。
chinesesimplified.DownloadFailedDetail=仍可使用Google翻译（免费、在线）。模型可稍后手动下载。
chinesetraditional.DownloadFailedDetail=仍可使用Google翻譯（免費、線上）。模型可稍後手動下載。

spanish.DownloadFailedDetail=Aún puede usar Google Translate (gratis, en línea). El modelo se puede descargar más tarde.
portuguese.DownloadFailedDetail=Ainda pode usar o Google Tradutor (gratuito, online). O modelo poderá ser transferido mais tarde.
brazilianportuguese.DownloadFailedDetail=Você ainda pode usar o Google Tradutor (grátis, online). O modelo pode ser baixado mais tarde.
french.DownloadFailedDetail=Vous pouvez continuer à utiliser Google Traduction (gratuit, en ligne). Le modèle pourra être téléchargé plus tard.
german.DownloadFailedDetail=Sie können weiterhin Google Übersetzer (kostenlos, online) verwenden. Das Modell kann später heruntergeladen werden.
italian.DownloadFailedDetail=È ancora possibile usare Google Traduttore (gratis, online). Il modello potrà essere scaricato in seguito.

english.DownloadRetry=Retry download?
korean.DownloadRetry=다운로드를 다시 시도하시겠습니까?
japanese.DownloadRetry=ダウンロードを再試行しますか？
chinesesimplified.DownloadRetry=是否重试下载？
chinesetraditional.DownloadRetry=是否重試下載？

spanish.DownloadRetry=¿Reintentar la descarga?
portuguese.DownloadRetry=Tentar transferir novamente?
brazilianportuguese.DownloadRetry=Tentar baixar novamente?
french.DownloadRetry=Réessayer le téléchargement ?
german.DownloadRetry=Download erneut versuchen?
italian.DownloadRetry=Riprovare il download?

english.DiskSpaceWarning=At least 3 GB free space recommended. Current: %1 GB. Continue?
korean.DiskSpaceWarning=최소 3GB 여유 공간 필요. 현재: %1 GB. 계속?
japanese.DiskSpaceWarning=最低3GBの空き容量が必要です。現在: %1 GB。続行しますか？
chinesesimplified.DiskSpaceWarning=至少需要3GB可用空间。当前: %1 GB。是否继续？
chinesetraditional.DiskSpaceWarning=至少需要3GB可用空間。目前: %1 GB。是否繼續？

spanish.DiskSpaceWarning=Se recomiendan al menos 3 GB de espacio libre. Actual: %1 GB. ¿Continuar?
portuguese.DiskSpaceWarning=São recomendados pelo menos 3 GB de espaço livre. Atual: %1 GB. Continuar?
brazilianportuguese.DiskSpaceWarning=Recomendado pelo menos 3 GB de espaço livre. Atual: %1 GB. Continuar?
french.DiskSpaceWarning=Au moins 3 Go d'espace libre sont recommandés. Actuel : %1 Go. Continuer ?
german.DiskSpaceWarning=Mindestens 3 GB freier Speicherplatz empfohlen. Aktuell: %1 GB. Fortfahren?
italian.DiskSpaceWarning=Si raccomandano almeno 3 GB di spazio libero. Attuale: %1 GB. Continuare?

english.UninstallCleanupPrompt=Do you want to remove user settings and diagnostic logs? (Recommended for clean uninstall)
korean.UninstallCleanupPrompt=사용자 설정 및 진단 로그를 삭제하시겠습니까? (클린 제거 시 권장)
japanese.UninstallCleanupPrompt=ユーザー設定と診断ログを削除しますか？（クリーンアンインストールに推奨）
chinesesimplified.UninstallCleanupPrompt=是否删除用户设置和诊断日志？（建议进行干净卸载）
chinesetraditional.UninstallCleanupPrompt=是否刪除使用者設定和診斷日誌？（建議進行乾淨移除）

spanish.UninstallCleanupPrompt=¿Desea eliminar la configuración de usuario y los registros de diagnóstico? (Recomendado para una desinstalación limpia)
portuguese.UninstallCleanupPrompt=Deseja remover as definições do utilizador e os registos de diagnóstico? (Recomendado para uma desinstalação limpa)
brazilianportuguese.UninstallCleanupPrompt=Deseja remover as configurações do usuário e os logs de diagnóstico? (Recomendado para uma desinstalação limpa)
french.UninstallCleanupPrompt=Voulez-vous supprimer les paramètres utilisateur et les journaux de diagnostic ? (Recommandé pour une désinstallation complète)
german.UninstallCleanupPrompt=Möchten Sie die Benutzereinstellungen und Diagnoseprotokolle entfernen? (Empfohlen für eine vollständige Deinstallation)
italian.UninstallCleanupPrompt=Desidera rimuovere le impostazioni utente e i log diagnostici? (Consigliato per una disinstallazione completa)

; Per B-1 spec: latin languages keep the English brand form "Emebala Chat".
english.ShortcutName=Emebala Chat
korean.ShortcutName=에메발라 챗
japanese.ShortcutName=エメバラチャット
chinesesimplified.ShortcutName=埃梅巴拉 翻译
chinesetraditional.ShortcutName=埃梅巴拉 翻譯
spanish.ShortcutName=Emebala Chat
portuguese.ShortcutName=Emebala Chat
brazilianportuguese.ShortcutName=Emebala Chat
french.ShortcutName=Emebala Chat
german.ShortcutName=Emebala Chat
italian.ShortcutName=Emebala Chat

; B-2 (session 260910_0002, architect plan 1.5): Model Consent page strings.
; Shown on wpReady+1 BEFORE the ~2 GB model download starts. Declining keeps
; the install going with the free online Google Translate engine, mirroring
; the DownloadFailedDetail wording above.
english.ConsentTitle=AI Translation Model Download
korean.ConsentTitle=AI 번역 모델 다운로드
japanese.ConsentTitle=AI翻訳モデルのダウンロード
chinesesimplified.ConsentTitle=AI翻译模型下载
chinesetraditional.ConsentTitle=AI翻譯模型下載

spanish.ConsentTitle=Descarga del modelo de traducción de IA
portuguese.ConsentTitle=Transferência do modelo de tradução por IA
brazilianportuguese.ConsentTitle=Download do modelo de tradução de IA
french.ConsentTitle=Téléchargement du modèle de traduction IA
german.ConsentTitle=Download des KI-Übersetzungsmodells
italian.ConsentTitle=Download del modello di traduzione IA

english.ConsentDesc=Emebala Chat can download a local AI translation model (about 2 GB) so translations work fully offline. If you skip, the free online Google Translate engine is used instead (requires internet).
korean.ConsentDesc=에메발라 챗은 번역을 완전 오프라인으로 사용할 수 있도록 로컬 AI 번역 모델(약 2GB)을 다운로드할 수 있습니다. 건너뛰면 무료 온라인 Google 번역 엔진이 대신 사용됩니다(인터넷 필요).
japanese.ConsentDesc=エメバラチャットは、翻訳を完全にオフラインで利用できるようにするため、ローカルAI翻訳モデル（約2GB）をダウンロードできます。スキップした場合は、無料のオンラインGoogle翻訳エンジンが代わりに使用されます（インターネット接続が必要）。
chinesesimplified.ConsentDesc=埃梅巴拉 翻译可下载本地AI翻译模型（约2GB），使翻译完全离线可用。如果跳过，将改用免费的在线Google翻译引擎（需要互联网）。
chinesetraditional.ConsentDesc=埃梅巴拉 翻譯可下載本地AI翻譯模型（約2GB），使翻譯完全離線可用。如果跳過，將改用免費的線上Google翻譯引擎（需要網路）。

spanish.ConsentDesc=Emebala Chat puede descargar un modelo local de traducción por IA (unos 2 GB) para que las traducciones funcionen completamente sin conexión. Si lo omite, se usará en su lugar el motor gratuito de Google Translate en línea (requiere internet).
portuguese.ConsentDesc=O Emebala Chat pode transferir um modelo local de tradução por IA (cerca de 2 GB) para que as traduções funcionem totalmente offline. Se ignorar, será utilizado em alternativa o Google Tradutor online gratuito (requer internet).
brazilianportuguese.ConsentDesc=O Emebala Chat pode baixar um modelo local de tradução de IA (cerca de 2 GB) para que as traduções funcionem totalmente offline. Se você pular, o Google Tradutor online gratuito será usado no lugar (requer internet).
french.ConsentDesc=Emebala Chat peut télécharger un modèle local de traduction IA (environ 2 Go) pour que les traductions fonctionnent entièrement hors ligne. Si vous passez cette étape, le moteur gratuit Google Traduction en ligne sera utilisé à la place (nécessite une connexion internet).
german.ConsentDesc=Emebala Chat kann ein lokales KI-Übersetzungsmodell (ca. 2 GB) herunterladen, damit Übersetzungen vollständig offline funktionieren. Wenn Sie dies überspringen, wird stattdessen der kostenlose Online-Dienst Google Übersetzer verwendet (Internet erforderlich).
italian.ConsentDesc=Emebala Chat può scaricare un modello locale di traduzione IA (circa 2 GB) per consentire alle traduzioni di funzionare completamente offline. Se scegli di ignorare, verrà utilizzato in alternativa Google Traduttore online gratuito (richiede internet).

english.ConsentYes=Download the AI model now (recommended - offline translation)
korean.ConsentYes=지금 AI 모델 다운로드 (권장 - 오프라인 번역)
japanese.ConsentYes=今すぐAIモデルをダウンロード（推奨 - オフライン翻訳）
chinesesimplified.ConsentYes=立即下载AI模型（推荐 - 离线翻译）
chinesetraditional.ConsentYes=立即下載AI模型（建議 - 離線翻譯）

spanish.ConsentYes=Descargar ahora el modelo de IA (recomendado - traducción sin conexión)
portuguese.ConsentYes=Transferir agora o modelo de IA (recomendado - tradução offline)
brazilianportuguese.ConsentYes=Baixar o modelo de IA agora (recomendado - tradução offline)
french.ConsentYes=Télécharger le modèle IA maintenant (recommandé - traduction hors ligne)
german.ConsentYes=KI-Modell jetzt herunterladen (empfohlen - Offline-Übersetzung)
italian.ConsentYes=Scarica ora il modello IA (consigliato - traduzione offline)

english.ConsentNo=Skip - use Google Translate (free, requires internet)
korean.ConsentNo=건너뛰기 - Google 번역 사용 (무료, 인터넷 필요)
japanese.ConsentNo=スキップ - Google翻訳を使用（無料、インターネットが必要）
chinesesimplified.ConsentNo=跳过 - 使用Google翻译（免费，需要互联网）
chinesetraditional.ConsentNo=略過 - 使用Google翻譯（免費，需要網路）

spanish.ConsentNo=Omitir - usar Google Translate (gratis, requiere internet)
portuguese.ConsentNo=Ignorar - usar o Google Tradutor (gratuito, requer internet)
brazilianportuguese.ConsentNo=Pular - usar o Google Tradutor (grátis, requer internet)
french.ConsentNo=Passer - utiliser Google Traduction (gratuit, nécessite internet)
german.ConsentNo=Überspringen - Google Übersetzer verwenden (kostenlos, Internet erforderlich)
italian.ConsentNo=Ignora - usa Google Traduttore (gratis, richiede internet)

; B-3 (session 260910_0002, architect plan 1.4): About page (shown right after
; Welcome, every install) and Usage Guide page (shown right after wpInstalling,
; before Finish, every install). %n = line separator: the [Code] helper
; MessageLines() converts every literal %n to #13#10 before the memo pages are
; fed, so the bodies render multi-line regardless of whether the running Inno
; version expands %n in CustomMessage() results (conversion is a no-op then).
; Hotkey tokens (F9, Ctrl + F9, Ctrl + Shift + Enter, Double Ctrl + C, 0.4s)
; and the config filename stay verbatim in ALL languages. Brand naming follows
; the B-1/B-2 rule: CJK locales use the per-locale brand form, latin locales
; keep "Emebala Chat".
english.AboutTitle=About Emebala Chat
korean.AboutTitle=에메발라 챗 소개
japanese.AboutTitle=エメバラチャットについて
chinesesimplified.AboutTitle=关于埃梅巴拉 翻译
chinesetraditional.AboutTitle=關於埃梅巴拉 翻譯
spanish.AboutTitle=Acerca de Emebala Chat
portuguese.AboutTitle=Sobre o Emebala Chat
brazilianportuguese.AboutTitle=Sobre o Emebala Chat
french.AboutTitle=À propos d'Emebala Chat
german.AboutTitle=Über Emebala Chat
italian.AboutTitle=Informazioni su Emebala Chat

english.AboutBody=Emebala Chat — real-time AI translation that works everywhere you type.%n%nKey features:%n  •  Works in any text box: games, chats, browsers, documents%n  •  Local AI model — your text never leaves your PC (offline mode)%n  •  38 languages supported, auto-detects the source language%n  •  Drag-select text to translate it instantly%n  •  Floating badge with quick controls in your system tray
korean.AboutBody=에메발라 챗 — 텍스트를 입력하는 모든 곳에서 동작하는 실시간 AI 번역입니다.%n%n핵심 기능:%n  •  모든 텍스트 상자에서 사용 가능: 게임, 채팅, 브라우저, 문서%n  •  로컬 AI 모델 — 텍스트가 PC를 떠나지 않음 (오프라인 모드)%n  •  38개 언어 지원, 원본 언어 자동 감지%n  •  텍스트를 드래그 선택하면 즉시 번역%n  •  시스템 트레이의 플로팅 배지에서 빠른 제어
japanese.AboutBody=エメバラチャット — テキストを入力するあらゆる場所で動くリアルタイムAI翻訳です。%n%n主な機能：%n  •  あらゆるテキストボックスで使用可能：ゲーム、チャット、ブラウザ、文書%n  •  ローカルAIモデル — テキストがPCから外部に送信されません（オフラインモード）%n  •  38言語対応、原文言語を自動検出%n  •  テキストをドラッグ選択すると即座に翻訳%n  •  システムトレイのフローティングバッジでクイック操作
chinesesimplified.AboutBody=埃梅巴拉 翻译——在您输入的任何地方都能使用的实时AI翻译。%n%n主要功能：%n  •  适用于任何文本框：游戏、聊天、浏览器、文档%n  •  本地AI模型——文本永远不会离开您的电脑（离线模式）%n  •  支持38种语言，自动检测源语言%n  •  拖动选择文本即可立即翻译%n  •  系统托盘中的浮动徽章提供快捷控制
chinesetraditional.AboutBody=埃梅巴拉 翻譯——在您輸入的任何地方都能使用的即時AI翻譯。%n%n主要功能：%n  •  適用於任何文字方塊：遊戲、聊天、瀏覽器、文件%n  •  本地AI模型——文字永遠不會離開您的電腦（離線模式）%n  •  支援38種語言，自動偵測來源語言%n  •  拖曳選取文字即可即時翻譯%n  •  系統匣中的浮動徽章提供快速控制
spanish.AboutBody=Emebala Chat — traducción por IA en tiempo real que funciona dondequiera que escriba.%n%nFunciones principales:%n  •  Funciona en cualquier cuadro de texto: juegos, chats, navegadores, documentos%n  •  Modelo de IA local — su texto nunca sale de su PC (modo sin conexión)%n  •  38 idiomas compatibles, con detección automática del idioma de origen%n  •  Seleccione texto arrastrando para traducirlo al instante%n  •  Insignia flotante con controles rápidos en la bandeja del sistema
portuguese.AboutBody=O Emebala Chat — tradução por IA em tempo real que funciona em qualquer lugar onde escreva.%n%nFuncionalidades principais:%n  •  Funciona em qualquer caixa de texto: jogos, chats, navegadores, documentos%n  •  Modelo de IA local — o seu texto nunca sai do PC (modo offline)%n  •  38 idiomas suportados, deteta automaticamente o idioma de origem%n  •  Selecione texto arrastando para o traduzir instantaneamente%n  •  Ícone flutuante com controlos rápidos na bandeja do sistema
brazilianportuguese.AboutBody=O Emebala Chat — tradução de IA em tempo real que funciona em qualquer lugar onde você digita.%n%nPrincipais recursos:%n  •  Funciona em qualquer caixa de texto: jogos, chats, navegadores, documentos%n  •  Modelo de IA local — seu texto nunca sai do PC (modo offline)%n  •  38 idiomas compatíveis, com detecção automática do idioma de origem%n  •  Selecione texto arrastando para traduzi-lo instantaneamente%n  •  Selo flutuante com controles rápidos na bandeja do sistema
french.AboutBody=Emebala Chat — la traduction IA en temps réel qui fonctionne partout où vous tapez.%n%nFonctionnalités clés :%n  •  Fonctionne dans n'importe quelle zone de texte : jeux, chats, navigateurs, documents%n  •  Modèle IA local — votre texte ne quitte jamais votre PC (mode hors ligne)%n  •  38 langues prises en charge, détection automatique de la langue source%n  •  Sélectionnez du texte en le faisant glisser pour le traduire instantanément%n  •  Badge flottant avec commandes rapides dans la barre d'état système
german.AboutBody=Emebala Chat — Echtzeit-KI-Übersetzung, die überall funktioniert, wo Sie Text eingeben.%n%nHauptfunktionen:%n  •  Funktioniert in jedem Textfeld: Spiele, Chats, Browser, Dokumente%n  •  Lokales KI-Modell — Ihr Text verlässt nie Ihren PC (Offline-Modus)%n  •  38 Sprachen unterstützt, automatische Erkennung der Quellensprache%n  •  Text per Ziehen auswählen und sofort übersetzen%n  •  Schwebendes Badge mit Schnellsteuerung im Systemtray
italian.AboutBody=Emebala Chat — traduzione IA in tempo reale che funziona ovunque scriva.%n%nFunzionalità principali:%n  •  Funziona in qualsiasi casella di testo: giochi, chat, browser, documenti%n  •  Modello IA locale — il suo testo non lascia mai il PC (modalità offline)%n  •  38 lingue supportate, rilevamento automatico della lingua di origine%n  •  Selezioni il testo trascinandolo per tradurlo all'istante%n  •  Badge fluttuante con controlli rapidi nell'area di notifica

english.GuideTitle=Quick Start — Hotkeys & Tips
korean.GuideTitle=빠른 시작 — 단축키 및 팁
japanese.GuideTitle=クイックスタート — ホットキーとヒント
chinesesimplified.GuideTitle=快速上手——快捷键与提示
chinesetraditional.GuideTitle=快速上手——快捷鍵與提示
spanish.GuideTitle=Inicio rápido — Teclas y consejos
portuguese.GuideTitle=Início rápido — Teclas e dicas
brazilianportuguese.GuideTitle=Início rápido — Teclas e dicas
french.GuideTitle=Démarrage rapide — Raccourcis et astuces
german.GuideTitle=Schnellstart — Tastenkürzel und Tipps
italian.GuideTitle=Avvio rapido — Tasti e suggerimenti

english.GuideBody=Hotkeys (configurable in config.json):%n  F9 — toggle translation on/off (also click the floating badge)%n  Ctrl + F9 — cycle the target language%n  Ctrl + Shift + Enter — toggle auto-send mode%n  Double Ctrl + C (within 0.4s) — translate selected text via drag%n%nTips:%n  •  The app runs in your system tray — look for the Emebala icon%n  •  Right-click the tray icon for engine and language settings%n  •  Hold a modifier (Ctrl/Shift/Alt/Win) to type the modifier key itself%n  •  If the AI model was not installed, the free Google Translate engine is used%n%nEnjoy Emebala Chat!
korean.GuideBody=단축키 (config.json에서 변경 가능):%n  F9 — 번역 켜기/끄기 (플로팅 배지를 클릭해도 동일)%n  Ctrl + F9 — 대상 언어 순환 변경%n  Ctrl + Shift + Enter — 자동 전송 모드 토글%n  Double Ctrl + C (0.4초 이내) — 드래그로 선택한 텍스트 번역%n%n팁:%n  •  앱은 시스템 트레이에서 실행됩니다 — 에메발라 아이콘을 확인하세요%n  •  트레이 아이콘을 마우스 우클릭하면 엔진 및 언어 설정%n  •  수정 키(Ctrl/Shift/Alt/Win)를 누른 채 유지하면 해당 키 자체를 입력할 수 있습니다%n  •  AI 모델을 설치하지 않은 경우 무료 Google 번역 엔진이 사용됩니다%n%n에메발라 챗을 즐겨보세요!
japanese.GuideBody=ホットキー（config.jsonで変更可能）：%n  F9 — 翻訳のオン/オフ切り替え（フローティングバッジのクリックでも可）%n  Ctrl + F9 — 対象言語を循環切り替え%n  Ctrl + Shift + Enter — 自動送信モードの切り替え%n  Double Ctrl + C（0.4秒以内）— ドラッグで選択したテキストを翻訳%n%nヒント：%n  •  アプリはシステムトレイで実行されます — エメバラアイコンを探してください%n  •  トレイアイコンを右クリックするとエンジンと言語の設定%n  •  修飾キー（Ctrl/Shift/Alt/Win）を押したままにすると、そのキー自体を入力できます%n  •  AIモデルをインストールしなかった場合は、無料のGoogle翻訳エンジンが使用されます%n%nエメバラチャットをお楽しみください！
chinesesimplified.GuideBody=快捷键（可在 config.json 中配置）：%n  F9 — 开启/关闭翻译（也可点击浮动徽章）%n  Ctrl + F9 — 循环切换目标语言%n  Ctrl + Shift + Enter — 切换自动发送模式%n  Double Ctrl + C（0.4秒内）— 翻译拖动选中的文本%n%n提示：%n  •  应用在系统托盘中运行——请寻找埃梅巴拉图标%n  •  右键单击托盘图标可设置引擎和语言%n  •  按住修饰键（Ctrl/Shift/Alt/Win）可输入该修饰键本身%n  •  如果未安装AI模型，将使用免费的Google翻译引擎%n%n祝您使用埃梅巴拉 翻译愉快！
chinesetraditional.GuideBody=快捷鍵（可在 config.json 中設定）：%n  F9 — 開啟/關閉翻譯（也可點擊浮動徽章）%n  Ctrl + F9 — 循環切換目標語言%n  Ctrl + Shift + Enter — 切換自動傳送模式%n  Double Ctrl + C（0.4秒內）— 翻譯拖曳選取的文字%n%n提示：%n  •  應用程式在系統匣中執行——請尋找埃梅巴拉圖示%n  •  右鍵點擊系統匣圖示可設定引擎與語言%n  •  按住修飾鍵（Ctrl/Shift/Alt/Win）可輸入該修飾鍵本身%n  •  如果未安裝AI模型，將使用免費的Google翻譯引擎%n%n祝您使用埃梅巴拉 翻譯愉快！
spanish.GuideBody=Teclas de acceso rápido (configurables en config.json):%n  F9 — activar/desactivar la traducción (también haciendo clic en la insignia flotante)%n  Ctrl + F9 — cambiar de idioma de destino en ciclo%n  Ctrl + Shift + Enter — alternar el modo de envío automático%n  Double Ctrl + C (en menos de 0,4 s) — traducir el texto seleccionado al arrastrar%n%nConsejos:%n  •  La aplicación se ejecuta en la bandeja del sistema — busque el icono de Emebala%n  •  Clic derecho en el icono de la bandeja para ajustar el motor y el idioma%n  •  Mantenga pulsada una tecla modificadora (Ctrl/Shift/Alt/Win) para escribir esa tecla en sí%n  •  Si no se instaló el modelo de IA, se usará el motor gratuito de Google Translate en línea%n%n¡Disfrute de Emebala Chat!
portuguese.GuideBody=Teclas de atalho (configuráveis em config.json):%n  F9 — ativar/desativar a tradução (também ao clicar no ícone flutuante)%n  Ctrl + F9 — alternar ciclicamente o idioma de destino%n  Ctrl + Shift + Enter — alternar o modo de envio automático%n  Double Ctrl + C (em menos de 0,4 s) — traduzir o texto selecionado ao arrastar%n%nDicas:%n  •  A aplicação funciona na bandeja do sistema — procure o ícone do Emebala%n  •  Clique com o botão direito no ícone da bandeja para definições de motor e idioma%n  •  Mantenha premida uma tecla modificadora (Ctrl/Shift/Alt/Win) para escrever a própria tecla%n  •  Se o modelo de IA não for instalado, é utilizado o Google Tradutor online gratuito%n%nAproveite o Emebala Chat!
brazilianportuguese.GuideBody=Teclas de atalho (configuráveis em config.json):%n  F9 — ativar/desativar a tradução (também clicando no selo flutuante)%n  Ctrl + F9 — alternar o idioma de destino em ciclo%n  Ctrl + Shift + Enter — alternar o modo de envio automático%n  Double Ctrl + C (em até 0,4 s) — traduzir o texto selecionado ao arrastar%n%nDicas:%n  •  O aplicativo roda na bandeja do sistema — procure o ícone do Emebala%n  •  Clique com o botão direito no ícone da bandeja para configurações de mecanismo e idioma%n  •  Segure uma tecla modificadora (Ctrl/Shift/Alt/Win) para digitar a própria tecla%n  •  Se o modelo de IA não foi instalado, o Google Tradutor online gratuito é usado%n%nAproveite o Emebala Chat!
french.GuideBody=Raccourcis clavier (configurables dans config.json) :%n  F9 — activer/désactiver la traduction (ou cliquer sur le badge flottant)%n  Ctrl + F9 — faire défiler la langue cible%n  Ctrl + Shift + Enter — basculer le mode d'envoi automatique%n  Double Ctrl + C (en moins de 0,4 s) — traduire le texte sélectionné par glissement%n%nAstuces :%n  •  L'application s'exécute dans la barre d'état système — cherchez l'icône Emebala%n  •  Clic droit sur l'icône de la barre d'état pour les paramètres de moteur et de langue%n  •  Maintenez une touche modificatrice (Ctrl/Shift/Alt/Win) enfoncée pour taper cette touche elle-même%n  •  Si le modèle IA n'a pas été installé, le moteur gratuit Google Traduction en ligne est utilisé%n%nProfitez d'Emebala Chat !
german.GuideBody=Tastenkürzel (in config.json konfigurierbar):%n  F9 — Übersetzung ein-/ausschalten (auch per Klick auf das schwebende Badge)%n  Ctrl + F9 — Zielsprache durchschalten%n  Ctrl + Shift + Enter — Auto-Send-Modus umschalten%n  Double Ctrl + C (innerhalb von 0,4 s) — per Ziehen ausgewählten Text übersetzen%n%nTipps:%n  •  Die App läuft im Systemtray — suchen Sie das Emebala-Symbol%n  •  Rechtsklick auf das Tray-Symbol für Engine- und Spracheinstellungen%n  •  Halten Sie eine Modifikatortaste (Ctrl/Shift/Alt/Win) gedrückt, um sie selbst zu tippen%n  •  Wenn das KI-Modell nicht installiert wurde, wird der kostenlose Online-Dienst Google Übersetzer verwendet%n%nViel Spaß mit Emebala Chat!
italian.GuideBody=Scorciatoie da tastiera (configurabili in config.json):%n  F9 — attiva/disattiva la traduzione (anche cliccando sul badge fluttuante)%n  Ctrl + F9 — scorri le lingue di destinazione%n  Ctrl + Shift + Enter — attiva/disattiva la modalità di invio automatico%n  Double Ctrl + C (entro 0,4 s) — traduci il testo selezionato trascinandolo%n%nSuggerimenti:%n  •  L'app funziona nell'area di notifica — cerca l'icona di Emebala%n  •  Clic destro sull'icona nell'area di notifica per le impostazioni di motore e lingua%n  •  Tieni premuto un modificatore (Ctrl/Shift/Alt/Win) per digitare il tasto modificatore stesso%n  •  Se il modello IA non è stato installato, viene usato il motore gratuito Google Traduttore online%n%nGoditi Emebala Chat!

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
Source: "..\build\Emebala_chat.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
; REQ-207/208 (session 260911_0002 T6, design 144800 §2.3): bundle the README so
; the first-run privacy notice's "re-read this anytime in the README file"
; guidance is actionable on a clean machine. Full privacy spec lives in
; {app}\README.md ("Privacy & Data Handling (Technical)" section).
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\assets\Emebala_Chat_Appicon.ico"; DestDir: "{app}\assets"; Flags: ignoreversion
Source: "..\assets\Emebala_Chat_Appicon.png"; DestDir: "{app}\assets"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\assets\Emebala_Chat_Appicon_small.png"; DestDir: "{app}\assets"; Flags: ignoreversion
Source: "..\assets\Emebala_Chat_Logo_small.png"; DestDir: "{app}\assets"; Flags: ignoreversion
Source: "..\assets\logo.png"; DestDir: "{app}\assets"; Flags: ignoreversion skipifsourcedoesntexist

; ------------------------------------------------------------------------
; [Icons] - Start Menu and Desktop shortcuts
; ------------------------------------------------------------------------
[Icons]
Name: "{group}\{cm:ShortcutName}"; Filename: "{app}\Emebala_chat.exe"
Name: "{group}\{cm:UninstallProgram,{cm:ShortcutName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{cm:ShortcutName}"; Filename: "{app}\Emebala_chat.exe"; Tasks: desktopicon

; ------------------------------------------------------------------------
; [Registry] - Auto-start entry (only if autostart task selected)
; ------------------------------------------------------------------------
[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Emebalachat"; ValueData: """{app}\Emebala_chat.exe"""; Tasks: autostart; Flags: uninsdeletevalue

; ------------------------------------------------------------------------
; [UninstallDelete] - Clean up extra files on uninstall
; ------------------------------------------------------------------------
[UninstallDelete]
Type: filesandordirs; Name: "{app}\models"
Type: files; Name: "{app}\config.json"
Type: filesandordirs; Name: "{app}\assets"

; ------------------------------------------------------------------------
; [Run] - Post-install launch option
; ------------------------------------------------------------------------
[Run]
Filename: "{app}\Emebala_chat.exe"; Description: "{cm:LaunchProgram,{cm:ShortcutName}}"; Flags: nowait postinstall skipifsilent runasoriginaluser

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
  EXPECTED_MODEL_SHA256 = '5c3fe0b1408a5ceb0143184ef247b11b579c525f4b02b060e6c851bb76fef1a4';

var
  DownloadPage: TDownloadWizardPage;
  ModelSkipped: Boolean;
  // B-3 (session 260910_0002, architect plan 1.4): About page (after
  // wpWelcome) and Usage Guide page (after wpInstalling, before Finish).
  // CreateOutputMsgMemoPage returns TOutputMsgMemoWizardPage (NOT the
  // sibling TOutputMsgWizardPage), so both vars use the memo class.
  // Shown on every install (user decision: no "first install only" gating);
  // custom wizard pages are automatically never shown under /SILENT or
  // /VERYSILENT, so silent installs need no ShouldSkipPage handling.
  AboutPage: TOutputMsgMemoWizardPage;
  GuidePage: TOutputMsgMemoWizardPage;
  // B-2 (session 260910_0002): consent page shown after wpReady, before the
  // ~2 GB model download. ModelDeclined is latched from the user's radio
  // choice in ReadConsentChoice() during ssInstall (before any file copy),
  // and consumed by DownloadModel() in ssPostInstall.
  ConsentPage: TInputOptionWizardPage;
  ModelDeclined: Boolean;

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
// ToRtf - mojibake fix (session 260911_0001): wrap plain text in hand-built
// RTF using \uN unicode escapes. CreateOutputMsgMemoPage's TRichEditViewer
// converts PLAIN text to RTF internally via ANSI hex escapes under the
// language code page (CP949 for Korean), which mangles every non-ASCII
// character at display time (proven byte-level in
// docs/260911_0001_session_installer-mojibake-regression/115200_debug-rootcause-installer-mojibake.md,
// A/B validated in tools_tmp_mb19_rtf.py). Supplying explicit RTF with \u
// escapes bypasses that lossy conversion entirely and keeps the memo
// scrollable. Escapes \ { }, emits \par for CR, \uN? for chars >= 128.
// ------------------------------------------------------------------------
function ToRtf(const S: String): String;
var
  I: Integer;
  C: Integer;
  SB: String;
begin
  SB := '{\rtf1\ansi\ansicpg1252\deff0{\fonttbl{\f0\fnil\fcharset129 Malgun Gothic;}}' +
         '\uc1\viewscale100\fs16\pard\sa60\slmult1\tx0\tx2268\f0\fs16' + #13#10;
  for I := 1 to Length(S) do
  begin
    C := Ord(S[I]);
    case C of
      13: SB := SB + '\par' + #13#10;
      10: ;
      92: SB := SB + '\\';
      123: SB := SB + '\{';
      125: SB := SB + '\}';
    else
      if (C < 128) then
        SB := SB + S[I]
      else
        SB := SB + '\u' + IntToStr(C) + '?';
    end;
  end;
  Result := SB + '}';
end;

// ------------------------------------------------------------------------
// MessageLines - B-3: fetch a [CustomMessages] body and guarantee real line
// breaks for memo pages. Current Inno Setup expands the %n constant when it
// loads [CustomMessages] (proven by the F2 hash-mismatch questions, which
// pass CustomMessage() straight to MsgBox multi-line); on such versions the
// StringChangeEx pass below is a no-op. It is kept as a safety net so the
// About/Guide memo bodies render multi-line even on a compiler that does not
// expand %n in CustomMessage results.
// The ToRtf() pass (mojibake fix) makes the return value an RTF document:
// memo pages (CreateOutputMsgMemoPage) detect the leading '{\rtf' and use it
// verbatim instead of running their lossy plain->RTF conversion. NEVER feed
// plain non-ASCII text to a memo page.
// ------------------------------------------------------------------------
function MessageLines(const MsgName: String): String;
begin
  Result := CustomMessage(MsgName);
  StringChangeEx(Result, '%n', #13#10, True);
  Result := ToRtf(Result);
end;

// ------------------------------------------------------------------------
// InitializeWizard - Create the download page using built-in API
// ------------------------------------------------------------------------
procedure InitializeWizard();
begin
  // B-3: About (소개) page — first custom page, right after Welcome.
  // Signature (verified against local Inno 6.7 Examples\AllPagesExample.iss
  // line 86): CreateOutputMsgMemoPage(AfterID, ACaption, ADescription,
  // ASubCaption, AMsg).
  AboutPage := CreateOutputMsgMemoPage(wpWelcome,
    CustomMessage('AboutTitle'), '', '', MessageLines('AboutBody'));

  DownloadPage := CreateDownloadPage(
    CustomMessage('DownloadingModel'),
    CustomMessage('DownloadingModelDesc'),
    @OnDownloadProgress
  );
  ModelSkipped := False;

  // B-2: model download consent page (right after wpReady, before installation).
  // Signature: CreateInputOptionPage(AfterID, ACaption, ADescription, ASubCaption,
  // Exclusive, ListBox) - the consent description text goes in the ASubCaption
  // slot (architect plan 1.3); Exclusive=True renders the two Add() entries as
  // radio buttons so only one choice is possible. Default selection is index 0
  // (Yes) so /SILENT and /VERYSILENT installs keep the existing auto-download
  // flow without any ShouldSkipPage handling.
  ConsentPage := CreateInputOptionPage(wpReady,
    CustomMessage('ConsentTitle'), '', CustomMessage('ConsentDesc'), True, False);
  ConsentPage.Add(CustomMessage('ConsentYes'));  // index 0
  ConsentPage.Add(CustomMessage('ConsentNo'));   // index 1
  ConsentPage.SelectedValueIndex := 0;
  ModelDeclined := False;

  // B-3: Usage guide (단축키/사용 안내) — created AFTER ConsentPage so the
  // creation order stays About -> Consent -> Guide. Placed after
  // wpInstalling: the wizard reaches it once the install (including the
  // ssPostInstall model download) has finished, right before Finish.
  GuidePage := CreateOutputMsgMemoPage(wpInstalling,
    CustomMessage('GuideTitle'), '', '', MessageLines('GuideBody'));
end;

// ------------------------------------------------------------------------
// ReadConsentChoice - B-2: latch the consent page selection into ModelDeclined.
// Called from CurStepChanged at ssInstall, i.e. before any file copy starts;
// the actual download happens later at ssPostInstall.
// ------------------------------------------------------------------------
procedure ReadConsentChoice();
begin
  ModelDeclined := (ConsentPage.SelectedValueIndex = 1);
  if ModelDeclined then
    Log('User declined AI model download at consent page (chose Google Translate).')
  else
    Log('User consented to AI model download.');
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
      if SuppressibleMsgBox(FmtMessage(CustomMessage('DiskSpaceWarning'), [FreeSpaceGB]),
                mbConfirmation, MB_YESNO, IDYES) = IDNO then
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
  // F2 (security, session 260909_0002): pre-existing model verification state
  ExistingHashOk: Boolean;
  ExistingHashError: Boolean;
  MismatchQuestion: String;
begin
  // Support /SKIPMODEL command-line parameter for silent/automated installs
  if ExpandConstant('{param:SKIPMODEL|0}') = '1' then
  begin
    Log('/SKIPMODEL parameter detected - skipping model download.');
    ModelSkipped := True;
    Exit;
  end;

  // B-2: user declined the model at the consent page - continue the install
  // without downloading. ModelSkipped := True makes CreateConfigFile choose
  // engine_type=google, matching the existing skip/failure paths.
  if ModelDeclined then
  begin
    Log('Model download skipped: user declined at consent page.');
    ModelSkipped := True;
    Exit;
  end;

  ModelDestDir := ExpandConstant('{app}\models');
  ModelDestPath := ModelDestDir + '\' + MODEL_FILENAME;
  ModelTmpPath := ExpandConstant('{tmp}\') + MODEL_FILENAME;

  // Check if the model file already exists at the destination
  if FileExists(ModelDestPath) then
  begin
    Log('Model already exists at: ' + ModelDestPath);
    // F2 (security, session 260909_0002, replaces the M2 log-only path): a
    // pre-existing model that FAILS the SHA-256 pin (or cannot be hashed at
    // all) is no longer a silent "warning + skip". The user is explicitly
    // asked (mbConfirmation / MB_YESNO):
    //   YES -> delete the unverified file and FALL THROUGH into the normal
    //          download flow below. The temp-file/retry logic is untouched:
    //          the destination slot is now free, so the post-download
    //          rename/copy succeeds and RequiredSHA256OfFile re-pins the
    //          fresh bytes (plus the VerifyDownloadedModel re-check).
    //   NO  -> historical behavior: keep the file untouched, skip the
    //          download, continue installation. The runtime now re-verifies
    //          the model before loading it (F3, src/engine.cpp), so a file
    //          left unverified here cannot silently serve translations.
    // Matching hash (or empty pin on dev builds) keeps the old info dialog.
    // Under /SILENT or /SUPPRESSIBLE the confirmation defaults to NO: the
    // installer never deletes a pre-existing file without a visible answer.
    ExistingHashOk := True;
    ExistingHashError := False;
    try
      ExistingHashOk := VerifyDownloadedModel(ModelDestPath);
    except
      Log('WARNING: could not hash pre-existing model file: ' + GetExceptionMessage());
      ExistingHashOk := False;
      ExistingHashError := True;
    end;

    if ExistingHashOk then
    begin
      SuppressibleMsgBox(CustomMessage('ModelAlreadyExists'), mbInformation, MB_OK, IDOK);
      Exit;
    end;

    if ExistingHashError then
      MismatchQuestion := CustomMessage('ModelHashUnverifiableQuestion')
    else
      MismatchQuestion := CustomMessage('ModelHashMismatchQuestion');

    if SuppressibleMsgBox(MismatchQuestion, mbConfirmation, MB_YESNO, IDNO) = IDYES then
    begin
      if DeleteFile(ModelDestPath) then
      begin
        Log('F2: user chose to delete the unverified pre-existing model; re-downloading to: ' + ModelDestPath);
        // fall through to the download flow below
      end
      else
      begin
        // Deletion failed (file locked). Do NOT download over it; surface
        // the state and continue without a model: ModelSkipped := True makes
        // CreateConfigFile choose engine_type=google, and the runtime F3
        // check keeps the unverifiable file from loading locally.
        Log('F2: user chose re-download but the unverified file could not be deleted: ' + ModelDestPath);
        SuppressibleMsgBox(CustomMessage('ModelHashDeleteFailed'), mbError, MB_OK, IDOK);
        ModelSkipped := True;
        Exit;
      end;
    end
    else
    begin
      Log('F2: user chose to keep the unverified pre-existing model; download skipped. It was NOT touched by this installer.');
      Exit;
    end;
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
      // Try rename first (zero-copy if same volume), fall back to copy+delete
      if RenameFile(ModelTmpPath, ModelDestPath) then
      begin
        Log('Model file moved (renamed) to: ' + ModelDestPath);
      end
      else if CopyFile(ModelTmpPath, ModelDestPath, False) then
      begin
        Log('Model file copied to: ' + ModelDestPath);
        if DeleteFile(ModelTmpPath) then
          Log('Temp model file deleted: ' + ModelTmpPath)
        else
          Log('WARNING: Could not delete temp model file: ' + ModelTmpPath);
      end
      else
      begin
        Log('Failed to copy model file to destination.');
        SuppressibleMsgBox(CustomMessage('DownloadFailed'), mbError, MB_OK, IDOK);
        ModelSkipped := True;
        Exit;
      end;
    end
    else
    begin
      // Download failed - offer Retry / Skip / Cancel
      UserChoice := SuppressibleMsgBox(
        CustomMessage('DownloadFailed') + #13#10#13#10 +
        CustomMessage('DownloadFailedDetail') + #13#10#13#10 +
        CustomMessage('DownloadRetry'),
        mbError,
        MB_YESNOCANCEL,
        IDNO
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

  // Choose engine type based on whether the model was downloaded.
  // SEC-1 (session 260911_0002, design 144800 §2.6 option (i)): writing
  // 'google' on decline is intentionally KEPT — it preserves out-of-box
  // translation on a clean machine. Its safety depends on the app-side
  // blocking first-run privacy notice (main.cpp REQ-208 gate): the notice
  // discloses the Google transmission and the engine/hook/worker are not
  // even constructed until it is dismissed, so no text can reach Google
  // before the user has acknowledged the disclosure. The full statement is
  // bundled at {app}\README.md ("Privacy & Data Handling" §4). Do not
  // remove that popup gate without revisiting this line.
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
// CurStepChanged - Latch consent choice pre-install; trigger model download
// and config creation post-install
// ------------------------------------------------------------------------
procedure CurStepChanged(CurStep: TSetupStep);
begin
  // B-2: read the consent choice before file copying starts (download runs
  // later at ssPostInstall, so the decision must be final by then).
  if CurStep = ssInstall then
    ReadConsentChoice();
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
var
  LocalAppData: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    RegDeleteValue(HKEY_CURRENT_USER,
      'Software\Microsoft\Windows\CurrentVersion\Run',
      'Emebalachat');
    Log('Auto-start registry entry removed.');

    // Offer to clean up user settings and diagnostic logs
    LocalAppData := ExpandConstant('{localappdata}\Emebalachat');
    if DirExists(LocalAppData) then
    begin
      if SuppressibleMsgBox(CustomMessage('UninstallCleanupPrompt'),
                            mbConfirmation, MB_YESNO, IDNO) = IDYES then
      begin
        DelTree(LocalAppData, True, True, True);
        Log('User data directory removed: ' + LocalAppData);
      end
      else
        Log('User chose to keep settings at: ' + LocalAppData);
    end;
  end;
end;
