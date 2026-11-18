# Flashes the whole box in one go: bootloader + flash layout + FreeMesh + Meshtastic + MeshCore.
# The saved settings (nvs) and each mode's storage are not erased.
#
# The folder tools\bins\ is not part of this repository. Build Meshtastic and MeshCore with the patches,
# as described in tools\patches\README.md, and put the results in tools\bins\ under the names
# meshtastic-freemesh.bin and meshcore-freemesh.bin.
#
# Usage (in the ESP-IDF PowerShell, from the firmware folder):
#   idf.py build
#   .\tools\flash-all.ps1 -Port COMx           (the box's port: "python -m serial.tools.list_ports -v" lists them)
#   .\tools\flash-all.ps1 -Port COMx -Merge    (also makes one file, freemesh-box-full.bin, for new boxes)

param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$Meshtastic = "tools\bins\meshtastic-freemesh.bin",
    [string]$MeshCore = "tools\bins\meshcore-freemesh.bin",
    [switch]$Merge
)

# Off = flash offset of each file. Must match partitions.csv.
$parts = @(
    @{ Off = "0x0";      File = "build\bootloader\bootloader.bin" },
    @{ Off = "0x8000";   File = "build\partition_table\partition-table.bin" },
    @{ Off = "0xf000";   File = "build\ota_data_initial.bin" },
    @{ Off = "0x20000";  File = "build\firmware.bin";  Max = 0x200000 },
    @{ Off = "0x220000"; File = $Meshtastic;           Max = 0x280000 },
    @{ Off = "0x4A0000"; File = $MeshCore;             Max = 0x200000 }
)

$args_ = @()
foreach ($p in $parts) {
    if (-not (Test-Path $p.File)) {
        Write-Host "Missing: $($p.File)" -ForegroundColor Red
        if ($p.File -like "build*") { Write-Host "Run 'idf.py build' first." }
        exit 1
    }
    $size = (Get-Item $p.File).Length
    if ($p.Max -and $size -gt $p.Max) {
        Write-Host ("Too big: {0} is {1:N0} bytes, its slot holds {2:N0}." -f $p.File, $size, $p.Max) -ForegroundColor Red
        exit 1
    }
    if ($p.Max) { Write-Host ("{0,-40} {1,10:N0} bytes  ({2}% of slot)" -f $p.File, $size, [int](100 * $size / $p.Max)) }
    $args_ += $p.Off, $p.File
}

if ($Merge) {
    python -m esptool --chip esp32s3 merge_bin -o freemesh-box-full.bin --flash_mode dio --flash_size 8MB @args_
    Write-Host "Made freemesh-box-full.bin: write it at 0x0 on a NEW box (erase-flash first)." -ForegroundColor Green
}

python -m esptool --chip esp32s3 -p $Port -b 921600 write_flash --flash_size 8MB @args_
if ($LASTEXITCODE -eq 0) {
    Write-Host "Done. The box starts in FreeMesh. Menu > Box mode shows all 3 as Installed." -ForegroundColor Green
}
