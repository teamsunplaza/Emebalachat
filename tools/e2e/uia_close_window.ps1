# uia_close_window.ps1 - dismiss the Win11 Notepad "save?" dialog that pops
# up when a DIRTY document window is closed, by invoking the non-saving
# button via UI Automation. Companion of req027_e2e.py's teardown.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File uia_close_window.ps1 -TargetHwnd <hwnd-int>
#
# Behavior: walks descendants of the window for Button controls whose Name
# matches (localized) "don't save" (ko: 저장 안 함 / en: Don't save / de:
# Nicht speichern ...) and Invoke()s it. Name pattern first; if only two
# buttons exist (save/cancel) it presses the LAST one conservatively ONLY
# when its name matches the cancel family. No dialog -> exit 0 silently.
# Never touches anything outside the given hwnd.
param(
    [Parameter(Mandatory = $true)]
    [long]$TargetHwnd
)

$ErrorActionPreference = 'Stop'
try {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes

    $hwnd = [IntPtr]$TargetHwnd
    $el = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
    if (-not $el) {
        Write-Output "UIACLOSE:NOWINDOW"
        exit 0
    }

    $all = $el.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Button)))

    # button name regexes, priority order: non-saving first. Win11 Notepad
    # (ko-KR 24H2, probed live 260907) labels the discard button
    # "저장하지 않음" with 않 (U+C07C) - the earlier pattern used 안
    # (U+C548) and never matched, so teardown stalled on NOMATCH. The
    # permissive 저장+않 pairing covers both spacing variants.
    $discardPatterns = @(
        '(?i)don.?t save', '(?i)save later',
        '저장.*않',
        '(?i)discard', '(?i)not save', 'Nicht speichern',
        '保存しない', '不保存'
    )
    $cancelPatterns = @(
        '(?i)^cancel$', '취소', '(?i)abort', 'Abbrechen', 'キャンセル'
    )

    function Match-Any($name, $pats) {
        foreach ($p in $pats) { if ($name -match $p) { return $true } }
        return $false
    }

    $buttons = @($all | ForEach-Object { @{ el = $_; name = $_.Current.Name } })
    if ($buttons.Count -eq 0) {
        Write-Output "UIACLOSE:NODIALOG"
        exit 0
    }

    foreach ($b in $buttons) {
        if (Match-Any $b.name $discardPatterns) {
            $bp = $b.el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
            $bp.Invoke()
            Write-Output ("UIACLOSE:INVOKED " + $b.name)
            exit 0
        }
    }
    # conservative second tier: explicit cancel button, only when the dialog
    # has exactly two buttons (save/cancel form)
    if ($buttons.Count -eq 2) {
        foreach ($b in $buttons) {
            if (Match-Any $b.name $cancelPatterns) {
                $bp = $b.el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
                $bp.Invoke()
                Write-Output ("UIACLOSE:INVOKED-CANCEL " + $b.name)
                exit 0
            }
        }
    }
    # names we refuse to press: list them so the harness log shows why
    $listed = ($buttons | ForEach-Object { $_.name }) -join ' | '
    Write-Output ("UIACLOSE:NOMATCH [" + $listed + "]")
    exit 0
}
catch {
    [Console]::Error.WriteLine("UIACLOSE:ERR $($_.Exception.Message)")
    exit 1
}
