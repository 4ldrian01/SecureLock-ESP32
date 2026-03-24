# SecureLock one-command deployment script (Windows / PowerShell)
# Runs clean -> build -> uploadfs -> upload -> monitor in sequence.

[CmdletBinding()]
param(
    [string]$UploadPort = "",
    [int]$MonitorBaud = 115200,
    [switch]$NoMonitor
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [int]$MaxAttempts = 1,
        [string]$RetryHint = ""
    )

    Write-Host "`n============================================================" -ForegroundColor DarkGray
    Write-Host "[DEPLOY] $Title" -ForegroundColor Cyan
    Write-Host "============================================================" -ForegroundColor DarkGray

    $attempt = 1
    while ($attempt -le $MaxAttempts) {
        & $Action

        if ($LASTEXITCODE -eq 0) {
            break
        }

        if ($attempt -lt $MaxAttempts) {
            Write-Host "[DEPLOY][WARN] Attempt $attempt/$MaxAttempts failed for '$Title' (exit code $LASTEXITCODE)." -ForegroundColor DarkYellow
            if ($RetryHint) {
                Write-Host "[DEPLOY][HINT] $RetryHint" -ForegroundColor Yellow
                try {
                    $null = Read-Host "Press Enter to retry"
                }
                catch {
                    Start-Sleep -Seconds 2
                }
            }
            else {
                Start-Sleep -Seconds 2
            }
        }

        $attempt++
    }

    if ($LASTEXITCODE -ne 0) {
        throw "Step failed: $Title (exit code $LASTEXITCODE)"
    }

    Write-Host "[DEPLOY] OK: $Title" -ForegroundColor Green
}

function Stop-StalePlatformIOMonitors {
    Write-Host "[DEPLOY] Releasing serial port locks (best effort)..." -ForegroundColor Yellow

    $killed = 0
    $candidates = Get-CimInstance Win32_Process | Where-Object {
        $_.CommandLine -and
        $_.CommandLine -match 'device\s+monitor' -and
        ($_.Name -match 'pio|python' -or $_.CommandLine -match 'platformio')
    }

    foreach ($proc in $candidates) {
        try {
            Stop-Process -Id $proc.ProcessId -Force -Confirm:$false -ErrorAction Stop
            $killed++
        }
        catch {
            Write-Host "[DEPLOY][WARN] Could not stop PID $($proc.ProcessId): $($_.Exception.Message)" -ForegroundColor DarkYellow
        }
    }

    if ($killed -gt 0) {
        Write-Host "[DEPLOY] Stopped $killed monitor process(es)." -ForegroundColor Green
    }
    else {
        Write-Host "[DEPLOY] No stale monitor processes found." -ForegroundColor DarkGray
    }
}

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

$pioCandidates = @(
    "C:\pio_core\penv\Scripts\platformio.exe",
    (Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe")
)

$pioExe = $pioCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $pioExe) {
    throw "PlatformIO executable not found. Checked: $($pioCandidates -join ', ')"
}

Write-Host "[DEPLOY] Project: $projectRoot" -ForegroundColor Yellow
Write-Host "[DEPLOY] PlatformIO: $pioExe" -ForegroundColor Yellow
if ($UploadPort) {
    Write-Host "[DEPLOY] Upload port override: $UploadPort" -ForegroundColor Yellow
}
else {
    Write-Host "[DEPLOY] Upload port: auto-detect" -ForegroundColor Yellow
}

Push-Location $projectRoot
try {
    Stop-StalePlatformIOMonitors

    Invoke-Step -Title "Clean" -Action {
        & $pioExe run --target clean
    }

    Invoke-Step -Title "Build" -Action {
        & $pioExe run
    }

    Invoke-Step -Title "Upload filesystem (LittleFS)" -MaxAttempts 3 -RetryHint "If flashing fails or says wrong boot mode, hold the BOOT button on ESP32 while retrying." -Action {
        if ($UploadPort) {
            & $pioExe run --target uploadfs --upload-port $UploadPort
        }
        else {
            & $pioExe run --target uploadfs
        }
    }

    Invoke-Step -Title "Upload firmware" -MaxAttempts 3 -RetryHint "If flashing fails or says wrong boot mode, hold the BOOT button on ESP32 while retrying." -Action {
        if ($UploadPort) {
            & $pioExe run --target upload --upload-port $UploadPort
        }
        else {
            & $pioExe run --target upload
        }
    }

    if (-not $NoMonitor) {
        Write-Host "`n[DEPLOY] Starting serial monitor (Ctrl+C to stop)..." -ForegroundColor Magenta
        if ($UploadPort) {
            & $pioExe device monitor -p $UploadPort -b $MonitorBaud -f direct
        }
        else {
            & $pioExe device monitor -b $MonitorBaud -f direct
        }
    }
    else {
        Write-Host "`n[DEPLOY] Done. Monitor skipped by -NoMonitor." -ForegroundColor Green
    }
}
finally {
    Pop-Location
}
