# Build the shareable release from the current build: ..\DotR-PC-builds\release\DotR-PC beside this tree (the window exe
# under the game's name, dotr.dll, README.txt, an empty Saves\card1) and DotR-PC.zip next to it.
# No disc data goes in: the program reads each player's own disc image at run time.
# Quick checks only (rc-check: every disc module translated; the self-test); no bot duels.
# usage: powershell -File tools\make-release.ps1 [-Dev]
#   -Dev: the developer build for private sharing (build.cmd Release, switches on): DotR-PC-dev with the console
#   DotR.exe and DEVELOPING.md added
param([switch]$Dev)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cfg = if ($Dev) { "Release" } else { "Package" }   # Package: Release without the developer switches
$bin = Join-Path $root "build\$cfg\bin"
$out = Join-Path (Split-Path -Parent $root) "DotR-PC-builds\release"
$name = if ($Dev) { "DotR-PC-dev" } else { "DotR-PC" }
$dst = Join-Path $out $name
$zip = Join-Path $out "$name.zip"
$exe = "Yu-Gi-Oh! The Duelists of the Roses.exe"

foreach ($f in "DotR.exe", "DotR_GUI.exe", "dotr.dll") {
    if (-not (Test-Path (Join-Path $bin $f))) { throw "not released: build\$cfg\bin\$f is missing (run build.cmd $cfg)" }
}
$dll = [IO.File]::ReadAllBytes((Join-Path $bin "dotr.dll"))
$switches = [Text.Encoding]::Unicode.GetString($dll).Contains("--debug-menu")
if ($switches -and -not $Dev) { throw "not released: dotr.dll has the developer switches (run build.cmd Package)" }
if ($Dev -and -not $switches) { throw "not released: the dev build has no developer switches (run build.cmd Release)" }
$rc = & (Join-Path $bin "DotR.exe") --rc-check 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { Write-Output $rc; throw "not released: --rc-check failed (modules missing from this build)" }
Write-Output ($rc -split "`n")[0]
$st = & (Join-Path $bin "DotR.exe") --selftest 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { Write-Output $st; throw "not released: the self-test failed" }
Write-Output "self-test: OK"

if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
New-Item -ItemType Directory -Force (Join-Path $dst "Saves\card1") | Out-Null
Copy-Item (Join-Path $bin "DotR_GUI.exe") (Join-Path $dst $exe)
Copy-Item (Join-Path $bin "dotr.dll") $dst
Copy-Item (Join-Path $root "tools\release\README.txt") $dst
$files = @($exe, "dotr.dll", "README.txt")
if ($Dev) {
    Copy-Item (Join-Path $bin "DotR.exe") $dst
    Copy-Item (Join-Path $root "docs\DEVELOPING.md") $dst
    $files += "DotR.exe", "DEVELOPING.md"
}

# nothing but the program: no disc image, no log, no saves, no settings
$extra = Get-ChildItem -Recurse -File $dst | Where-Object { $_.Name -notin $files }
if ($extra) { throw "not released: unexpected files: $($extra.Name -join ', ')" }
# no user name, account name or user path in the program files (plain or UTF-16 text)
# (more words, e.g. account names, one per line in tools\release-guard.txt: kept out of git)
$guard = @(); $gf = Join-Path $PSScriptRoot "release-guard.txt"
if (Test-Path $gf) { $guard = @(Get-Content $gf | Where-Object { $_.Trim() -and -not $_.StartsWith("#") }) }
foreach ($f in $files | Where-Object { $_ -match "\.(exe|dll)$" }) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $dst $f))
    $texts = [Text.Encoding]::ASCII.GetString($bytes), [Text.Encoding]::Unicode.GetString($bytes),
             [Text.Encoding]::Unicode.GetString($bytes, 1, $bytes.Length - 1)
    foreach ($word in @($env:USERNAME, "C:\Users", "c:\users") + $guard) {
        foreach ($t in $texts) { if ($t.Contains($word)) { throw "not released: $f contains '$word'" } }
    }
}

if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $dst -DestinationPath $zip
$commit = (git -C $root rev-parse --short HEAD)
Write-Output "$name ($commit) -> $dst"
Write-Output "zip: $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
