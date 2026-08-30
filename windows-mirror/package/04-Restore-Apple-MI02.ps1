[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$activationExe = Join-Path $PSScriptRoot 'bin\valeria-activate.exe'
$activateScript = Join-Path $PSScriptRoot 'scripts\Activate-ValeriaUsbMode.ps1'
$restoreScript = Join-Path $PSScriptRoot 'scripts\Restore-AppleMi02.ps1'
foreach ($requiredPath in @($activationExe, $activateScript, $restoreScript)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "The package is incomplete. Missing: $requiredPath"
    }
}

$recoveryPointer = Join-Path $env:ProgramData 'ValeriaMirror\PortableTesterRecoveryManifest.txt'
if (-not (Test-Path -LiteralPath $recoveryPointer -PathType Leaf)) {
    throw 'No pre-WinUSB recovery manifest was saved. Select the original USB NCM/network driver manually in Device Manager.'
}
$manifestPath = (Get-Content -LiteralPath $recoveryPointer -Raw).Trim()
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "The saved recovery manifest no longer exists: $manifestPath"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ([int] $manifest.SchemaVersion -ne 2 -or
    $manifest.HardwareId -ine 'USB\VID_05AC&PID_12A8&MI_02' -or
    -not [bool] $manifest.OriginalBindingCapturedBeforeManualSelection) {
    throw 'The saved manifest was not captured before WinUSB selection; automatic restore is unsafe.'
}

& $activateScript -ActivationExe $activationExe
& $restoreScript -ManifestPath $manifestPath

Write-Host ''
Write-Host '[NEXT] Unplug the iPhone and reconnect it to the SAME physical USB port.'
