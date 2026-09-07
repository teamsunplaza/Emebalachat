# uia_read_edit.ps1 - read the text of the focused edit/document control of a
# target process via UI Automation (ValuePattern), as the Layer-2 fallback
# reader for tools/e2e/req027_e2e.py when WM_GETTEXT is unavailable.
#
# Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File uia_read_edit.ps1 -TargetPid <pid>
# Output (stdout, UTF-8):
#   "UIA:EMPTY"            control found, text is empty
#   "UIA:TEXT\n<content>"  control found, text follows the marker line
#   exit code 1 + "UIA:ERR <reason>" on stderr when no readable control found
#
# No installation required: System.Windows.Automation ships with .NET Framework
# (UIAutomationClient / UIAutomationTypes assemblies).
param(
    [Parameter(Mandatory = $true)]
    [int]$TargetPid
)

$ErrorActionPreference = 'Stop'
try {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes

    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $pidCondition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $TargetPid)
    $windows = $root.FindAll(
        [System.Windows.Automation.TreeScope]::Children, $pidCondition)

    if ($windows.Count -eq 0) {
        [Console]::Error.WriteLine("UIA:ERR no top-level window for pid $TargetPid")
        exit 1
    }

    # Preferred control types for a text-editing surface (Notepad Win11:
    # RichEditD2DPT exposes ControlType Document with ValuePattern).
    $wanted = @(
        [System.Windows.Automation.ControlType]::Document,
        [System.Windows.Automation.ControlType]::Edit,
        [System.Windows.Automation.ControlType]::Text
    )

    $best = $null
    foreach ($type in $wanted) {
        $cond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty, $type)
        foreach ($win in $windows) {
            $hits = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
            foreach ($h in $hits) {
                if ($h -and $h.GetCurrentPattern(
                        [System.Windows.Automation.ValuePattern]::Pattern)) {
                    $best = $h
                    break
                }
            }
            if ($best) { break }
        }
        if ($best) { break }
    }

    if (-not $best) {
        # last resort: any descendant exposing ValuePattern
        foreach ($win in $windows) {
            $all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.Condition]::TrueCondition)
            foreach ($e in $all) {
                if ($e.GetCurrentPattern(
                        [System.Windows.Automation.ValuePattern]::Pattern)) {
                    $best = $e
                    break
                }
            }
            if ($best) { break }
        }
    }

    if (-not $best) {
        [Console]::Error.WriteLine("UIA:ERR no ValuePattern control under pid $TargetPid")
        exit 1
    }

    $vp = $best.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
    $text = $vp.Current.Value

    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    if ([string]::IsNullOrEmpty($text)) {
        Write-Output "UIA:EMPTY"
    }
    else {
        Write-Output "UIA:TEXT"
        Write-Output $text
    }
    exit 0
}
catch {
    [Console]::Error.WriteLine("UIA:ERR $($_.Exception.Message)")
    exit 1
}
