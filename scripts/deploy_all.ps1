# SecureLock one-command deployment script (Windows / PowerShell)
# Runs clean -> build -> uploadfs -> upload with endpoint validation.
# Serial monitor is optional and disabled by default.

[CmdletBinding()]
param(
    [string]$UploadPort = "",
    [int]$MonitorBaud = 115200,
    [switch]$NoMonitor,
    [switch]$WithMonitor,
    [switch]$SkipClean,
    [switch]$ForceUploadFS,
    [string]$ExpectedIp = "",
    [int]$EndpointTimeoutSec = 4,
    [int]$UploadMaxAttempts = 3,
    [int]$RetryDelaySec = 2,
    [switch]$InteractiveRetry,
    [switch]$SkipEndpointChecks
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [int]$MaxAttempts = 1,
        [string]$RetryHint = "",
        [int]$RetryDelaySec = 2,
        [switch]$InteractiveRetry,
        [scriptblock]$BeforeAttempt = $null
    )

    Write-Host "`n============================================================" -ForegroundColor DarkGray
    Write-Host "[DEPLOY] $Title" -ForegroundColor Cyan
    Write-Host "============================================================" -ForegroundColor DarkGray

    $attempt = 1
    while ($attempt -le $MaxAttempts) {
        if ($BeforeAttempt) {
            & $BeforeAttempt
        }

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
            }

            $safeDelaySec = [Math]::Max(1, $RetryDelaySec)
            if ($InteractiveRetry) {
                try {
                    $null = Read-Host "Press Enter to retry attempt $($attempt + 1)/$MaxAttempts"
                }
                catch {
                    Start-Sleep -Seconds $safeDelaySec
                }
            }
            else {
                Write-Host "[DEPLOY] Auto-retrying in ${safeDelaySec}s..." -ForegroundColor DarkGray
                Start-Sleep -Seconds $safeDelaySec
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
    param(
        [string]$Port = ""
    )

    Write-Host "[DEPLOY] Releasing serial port locks (best effort)..." -ForegroundColor Yellow

    $killed = 0
    $candidates = Get-CimInstance Win32_Process | Where-Object {
        $_.CommandLine -and
        (
            $_.CommandLine -match 'device\s+monitor' -or
            $_.CommandLine -match 'esptool\.py' -or
            $_.CommandLine -match '--target\s+upload' -or
            $_.CommandLine -match '--target\s+uploadfs'
        ) -and
        ($_.Name -match 'pio|python|powershell' -or $_.CommandLine -match 'platformio') -and
        (
            -not $Port -or
            $_.CommandLine -match [Regex]::Escape($Port)
        )
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
        $sortByComNumber = @{ Expression = {
            $raw = $_.DeviceID -replace '[^0-9]', ''
            if ([string]::IsNullOrWhiteSpace($raw)) { return 9999 }
            return [int]$raw
        } }

        $usbCandidates = @($serialPorts | Where-Object {
            $_.DeviceID -match '^COM\d+$' -and (
                $_.Description -match 'CP210|CH340|CH910|FTDI|USB|UART Bridge|Silicon Labs|ESP32'
            )
        } | Sort-Object $sortByComNumber)

        if ($usbCandidates.Count -gt 0) {
            return $usbCandidates[0].DeviceID
        }

        $fallbackCandidates = @($serialPorts | Where-Object {
            $_.DeviceID -match '^COM\d+$' -and $_.DeviceID -notmatch '^COM(1|2)$'
        } | Sort-Object $sortByComNumber)

        if ($fallbackCandidates.Count -eq 0) {
            $fallbackCandidates = @($serialPorts | Where-Object {
                $_.DeviceID -match '^COM\d+$'
            } | Sort-Object $sortByComNumber)
        }

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
            $ports = [regex]::Matches($deviceListRaw, 'COM\d+') | ForEach-Object { $_.Value } | Select-Object -Unique
            if ($ports.Count -gt 0) {
                $preferred = @($ports | Where-Object { $_ -notmatch '^COM(1|2)$' })
                if ($preferred.Count -gt 0) {
                    return $preferred[0]
                }

                return $ports[0]
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

    $baseUrls = @('http://securelock.local')
    if ($ExpectedIp) {
        $baseUrls += "http://$ExpectedIp"
    }

    $baseUrls = $baseUrls | Where-Object { $_ -and $_.Trim().Length -gt 0 } | Select-Object -Unique

    $urls = @()
    foreach ($baseUrl in $baseUrls) {
        $normalizedBase = $baseUrl.TrimEnd('/')
        $urls += "$normalizedBase/"
        $urls += "$normalizedBase/api/auth/status"
    }

    $urls = $urls | Select-Object -Unique

    Write-Host "`n[DEPLOY] Post-deploy endpoint checks" -ForegroundColor Cyan
    foreach ($url in $urls) {
        $probe = Test-Endpoint -Url $url -TimeoutSec $TimeoutSec
        if ($probe.Reachable) {
            Write-Host "[DEPLOY] OK   $($probe.Url) -> HTTP $($probe.StatusCode) in $($probe.DurationMs) ms" -ForegroundColor Green
        }
        elseif (
            $probe.Url -match 'securelock\.local' -and
            $probe.Error -match 'could not be resolved|No such host is known|remote name could not be resolved'
        ) {
            Write-Host "[DEPLOY][INFO] mDNS name is not resolvable from this host right now: $($probe.Url)" -ForegroundColor DarkYellow
            Write-Host "[DEPLOY][INFO] This does not always mean firmware failure. If available, verify using ExpectedIp." -ForegroundColor DarkGray
        }
        else {
            Write-Host "[DEPLOY][WARN] FAIL $($probe.Url) -> $($probe.Error)" -ForegroundColor DarkYellow
        }
    }
}

function Get-SourceDataFingerprint {
    param(
        [Parameter(Mandatory = $true)][string]$DataPath
    )

    if (-not (Test-Path $DataPath)) {
        return ""
    }

    $resolvedDataPath = (Resolve-Path $DataPath).Path
    if (-not $resolvedDataPath.EndsWith("\")) {
        $resolvedDataPath += "\"
    }

    $files = Get-ChildItem -Path $DataPath -File -Recurse |
        Where-Object {
            $name = $_.Name.ToLowerInvariant()
            -not $name.EndsWith('.gz') -and $name -ne 'logs.json' -and $name -ne 'users.json'
        } |
        Sort-Object FullName

    $builder = New-Object System.Text.StringBuilder
    foreach ($file in $files) {
        $fullName = $file.FullName
        $relative = $fullName

        if ($fullName.StartsWith($resolvedDataPath, [System.StringComparison]::OrdinalIgnoreCase)) {
            $relative = $fullName.Substring($resolvedDataPath.Length)
        }

        $relative = $relative.Replace('\\', '/')
        [void]$builder.Append($relative)
        [void]$builder.Append('|')
        [void]$builder.Append($file.Length)
        [void]$builder.Append('|')
        [void]$builder.Append($file.LastWriteTimeUtc.Ticks)
        [void]$builder.Append("`n")
    }

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($builder.ToString())
        $hashBytes = $sha.ComputeHash($bytes)
        return -join ($hashBytes | ForEach-Object { $_.ToString("x2") })
    }
    finally {
        $sha.Dispose()
    }
}

function Read-TextFileSafely {
    param(
        [Parameter(Mandatory = $true)][string]$Path
    )

    if (-not (Test-Path $Path)) {
        return ""
    }

    try {
        return (Get-Content -Path $Path -Raw -ErrorAction Stop).Trim()
    }
    catch {
        return ""
    }
}

function Write-TextFileSafely {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Value
    )

    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path $directory)) {
        New-Item -Path $directory -ItemType Directory -Force | Out-Null
    }

    Set-Content -Path $Path -Value $Value -Encoding UTF8
}

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$dataRoot = Join-Path $projectRoot "data"
$uploadFsFingerprintCachePath = Join-Path $projectRoot ".cache\uploadfs_source_fingerprint.txt"

