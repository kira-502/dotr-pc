# The merge gate: nothing reaches main without it.
# usage: tools\gate.cmd <build folder>      e.g. tools\gate.cmd build\Release\bin
# Runs on a scratch copy in %TEMP% (never in the build folder, never on DotR-PC-builds\M1g), Idle priority, no window:
#   1. self-test, rc-check (762 modules), the unit tests found in the folder
#   2. pictures: lane C's check (tools\gate\pictures.cmd <folder>), GPU and exact identical
#   3. duels: the bot-duel seeds listed in tools\gate\m1g-baseline.txt, in parallel, compared with their lines there
# Exit 0 = gate OK. Exit 2 = only duel results differ from the baseline: allowed only with the reason in the commit AND the
# oracle (original PS2 code) agreeing with the new result on that seed. Exit 1 = failed.
param([Parameter(Mandatory = $true)][string]$Build)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$gate = Join-Path $PSScriptRoot "gate"
$Build = (Resolve-Path $Build).Path
$iso = if ($env:DOTR_ISO) { $env:DOTR_ISO } else { Join-Path (Split-Path -Parent $root) "ISOs\Yu-Gi-Oh! The Duelists of the Roses (USA).iso" }
# the seeds and their expected results: tools\gate\m1g-baseline.txt (the duel baseline, recorded from main)
$base = @{}
$cpubase = @{}      # "cpu seed N: X us per field": the game thread's CPU time per duel field (speed check)
foreach ($l in Get-Content (Join-Path $gate "m1g-baseline.txt")) {
    if ($l -match "^seed (\d+): (duel end: .*)$") { $base[[int]$Matches[1]] = $Matches[2] }
    if ($l -match "^cpu seed (\d+): (\d+) us per field") { $cpubase[[int]$Matches[1]] = [int]$Matches[2] }
}
$seeds = $base.Keys | Sort-Object
$fails = New-Object System.Collections.Generic.List[string]
$differ = New-Object System.Collections.Generic.List[string]
$blocked = New-Object System.Collections.Generic.List[string]
$t0 = Get-Date

