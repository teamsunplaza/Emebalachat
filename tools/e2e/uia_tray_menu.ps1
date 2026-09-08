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
#   TRAY:OK:<item>          selection invoked (InvokeTargetLang mode)
#   TRAY:NAME:<name>        one submenu item Name (EnumUiLang mode, UTF-8)
#   TRAY:ENUMDONE:<count>   enumeration finished (EnumUiLang mode)
#   TRAY:NOTFOUND:<step>    a walker step could not resolve its element
#   TRAY:ERR:<message>      exception (also ESC-cleanup attempted)
# Exit code mirrors the verdict (0 OK, 1 refusal) but the harness judges on
# the TRAY: lines. ANY non-OK result simply triggers the documented
# config-seed fallback in the harness - this script never has to "pass".
#
# Parameters:
#   -ItemRegex   regex for the final menu item Name (required in
#                InvokeTargetLang / InvokeMenuItem modes, unused in EnumUiLang)
#   -IconName    regex to find our tray icon (default 'Emebala')
#   -Mode        'InvokeTargetLang' (default, unchanged B-6c contract) or
#                'EnumUiLang' (REQ-037/B-4, design §4.3 E2E-UILANG-2): open
#                the Interface-Language submenu and dump EVERY item Name as
#                TRAY:NAME:<name> lines + TRAY:ENUMDONE:<count>, then ESC.
#                Read-only: nothing is invoked, config is never written.
#                'InvokeMenuItem' (REQ-040/B-6, design §4.3 E2E-RESET-1):
#                open the root tray menu and Invoke the FIRST item whose Name
#                matches -ItemRegex anywhere in the menu tree (used to open
#                the localized "About Emebala Chat..." entry without knowing
#                the UI language). Same output contract as InvokeTargetLang.
param(
    [string]$ItemRegex = '',
    [string]$IconName = 'Emebala',
    [int]$TimeoutSec = 40,
    [ValidateSet('InvokeTargetLang', 'EnumUiLang', 'InvokeMenuItem')]
    [string]$Mode = 'InvokeTargetLang'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

# EnumUiLang dumps ENDONYMS (한국어, العربية, မြန်မာစာ, ...). Windows PowerShell
# 5.1 writes stdout with the OEM console codepage, which would mojibake every
# non-ASCII name into the harness pipe - force UTF-8 (ASCII TRAY: verdict
# lines are byte-identical either way, req027_e2e.py decodes utf-8-sig).
$enc = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $enc
$OutputEncoding = $enc

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

function Send-Escape {
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
        Send-Escape
        Write-Tray 'NOTFOUND:menu-items-uia-empty(TrackPopupMenu modal loop starves the UIA menu provider - tray automation INFEASIBLE via UIA)'
        exit 1
    }

    # -- 3c. EnumUiLang mode (REQ-037/B-4, design §4.3 E2E-UILANG-2) ---------
    # Open the Interface-Language submenu and dump every MenuItem Name across
    # ALL #32768 panes (a Win32 submenu opens as an ADDITIONAL desktop-child
    # popup, so the root-pane walk below would miss the language entries).
    # Read-only: ESC-closes the menu, never invokes an item.
    if ($Mode -eq 'EnumUiLang') {
        $ulRx = [regex]'(인터페이스 언어|Interface Language)'
        $ul = $null
        while ((Get-Date) -lt $deadline) {
            $ul = Find-ByName $menu $ulRx 7
            if ($ul) { break }
            Start-Sleep -Milliseconds 150
        }
        if (-not $ul) { Send-Escape; Write-Tray 'NOTFOUND:uilang-submenu'; exit 1 }
        if (-not (Expand-Item $ul)) { Send-Escape; Write-Tray 'NOTFOUND:uilang-expand'; exit 1 }

        # An expanded Win32 submenu is BOTH a separate desktop-child #32768
        # pane AND (per the UIA bridge) reachable through the root pane's
        # Descendants tree - so the all-panes walk can see the same item
        # twice. First-seen-order dedupe keeps the dump unambiguous and the
        # UIA tree order (top-to-bottom menu order) intact.
        $names = @()
        $seen = @{}
        $clsCond2 = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ClassNameProperty, '#32768')
        $waitUntil = (Get-Date).AddSeconds(5)
        while ((Get-Date) -lt $waitUntil) {
            $panes = $desktop.FindAll([System.Windows.Automation.TreeScope]::Children, $clsCond2)
            $names = @(); $seen = @{}
            foreach ($p in $panes) {
                $mi = $p.FindAll([System.Windows.Automation.TreeScope]::Descendants, $miCond)
                foreach ($m in $mi) {
                    try { $n = $m.Current.Name } catch { $n = '' }
                    if (-not $n) { continue }        # separators surface empty
                    if (-not $seen.ContainsKey($n)) { $seen[$n] = $true; $names += $n }
                }
            }
            # Auto + 37 endonyms = 38 unique names once the submenu is
            # readable; 38 is the conservative "tree readable" threshold
            # (root-pane items only add to it). Below it -> partial starve,
            # honest NOTFOUND (harness judges INCONCLUSIVE, never FAIL).
            if ($names.Count -ge 38) { break }
            Start-Sleep -Milliseconds 150
        }
        Send-Escape
        Start-Sleep -Milliseconds 120
        Send-Escape
        if ($names.Count -lt 38) {
            Write-Tray "NOTFOUND:uilang-items-partial($($names.Count))"
            exit 1
        }
        foreach ($n in $names) { Write-Tray "NAME:$n" }
        Write-Tray "ENUMDONE:$($names.Count)"
        exit 0
    }
    if (-not $ItemRegex) { Write-Tray 'ERR:missing-ItemRegex(InvokeTargetLang mode)'; exit 1 }

    # -- 3d. InvokeMenuItem mode (REQ-040/B-6 E2E-RESET-1) -------------------
    # Single generic step: wait for the item to appear in the open menu tree
    # and invoke it (the caller's regex names the target, e.g. the About
    # entry 'Emebala Chat.*\u2026' which is locale-independent because every
    # menu_about string embeds the brand token and ends with an ellipsis
    # while no other item pairs both). No submenu walking.
    if ($Mode -eq 'InvokeMenuItem') {
        $wantRx = [regex]$ItemRegex
        $hit = $null
        while ((Get-Date) -lt $deadline) {
            $hit = Find-ByName $menu $wantRx 6
            if ($hit) { break }
            Start-Sleep -Milliseconds 150
        }
        if (-not $hit) { Send-Escape; Write-Tray "NOTFOUND:item($ItemRegex)"; exit 1 }
        $hitName = ''
        try { $hitName = $hit.Current.Name } catch { }
        if (-not (Invoke-Item $hit)) {
            try { $hit.SetFocus(); Start-Sleep -Milliseconds 100
                [NativeMethods]::keybd_event(0x0D, 0, 0, 0)
                Start-Sleep -Milliseconds 60
                [NativeMethods]::keybd_event(0x0D, 0, $KEYEVENTF_KEYUP, 0) }
            catch { Send-Escape; Write-Tray 'ERR:invoke-failed'; exit 1 }
        }
        Write-Tray "OK:$hitName"
        exit 0
    }

    # -- 4. walk: type group -> target submenu -> item ----------------------
    $groupRx = [regex]'(키보드 타이핑|Keyboard Typing)'
    $group = Find-ByName $menu $groupRx 5
    if (-not $group) { Send-Escape; Write-Tray 'NOTFOUND:typing-group'; exit 1 }
    if (-not (Expand-Item $group)) { Send-Escape; Write-Tray 'NOTFOUND:typing-expand'; exit 1 }

    $tgtRx = [regex]'(도착 언어|Target language)'
    $tgt = $null
    while ((Get-Date) -lt $deadline) {
        $tgt = Find-ByName $menu $tgtRx 7
        if ($tgt) { break }
        Start-Sleep -Milliseconds 150
    }
    if (-not $tgt) { Send-Escape; Write-Tray 'NOTFOUND:target-lang-submenu'; exit 1 }
    if (-not (Expand-Item $tgt)) { Send-Escape; Write-Tray 'NOTFOUND:target-expand'; exit 1 }

    $itemRx = [regex]$ItemRegex
    $item = $null
    while ((Get-Date) -lt $deadline) {
        $item = Find-ByName $menu $itemRx 8
        if ($item) { break }
        Start-Sleep -Milliseconds 150
    }
    if (-not $item) { Send-Escape; Write-Tray "NOTFOUND:item($ItemRegex)"; exit 1 }

    if (-not (Invoke-Item $item)) {
        # last resort: keyboard-activate the focused item
        try { $item.SetFocus(); Start-Sleep -Milliseconds 100
            [NativeMethods]::keybd_event(0x0D, 0, 0, 0)
            Start-Sleep -Milliseconds 60
            [NativeMethods]::keybd_event(0x0D, 0, $KEYEVENTF_KEYUP, 0) }
        catch { Send-Escape; Write-Tray 'ERR:invoke-failed'; exit 1 }
    }
    Write-Tray "OK:$($item.Current.Name)"
    exit 0
}
catch {
    try { Send-Escape } catch { }
    Write-Tray "ERR:$($_.Exception.Message)"
    exit 1
}
