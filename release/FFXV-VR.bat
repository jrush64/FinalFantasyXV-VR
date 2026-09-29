@echo off
setlocal
set "FFXVVR_BAT=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s=[IO.File]::ReadAllText($env:FFXVVR_BAT); $i=$s.IndexOf('#'+'POWERSHELL'+'#'); iex $s.Substring($i)"
exit /b %errorlevel%
#POWERSHELL#
# FFXV VR installer. Everything below runs in PowerShell (the lines above hand it over).
# Drop-in model: all files are extracted into the folder with ffxv_s.exe and this runs from there.

$bat = $env:FFXVVR_BAT
$dir = Split-Path -Parent $bat
$testMode = [bool]$env:FFXVVR_ANSWERS
$answers = New-Object System.Collections.Queue
if ($testMode) { foreach ($a in ($env:FFXVVR_ANSWERS -split ',')) { $answers.Enqueue($a) } }
$Host.UI.RawUI.WindowTitle = 'FFXV VR'

function Say([string]$t = '') { Write-Host "  $t" }
function Ask([string]$prompt, [string]$default) {
    if ($testMode) { $a = if ($answers.Count) { $answers.Dequeue() } else { '' } }
    else { $a = Read-Host "  $prompt [$default]" }
    if ([string]::IsNullOrWhiteSpace($a)) { return $default }
    return $a.Trim()
}
function Finish([int]$code) {
    Write-Host ''
    if (-not $testMode) {
        Write-Host '  Press any key to close.'
        try { [void]$Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown') } catch { Read-Host | Out-Null }
    }
    exit $code
}

# ---- ini files: set Key=Value inside [Section], keeping everything else and the file's encoding
function Get-TextEncoding([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return New-Object System.Text.ASCIIEncoding }
    $b = [IO.File]::ReadAllBytes($path)
    if ($b.Length -ge 2 -and $b[0] -eq 0xFF -and $b[1] -eq 0xFE) { return [Text.Encoding]::Unicode }
    if ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) { return New-Object System.Text.UTF8Encoding($true) }
    return New-Object System.Text.ASCIIEncoding
}
function Set-IniValues([string]$path, [string]$section, [hashtable]$values) {
    $enc = Get-TextEncoding $path
    $lines = New-Object System.Collections.Generic.List[string]
    if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllLines($path, $enc) | ForEach-Object { $lines.Add($_) } }
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim() -ieq "[$section]") { $start = $i; break } }
    if ($start -lt 0) {
        if ($lines.Count -and $lines[$lines.Count - 1].Trim()) { $lines.Add('') }
        $lines.Add("[$section]"); $start = $lines.Count - 1
    }
    $end = $lines.Count
    for ($i = $start + 1; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim().StartsWith('[')) { $end = $i; break } }
    foreach ($key in $values.Keys) {
        $found = $false
        for ($i = $start + 1; $i -lt $end; $i++) {
            if ($lines[$i] -match "^\s*$([regex]::Escape($key))\s*=") { $lines[$i] = "$key=$($values[$key])"; $found = $true; break }
        }
        if (-not $found) { $lines.Insert($end, "$key=$($values[$key])"); $end++ }
    }
    [IO.File]::WriteAllLines($path, $lines, $enc)
}
function Get-IniValue([string]$path, [string]$section, [string]$key) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $in = $false
    foreach ($l in [IO.File]::ReadAllLines($path, (Get-TextEncoding $path))) {
        $t = $l.Trim()
        if ($t.StartsWith('[')) { $in = ($t -ieq "[$section]"); continue }
        if ($in -and $t -match "^$([regex]::Escape($key))\s*=\s*(.*)$") { return $Matches[1].Trim() }
    }
    return $null
}
# The keys are rebindable in the Insert menu (saved in ffxv-vr.ini as key codes).
function Get-KeyName([int]$vk) {
    $named = @{ 0x2D = 'Insert'; 0x2E = 'Delete'; 0x24 = 'Home'; 0x23 = 'End'; 0x21 = 'Page Up'; 0x22 = 'Page Down'
                0x20 = 'Space'; 0x09 = 'Tab'; 0x0D = 'Enter'; 0x13 = 'Pause'; 0x08 = 'Backspace'
                0x25 = 'Left'; 0x26 = 'Up'; 0x27 = 'Right'; 0x28 = 'Down' }
    if ($named.ContainsKey($vk)) { return $named[$vk] }
    if (($vk -ge 0x30 -and $vk -le 0x39) -or ($vk -ge 0x41 -and $vk -le 0x5A)) { return [string][char]$vk }
    if ($vk -ge 0x70 -and $vk -le 0x87) { return 'F' + ($vk - 0x6F) }
    if ($vk -ge 0x60 -and $vk -le 0x69) { return 'Numpad ' + ($vk - 0x60) }
    return ('key 0x{0:X2}' -f $vk)
}
function Get-BoundKey([string]$ini, [string]$key, [int]$default) {
    $v = 0
    if ([int]::TryParse([string](Get-IniValue $ini 'Display' $key), [ref]$v) -and $v -gt 0) { return Get-KeyName $v }
    return Get-KeyName $default
}

