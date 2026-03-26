# SecureLock one-command deployment script (Windows / PowerShell)
# Runs clean -> build -> uploadfs -> upload -> monitor in sequence.

[CmdletBinding()]
param(
    [string]$UploadPort = "",
    [int]$MonitorBaud = 115200,
    [switch]$NoMonitor,
    [switch]$SkipClean,
    [string]$ExpectedIp = "",
    [int]$EndpointTimeoutSec = 4
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
        $stepStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
        & $Action
        $stepStopwatch.Stop()

        if ($LASTEXITCODE -eq 0) {
            Write-Host "[DEPLOY] Step duration: $($stepStopwatch.Elapsed.TotalSeconds.ToString('0.00'))s" -ForegroundColor DarkGray
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

function Resolve-UploadPort {
    param(
        [string]$RequestedPort,
        [string]$PioExecutable
    )

    if ($RequestedPort) {
        return $RequestedPort
    }

    try {
        $serialPorts = Get-CimInstance Win32_SerialPort -ErrorAction Stop
        $usbCandidates = @($serialPorts | Where-Object {
            $_.DeviceID -match '^COM\d+$' -and (
                $_.Description -match 'CP210|CH340|CH910|FTDI|USB|UART Bridge|Silicon Labs|ESP32'
            )
        } | Sort-Object DeviceID)

        if ($usbCandidates.Count -gt 0) {
            return $usbCandidates[0].DeviceID
        }

        $fallbackCandidates = @($serialPorts | Where-Object {
            $_.DeviceID -match '^COM\d+$'
        } | Sort-Object DeviceID)

        if ($fallbackCandidates.Count -gt 0) {
            return $fallbackCandidates[0].DeviceID
        }
    }
    catch {
        Write-Host "[DEPLOY][WARN] Serial port discovery via CIM failed: $($_.Exception.Message)" -ForegroundColor DarkYellow
    }

    if ($PioExecutable -and (Test-Path $PioExecutable)) {
        try {
            $deviceListRaw = (& $PioExecutable device list | Out-String)
            $portMatches = [regex]::Matches($deviceListRaw, 'COM\d+')
            if ($portMatches.Count -gt 0) {
                return $portMatches[0].Value
            }
        }
        catch {
            Write-Host "[DEPLOY][WARN] PlatformIO device list failed: $($_.Exception.Message)" -ForegroundColor DarkYellow
        }
    }

    return ""
}

function Test-SecretsReadiness {
    param(
        [Parameter(Mandatory = $true)][string]$SecretsPath
    )

    if (-not (Test-Path $SecretsPath)) {
        Write-Host "[DEPLOY][WARN] include/secrets.h not found. Build may fail or run with defaults." -ForegroundColor DarkYellow
        return
    }

    $content = Get-Content -Raw -Path $SecretsPath
    $warnings = @()

    if ($content -match 'YOUR_WIFI_SSID|YOUR_WIFI_PASSWORD') {
        $warnings += "WiFi credentials still look like placeholders"
    }

    if ($content -match 'CHANGE_ME_NOW') {
        $warnings += "Dashboard password still contains CHANGE_ME_NOW placeholder"
    }

    if ($content -match '1234567890:ABCdefGHIjklMNOpqrSTUvwxYZ') {
        $warnings += "Telegram bot token still appears to be template value"
    }

    if ($warnings.Count -gt 0) {
        Write-Host "[DEPLOY][WARN] Secrets preflight found potential issues:" -ForegroundColor DarkYellow
        foreach ($warning in $warnings) {
            Write-Host "  - $warning" -ForegroundColor Yellow
        }
    }
    else {
        Write-Host "[DEPLOY] Secrets preflight: looks ready" -ForegroundColor Green
    }
}

function Test-Endpoint {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [int]$TimeoutSec = 4
    )

    $result = [ordered]@{
        Url = $Url
        Reachable = $false
        StatusCode = $null
        DurationMs = $null
        Error = ""
    }

    try {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $response = Invoke-WebRequest -Uri $Url -Method GET -TimeoutSec $TimeoutSec -UseBasicParsing
        $sw.Stop()

        $result.Reachable = $true
        $result.StatusCode = [int]$response.StatusCode
        $result.DurationMs = [int][Math]::Round($sw.Elapsed.TotalMilliseconds)
    }
    catch {
        $result.Error = $_.Exception.Message
    }

    [pscustomobject]$result
}

function Test-PostDeployEndpoints {
    param(
        [string]$ExpectedIp,
        [int]$TimeoutSec = 4
    )

    $urls = @('http://securelock.local/')
    if ($ExpectedIp) {
        $urls += "http://$ExpectedIp/"
    }

    $urls = $urls | Where-Object { $_ -and $_.Trim().Length -gt 0 } | Select-Object -Unique

    Write-Host "`n[DEPLOY] Post-deploy endpoint checks" -ForegroundColor Cyan
    foreach ($url in $urls) {
        $probe = Test-Endpoint -Url $url -TimeoutSec $TimeoutSec
        if ($probe.Reachable) {
            Write-Host "[DEPLOY] OK   $($probe.Url) -> HTTP $($probe.StatusCode) in $($probe.DurationMs) ms" -ForegroundColor Green
        }
        else {
            Write-Host "[DEPLOY][WARN] FAIL $($probe.Url) -> $($probe.Error)" -ForegroundColor DarkYellow
        }
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

$resolvedUploadPort = Resolve-UploadPort -RequestedPort $UploadPort -PioExecutable $pioExe

Write-Host "[DEPLOY] Project: $projectRoot" -ForegroundColor Yellow
Write-Host "[DEPLOY] PlatformIO: $pioExe" -ForegroundColor Yellow
if ($UploadPort) {
    Write-Host "[DEPLOY] Upload port override: $UploadPort" -ForegroundColor Yellow
}
elseif ($resolvedUploadPort) {
    Write-Host "[DEPLOY] Upload port auto-selected: $resolvedUploadPort" -ForegroundColor Yellow
}
else {
    Write-Host "[DEPLOY] Upload port: auto-detect" -ForegroundColor Yellow
}

if ($SkipClean) {
    Write-Host "[DEPLOY] Clean step: skipped by -SkipClean" -ForegroundColor Yellow
}
else {
    Write-Host "[DEPLOY] Clean step: enabled" -ForegroundColor Yellow
}

if ($ExpectedIp) {
    Write-Host "[DEPLOY] Expected dashboard IP: http://$ExpectedIp/" -ForegroundColor Yellow
}

Push-Location $projectRoot
try {
    $deployStopwatch = [System.Diagnostics.Stopwatch]::StartNew()

    Test-SecretsReadiness -SecretsPath (Join-Path $projectRoot "include\secrets.h")

    Stop-StalePlatformIOMonitors

    if (-not $SkipClean) {
        Invoke-Step -Title "Clean" -Action {
            & $pioExe run --target clean
        }
    }

    Invoke-Step -Title "Build" -Action {
        & $pioExe run
    }

    Invoke-Step -Title "Upload filesystem (LittleFS)" -MaxAttempts 3 -RetryHint "If flashing fails or says wrong boot mode, hold the BOOT button on ESP32 while retrying." -Action {
        if ($resolvedUploadPort) {
            & $pioExe run --target uploadfs --upload-port $resolvedUploadPort
        }
        else {
            & $pioExe run --target uploadfs
        }
    }

    Invoke-Step -Title "Upload firmware" -MaxAttempts 3 -RetryHint "If flashing fails or says wrong boot mode, hold the BOOT button on ESP32 while retrying." -Action {
        if ($resolvedUploadPort) {
            & $pioExe run --target upload --upload-port $resolvedUploadPort
        }
        else {
            & $pioExe run --target upload
        }
    }

    Test-PostDeployEndpoints -ExpectedIp $ExpectedIp -TimeoutSec $EndpointTimeoutSec

    $deployStopwatch.Stop()
    Write-Host "`n[DEPLOY] Total pipeline time: $($deployStopwatch.Elapsed.ToString())" -ForegroundColor Green

    if (-not $NoMonitor) {
        Write-Host "`n[DEPLOY] Starting serial monitor (Ctrl+C to stop)..." -ForegroundColor Magenta
        if ($resolvedUploadPort) {
            & $pioExe device monitor -p $resolvedUploadPort -b $MonitorBaud -f direct
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
