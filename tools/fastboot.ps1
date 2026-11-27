# Developer fast boot (automated tests): starts the game straight into a level, intro movies skipped.
#   tools\fastboot.ps1 -Level <name|location[:checkpoint]> [-Exe <path>] [-Seconds N] [-GameDir <dir>] [-Log <file>]
#                      [-Cutscenes] [-KeepSaves] [-List]
# -Level    a level name from the list printed below (haven_town_ridge), or <location>[:<checkpoint>] as in the
#           game's Campaign > Load Level: location 1-5 or haven / stlouis / mtpleasant / graterford / newyork,
#           checkpoint = row in that location's list (1 = first). The port presses through the menus for you.
# -Exe      the Resistance3.exe to run (default: build\Resistance3.exe next to this tools folder).
# -GameDir  the installed game folder: game\EBOOT.elf, gamefiles, gamedata (saves), settings.bat (default: this
#           repo if it has game\EBOOT.elf, else the main git worktree). The game runs in this folder.
# -Seconds  wait up to N s for gameplay (the "[fastboot] gameplay" log line) and print the boot time; 0 = don't wait.
# -Cutscenes  also skip story movies (PORT_FAST_BOOT_CUTSCENES=1).
# Saves: the savedata folder is copied first and put back by a watcher when the game exits (-KeepSaves: not).
# Prints the game's PID. The game keeps running; close only that PID.
# Env: PORT_FAST_BOOT=1, PORT_BOOT_LEVEL=<level>, R3_UNLOCK_ALL=1 (every location selectable, nothing saved).
param([string]$Level = '', [string]$Exe = '', [int]$Seconds = 0, [string]$GameDir = '', [string]$Log = '',
      [switch]$Cutscenes, [switch]$KeepSaves, [switch]$List)