function Get-DllVersion([string]$path) {
    try {
        $v = (Get-Item -LiteralPath $path).VersionInfo.FileVersion -replace ',', '.' -replace '\s', ''
        return [version]$v
    } catch { return [version]'0.0' }
}

Write-Host ''
Say '--------------------------------------------------'
Say '  FFXV VR - FINAL FANTASY XV WINDOWS EDITION'
Say '--------------------------------------------------'
Write-Host ''

# ---- must run from the game folder
if (-not (Test-Path -LiteralPath (Join-Path $dir 'ffxv_s.exe'))) {
    Say 'ffxv_s.exe is not next to this installer.'
    Write-Host ''
    Say 'Extract ALL the FFXV VR files into your game folder (the one with'
    Say 'ffxv_s.exe) and run FFXV-VR.bat from there.'
    Say 'Steam: right-click FINAL FANTASY XV > Manage > Browse local files.'
    Finish 1
}
if (Get-Process -Name 'ffxv_s' -ErrorAction SilentlyContinue) {
    Say 'Close FINAL FANTASY XV first, then run this again.'
    Finish 1
}

# ---- write access (Steam under Program Files needs Administrator)
try {
    $probe = Join-Path $dir '.ffxvvr_writetest'
    [IO.File]::WriteAllText($probe, 'x'); Remove-Item -LiteralPath $probe -Force
} catch {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin -and -not $testMode) {
        Say 'This folder needs Administrator rights. Click YES on the popup.'
        Start-Process -FilePath $bat -WorkingDirectory $dir -Verb RunAs
        exit 0
    }
    Say 'Cannot write to this folder. Check it is not read-only and that'
    Say 'antivirus is not blocking it.'
    Finish 1
}

$modIni = Join-Path $dir 'ffxv-vr.ini'
$oldIni = Join-Path $dir 'ffxv_aer.ini'   # the settings file's old name
if ((Test-Path -LiteralPath $oldIni) -and -not (Test-Path -LiteralPath $modIni)) { Move-Item -LiteralPath $oldIni -Destination $modIni }
$installed = [bool](Get-IniValue $modIni 'Display' 'BackbufferWH')
Say ($(if ($installed) { 'FFXV VR is installed here.' } else { 'FFXV VR is not installed here yet.' }))
Write-Host ''
Say '[1] Install / change quality'
Say '[2] Uninstall'
Say '[3] Cancel'
Write-Host ''
$action = Ask 'Enter 1-3' '1'
if ($action -eq '3') { Say 'Cancelled. Nothing was changed.'; Finish 0 }

$docs = if ($env:FFXVVR_DOCS) { $env:FFXVVR_DOCS } else { [Environment]::GetFolderPath('MyDocuments') }
$configs = @(Get-ChildItem -Path (Join-Path $docs 'My Games\FINAL FANTASY XV\Steam\*\savestorage\GraphicsConfig.ini') -ErrorAction SilentlyContinue)
$dlssGame = Join-Path $dir 'nvngx_dlss.dll'
$dlssOriginal = Join-Path $dir 'nvngx_dlss.dll.ffxvvr-original'

