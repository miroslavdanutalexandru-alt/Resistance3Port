<#
Writes tools\pipeline_seed.bin for a release: the shader-cache pipelines the game linked in play (cache misses on
a first visit) that shaderscan does not create from the game files. The game copies them into
shadercache\rpcs3\shaders_cache on start-up (ps3recomp\libs\video\rpcs3_bridge\r2rsx_pipeline_seed.h), so the
start-up preload builds them before play instead of the game skipping those draws the first time.

No game data: only pipelines\vulkan\<ver>\*.bin records go in (shader hashes and render state, a fixed-size struct
without microcode). raw\*.vp / *.fp (the RSX shader microcode) are never read or written; on the player's PC they
come from shaderscan, which extracts them from the player's own game files. A record is only taken when both of its
shaders are in shaderscan_manifest.txt (so shaderscan recreates them there).

Usage (after a playthrough with shaderscan installed):
  powershell -NoProfile -File scripts\export_pipeline_seed.ps1 [-Cache <shaders_cache>] [-Out <file>] [-Merge]
-Merge keeps the records already in -Out (adds the new ones), so several playthroughs / PCs add up.
#>
param(
    [string]$Cache = '',
    [string]$Out = '',
    [switch]$Merge
)
$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
if (-not $Cache) { $Cache = Join-Path $Repo 'shadercache\rpcs3\shaders_cache' }
if (-not $Out) { $Out = Join-Path $Repo 'tools\pipeline_seed.bin' }

$manifest = Join-Path $Cache 'shaderscan_manifest.txt'
if (-not (Test-Path -LiteralPath $manifest)) { throw "$manifest not found: run the launcher with shaderscan first (it tells which shaders come from the game files)" }
$scanRaw = @{}; $scanPipe = @{}
foreach ($l in [IO.File]::ReadAllLines($manifest)) {
    if ($l -match '^R raw/([0-9A-Fa-f]+\.(vp|fp))\s*$') { $scanRaw[$matches[1].ToUpperInvariant()] = 1 }
    elseif ($l -match '^P pipelines/.*/([^/]+\.bin)\s*$') { $scanPipe[$matches[1].ToUpperInvariant()] = 1 }
}
if (-not $scanRaw.Count) { throw "$manifest lists no shaders" }

$verDirs = @(Get-ChildItem -LiteralPath (Join-Path $Cache 'pipelines\vulkan') -Directory | Where-Object { $_.Name -match '^v[0-9.]+$' })
if ($verDirs.Count -ne 1) { throw "expected one pipelines\vulkan\v<version> folder in $Cache, found $($verDirs.Count)" }
$subdir = "pipelines/vulkan/$($verDirs[0].Name)"

$records = [ordered]@{}   # name -> byte[]
$recSize = 0
if ($Merge -and (Test-Path -LiteralPath $Out)) {
    $br = New-Object IO.BinaryReader([IO.File]::OpenRead($Out))
    try {
        if ([Text.Encoding]::ASCII.GetString($br.ReadBytes(8)) -ne 'PORTPSD1') { throw "$Out is not a pipeline seed" }
        $recSize = $br.ReadUInt32(); $n = $br.ReadUInt32()
        $oldSub = [Text.Encoding]::ASCII.GetString($br.ReadBytes(32)).TrimEnd([char]0)
        if ($oldSub -ne $subdir) { Write-Host "Old seed is for $oldSub, the cache is $subdir`: not merged (start a new seed)" -ForegroundColor Yellow; $recSize = 0 }
        else { for ($i = 0; $i -lt $n; $i++) { $nm = [Text.Encoding]::ASCII.GetString($br.ReadBytes(80)).TrimEnd([char]0); $records[$nm] = $br.ReadBytes($recSize) } }
    } finally { $br.Close() }
    if (-not $recSize) { $records.Clear() }
}
$kept = $records.Count

$taken = 0; $byScan = 0; $noRaw = 0; $badSize = 0; $badName = 0
foreach ($f in Get-ChildItem -LiteralPath $verDirs[0].FullName -File -Filter '*.bin' | Sort-Object Name) {
    $name = $f.Name.ToUpperInvariant() -replace '\.BIN$', '.bin'
    if ($name -notmatch '^([0-9A-F]{1,16})\+([0-9A-F]{1,16})\+[0-9A-F]{1,16}\+[0-9A-F]{1,16}\.bin$') { $badName++; continue }
    if ($scanPipe.ContainsKey($f.Name.ToUpperInvariant())) { $byScan++; continue }
    if (-not ($scanRaw.ContainsKey("$($matches[1]).VP") -and $scanRaw.ContainsKey("$($matches[2]).FP"))) { $noRaw++; continue }
    if (-not $recSize) { $recSize = [int]$f.Length }
    if ($f.Length -ne $recSize -or $recSize -gt 4096) { $badSize++; continue }
    if (-not $records.Contains($name)) { $records[$name] = [IO.File]::ReadAllBytes($f.FullName); $taken++ }
}
if (-not $records.Count) { throw "no pipelines to export (all $byScan came from shaderscan, $noRaw had shaders it does not find)" }

$tmp = "$Out.tmp"
$bw = New-Object IO.BinaryWriter([IO.File]::Create($tmp))
try {
    $pad = { param([string]$s, [int]$len) $b = New-Object byte[] $len; $a = [Text.Encoding]::ASCII.GetBytes($s); [Array]::Copy($a, $b, $a.Length); , $b }
    $bw.Write([Text.Encoding]::ASCII.GetBytes('PORTPSD1'))
    $bw.Write([uint32]$recSize)
    $bw.Write([uint32]$records.Count)
    $bw.Write((& $pad $subdir 32))
    foreach ($k in $records.Keys) { $bw.Write((& $pad $k 80)); $bw.Write([byte[]]$records[$k]) }
} finally { $bw.Close() }
Move-Item -LiteralPath $tmp -Destination $Out -Force
Write-Host ("{0}: {1} pipelines ({2} new, {3} kept), {4}, {5} bytes each, {6:N0} KB" -f $Out, $records.Count, $taken, $kept, $subdir, $recSize, ((Get-Item -LiteralPath $Out).Length / 1KB))
Write-Host ("Skipped: {0} made by shaderscan, {1} using shaders shaderscan does not find, {2} wrong size, {3} bad name" -f $byScan, $noRaw, $badSize, $badName)