$ErrorActionPreference = 'Stop'
$ExeName = 'Resistance3.exe'
# run.bat's fixed settings (the user's settings.bat comes after them)
$RunEnv = [ordered]@{ SPURS_PM_YIELD = '0'; SPURS_IDLE_POLL = '0'; FREEZE_RELAUNCH = '900'; PS3_MAPPER_BASE = '30000000'; SKIP_INTRO = '1' }
$CfgUser = Join-Path $env:USERPROFILE 'Saved Games\Resistance 3\config'   # run.bat's CFG_USER
# same tables as project\port_fastboot.h
$Cities = @('haven', 'stlouis', 'mtpleasant', 'graterford', 'newyork')
$CityTitles = @('Haven, OK', 'St. Louis, MO', 'Mt. Pleasant, PA', 'Graterford, PA', 'New York, NY')
$Named = [ordered]@{ 'haven_town_ridge' = 'haven:4  (Haven, OK - Run!)' }
$HavenRows = 'Haven Underground, Death Squad, Haven Outskirts, Run!, Farmhouse, Gulch, Creekbed, The Granary, Covered Bridge, Junkyard Defense, Main Street Defense'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $GameDir) {
    $GameDir = $repo
    if (-not (Test-Path (Join-Path $repo 'game\EBOOT.elf'))) {
        $main = (& git -C $repo worktree list --porcelain 2>$null | Select-Object -First 1) -replace '^worktree ', ''
        if ($main) { $GameDir = $main -replace '/', '\' }
    }
}
if (-not $Exe) { $Exe = Join-Path $repo "build\$ExeName" }
$GameDir = (Resolve-Path $GameDir).Path

# the user's config: the launcher's settings.bat (set "NAME=value" lines)
$cfg = @{}
$sb = Join-Path $GameDir 'settings.bat'
if (-not (Test-Path $sb)) { $sb = Join-Path $CfgUser 'settings.bat' }   # as run.bat: the per-user copy
if (Test-Path $sb) {
    foreach ($l in Get-Content $sb) { if ($l -match '^\s*set\s+"([^=]+)=(.*)"\s*$') { $cfg[$Matches[1]] = $Matches[2] } }
}
$vfs = if ($env:PS3_VFS_ROOT) { $env:PS3_VFS_ROOT } elseif ($cfg['PORT_GAMEFILES']) { $cfg['PORT_GAMEFILES'] } else { Join-Path $GameDir 'gamefiles' }

$levels = @()
$ld = Join-Path $vfs 'PS3_GAME\USRDIR\packed\levels'
if (Test-Path $ld) { $levels = @(Get-ChildItem $ld -Directory | ForEach-Object Name) }
Write-Host 'Locations (-Level <location>[:<checkpoint>], Campaign > Load Level):'
for ($i = 0; $i -lt $Cities.Count; $i++) { Write-Host ('  {0}  {1,-11} {2}' -f ($i + 1), $Cities[$i], $CityTitles[$i]) }
Write-Host "  haven checkpoints: $HavenRows"
Write-Host 'Level names:'
foreach ($k in $Named.Keys) { Write-Host "  $k = $($Named[$k])" }
if ($levels) { Write-Host ('Level folders (the log shows them as "[r3x] level: <name>"): ' + ($levels -join ', ')) }
if ($List -or -not $Level) { if (-not $Level -and -not $List) { Write-Host 'Give -Level <name|location[:checkpoint]>.' }; exit 0 }

$name = ($Level -split '[/\\]')[-1].ToLower()
if (-not $Named.Contains($name)) {
    $a, $b = $name -split ':', 2
    $ok = ($a -match '^[1-5]$' -or $Cities -contains $a) -and (-not $b -or $b -match '^[1-9]\d*$')
    if (-not $ok) { throw "unknown level '$Level': a level name above, or <location 1-5|name>[:<checkpoint>]" }
}
if (-not (Test-Path $Exe)) { throw "no game executable at $Exe (build first, or give -Exe)" }
$Exe = (Resolve-Path $Exe).Path

foreach ($p in Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(Resistance|ps3recomp)' }) {
    Write-Warning "already running: $($p.Name) PID $($p.Id) (two instances deadlock on the cache files)"
}

# run.bat's fixed settings, then the user's settings.bat, then the fast boot switches
$envs = [ordered]@{}
foreach ($k in $RunEnv.Keys) { $envs[$k] = $RunEnv[$k] }
foreach ($k in $cfg.Keys) { $envs[$k] = $cfg[$k] }
$envs['PS3_VFS_ROOT'] = $vfs
$envs['PORT_FAST_BOOT'] = '1'
$envs['PORT_BOOT_LEVEL'] = $name
$envs['R3_UNLOCK_ALL'] = '1'
$envs['PS3_ENTER_BUTTON'] = ''   # the menu driver presses Cross to accept
$envs['PORT_FAST_BOOT_CUTSCENES'] = if ($Cutscenes) { '1' } else { '' }
foreach ($k in $envs.Keys) { [Environment]::SetEnvironmentVariable($k, $(if ($envs[$k] -eq '') { $null } else { $envs[$k] }), 'Process') }

New-Item -ItemType Directory -Force (Join-Path $GameDir 'logs') | Out-Null
if (-not $Log) { $Log = Join-Path $GameDir 'logs\fastboot.log' }
$elf = Join-Path $GameDir 'game\EBOOT.elf'
# the menu driver picks Load Level, which writes the save: keep a copy and put it back when the game exits
$save = Join-Path $GameDir 'gamedata\dev_hdd0\home\00000001\savedata'
$bk = Join-Path $GameDir 'logs\fastboot_savedata'
if (-not $KeepSaves -and (Test-Path $save)) {
    if (Test-Path $bk) { Write-Host "an earlier saves copy was not put back yet: keeping it ($bk)" }
    else { & robocopy $save $bk /MIR /NJH /NJS /NFL /NDL /NP | Out-Null }
}
$t0 = Get-Date
$cmd = Start-Process -FilePath $env:ComSpec -ArgumentList "/c `"`"$Exe`" `"$elf`" > `"$Log`" 2>&1`"" `
    -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru
$game = $null
for ($i = 0; $i -lt 100 -and -not $game; $i++) {
    Start-Sleep -Milliseconds 100
    $game = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($cmd.Id) AND Name='$ExeName'" | Select-Object -First 1
}
if (-not $game) { throw "the game did not start (see $Log)" }
Write-Host "level $name  exe $Exe  log $Log"
Write-Host "PID $($game.ProcessId)"
if (-not $KeepSaves -and (Test-Path $bk)) {
    $w = "Wait-Process -Id $($game.ProcessId) -ErrorAction SilentlyContinue; Start-Sleep 2; " +
         "robocopy '$bk' '$save' /MIR /NJH /NJS /NFL /NDL /NP | Out-Null; if (`$LASTEXITCODE -lt 8) { Remove-Item -Recurse -Force '$bk' }"
    Start-Process powershell -ArgumentList '-NoProfile', '-WindowStyle', 'Hidden', '-Command', $w -WindowStyle Hidden | Out-Null
    Write-Host "saves: copied to $bk, put back when PID $($game.ProcessId) exits"
}
if ($Seconds -le 0) { exit 0 }
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep -Milliseconds 500
    $hit = Select-String -Path $Log -Pattern '^\[fastboot\] gameplay' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hit) { Write-Host ('{0}  (helper: {1:N1} s after launch)' -f $hit.Line, ((Get-Date) - $t0).TotalSeconds); exit 0 }
    if (-not (Get-Process -Id $game.ProcessId -ErrorAction SilentlyContinue)) { Write-Warning "the game exited (see $Log)"; exit 2 }
}
Write-Warning "no gameplay after $Seconds s (see $Log)"
exit 1