if ($action -eq '2') {
    Write-Host ''
    if ((Ask 'Uninstall FFXV VR? (Y/N)' 'N') -notmatch '^[Yy]') { Say 'Cancelled. Nothing was changed.'; Finish 0 }
    foreach ($f in 'dxgi.dll', 'openxr_loader.dll') { Remove-Item -LiteralPath (Join-Path $dir $f) -Force -ErrorAction SilentlyContinue }; Remove-Item -LiteralPath (Join-Path $dir 'ofxr') -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $dlssOriginal) { Move-Item -LiteralPath $dlssOriginal -Destination $dlssGame -Force }
    Say 'Removed the mod. The game runs normally again.'
    $backups = @($configs | Where-Object { Test-Path -LiteralPath ($_.FullName + '.ffxvvr-backup') })
    if ($backups.Count -and (Ask 'Put back your old game video settings? (Y/N)' 'Y') -match '^[Yy]') {
        foreach ($c in $backups) { Move-Item -LiteralPath ($c.FullName + '.ffxvvr-backup') -Destination $c.FullName -Force }
        Say 'Game video settings restored.'
    }
    if ((Ask 'Also delete the mod settings and logs? (Y/N)' 'N') -match '^[Yy]') {
        Remove-Item -LiteralPath $modIni, $oldIni -Force -ErrorAction SilentlyContinue
        Get-ChildItem -LiteralPath $dir -File | Where-Object { $_.Name -like 'ffxv-vr*.log' -or $_.Name -like 'retail_flat_sbs*.log' } |
            Remove-Item -Force -ErrorAction SilentlyContinue
        Say 'Deleted.'
    }
    Finish 0
}

# ---- INSTALL
$missing = @('dxgi.dll', 'openxr_loader.dll', 'ffxv_vr_dlss.dll' | Where-Object { -not (Test-Path -LiteralPath (Join-Path $dir $_)) })
if ($missing.Count) {
    Say ('Missing: ' + ($missing -join ', '))
    Say 'Extract ALL the FFXV VR files into this folder, then run this again.'
    Finish 1
}

$tiers = @(
    @{ Name = 'Low';        W = 1920; H = 1080 },
    @{ Name = 'Balanced';   W = 2560; H = 1440 },
    @{ Name = 'Sharp';      W = 3840; H = 2160 },
    @{ Name = 'Very sharp'; W = 5120; H = 2880 },
    @{ Name = 'Max';        W = 6144; H = 3456 })
$current = Get-IniValue $modIni 'Display' 'BackbufferWH'
$def = 2
for ($i = 0; $i -lt $tiers.Count; $i++) { if ($current -eq "$($tiers[$i].W)x$($tiers[$i].H)") { $def = $i + 1 } }
Write-Host ''
Say 'Image quality (the size each eye is rendered at):'
Write-Host ''
for ($i = 0; $i -lt $tiers.Count; $i++) {
    $t = $tiers[$i]
    $note = if ($i -eq 1) { '  (recommended)' } else { '' }
    Say ("[{0}] {1,-11} {2}x{3}{4}" -f ($i + 1), $t.Name, $t.W, $t.H, $note)
}
Write-Host ''
$pick = Ask 'Enter 1-5' "$def"
$n = 0
if (-not [int]::TryParse($pick, [ref]$n) -or $n -lt 1 -or $n -gt $tiers.Count) { $n = $def }
$tier = $tiers[$n - 1]
$wh = "$($tier.W)x$($tier.H)"
Write-Host ''
Say "Quality: $($tier.Name) ($wh)"

# ---- VR mode: what the head tracking key starts (can be switched any time in the menu)
$savedMode = Get-IniValue $modIni 'Display' 'VrMode'
$modeDef = if ($savedMode -eq '2' -or $savedMode -eq '3') { $savedMode } else { '1' }
Write-Host ''
Say 'VR mode:'
Write-Host ''
Say '[1] Stereo  both eyes every frame, smoothest  (recommended)'
Say '[2] AER     one eye per frame, lighter on the GPU'
Say '[3] Mono    one picture to both eyes, no 3D, lightest'
Write-Host ''
$mode = Ask 'Enter 1-3' $modeDef
if ($mode -ne '2' -and $mode -ne '3') { $mode = '1' }
Write-Host ''
Say "VR mode: $(@{ '1' = 'Stereo'; '2' = 'AER'; '3' = 'Mono' }[$mode])"

