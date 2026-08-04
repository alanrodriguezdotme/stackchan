<#
.SYNOPSIS
    Packages a standalone flash bundle from a completed ESP-IDF build so the
    StackChan firmware can be flashed on a machine that has esptool but not the
    full ESP-IDF toolchain.

.DESCRIPTION
    Reads firmware/build/flasher_args.json, copies the referenced .bin files
    (flattening bootloader/ and partition_table/ subfolders), and emits a
    self-contained folder + zip containing:
      - the five flash artifacts
      - flasher_args.json (authoritative offsets)
      - flash.ps1  (wrapper: .\flash.ps1 -Port COM5)
      - README.txt (offsets + raw esptool command + download-mode steps)

.EXAMPLE
    .\scripts\make-flash-bundle.ps1
    .\scripts\make-flash-bundle.ps1 -BuildDir C:\path\to\firmware\build -OutDir C:\out
#>
[CmdletBinding()]
param(
    [string]$BuildDir,
    [string]$OutDir
)

$ErrorActionPreference = "Stop"

# $PSScriptRoot can be empty depending on how the script is invoked; fall back
# to the script's own path so the default locations resolve either way.
$scriptRoot = $PSScriptRoot
if (-not $scriptRoot) { $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $BuildDir) { $BuildDir = Join-Path $scriptRoot "..\firmware\build" }
if (-not $OutDir)   { $OutDir   = Join-Path $scriptRoot "..\dist" }

$BuildDir = (Resolve-Path $BuildDir).Path
$argsJsonPath = Join-Path $BuildDir "flasher_args.json"
if (-not (Test-Path $argsJsonPath)) {
    throw "flasher_args.json not found in $BuildDir. Run 'idf.py build' first."
}

$flasher = Get-Content $argsJsonPath -Raw | ConvertFrom-Json
$bundleDir = Join-Path $OutDir "stackchan-flash"
if (Test-Path $bundleDir) { Remove-Item -Recurse -Force $bundleDir }
New-Item -ItemType Directory -Force $bundleDir | Out-Null

$mode = $flasher.flash_settings.flash_mode
$size = $flasher.flash_settings.flash_size
$freq = $flasher.flash_settings.flash_freq
$chip = $flasher.extra_esptool_args.chip

$entries = @()
foreach ($prop in $flasher.flash_files.PSObject.Properties) {
    $offset = $prop.Name
    $relPath = $prop.Value
    $src = Join-Path $BuildDir $relPath
    if (-not (Test-Path $src)) { throw "Missing artifact: $src" }
    $bare = Split-Path $relPath -Leaf
    Copy-Item $src (Join-Path $bundleDir $bare) -Force
    $entries += [pscustomobject]@{ Offset = $offset; File = $bare }
}
$entries = $entries | Sort-Object { [Convert]::ToInt64($_.Offset, 16) }

Copy-Item $argsJsonPath (Join-Path $bundleDir "flasher_args.json") -Force

$pairs = ($entries | ForEach-Object { "{0,-9} {1}" -f $_.Offset, $_.File }) -join "`r`n  "

$flashLines = ($entries | ForEach-Object { "  {0} {1} ``" -f $_.Offset, $_.File }) -join "`r`n"
$flashPs1 = @"
<# Flash the StackChan firmware bundle. Usage: .\flash.ps1 -Port COM5 [-Baud 460800] #>
param([Parameter(Mandatory=`$true)][string]`$Port, [int]`$Baud = 460800)
`$ErrorActionPreference = "Stop"
Set-Location `$PSScriptRoot
python -m esptool --chip $chip -p `$Port -b `$Baud --before default_reset --after hard_reset ``
  write_flash --flash_mode $mode --flash_size $size --flash_freq $freq ``
$flashLines
"@
$flashPs1 = $flashPs1.TrimEnd("`r","`n"," ","``")
Set-Content (Join-Path $bundleDir "flash.ps1") $flashPs1 -Encoding UTF8

$rawCmd = "python -m esptool --chip $chip -p COM<N> -b 460800 --before default_reset --after hard_reset write_flash --flash_mode $mode --flash_size $size --flash_freq $freq " + (($entries | ForEach-Object { $_.Offset + ' ' + $_.File }) -join ' ')
$readme = @"
StackChan firmware flash bundle
===============================
chip=$chip  flash_mode=$mode  flash_size=$size  flash_freq=$freq

Artifacts and offsets (authoritative; see flasher_args.json):
  $pairs

Prereqs on the flashing machine:
  pip install esptool

Put the CoreS3 into download mode:
  1. Connect a USB-C DATA cable.
  2. Hold RESET ~2-3s until the internal green LED lights, then release.
  3. Screen stays black with backlight on = download mode.
  4. Note the new COM port.

Flash:
  .\flash.ps1 -Port COM<N>

or raw:
  $rawCmd

Recovery: M5Burner -> StackChan-UserDemo restores factory firmware.
"@
Set-Content (Join-Path $bundleDir "README.txt") $readme -Encoding UTF8

$zipPath = Join-Path $OutDir "stackchan-flash.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path (Join-Path $bundleDir "*") -DestinationPath $zipPath

Write-Host "Bundle folder: $bundleDir"
Write-Host "Bundle zip:    $zipPath"
Get-ChildItem $bundleDir | Select-Object Name, Length | Format-Table -AutoSize
