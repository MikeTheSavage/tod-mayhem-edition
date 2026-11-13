# install_copy.ps1 - install / uninstall TODGameplay into a COPY of the game under <repo>\work\ only.
# It refuses any other folder (the real game folders are installed by hand, see dll\README.md).
#   powershell -ExecutionPolicy Bypass -File dll\install_copy.ps1 -Target <repo>\work\testgame
#   powershell -ExecutionPolicy Bypass -File dll\install_copy.ps1 -Target <repo>\work\testgame -Uninstall
param(
    [Parameter(Mandatory = $true)][string]$Target,
    [switch]$Uninstall,
    [switch]$KeepIni
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$work = [IO.Path]::GetFullPath((Join-Path $root 'work')).TrimEnd('\') + '\'
$t = [IO.Path]::GetFullPath($Target).TrimEnd('\') + '\'
if (-not $t.StartsWith($work, [StringComparison]::OrdinalIgnoreCase) -or $t -eq $work) {
    throw "Target must be a folder inside $work (got $t). Nothing done."
}
if (-not (Test-Path (Join-Path $t 'TOD.exe'))) { throw "No TOD.exe in ${t}: not a game copy. Nothing done." }
$dist = Join-Path $root 'dist'
$files = 'TODGameplay.dll', 'dsound.dll', 'TODPhysics.dll'   # TODPhysics.dll: only loaded with [physics] enabled = 1
function Same($a, $b) { (Test-Path $a) -and (Test-Path $b) -and ((Get-FileHash $a).Hash -eq (Get-FileHash $b).Hash) }

if ($Uninstall) {
    foreach ($f in $files) {
        $p = Join-Path $t $f
        if (-not (Test-Path $p)) { continue }
        if ($f -eq 'dsound.dll' -and -not (Same $p (Join-Path $dist $f))) {
            # an older TODGameplay loader is fine to remove, a foreign dsound.dll is not
            $txt = [IO.File]::ReadAllBytes($p); $s = [Text.Encoding]::ASCII.GetString($txt)
            if ($s -notmatch 'TODGameplay\.dll') { Write-Host "keep $p (not the TODGameplay loader)"; continue }
        }
        Remove-Item $p; Write-Host "removed $p"
    }
    $ini = Join-Path $t 'TODGameplay.ini'
    if ((Test-Path $ini) -and -not $KeepIni) { Remove-Item $ini; Write-Host "removed $ini" }
    Write-Host 'uninstalled'
    exit 0
}

$ds = Join-Path $t 'dsound.dll'
if ((Test-Path $ds) -and -not (Same $ds (Join-Path $dist 'dsound.dll'))) {
    $s = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($ds))
    if ($s -notmatch 'TODGameplay\.dll') { throw "${ds} exists and is not the TODGameplay loader: rename it and set [loader] chain_dsound. Nothing done." }
}
foreach ($f in $files) { Copy-Item (Join-Path $dist $f) (Join-Path $t $f) -Force; Write-Host "copied $f" }
$ini = Join-Path $t 'TODGameplay.ini'
if (-not (Test-Path $ini)) { Copy-Item (Join-Path $dist 'TODGameplay.ini') $ini; Write-Host 'copied TODGameplay.ini' }
else { Write-Host 'kept the existing TODGameplay.ini' }
Write-Host "installed into $t"
