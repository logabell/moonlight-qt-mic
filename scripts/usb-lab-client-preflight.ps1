# Collect safe Moonlight USB passthrough client preflight artifacts.
#
# This script does not launch installers, UAC prompts, polkit prompts, real USB
# exports, or USB attach commands. The exporter probe is opt-in because it may
# start a managed usbipd process on Linux.
param(
    [string]$MoonlightExe = "",
    [string]$OutputRoot = "$PSScriptRoot\..\build\usb-lab-client-preflight",
    [switch]$TestExporter,
    [switch]$Help
)

$ErrorActionPreference = "Stop"
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

function Show-Usage {
    Write-Host "Usage: powershell -ExecutionPolicy Bypass -File scripts\usb-lab-client-preflight.ps1 [options]"
    Write-Host ""
    Write-Host "Options:"
    Write-Host "  -MoonlightExe <path>  Path to Moonlight.exe or moonlight"
    Write-Host "  -OutputRoot <path>    Artifact root directory"
    Write-Host "  -TestExporter         Also run usb-lab-list --test-exporter"
    Write-Host "  -Help                 Show this help"
    Write-Host ""
    Write-Host "Default behavior is safe-only:"
    Write-Host "  moonlight usb-lab-install --dry-run --json --output <file>"
    Write-Host "  moonlight usb-lab-list --json --output <file>"
}

function Resolve-RequiredFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }

    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-Moonlight {
    param([string]$Path)

    if (-not [string]::IsNullOrWhiteSpace($Path)) {
        return Resolve-RequiredFile -Path $Path -Label "Moonlight executable"
    }

    $command = Get-Command Moonlight.exe -ErrorAction SilentlyContinue
    if ($null -ne $command -and -not [string]::IsNullOrWhiteSpace($command.Source)) {
        return $command.Source
    }

    $command = Get-Command moonlight -ErrorAction SilentlyContinue
    if ($null -ne $command -and -not [string]::IsNullOrWhiteSpace($command.Source)) {
        return $command.Source
    }

    $repoCandidate = Join-Path $PSScriptRoot "..\build-codex\app\debug\Moonlight.exe"
    if (Test-Path -LiteralPath $repoCandidate -PathType Leaf) {
        return (Resolve-Path -LiteralPath $repoCandidate).Path
    }

    throw "Moonlight executable not found. Pass -MoonlightExe <path>."
}

function Read-JsonFile {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }

    return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
}

function Invoke-LabCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$Executable,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [Parameter(Mandatory = $true)]
        [string]$JsonPath,
        [Parameter(Mandatory = $true)]
        [string]$RunDir
    )

    $logPath = Join-Path $RunDir "$Name.log"
    $startedAt = (Get-Date).ToUniversalTime()
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $output = & $Executable @Arguments 2>&1 | ForEach-Object { $_.ToString() } | Out-String
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($null -eq $exitCode) {
        $exitCode = 0
    }
    $finishedAt = (Get-Date).ToUniversalTime()

    $output | Out-File -LiteralPath $logPath -Encoding utf8 -Force

    $jsonParsed = $false
    $jsonError = $null
    try {
        $jsonPayload = Read-JsonFile -Path $JsonPath
        $jsonParsed = $null -ne $jsonPayload
    } catch {
        $jsonError = $_.Exception.Message
    }

    return [ordered]@{
        name = $Name
        executable = $Executable
        arguments = $Arguments
        exitCode = $exitCode
        startedAt = $startedAt.ToString("o")
        finishedAt = $finishedAt.ToString("o")
        logPath = $logPath
        jsonPath = $JsonPath
        jsonParsed = $jsonParsed
        jsonError = $jsonError
    }
}

function Summarize-Install {
    param($Payload)

    if ($null -eq $Payload) {
        return $null
    }

    $action = $Payload.installAction
    return [ordered]@{
        state = $Payload.state
        supported = $Payload.supported
        backend = $Payload.backend
        dependenciesReady = $Payload.dependenciesReady
        actionAvailable = $action.available
        action = $action.action
        message = $Payload.message
    }
}

