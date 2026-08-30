[CmdletBinding()]
param(
    [string] $FfplayPath,
    [switch] $AllowHevc
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$activationExe = Join-Path $PSScriptRoot 'bin\valeria-activate.exe'
$mirrorExe = Join-Path $PSScriptRoot 'bin\iphone-valeria-mirror.exe'
$enableScript = Join-Path $PSScriptRoot 'scripts\Enable-ValeriaMirrorMode.ps1'
$stateScript = Join-Path $PSScriptRoot 'scripts\Get-ValeriaUsbState.ps1'
foreach ($requiredPath in @($activationExe, $mirrorExe, $enableScript, $stateScript)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "The package is incomplete. Missing: $requiredPath"
    }
}

if (-not $FfplayPath -and $env:VALERIA_FFPLAY) {
    $FfplayPath = $env:VALERIA_FFPLAY
}
if (-not $FfplayPath) {
    $packagedFfplay = Join-Path $PSScriptRoot 'tools\ffplay.exe'
    if (Test-Path -LiteralPath $packagedFfplay -PathType Leaf) {
        $FfplayPath = $packagedFfplay
    }
}
if (-not $FfplayPath) {
    $ffplayCommand = Get-Command 'ffplay.exe' -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($ffplayCommand) {
        $FfplayPath = $ffplayCommand.Source
    }
}
if (-not $FfplayPath) {
    $FfplayPath = (Read-Host 'Enter the full path to ffplay.exe').Trim('"')
}
$resolvedFfplay = (Resolve-Path -LiteralPath $FfplayPath -ErrorAction Stop).Path

Write-Host '=== Activating and verifying the realtime Valeria USB path ==='
& $enableScript -ActivationExe $activationExe
& $stateScript -RequireReady

$mirrorArguments = @('--ffplay', $resolvedFfplay)
if (-not $AllowHevc) {
    $mirrorArguments += '--force-h264'
}

Write-Host ''
Write-Host '=== Starting continuous screen mirror (Ctrl+C to stop) ==='
& $mirrorExe @mirrorArguments
if ($LASTEXITCODE -ne 0) {
    throw "iphone-valeria-mirror.exe exited with code $LASTEXITCODE."
}