# ---- mod settings: only the render size; everything else uses the built-in defaults
Set-IniValues $modIni 'Display' @{ BackbufferWH = $wh; VrMode = $mode }
if ((Get-IniValue $modIni 'Display' 'BackbufferWH') -ne $wh) { Say 'WARNING: could not write ffxv-vr.ini.' }

# ---- DLSS 4: keep the game's own file to put back on uninstall
$ours = Join-Path $dir 'ffxv_vr_dlss.dll'
if ((Get-DllVersion $dlssGame) -lt (Get-DllVersion $ours)) {
    if ((Test-Path -LiteralPath $dlssGame) -and -not (Test-Path -LiteralPath $dlssOriginal)) {
        Copy-Item -LiteralPath $dlssGame -Destination $dlssOriginal -Force
    }
    Copy-Item -LiteralPath $ours -Destination $dlssGame -Force
}
Say "DLSS: $(Get-DllVersion $dlssGame)"

# ---- the game's own video settings (every Steam profile on this PC)
$game = [ordered]@{
    'BasicSettings'           = @{ DisplayResolutionWH = $wh; RenderingResolutionRatio = '100'; MaxFramerate = '120' }
    'DisplaySettings'         = @{ FullScreenModeOnStartup = '1'; HardwareFullScreenMode = '0'; VSync = '0' }
    'RenderingSettings'       = @{ Antialias = '2'; MotionBlur = '0'; ModelLODScaling = '100' }
    'NVIDIAGameWorksSettings' = @{ NvidiaHairWorks = '0'; NvidiaShadowLibs = '0'; NvidiaTurf = '0'; NvidiaVXAO = '0' }
}
if (-not $configs.Count) {
    Write-Host ''
    Say 'WARNING: the game has not made its settings file yet.'
    Say 'Start FINAL FANTASY XV once, reach the title screen, quit,'
    Say 'then run this installer again.'
} else {
    $bad = @()
    foreach ($c in $configs) {
        $backup = $c.FullName + '.ffxvvr-backup'
        if (-not (Test-Path -LiteralPath $backup)) { Copy-Item -LiteralPath $c.FullName -Destination $backup -Force }
        foreach ($section in $game.Keys) { Set-IniValues $c.FullName $section $game[$section] }
        foreach ($section in $game.Keys) {
            foreach ($key in $game[$section].Keys) {
                if ((Get-IniValue $c.FullName $section $key) -ne $game[$section][$key]) { $bad += "$key" }
            }
        }
    }
    if ($bad.Count) { Say ('WARNING: could not set ' + (($bad | Select-Object -Unique) -join ', ')) }
    else { Say "Game set to $wh, borderless, TAA, motion blur off." }
}

$kVr = Get-BoundKey $modIni 'VrEnableKey' 0x78
$kMono = Get-BoundKey $modIni 'MonoKey' 0x23
$kMenu = Get-BoundKey $modIni 'MenuKey' 0x2D
$kRecenter = Get-BoundKey $modIni 'RecenterKey' 0x52
Write-Host ''
Say '--------------------------------------------------'
Say '  DONE. FFXV VR is installed.'
Say '--------------------------------------------------'
Write-Host ''
Say '1. Start your VR runtime, then launch FINAL FANTASY XV.'
Say "2. Load your save. Once you are in the game, press $kVr."
Say '   Wait for the beep: head tracking takes a few seconds.'
Write-Host ''
Say "HEAD TRACKING KEY: $kVr"
Say "$kMenu = menu   $kRecenter = recenter   $kMono = Mono"
Write-Host ''
Say 'GOOD TO KNOW'
Say "- Press $kVr only once you are IN the game, not on the title screen,"
Say '  a menu or a loading screen. The mod finds the game camera then.'
Say '- No head tracking after the beep, or it stops working?'
Say "  Press $kVr again."
Say '- If the game freezes or hangs in Stereo (it can happen in'
Say "  conversations), press $kMono to switch to Mono."
Say "- Every key can be changed in the menu ($kMenu)."
Write-Host ''
Say 'Run this again any time to change quality or uninstall.'
Finish 0