function Summarize-Status {
    param($Payload)

    if ($null -eq $Payload) {
        return $null
    }

    $devices = @($Payload.devices)
    return [ordered]@{
        supported = $Payload.supported
        backend = $Payload.backend
        dependenciesReady = $Payload.dependenciesReady
        usbipPath = $Payload.usbipPath
        usbipdPath = $Payload.usbipdPath
        usbipdServiceState = $Payload.usbipdServiceState
        usbipCoreLoaded = $Payload.usbipCoreLoaded
        usbipHostLoaded = $Payload.usbipHostLoaded
        exporterTested = $Payload.exporterTested
        exporterReady = $Payload.exporterReady
        deviceCount = $devices.Count
        statusMessage = $Payload.statusMessage
        lastError = $Payload.lastError
    }
}

if ($Help) {
    Show-Usage
    exit 0
}

$moonlight = Resolve-Moonlight -Path $MoonlightExe
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$runDir = Join-Path $OutputRoot $timestamp
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$runDir = (Resolve-Path -LiteralPath $runDir).Path

$commands = @()

$clientInstallJson = Join-Path $runDir "client-install-dry-run.json"
$commands += Invoke-LabCommand `
    -Name "client-install-dry-run" `
    -Executable $moonlight `
    -Arguments @("usb-lab-install", "--dry-run", "--json", "--output", $clientInstallJson) `
    -JsonPath $clientInstallJson `
    -RunDir $runDir

$clientStatusJson = Join-Path $runDir "client-status.json"
$commands += Invoke-LabCommand `
    -Name "client-status" `
    -Executable $moonlight `
    -Arguments @("usb-lab-list", "--json", "--output", $clientStatusJson) `
    -JsonPath $clientStatusJson `
    -RunDir $runDir

$clientExporterJson = $null
if ($TestExporter) {
    $clientExporterJson = Join-Path $runDir "client-exporter-test.json"
    $commands += Invoke-LabCommand `
        -Name "client-exporter-test" `
        -Executable $moonlight `
        -Arguments @("usb-lab-list", "--json", "--test-exporter", "--output", $clientExporterJson) `
        -JsonPath $clientExporterJson `
        -RunDir $runDir
}

$manifest = [ordered]@{
    generatedAt = (Get-Date).ToUniversalTime().ToString("o")
    outputDir = $runDir
    moonlightExe = $moonlight
    safeOnly = -not [bool]$TestExporter
    exporterProbeRequested = [bool]$TestExporter
    commands = $commands
    client = [ordered]@{
        installDryRun = Summarize-Install (Read-JsonFile -Path $clientInstallJson)
        status = Summarize-Status (Read-JsonFile -Path $clientStatusJson)
        exporterProbe = Summarize-Status (Read-JsonFile -Path $clientExporterJson)
    }
}

$manifestPath = Join-Path $runDir "manifest.json"
$manifest | ConvertTo-Json -Depth 12 | Out-File -LiteralPath $manifestPath -Encoding utf8 -Force

Write-Host "Moonlight USB lab preflight artifacts written to:"
Write-Host "  $runDir"
Write-Host "Manifest:"
Write-Host "  $manifestPath"
Write-Host ""
Write-Host "Client:"
Write-Host "  install dry-run: $($manifest.client.installDryRun.state) action=$($manifest.client.installDryRun.action)"
Write-Host "  status: backend=$($manifest.client.status.backend) dependenciesReady=$($manifest.client.status.dependenciesReady) devices=$($manifest.client.status.deviceCount)"
if ($manifest.client.exporterProbe) {
    Write-Host "  exporter probe: ready=$($manifest.client.exporterProbe.exporterReady) status=$($manifest.client.exporterProbe.statusMessage)"
} else {
    Write-Host "  exporter probe: skipped (pass -TestExporter to include it)"
}
