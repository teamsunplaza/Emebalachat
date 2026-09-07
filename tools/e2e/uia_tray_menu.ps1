# uia_tray_menu.ps1 - B-6c multi_lang tray-menu automation (1st-choice path,
# design 210000_architect section 2.4 scenario 4).
#
# What it does:
#   1. Locate the app's notification-area icon via UIA (System.Windows.
#      Automation). Win11: the icon lives either directly in the tray
#      toolbar or inside the "hidden icons" overflow flyout, which we open
#      first if needed.
#   2. Hover it and send a REAL right-click at the icon center. The Shell
#      delivers WM_TRAYICON/WM_RBUTTONUP to the app's controller window ->
#      SystemTray::ShowContextMenu -> TrackPopupMenuEx at GetCursorPos
#      (src/ui/tray.cpp L386-390), so moving the physical cursor onto the
#      icon is REQUIRED (the menu tracks the cursor position).
#   3. Walk the resulting menu (#32768) with UIA: expand the type group
#      ("키보드 타이핑" / "Keyboard Typing"), then the target-language
#      submenu ("도착 언어 ..." / "Target language ..."), then Invoke the
#      item whose Name matches -ItemRegex (e.g. '^JA\s*-'). The app's
#      ApplyLanguageChange coordinator persists + refreshes (runtime path,
#      REQ-019).
#
# Output contract (parsed by req027_e2e.py): lines starting with TRAY:.
#   TRAY:OK:<item>          selection invoked
#   TRAY:NOTFOUND:<step>    a walker step could not resolve its element
#   TRAY:ERR:<message>      exception (also ESC-cleanup attempted)
# Exit code mirrors the verdict (0 OK, 1 refusal) but the harness judges on
# the TRAY: lines. ANY non-OK result simply triggers the documented
# config-seed fallback in the harness - this script never has to "pass".
#
# Parameters:
#   -ItemRegex   regex for the final menu item Name (required)
#   -IconName    regex to find our tray icon (default 'Emebala')
param(
    [Parameter(Mandatory = $true)][string]$ItemRegex,
    [string]$IconName = 'Emebala',
    [int]$TimeoutSec = 40
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

if (-not ('NativeMethods' -as [type])) {
    Add-Type -Namespace '' -Name 'NativeMethods' -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, int dwExtraInfo);
[DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, int dwExtraInfo);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
'@
}

$MK_RBUTTONDOWN = 0x0008; $MK_RBUTTONUP = 0x0010
$VK_ESCAPE = 0x1B; $KEYEVENTF_KEYUP = 0x0002

function Write-Tray([string]$msg) { Write-Output "TRAY:$msg" }

function Press-Escape {
    [NativeMethods]::keybd_event($VK_ESCAPE, 0, 0, 0)
    Start-Sleep -Milliseconds 60
    [NativeMethods]::keybd_event($VK_ESCAPE, 0, $KEYEVENTF_KEYUP, 0)
}

function Find-ByName($root, [regex]$rx, [int]$depth = 8) {
    # breadth-ish recursive search; menus are shallow. Guarded by depth.
    if ($null -eq $root) { return $null }
    try { $name = $root.Current.Name } catch { $name = '' }
    if ($name -and $rx.IsMatch($name)) { return $root }
    if ($depth -le 0) { return $null }
    try {
        $children = $root.FindAll([System.Windows.Automation.TreeScope]::Children,
                                  [System.Windows.Automation.Condition]::TrueCondition)
    } catch { return $null }
    foreach ($c in $children) {
        $hit = Find-ByName $c $rx ($depth - 1)
        if ($hit) { return $hit }
    }
    return $null
}

function Expand-Item($item) {
    # Win32 submenu items expose ExpandCollapse through UIA; fall back to a
    # hover + right arrow keystroke (menus accept keyboard navigation and
    # VK_RIGHT on a selected item opens its submenu).
    try {
        $ec = $item.GetCurrentPattern(
            [System.Windows.Automation.ExpandCollapsePattern]::Pattern)
        $ec.Expand()
        Start-Sleep -Milliseconds 350
        return $true
    } catch { }
    try {
        $item.SetFocus()
        Start-Sleep -Milliseconds 150
        [NativeMethods]::keybd_event(0x27, 0, 0, 0)   # VK_RIGHT down
        Start-Sleep -Milliseconds 60
        [NativeMethods]::keybd_event(0x27, 0, $KEYEVENTF_KEYUP, 0)
        Start-Sleep -Milliseconds 350
        return $true
    } catch { return $false }
}

function Invoke-Item($item) {
    try {
        $vp = $item.GetCurrentPattern(
            [System.Windows.Automation.InvokePattern]::Pattern)
        $vp.Invoke()
        return $true
    } catch { }
    return $false
}

