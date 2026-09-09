# Phase 2 include-removal independent audit (debug mode)
# For each removed header x file, search for symbols that header provides.
# Result "" / 0 hits => removal safe (symbol unused).
$ErrorActionPreference = 'Stop'

$checks = @(
    @{ File='src/ui/about_window.cpp'; Header='<algorithm>'; Pat='std::(find|sort|for_each|max|min|clamp|remove|transform|copy|unique|adjacent_find|count)\b|\bmax\(|\bmin\(|\bclamp\(' },
    @{ File='src/ui/about_window.cpp'; Header='<cmath>';     Pat='std::(ceil|floor|fabs|sqrt|pow|abs|round|fmod|cos|sin|atan|isnan|isinf)\b|\bceil\(|\bfloor\(|\bfabs\(|\bsqrt\(|\bround\(' },
    @{ File='src/ui/about_window.cpp'; Header='<cstdio>';   Pat='printf|sprintf|snprintf|swprintf|fprintf|scanf|\bFILE\b|stdout|stderr|fopen|fclose|fputs|fputc' },
    @{ File='src/ui/about_window.cpp'; Header='<cwchar>';   Pat='wcslen|wcscpy|wcsncpy|wcscmp|wcsncmp|wcschr|wcsrchr|wmemset|wcstok|wmemcpy|wmemset|wcsspn|wcscspn' },
    @{ File='src/ui/about_window.cpp'; Header='<iterator>'; Pat='std::(next|prev|advance|distance|begin|end|back_inserter|front_inserter|inserter)\b' },
    @{ File='src/ui/tray.cpp';         Header='<cmath>';    Pat='std::(ceil|floor|fabs|sqrt|pow|abs|round|fmod|cos|sin|atan)\b|\bceil\(|\bfloor\(|\bsqrt\(|\babs\(|\bfabs\(' },
    @{ File='src/ui/badge.cpp';         Header='<cstdio>';  Pat='printf|sprintf|snprintf|swprintf|fprintf|scanf|\bFILE\b|stdout|stderr|fopen|fclose' },
    @{ File='src/ui/drag_icon.cpp';    Header='<cmath>';    Pat='std::(ceil|floor|fabs|sqrt|pow|abs|round|fmod|cos|sin|atan)\b|\bceil\(|\bfloor\(|\bsqrt\(|\babs\(|\bfabs\(' },
    @{ File='src/ui/drag_icon.cpp';    Header='<cstdio>';  Pat='printf|sprintf|snprintf|swprintf|fprintf|scanf|\bFILE\b|stdout|stderr|fopen|fclose' },
    @{ File='src/ui/dpi.cpp';           Header='<cstdio>';  Pat='printf|sprintf|snprintf|swprintf|fprintf|scanf|\bFILE\b|stdout|stderr|fopen|fclose' },
    @{ File='tests/run_tests.cpp';     Header='<cassert>'; Pat='\bassert\s*\(' },
    @{ File='tests/run_tests.cpp';     Header='<memory>';  Pat='std::(unique_ptr|shared_ptr|make_unique|make_shared|weak_ptr|addressof|allocator)\b|unique_ptr<|shared_ptr<' }
)

$results = foreach ($c in $checks) {
    $content = Get-Content $c.File -Raw
    $lines = Get-Content $c.File
    $hits = 0
    $samples = @()
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match $c.Pat) {
            # exclude comment-only lines mentioning the header docs
            $hits++
            if ($samples.Count -lt 3) { $samples += ("L" + ($i+1) + ": " + $lines[$i].Trim()) }
        }
    }
    [PSCustomObject]@{
        File = $c.File; Header = $c.Header; Hits = $hits; Samples = ($samples -join ' | ')
    }
}
$results | Format-Table File, Header, Hits -AutoSize | Out-String -Width 200
$results | Where-Object { $_.Hits -gt 0 } | ForEach-Object { "HIT DETAIL: $($_.File) $($_.Header)"; $_.Samples }
"=== if no HIT DETAIL lines above, all removals independently confirmed safe ==="
