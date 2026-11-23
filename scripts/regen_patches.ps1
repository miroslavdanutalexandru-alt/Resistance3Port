<#
.SYNOPSIS
  Regenerates this port's RPCS3 patch (ps3recomp\patches\r2-rpcs3.patch or r3-rpcs3.patch) from the
  modified RPCS3 tree in ps3recomp\third_party\rpcs3. The patch is verified before it replaces the old one.

.DESCRIPTION
  This uses the same method the committed patches were made with:
    1. The RPCS3 tree must be at the commit pinned in tools\fetch_rpcs3.ps1.
    2. Every untracked file outside 3rdparty\ (new VKR2*.cpp/.h and the like) is marked intent-to-add
       (git add -N) in a temporary copy of the tree's index, so that `git diff` includes it as a new file
       and the RPCS3 tree's own index is not changed.
    3. git -c core.autocrlf=false diff --binary -- . ':(exclude)3rdparty'
       3rdparty is excluded: fetch_rpcs3.ps1 brings pugixml / Vulkan-Headers / submodules itself,
       and a "Subproject commit" hunk for 3rdparty/pugixml must not be in the patch.
       The output is written byte-exact (LF; .gitattributes has *.patch -text).
  The new patch is then checked, and nothing is replaced if any check fails:
    a. It contains no 3rdparty paths and every untracked/intent-to-add file in the tree.
    b. Every VKR2*.cpp named in the tree's CMakeLists.txt is in the patch or in the pinned commit.
    c. Reverse check: `git apply -R --check` against the current tree (the patch is exactly the tree's changes).
    d. Forward check: `git apply --check` on a clean detached worktree of the pinned commit in
       scratch\rpcs3_patch_test, the way fetch_rpcs3.ps1 applies it. The worktree is removed afterwards.

  -Check only builds the new patch in scratch\ and says whether it differs from the committed one.
  It writes nothing to ps3recomp\patches. Neither mode changes the RPCS3 tree's files or index
  (the forward check adds and removes a temporary git worktree).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\regen_patches.ps1 -Check
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\regen_patches.ps1
#>
[CmdletBinding()]
param(
    [switch]$Check,            # build + verify into scratch\ only; don't replace the patch
    [switch]$NoForwardTest,    # skip the clean-worktree forward apply test
    [switch]$AllowJunk         # don't stop on *.orig / *.rej / *.bak / *.tmp files in the tree
)

$ErrorActionPreference = 'Stop'
# Windows PowerShell started from a PowerShell 7 window (directly or through cmd / a .bat) inherits pwsh's module
# paths and then cannot load its own modules: Get-FileHash and Get-AuthenticodeSignature are 'not recognized', and
# Expand-Archive loads pwsh's copy and fails. Keep only Windows PowerShell's own paths. Get-PortFileHash and
# Expand-PortZip fall back to .NET if the cmdlets still cannot run (same output: upper-case hex / the same files).
# (Same block in setup.ps1, tools\update.ps1, tools\get_shaderscan.ps1, tools\installer\prereqs.ps1, scripts\*.ps1.)
if ($PSVersionTable.PSVersion.Major -lt 6) {
    $pmp = @(($env:PSModulePath -split ';') | Where-Object { $_ -and $_ -notmatch '\\PowerShell\\(7[^\\]*\\)?Modules\\?$' })
    $pmpSys = Join-Path $PSHOME 'Modules'
    if (-not @($pmp | Where-Object { $_.TrimEnd('\') -eq $pmpSys }).Count) { $pmp += $pmpSys }
    $env:PSModulePath = $pmp -join ';'
}
$script:PortHashCmd = $null
function Get-PortFileHash([string]$LiteralPath, [string]$Algorithm = 'SHA256') {
    if ($script:PortHashCmd -ne $false) {
        try {
            $r = Get-FileHash -LiteralPath $LiteralPath -Algorithm $Algorithm
            $script:PortHashCmd = $true
            if ($null -eq $r) { return $null }
            return "$($r.Hash)".ToUpperInvariant()
        } catch [System.Management.Automation.CommandNotFoundException] { $script:PortHashCmd = $false }
    }
    $a = switch ($Algorithm.ToUpperInvariant()) {
        'SHA1' { [Security.Cryptography.SHA1]::Create() }
        'SHA384' { [Security.Cryptography.SHA384]::Create() }
        'SHA512' { [Security.Cryptography.SHA512]::Create() }
        'MD5' { [Security.Cryptography.MD5]::Create() }
        default { [Security.Cryptography.SHA256]::Create() }
    }
    $s = [IO.File]::OpenRead($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LiteralPath))
    try { return ([BitConverter]::ToString($a.ComputeHash($s)) -replace '-', '') } finally { $s.Dispose(); $a.Dispose() }
}
function Expand-PortZip([string]$LiteralPath, [string]$DestinationPath, [switch]$Force) {
    try { Expand-Archive -LiteralPath $LiteralPath -DestinationPath $DestinationPath -Force:$Force -ErrorAction Stop; return }
    catch {
        $e = $_.Exception
        while ($e -is [System.Management.Automation.ActionPreferenceStopException] -and $e.InnerException) { $e = $e.InnerException }
        if (-not ($e -is [System.Management.Automation.CommandNotFoundException] -or $e -is [System.Management.Automation.MethodException])) { Write-Error -ErrorRecord $_; return }
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $src = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LiteralPath)
    $dst = [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($DestinationPath))
    [void][IO.Directory]::CreateDirectory($dst)
    $root = $dst.TrimEnd('\') + '\'
    $z = [IO.Compression.ZipFile]::OpenRead($src)
    try {
        foreach ($en in $z.Entries) {
            $t = [IO.Path]::GetFullPath([IO.Path]::Combine($dst, $en.FullName))
            if (-not $t.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe path in $src`: $($en.FullName)" }
            if ($en.FullName.EndsWith('/') -or $en.FullName.EndsWith('\')) { [void][IO.Directory]::CreateDirectory($t); continue }
            if (-not $Force -and [IO.File]::Exists($t)) { throw "$t already exists (Expand-Archive without -Force)" }
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($t))
            [IO.Compression.ZipFileExtensions]::ExtractToFile($en, $t, $true)
        }
    } finally { $z.Dispose() }
}
Set-StrictMode -Version 2.0
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Tree = Join-Path $Repo 'ps3recomp\third_party\rpcs3'
$Scratch = Join-Path $Repo 'scratch'

function Git-In([string]$dir) {
    # Runs git in $dir and returns its stdout lines; throws on a non-zero exit code.
    $a = @('-C', $dir, '-c', 'core.autocrlf=false', '-c', 'core.quotepath=false') + $args
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $out = & git @a 2>&1 } finally { $ErrorActionPreference = $old }
    if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed in ${dir}:`n$($out -join "`n")" }
    return @($out | ForEach-Object { "$_" })
}
function Git-Ok([string]$dir) {
    # Same, but returns $true / $false instead of throwing.
    $a = @('-C', $dir, '-c', 'core.autocrlf=false') + $args
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $out = & git @a 2>&1 } finally { $ErrorActionPreference = $old }
    if ($LASTEXITCODE -ne 0) { Write-Host ($out -join "`n") -ForegroundColor DarkYellow; return $false }
    return $true
}

# ---------------------------------------------------------------- inputs
$fetch = Get-Content -Raw -LiteralPath (Join-Path $Repo 'tools\fetch_rpcs3.ps1')
if ($fetch -notmatch "\`$(?:script:)?Commit\s*=\s*'([0-9a-f]{40})'") { throw "pinned commit not found in tools\fetch_rpcs3.ps1" }
$Commit = $Matches[1]
# fetch_rpcs3.ps1 applies the single ps3recomp\patches\*-rpcs3.patch (older versions named it explicitly).
if ($fetch -match "patches\\([\w\-]+\.patch)") { $PatchName = $Matches[1] }
else {
    $found = @(Get-ChildItem -Path (Join-Path $Repo 'ps3recomp\patches') -Filter '*-rpcs3.patch' -File)
    if ($found.Count -ne 1) { throw "expected exactly one ps3recomp\patches\*-rpcs3.patch, found $($found.Count)" }
    $PatchName = $found[0].Name
}
$Patch = Join-Path $Repo "ps3recomp\patches\$PatchName"

Write-Host "== $PatchName  (RPCS3 tree: $Tree)"
if (-not (Test-Path -LiteralPath (Join-Path $Tree '.git'))) { throw "$Tree is not a git checkout (run tools\fetch_rpcs3.ps1 first)" }
$head = @(Git-In $Tree rev-parse HEAD)[0].Trim()
if ($head -ne $Commit) { throw "RPCS3 tree is at $head, but fetch_rpcs3.ps1 pins $Commit. The patch must be made against the pinned commit." }
Write-Host "   tree HEAD = pinned commit $($Commit.Substring(0,10))"

# ---------------------------------------------------------------- new files
$untracked = @(Git-In $Tree ls-files --others --exclude-standard -- . ':(exclude)3rdparty' ':(exclude).port_fetch') | Where-Object { $_ }
$junk = @($untracked | Where-Object { $_ -match '\.(orig|rej|bak\w*|tmp|swp)$|~$|(^|/)(build|out|\.vs)/' })
if ($junk.Count -and -not $AllowJunk) {
    $junk | ForEach-Object { Write-Host "   junk: $_" -ForegroundColor Red }
    throw "untracked junk files in the RPCS3 tree would end up in the patch; delete them or use -AllowJunk"
}
$newFiles = @($untracked | Where-Object { $junk -notcontains $_ })

# The intent-to-add marks go into a throwaway copy of the tree's index, so the RPCS3 tree is left untouched.
New-Item -ItemType Directory -Force -Path $Scratch | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$NewPatch = Join-Path $Scratch "$PatchName.new"
$errFile = Join-Path $Scratch "$PatchName.err"
$TmpIndex = Join-Path $Scratch "$PatchName.index"
Copy-Item -LiteralPath (Join-Path @(Git-In $Tree rev-parse --absolute-git-dir)[0].Trim() 'index') -Destination $TmpIndex -Force
$env:GIT_INDEX_FILE = $TmpIndex
try {
    if ($newFiles.Count) {
        Write-Host "   marking $($newFiles.Count) new file(s) intent-to-add (in a temporary index):"
        $newFiles | ForEach-Object { Write-Host "     + $_" }
        Git-In $Tree add -N -- @newFiles | Out-Null
    }
    # Every file the patch must add: intent-to-add entries (git diff --name-only --diff-filter=A).
    $added = @(Git-In $Tree diff --name-only --diff-filter=A -- . ':(exclude)3rdparty' ':(exclude).port_fetch') | Where-Object { $_ }

    # ------------------------------------------------------------ make the patch (byte-exact)
    # cmd's redirection keeps git's bytes untouched (PowerShell's pipeline would re-encode them).
    $cmdLine = "git -C `"$Tree`" -c core.autocrlf=false -c core.quotepath=false diff --no-color --no-ext-diff --binary -- . `":(exclude)3rdparty`" `":(exclude).port_fetch`" > `"$NewPatch`" 2> `"$errFile`""
    & cmd.exe /d /c $cmdLine
    if ($LASTEXITCODE -ne 0) { throw "git diff failed: $(Get-Content -Raw -LiteralPath $errFile)" }
} finally {
    Remove-Item Env:\GIT_INDEX_FILE -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $TmpIndex -ErrorAction SilentlyContinue
}
Remove-Item -LiteralPath $errFile -ErrorAction SilentlyContinue
$bytes = [System.IO.File]::ReadAllBytes($NewPatch)
if ($bytes.Length -lt 1000) { throw "the generated patch is suspiciously small ($($bytes.Length) bytes)" }

# ---------------------------------------------------------------- verify
$problems = New-Object System.Collections.Generic.List[string]
$text = [System.Text.Encoding]::UTF8.GetString($bytes)
$diffFiles = @([regex]::Matches($text, '(?m)^diff --git a/(\S+) b/') | ForEach-Object { $_.Groups[1].Value })
$newInPatch = @([regex]::Matches($text, '(?m)^diff --git a/(\S+) b/\S+\r?\nnew file mode') | ForEach-Object { $_.Groups[1].Value })
Write-Host "   patch: $($bytes.Length) bytes, $($diffFiles.Count) files ($($newInPatch.Count) new)"

# a. no 3rdparty, no CRLF conversion surprises, every new file present
$third = @($diffFiles | Where-Object { $_ -like '3rdparty/*' })
if ($third.Count) { $problems.Add("patch touches 3rdparty: $($third -join ', ')") }
if ($text -match '(?m)^\+?Subproject commit ') { $problems.Add("patch contains a submodule (Subproject commit) hunk") }
foreach ($f in $added) { if ($newInPatch -notcontains $f) { $problems.Add("new file missing from the patch: $f") } }

# b. every VKR2*.cpp the patched CMakeLists.txt compiles exists in the patch or the pinned commit
$cmake = Join-Path $Tree 'CMakeLists.txt'
if (Test-Path -LiteralPath $cmake) {
    $srcs = @([regex]::Matches((Get-Content -Raw -LiteralPath $cmake), '[\w/\$\{\}]*?(VKR2\w+\.cpp|port_\w+\.(?:cpp|h))') |
              ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
    foreach ($s in $srcs) {
        $inPatch = @($diffFiles | Where-Object { $_ -like "*/$s" -or $_ -eq $s }).Count -gt 0
        $inCommit = @(Git-In $Tree ls-tree -r --name-only $Commit -- 'rpcs3/Emu/RSX' | Where-Object { $_ -like "*/$s" }).Count -gt 0
        if (-not ($inPatch -or $inCommit)) { $problems.Add("CMakeLists.txt builds $s, but it is neither in the patch nor in RPCS3 $($Commit.Substring(0,7))") }
    }
    Write-Host "   CMakeLists.txt sources checked: $($srcs -join ' ')"
}

# c. reverse apply on the current tree
if (Git-Ok $Tree apply -R --check --whitespace=nowarn $NewPatch) { Write-Host "   reverse check on the tree: OK" }
else { $problems.Add("git apply -R --check failed on the current tree") }

# d. forward apply on a clean worktree of the pinned commit (the way fetch_rpcs3.ps1 applies it)
if (-not $NoForwardTest) {
    $wt = Join-Path $Scratch 'rpcs3_patch_test'
    if (Test-Path -LiteralPath $wt) { $null = Git-Ok $Tree worktree remove --force $wt; Remove-Item -LiteralPath $wt -Recurse -Force -ErrorAction SilentlyContinue }
    $null = Git-Ok $Tree worktree prune
    Git-In $Tree worktree add --detach $wt $Commit | Out-Null
    try {
        if (Git-Ok $wt apply --check --whitespace=nowarn $NewPatch) { Write-Host "   forward check on a clean $($Commit.Substring(0,7)) worktree: OK" }
        else { $problems.Add("git apply --check failed on a clean worktree of $Commit") }
    } finally {
        $null = Git-Ok $Tree worktree remove --force $wt
        $null = Git-Ok $Tree worktree prune
    }
}

if ($problems.Count) {
    $problems | ForEach-Object { Write-Host "   FAIL: $_" -ForegroundColor Red }
    throw "The new patch did not pass its checks; $Patch was NOT changed. Candidate: $NewPatch"
}

# ---------------------------------------------------------------- compare / replace
$oldBytes = if (Test-Path -LiteralPath $Patch) { [System.IO.File]::ReadAllBytes($Patch) } else { [byte[]]@() }
$same = ($oldBytes.Length -eq $bytes.Length) -and ([Convert]::ToBase64String($oldBytes) -eq [Convert]::ToBase64String($bytes))
$hash = (Get-PortFileHash $NewPatch SHA256)
if ($same) {
    Write-Host "   unchanged: $PatchName already matches the tree ($hash)" -ForegroundColor Green
    Remove-Item -LiteralPath $NewPatch
    exit 0
}
if ($Check) {
    Write-Host "   $PatchName is OUT OF DATE ($($oldBytes.Length) -> $($bytes.Length) bytes). Candidate: $NewPatch" -ForegroundColor Yellow
    exit 1
}
if ($oldBytes.Length) { Copy-Item -LiteralPath $Patch -Destination (Join-Path $Scratch "$PatchName.prev_$stamp") }
Move-Item -LiteralPath $NewPatch -Destination $Patch -Force
Write-Host "   wrote $Patch ($($bytes.Length) bytes, SHA-256 $hash); previous copy in scratch\$PatchName.prev_$stamp" -ForegroundColor Green
Git-In $Repo diff --stat -- "ps3recomp/patches/$PatchName" | ForEach-Object { Write-Host "   $_" }
Write-Host "   Next: commit the patch; then test a fresh clone (tools\fetch_rpcs3.ps1 + build.bat)."
