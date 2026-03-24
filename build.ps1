# SecureLock Build Script
# Cleans and builds the project

Write-Host "=== SecureLock Build Script ===" -ForegroundColor Cyan

# Set PlatformIO path (prefer stable local core install on this workstation)
$pioCandidates = @(
    "C:\pio_core\penv\Scripts\platformio.exe",
    "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
)

$pioPath = $pioCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

# Check if PlatformIO exists
if (-not $pioPath) {
    Write-Host "ERROR: PlatformIO not found. Checked: $($pioCandidates -join ', ')" -ForegroundColor Red
    exit 1
}

Write-Host "Using PlatformIO: $pioPath" -ForegroundColor DarkGray

# Clean .pio folder
Write-Host "`n[1/4] Cleaning build folder..." -ForegroundColor Yellow
if (Test-Path ".pio") {
    Remove-Item ".pio" -Recurse -Force
    Write-Host "      Removed .pio folder" -ForegroundColor Green
}

# Install libraries
Write-Host "`n[2/4] Installing libraries..." -ForegroundColor Yellow
& $pioPath pkg install

# Update platform
Write-Host "`n[3/4] Updating ESP32 platform..." -ForegroundColor Yellow
& $pioPath platform update espressif32

# Build
Write-Host "`n[4/4] Building project..." -ForegroundColor Yellow
& $pioPath run

Write-Host "`n=== Build Complete ===" -ForegroundColor Cyan