$pioCandidates = @(
    "C:\pio_core\penv\Scripts\platformio.exe",
    (Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe")
)

$pioExe = $pioCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $pioExe) {
    throw "PlatformIO executable not found. Checked: $($pioCandidates -join ', ')"
}

$resolvedUploadPort = Resolve-UploadPort -RequestedPort $UploadPort -PioExecutable $pioExe
$UploadMaxAttempts = [Math]::Max(1, $UploadMaxAttempts)
$RetryDelaySec = [Math]::Max(1, $RetryDelaySec)

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

Write-Host "[DEPLOY] Upload max attempts: $UploadMaxAttempts" -ForegroundColor Yellow
Write-Host "[DEPLOY] Retry mode: $(if ($InteractiveRetry) { 'interactive' } else { 'automatic' })" -ForegroundColor Yellow
if (-not $InteractiveRetry) {
    Write-Host "[DEPLOY] Auto-retry delay: ${RetryDelaySec}s" -ForegroundColor Yellow
}
if ($SkipEndpointChecks) {
    Write-Host "[DEPLOY] Endpoint checks: skipped by -SkipEndpointChecks" -ForegroundColor Yellow
}
if ($WithMonitor -and $NoMonitor) {
    Write-Host "[DEPLOY][WARN] Both -WithMonitor and -NoMonitor were supplied. -NoMonitor wins." -ForegroundColor DarkYellow
}
if ($ForceUploadFS) {
    Write-Host "[DEPLOY] UploadFS skip optimization: disabled by -ForceUploadFS" -ForegroundColor Yellow
}

