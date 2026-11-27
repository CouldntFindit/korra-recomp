param([string]$Logs = "")
if (-not $Logs) { $Logs = Join-Path (Split-Path -Parent $PSScriptRoot) "logs" }
$tags = @("s1c1","s1c2","s1c3","s1c4","s1c5","s1w100_1","s1w100_2","s1w100_3","s1w100_4","s1w100_5")
foreach ($t in $tags) {
    $p = Join-Path $Logs ("{0}_perf.txt" -f $t)
    if (-not (Test-Path $p)) { continue }
    $dt = @(); $sl = @(); $lk = @(); $calls = @()
    foreach ($line in Get-Content $p) {
        if ($line -match '^\[perf\] t=(\d+)s .*dt=([0-9.]+)/.*sleep=([0-9.]+)/.*lock=([0-9.]+)/.*calls=(\d+)/') {
            $sec = [int]$matches[1]
            if ($sec -ge 150 -and $sec -le 280) {
                $dt += [double]$matches[2]; $sl += [double]$matches[3]
                $lk += [double]$matches[4]; $calls += [double]$matches[5]
            }
        }
    }
    if ($dt.Count -eq 0) { continue }
    $m = { param($a) ($a | Measure-Object -Average).Average }
    "{0,-10} n={1,3}  dt={2,6:N1}  sleep={3,6:N1}  lock={4,6:N1}  calls={5,6:N0}" -f `
        $t, $dt.Count, (& $m $dt), (& $m $sl), (& $m $lk), (& $m $calls)
}
