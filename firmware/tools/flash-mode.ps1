# Writes a Meshtastic or MeshCore program into its own slot on the box.
# FreeMesh, the saved settings and the bootloader are not touched.
#
# Usage (in the ESP-IDF PowerShell, from the firmware folder):
#   .\tools\flash-mode.ps1 meshtastic .\downloads\firmware-seeed-xiao-s3-2.x.x.bin -Port COMx
#   .\tools\flash-mode.ps1 meshcore   .\downloads\Xiao_S3_WIO_companion_radio_ble-vX.bin -Port COMx
#
# Use the app-only .bin, not the "factory" or "merged" one, which would overwrite the rest of the box.

param(
    [Parameter(Mandatory = $true)][ValidateSet("meshtastic", "meshcore")][string]$Mode,
    [Parameter(Mandatory = $true)][string]$Bin,
    [Parameter(Mandatory = $true)][string]$Port
)

# Must match partitions.csv.
$slots = @{
    "meshtastic" = @{ Offset = "0x220000"; Size = 0x280000 }
    "meshcore"   = @{ Offset = "0x4A0000"; Size = 0x200000 }
}
$slot = $slots[$Mode]

if (-not (Test-Path $Bin)) { Write-Host "File not found: $Bin" -ForegroundColor Red; exit 1 }
$size = (Get-Item $Bin).Length

# An app-only file starts with byte 0xE9, the mark of an ESP32 program.
# A merged or factory file also starts with 0xE9 because it begins with the bootloader,
# so the file name and the size are checked too.
$first = [System.IO.File]::ReadAllBytes($Bin)[0]
if ($first -ne 0xE9) { Write-Host "This doesn't look like an ESP32 program file." -ForegroundColor Red; exit 1 }
if ($Bin -match "factory|merged") {
    Write-Host "This is a full 'factory' file. Use the app-only .bin instead." -ForegroundColor Red; exit 1
}
if ($size -gt $slot.Size) {
    Write-Host ("Too big: {0:N0} bytes, the {1} slot holds {2:N0}. partitions.csv must change." -f $size, $Mode, $slot.Size) -ForegroundColor Red
    exit 1
}

Write-Host ("{0}: {1:N0} of {2:N0} bytes used ({3}%). Writing to {4}..." -f $Mode, $size, $slot.Size, [int](100 * $size / $slot.Size), $slot.Offset)
python -m esptool --chip esp32s3 -p $Port write_flash $slot.Offset $Bin
if ($LASTEXITCODE -eq 0) {
    Write-Host "Done. On the page: Menu > Box mode now shows $Mode as Installed." -ForegroundColor Green
}
