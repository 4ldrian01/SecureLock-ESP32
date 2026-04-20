# SecureLock Build & Deploy Helper
# Default behavior: full "latest" deployment (clean -> build -> uploadfs -> upload)
# Optional behavior: build-only mode for local compile verification.

[CmdletBinding()]
param(
    [switch]$BuildOnly,
    [switch]$NoMonitor,
    [switch]$WithMonitor,
    [switch]$SkipClean,
    [switch]$ForceUploadFS,
    [switch]$RefreshPlatform,
    [string]$UploadPort = "",
    [string]$ExpectedIp = "",
    [int]$UploadMaxAttempts = 3,
    [int]$RetryDelaySec = 2,
    [switch]$InteractiveRetry
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-PlatformIO {
    $pioCandidates = @(
        "C:\pio_core\penv\Scripts\platformio.exe",
        "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
    )

    $resolved = $pioCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $resolved) {
        throw "PlatformIO not found. Checked: $($pioCandidates -join ', ')"
    }

    return $resolved
}

function Invoke-PioStep {
    param(
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$PioPath,
        [Parameter(Mandatory = $true)][string[]]$Args
    )

    Write-Host "`n$Title" -ForegroundColor Yellow
    & $PioPath @Args
    if ($LASTEXITCODE -ne 0) {
        throw "Step failed: $Title (exit code $LASTEXITCODE)"
    }
}

Write-Host "=== SecureLock Build / Deploy Script ===" -ForegroundColor Cyan

if ($UploadMaxAttempts -lt 1) {
    throw "UploadMaxAttempts must be >= 1"
}

if ($RetryDelaySec -lt 0) {
    throw "RetryDelaySec must be >= 0"
}

if ($BuildOnly) {
    $pioPath = Resolve-PlatformIO
    Write-Host "Mode: BUILD ONLY" -ForegroundColor Cyan
    Write-Host "Using PlatformIO: $pioPath" -ForegroundColor DarkGray

    if (-not $SkipClean) {
        Write-Host "`n[clean] Removing .pio build cache" -ForegroundColor Yellow
        if (Test-Path ".pio") {
            Remove-Item ".pio" -Recurse -Force
            Write-Host "      Removed .pio folder" -ForegroundColor Green
        }
    }

    Invoke-PioStep -Title "[deps] Installing project packages" -PioPath $pioPath -Args @("pkg", "install")

    if ($RefreshPlatform) {
        Invoke-PioStep -Title "[platform] Updating espressif32 platform" -PioPath $pioPath -Args @("platform", "update", "espressif32")
    }

    Invoke-PioStep -Title "[build] Compiling firmware" -PioPath $pioPath -Args @("run")
    Write-Host "`n=== Build completed successfully ===" -ForegroundColor Green
    exit 0
}

Write-Host "Mode: FULL DEPLOY (latest dashboard + firmware)" -ForegroundColor Cyan

$deployScript = Join-Path $PSScriptRoot "scripts\deploy_all.ps1"
if (-not (Test-Path $deployScript)) {
    throw "Missing deployment script: $deployScript"
}

$deployArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $deployScript,
    "-UploadMaxAttempts", "$UploadMaxAttempts",
    "-RetryDelaySec", "$RetryDelaySec"
)

if ($UploadPort) {
    $deployArgs += @("-UploadPort", $UploadPort)
}

if ($ExpectedIp) {
    $deployArgs += @("-ExpectedIp", $ExpectedIp)
}

if ($NoMonitor) {
    $deployArgs += "-NoMonitor"
}

if ($WithMonitor) {
    $deployArgs += "-WithMonitor"
}

if ($SkipClean) {
    $deployArgs += "-SkipClean"
}

if ($InteractiveRetry) {
    $deployArgs += "-InteractiveRetry"
}

if ($ForceUploadFS) {
    $deployArgs += "-ForceUploadFS"
}

Write-Host "Delegating to: $deployScript" -ForegroundColor DarkGray
& powershell @deployArgs
if ($LASTEXITCODE -ne 0) {
    throw "Full deploy failed (exit code $LASTEXITCODE)"
}

Write-Host "`n=== Full deploy completed successfully ===" -ForegroundColor Green