if (-not (Test-Path (Join-Path $Build "DotR.exe")) -or -not (Test-Path (Join-Path $Build "dotr.dll"))) {
    Write-Output "GATE FAILED: no DotR.exe + dotr.dll in $Build"; exit 1
}
$scratch = Join-Path $env:TEMP ("dotr-gate-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory $scratch | Out-Null
Get-ChildItem $Build -File | Where-Object { $_.Extension -in ".exe", ".dll", ".pdb" } | Copy-Item -Destination $scratch
# Where Windows app control only allows one fixed loader, put it in ..\DotR-PC-loader\DotR.exe: every dotr.dll then
# runs through it. Otherwise the freshly built DotR.exe runs.
$loader = Join-Path (Split-Path -Parent $root) "DotR-PC-loader\DotR.exe"
if (Test-Path $loader) { Copy-Item $loader (Join-Path $scratch "DotR.exe") -Force }
Write-Output "gate: $Build (scratch copy $scratch)"

function Run([string]$exe, [string]$argline, [string]$dir) {
    $out = Join-Path $dir ([IO.Path]::GetFileNameWithoutExtension($exe) + "-" + [guid]::NewGuid().ToString("N").Substring(0, 6) + ".txt")
    $sp = @{ FilePath = $exe; WorkingDirectory = $dir; PassThru = $true; NoNewWindow = $true
             RedirectStandardOutput = $out; RedirectStandardError = "$out.err" }
    if ($argline) { $sp.ArgumentList = $argline }
    try { $p = Start-Process @sp }
    catch {
        # Windows app control (Smart App Control) refusing a freshly built exe is the owner's setting: report, never work around
        if ("$_" -match "Application Control|blocked") { return @{ code = -2; text = "$_" } }
        return @{ code = -1; text = "$_" }
    }
    $null = $p.Handle                            # keeps the exit code readable after the exit
    try { $p.PriorityClass = "Idle" } catch {}
    $p.WaitForExit()
    return @{ code = $p.ExitCode; text = (Get-Content $out -Raw -ErrorAction SilentlyContinue) + (Get-Content "$out.err" -Raw -ErrorAction SilentlyContinue) }
}

# 3 (started first: the longest part) duels, one folder each
$duels = foreach ($s in $seeds) {
    $d = Join-Path $scratch "seed$s"
    New-Item -ItemType Directory $d | Out-Null
    Copy-Item (Join-Path $scratch "DotR.exe"), (Join-Path $scratch "dotr.dll") $d
    $p = Start-Process (Join-Path $d "DotR.exe") -ArgumentList "--boot --headless --fast --bot-duel $s --bot-log `"$d\duel.log`" --iso `"$iso`"" -WorkingDirectory $d -PassThru -NoNewWindow -RedirectStandardOutput "$d\out.txt" -RedirectStandardError "$d\err.txt"
    $null = $p.Handle
    try { $p.PriorityClass = "Idle" } catch {}
    @{ seed = $s; p = $p; dir = $d }
}

# 1. self-test, rc-check, unit tests
$r = Run (Join-Path $scratch "DotR.exe") "--selftest --iso `"$iso`"" $scratch
if ($r.code -eq -2) { Write-Output "GATE STOPPED: Windows app control blocked DotR.exe/dotr.dll. Tell the lead at once."; exit 1 }
if ($r.code -ne 0) { $fails.Add("self-test (exit $($r.code))") } else { Write-Output "  [ok]   self-test" }
$r = Run (Join-Path $scratch "DotR.exe") "--rc-check --iso `"$iso`"" $scratch
if ($r.code -ne 0 -or $r.text -notmatch "rc-check passed|762") { $fails.Add("rc-check (exit $($r.code))") }
else { Write-Output ("  [ok]   rc-check: " + (($r.text -split "`n") | Where-Object { $_ -match "rc-check:" } | Select-Object -First 1).Trim()) }
foreach ($t in "gfx_tests", "gfx_vu0_tests", "snd_tests", "mcimport_tests", "rc_unit") {
    $exe = Join-Path $scratch "$t.exe"
    if (-not (Test-Path $exe)) { Write-Output "  [--]   $t not in this build"; continue }
    $r = Run $exe "" $scratch
    if ($r.code -eq -2) { $blocked.Add("$t (Windows app control blocked it; owner's setting)"); continue }
    if ($r.code -ne 0) { $fails.Add("$t (exit $($r.code))") } else { Write-Output "  [ok]   $t" }
}

# 2. pictures (lane C's command)
$pic = Join-Path $gate "pictures.cmd"
if (Test-Path $pic) {
    $out = & cmd /c "`"$pic`" `"$scratch`"" 2>&1 | Out-String
    if ($LASTEXITCODE -eq 4) { Write-Output "  [skip] pictures: no reference captures (tools\gate\pictures.cmd)" }
    elseif ($LASTEXITCODE -ne 0) { $fails.Add("pictures (lane C's check, exit $LASTEXITCODE)"); Write-Output $out } else { Write-Output "  [ok]   pictures: GPU and exact identical" }
} else {
    $fails.Add("pictures: not wired yet (tools\gate\pictures.cmd from lane C)")
}

# 3. duels against the baseline
foreach ($d in $duels) {
    $d.p.WaitForExit()
    $log = Join-Path $d.dir "duel.log"
    $end = if (Test-Path $log) { Select-String $log -Pattern "duel end:" | ForEach-Object { $_.Line } | Select-Object -Last 1 } else { $null }
    if ($d.p.ExitCode -ne 0 -or -not $end) {
        $fails.Add("duel seed $($d.seed): exit $($d.p.ExitCode), no result (see $log)")
        continue
    }
    $end = $end -replace "^.*?(duel end: )", '$1'
    # speed: the game thread's own CPU time per field (busy-PC proof) against the baseline's, fail above 1.5x
    $cpu = Select-String $log -Pattern "cpu: (\d+) us per field" | Select-Object -Last 1
    if ($cpu -and $cpubase.ContainsKey($d.seed)) {
        $us = [int]$cpu.Matches[0].Groups[1].Value
        $ratio = $us / [double]$cpubase[$d.seed]
        $txt = "duel seed $($d.seed) speed: {0} us of game-thread CPU per field, baseline {1} ({2:0.00}x)" -f $us, $cpubase[$d.seed], $ratio
        if ($ratio -gt 1.5) { $fails.Add("$($txt): slower than 1.5x") } else { Write-Output "  [ok]   $txt" }
    } elseif ($cpubase.Count) {
        $fails.Add("duel seed $($d.seed) speed: no cpu line in its log or no baseline for it")
    }
    $want = $base[$d.seed]
    if ($end -eq $want) { Write-Output "  [ok]   duel seed $($d.seed): same as the baseline ($end)"; continue }
    $parse = { param($x) if ($x -match "winner (.+?), reason (\d+), turns (\d+), lp (\d+) (\d+), frames (\d+), hash (\w+)") {
                 @{ winner = $Matches[1]; reason = $Matches[2]; turns = $Matches[3]; lp = "$($Matches[4]) $($Matches[5])"; frames = $Matches[6]; hash = $Matches[7] } } }
    $a = & $parse $want; $b = & $parse $end
    $what = foreach ($k in "winner", "reason", "turns", "lp", "frames", "hash") { if ($a[$k] -ne $b[$k]) { "$k $($a[$k]) -> $($b[$k])" } }
    $play = ($a.winner -ne $b.winner) -or ($a.turns -ne $b.turns) -or ($a.lp -ne $b.lp)
    $differ.Add("duel seed $($d.seed): " + ($what -join ", ") + $(if ($play) { "  (PLAY CHANGED: run the oracle on this seed)" } else { "  (state only)" }))
}

$secs = [int]((Get-Date) - $t0).TotalSeconds
Write-Output ""
foreach ($f in $fails) { Write-Output "  [FAIL] $f" }
foreach ($x in $differ) { Write-Output "  [DIFF] $x" }
foreach ($x in $blocked) { Write-Output "  [BLOCKED] $x" }
if ($fails.Count) { Write-Output "GATE FAILED ($secs s). Scratch copy kept: $scratch"; exit 1 }
Remove-Item -Recurse -Force $scratch
if ($differ.Count) {
    Write-Output "GATE: duel results differ from the baseline ($secs s). Allowed only with the reason in the commit and, where play changed, the oracle agreeing on that seed."
    exit 2
}
Write-Output "GATE OK ($secs s)"
exit 0