$deadline = (Get-Date).AddSeconds($TimeoutSec)
try {
    $desktop = [System.Windows.Automation.AutomationElement]::RootElement

    # -- 1. find our tray icon (toolbar directly, then via overflow) --------
    $iconRx = [regex]$IconName
    $icon = Find-ByName $desktop $iconRx 9
    if (-not $icon) {
        $ovRx = [regex]'(숨겨진 아이콘|Show hidden icons|Hidden icons)'
        $ov = Find-ByName $desktop $ovRx 9
        if ($ov) {
            try {
                $vp = $ov.GetCurrentPattern(
                    [System.Windows.Automation.InvokePattern]::Pattern)
                $vp.Invoke()
                Start-Sleep -Milliseconds 800
            } catch { }
            $icon = Find-ByName $desktop $iconRx 10
        }
    }
    if (-not $icon) { Write-Tray 'NOTFOUND:icon'; exit 1 }

    # -- 2. real right-click at the icon center -----------------------------
    $r = $icon.Current.BoundingRectangle
    if ($r.Width -le 0 -or $r.Height -le 0) { Write-Tray 'NOTFOUND:icon-rect'; exit 1 }
    $cx = [int]($r.X + $r.Width / 2); $cy = [int]($r.Y + $r.Height / 2)
    [void][NativeMethods]::SetCursorPos($cx, $cy)
    Start-Sleep -Milliseconds 250
    [NativeMethods]::mouse_event($MK_RBUTTONDOWN, 0, 0, 0, 0)
    Start-Sleep -Milliseconds 80
    [NativeMethods]::mouse_event($MK_RBUTTONUP, 0, 0, 0, 0)

    # -- 3. the popup menu window (class #32768) ----------------------------
    # GetForegroundWindow does NOT work here: the app's tray owner is a
    # message-only window (HWND_MESSAGE, src/main.cpp L395-401) which can
    # never own the foreground, so the menu tracks while the previous
    # window stays foreground (probed live 260907). Enumerate the desktop
    # children for a #32768 Menu instead.
    $menu = $null
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 150
        try {
            $clsCond = New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::ClassNameProperty,
                '#32768')
            $found = $desktop.FindAll(
                [System.Windows.Automation.TreeScope]::Children, $clsCond)
            foreach ($f in $found) { $menu = $f; break }
        } catch { }
        if ($menu) { break }
    }
    if (-not $menu) { Write-Tray 'NOTFOUND:menu(#32768 not in desktop children)'; exit 1 }

    # -- 3b. definitive probe of UIA menu contents --------------------------
    # Live finding (260907, twice): the #32768 pane opens at the right rect,
    # but Children(True) == 0 AND Descendants(MenuItem) == 0 while THIS
    # process tracks the menu: the app's GUI thread sits in the modal
    # TrackPopupMenuEx loop and the MSAA->UIA bridge serves no item tree.
    # Enumerate both ways; an empty tree is an INFEASIBLE verdict (the
    # harness then uses the documented config-seed fallback), NOT a bug in
    # this walker.
    $miCond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::MenuItem)
    $kids = $menu.FindAll([System.Windows.Automation.TreeScope]::Children,
        [System.Windows.Automation.Condition]::TrueCondition)
    $items = $menu.FindAll([System.Windows.Automation.TreeScope]::Descendants, $miCond)
    if (($kids.Count -eq 0) -and ($items.Count -eq 0)) {
        Press-Escape
        Write-Tray 'NOTFOUND:menu-items-uia-empty(TrackPopupMenu modal loop starves the UIA menu provider - tray automation INFEASIBLE via UIA)'
        exit 1
    }

    # -- 4. walk: type group -> target submenu -> item ----------------------
    $groupRx = [regex]'(키보드 타이핑|Keyboard Typing)'
    $group = Find-ByName $menu $groupRx 5
    if (-not $group) { Press-Escape; Write-Tray 'NOTFOUND:typing-group'; exit 1 }
    if (-not (Expand-Item $group)) { Press-Escape; Write-Tray 'NOTFOUND:typing-expand'; exit 1 }

    $tgtRx = [regex]'(도착 언어|Target language)'
    $tgt = $null
    while ((Get-Date) -lt $deadline) {
        $tgt = Find-ByName $menu $tgtRx 7
        if ($tgt) { break }
        Start-Sleep -Milliseconds 150
    }
    if (-not $tgt) { Press-Escape; Write-Tray 'NOTFOUND:target-lang-submenu'; exit 1 }
    if (-not (Expand-Item $tgt)) { Press-Escape; Write-Tray 'NOTFOUND:target-expand'; exit 1 }

    $itemRx = [regex]$ItemRegex
    $item = $null
    while ((Get-Date) -lt $deadline) {
        $item = Find-ByName $menu $itemRx 8
        if ($item) { break }
        Start-Sleep -Milliseconds 150
    }
    if (-not $item) { Press-Escape; Write-Tray "NOTFOUND:item($ItemRegex)"; exit 1 }

    if (-not (Invoke-Item $item)) {
        # last resort: keyboard-activate the focused item
        try { $item.SetFocus(); Start-Sleep -Milliseconds 100
            [NativeMethods]::keybd_event(0x0D, 0, 0, 0)
            Start-Sleep -Milliseconds 60
            [NativeMethods]::keybd_event(0x0D, 0, $KEYEVENTF_KEYUP, 0) }
        catch { Press-Escape; Write-Tray 'ERR:invoke-failed'; exit 1 }
    }
    Write-Tray "OK:$($item.Current.Name)"
    exit 0
}
catch {
    try { Press-Escape } catch { }
    Write-Tray "ERR:$($_.Exception.Message)"
    exit 1
}
