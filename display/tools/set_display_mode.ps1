# set_display_mode.ps1 - switch the GOG Total Overdose between windowed and borderless fullscreen.
#   -Mode borderless   configpc fullscreen = true + screensize = the monitor's native size (default: the primary
#                      monitor's current mode, e.g. 3024,1964), TODGameplay.ini [display] borderless = 1, registry
#                      XRes / YRes the same size
#   -Mode windowed     configpc fullscreen = false + screensize = -Width,-Height (default 1920,1080),
#                      [display] borderless = 0, registry the same size
# Backs up configpc.txt, TODGameplay.ini and the registry key first (work\display_install\mode_<ts>). Every other line
# of both files stays as it is. Refuses while TOD.exe runs. Needs TODGameplay.dll from dist (Install Display Fix.cmd).
param([Parameter(Mandatory = $true)][ValidateSet('borderless', 'windowed')][string]$Mode, [int]$Width = 0, [int]$Height = 0)
$ErrorActionPreference = 'Stop'
$game = 'C:\GOG Games\Total Overdose'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$py = if ($env:PY) { $env:PY } else { 'python' }
if (Get-Process TOD -ErrorAction SilentlyContinue) { throw 'Quit Total Overdose first, then run this again.' }
if ($Width -le 0 -or $Height -le 0) {
    if ($Mode -eq 'windowed') { $Width = 1920; $Height = 1080 }
    else {
        Add-Type @'
using System; using System.Runtime.InteropServices;
public static class DsMode {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)] public struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName; public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; public int dmFields;
    public int dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput; public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName; public short dmLogPixels; public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency; }
  [DllImport("user32.dll", CharSet = CharSet.Ansi)] public static extern bool EnumDisplaySettingsA(string dev, int mode, ref DEVMODE dm);
  public static int[] Current() { DEVMODE d = new DEVMODE(); d.dmSize = (short)Marshal.SizeOf(d); EnumDisplaySettingsA(null, -1, ref d); return new int[] { d.dmPelsWidth, d.dmPelsHeight }; }
}
'@
        $c = [DsMode]::Current(); $Width = $c[0]; $Height = $c[1]
    }
}
$ts = Get-Date -Format yyyyMMdd_HHmmss
$bk = Join-Path $root "work\display_install\mode_$ts"
New-Item -ItemType Directory -Force $bk | Out-Null
Copy-Item "$game\configpc.txt" $bk
Copy-Item "$game\TODGameplay.ini" $bk
reg export 'HKCU\Software\Eidos\Total Overdose' (Join-Path $bk 'eidos.reg') /y | Out-Null
Write-Host "Backup: $bk"
# configpc.txt: replace (or add) the fullscreen and screensize lines, keep everything else
$fs = if ($Mode -eq 'borderless') { 'true' } else { 'false' }
$lines = [IO.File]::ReadAllLines("$game\configpc.txt")
$out = New-Object System.Collections.Generic.List[string]; $hasF = $false; $hasS = $false
foreach ($l in $lines) {
    if ($l -match '^\s*fullscreen\s*=') { $out.Add("fullscreen = $fs"); $hasF = $true }
    elseif ($l -match '^\s*screensize\s*=') { $out.Add("screensize = $Width,$Height"); $hasS = $true }
    else { $out.Add($l) }
}
if (-not $hasS) { $out.Insert(0, "screensize = $Width,$Height") }
if (-not $hasF) { $out.Insert(0, "fullscreen = $fs") }
[IO.File]::WriteAllLines("$game\configpc.txt", $out)
# TODGameplay.ini: [display] enabled = 1, borderless = 0 / 1 (merge_ini keeps every other line)
$bl = if ($Mode -eq 'borderless') { 1 } else { 0 }
& $py -I (Join-Path $root 'seamless\tools\merge_ini.py') "$game\TODGameplay.ini" (Join-Path $root 'dist\TODGameplay.ini') display.enabled=1 "display.borderless=$bl"
if ($LASTEXITCODE -ne 0) { throw 'ini merge failed (the backup is in place)' }
reg add 'HKCU\Software\Eidos\Total Overdose' /v XRes /t REG_DWORD /d $Width /f | Out-Null
reg add 'HKCU\Software\Eidos\Total Overdose' /v YRes /t REG_DWORD /d $Height /f | Out-Null
Write-Host "Set: $Mode $Width x $Height (configpc fullscreen = $fs, [display] borderless = $bl, registry XRes/YRes)."
Write-Host "Undo: copy configpc.txt / TODGameplay.ini back from $bk and import eidos.reg."
