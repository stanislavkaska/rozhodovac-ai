# Jednoklikové nahrání firmwaru Rozhodovač do Waveshare ESP32-S3-Touch-AMOLED-2.16
param(
    [string]$Port = "",
    [switch]$Monitor,
    [switch]$EraseFirst
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$py = (Join-Path $env:LOCALAPPDATA "Programs\Python\Python312\python.exe")
if (-not (Test-Path $py)) { $py = "python" }

$portArgs = @()
$monitorArgs = @()
if ($Port -ne "") {
    $portArgs = @("--upload-port", $Port)
    $monitorArgs = @("--port", $Port)
}

# Nativní příkazy nevyhazují výjimku, chybový kód je nutné hlídat ručně
function Assert-ExitCode([string]$Step) {
    if ($LASTEXITCODE -ne 0) { throw "$Step selhalo (exit code $LASTEXITCODE)." }
}

function Stop-BridgeDaemon {
    & $py bridge/rozhodovac_ctl.py stop --keep-autostart | Out-Host
    # Pojistka pro zaseknutý daemon: ukončí jen procesy s agy_buddy_daemon.py, ne jiné Python skripty
    Get-CimInstance Win32_Process -Filter "Name LIKE 'python%'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like "*agy_buddy_daemon.py*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

# Zámek: hook během nahrávání daemon znovu nespouští a nezabere sériový port
$flashingLock = Join-Path $PSScriptRoot "bridge\.flashing"
New-Item -ItemType File -Path $flashingLock -Force | Out-Null

try {
    Write-Host "=== 1/4 Kompiluji firmware Rozhodovač ===" -ForegroundColor Cyan
    & $py -m platformio run
    Assert-ExitCode "Kompilace"

    Write-Host "=== 2/4 Uvolňuji sériový port (zastavuji daemon) ===" -ForegroundColor Yellow
    Stop-BridgeDaemon
    Start-Sleep -Milliseconds 800

    if ($EraseFirst) {
        Write-Host "=== Mažu flash paměť ESP32-S3 ===" -ForegroundColor Yellow
        & $py -m platformio run -t erase @portArgs
        Assert-ExitCode "Mazání flash"
    }

    Write-Host "=== 3/4 Nahrávám firmware do krabičky ===" -ForegroundColor Cyan
    & $py -m platformio run -t upload @portArgs
    Assert-ExitCode "Nahrání firmwaru"
}
finally {
    Remove-Item -Path $flashingLock -Force -ErrorAction SilentlyContinue
}

if ($Monitor) {
    Write-Host "=== 4/4 Otevírám sériový monitor (ukončení: Ctrl+C) ===" -ForegroundColor Green
    & $py -m platformio device monitor @monitorArgs
} else {
    Write-Host "=== 4/4 Spouštím znovu bridge daemon ===" -ForegroundColor Green
    Start-Sleep -Seconds 2
    & $py bridge/rozhodovac_ctl.py start
}