Push-Location $projectRoot
try {
    $deployStopwatch = [System.Diagnostics.Stopwatch]::StartNew()

    Test-SecretsReadiness -SecretsPath (Join-Path $projectRoot "include\secrets.h")

    Stop-StalePlatformIOMonitors -Port $resolvedUploadPort

    if (-not $SkipClean) {
        Invoke-Step -Title "Clean" -Action {
            & $pioExe run --target clean
        } -RetryDelaySec $RetryDelaySec -InteractiveRetry:$InteractiveRetry
    }

    Invoke-Step -Title "Build" -Action {
        & $pioExe run
    } -RetryDelaySec $RetryDelaySec -InteractiveRetry:$InteractiveRetry

    $sourceFingerprintCurrent = Get-SourceDataFingerprint -DataPath $dataRoot
    $sourceFingerprintPrevious = Read-TextFileSafely -Path $uploadFsFingerprintCachePath
    $shouldUploadFs = $true

    if (-not $ForceUploadFS -and $sourceFingerprintCurrent -and $sourceFingerprintCurrent -eq $sourceFingerprintPrevious) {
        $shouldUploadFs = $false
    }

    if ($shouldUploadFs) {
        $uploadFsStepParams = @{
            Title = "Upload filesystem (LittleFS)"
            MaxAttempts = $UploadMaxAttempts
            RetryDelaySec = $RetryDelaySec
            RetryHint = "If flashing fails or says wrong boot mode, hold the BOOT button while retrying; release after 'Connecting...'."
            BeforeAttempt = {
                $script:resolvedUploadPort = Resolve-UploadPort -RequestedPort $UploadPort -PioExecutable $pioExe
                Stop-StalePlatformIOMonitors -Port $script:resolvedUploadPort
                if ($script:resolvedUploadPort) {
                    Write-Host "[DEPLOY] Uploadfs attempt using port: $script:resolvedUploadPort" -ForegroundColor DarkGray
                }
                else {
                    Write-Host "[DEPLOY] Uploadfs attempt using auto-detected port" -ForegroundColor DarkGray
                }
            }
            Action = {
                if ($script:resolvedUploadPort) {
                    & $pioExe run --target uploadfs --upload-port $script:resolvedUploadPort
                }
                else {
                    & $pioExe run --target uploadfs
                }
            }
        }
        if ($InteractiveRetry) {
            $uploadFsStepParams.InteractiveRetry = $true
        }
        Invoke-Step @uploadFsStepParams

        if ($sourceFingerprintCurrent) {
            Write-TextFileSafely -Path $uploadFsFingerprintCachePath -Value $sourceFingerprintCurrent
        }
    }
    else {
        Write-Host "`n[DEPLOY] Upload filesystem (LittleFS): skipped (no source data changes detected)" -ForegroundColor Green
    }

    $uploadFwStepParams = @{
        Title = "Upload firmware"
        MaxAttempts = $UploadMaxAttempts
        RetryDelaySec = $RetryDelaySec
        RetryHint = "If flashing fails or says wrong boot mode, hold the BOOT button while retrying; release after 'Connecting...'."
        BeforeAttempt = {
            $script:resolvedUploadPort = Resolve-UploadPort -RequestedPort $UploadPort -PioExecutable $pioExe
            Stop-StalePlatformIOMonitors -Port $script:resolvedUploadPort
            if ($script:resolvedUploadPort) {
                Write-Host "[DEPLOY] Upload attempt using port: $script:resolvedUploadPort" -ForegroundColor DarkGray
            }
            else {
                Write-Host "[DEPLOY] Upload attempt using auto-detected port" -ForegroundColor DarkGray
            }
        }
        Action = {
            if ($script:resolvedUploadPort) {
                & $pioExe run --target upload --upload-port $script:resolvedUploadPort
            }
            else {
                & $pioExe run --target upload
            }
        }
    }
    if ($InteractiveRetry) {
        $uploadFwStepParams.InteractiveRetry = $true
    }
    Invoke-Step @uploadFwStepParams

    if (-not $SkipEndpointChecks) {
        Test-PostDeployEndpoints -ExpectedIp $ExpectedIp -TimeoutSec $EndpointTimeoutSec
    }

    $deployStopwatch.Stop()
    Write-Host "`n[DEPLOY] Total pipeline time: $($deployStopwatch.Elapsed.ToString())" -ForegroundColor Green

    $startMonitor = $WithMonitor -and -not $NoMonitor
    if ($startMonitor) {
        Write-Host "`n[DEPLOY] Starting serial monitor (Ctrl+C to stop)..." -ForegroundColor Magenta
        if ($resolvedUploadPort) {
            & $pioExe device monitor -p $resolvedUploadPort -b $MonitorBaud -f direct
        }
        else {
            & $pioExe device monitor -b $MonitorBaud -f direct
        }
    }
    else {
        Write-Host "`n[DEPLOY] Done. Monitor skipped (default). Use -WithMonitor when you explicitly need serial output." -ForegroundColor Green
    }
}
finally {
    Pop-Location
}
