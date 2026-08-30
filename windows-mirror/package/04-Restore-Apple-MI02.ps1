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
$capturedProperty = $manifest.PSObject.Properties['OriginalBindingCapturedBeforeManualSelection']
$manifestDevices = @($manifest.Devices)
if ([int] $manifest.SchemaVersion -ne 2 -or
    $manifest.BindingMethod -ine 'MicrosoftInboxWinUsbManualSelection' -or
    $manifest.HardwareId -ine 'USB\VID_05AC&PID_12A8&MI_02' -or
    $manifestDevices.Count -ne 1 -or
    -not $capturedProperty -or
    $capturedProperty.Value -isnot [bool] -or
    $capturedProperty.Value -ne $true) {
    throw 'The saved manifest was not captured before WinUSB selection; automatic restore is unsafe.'
}
$manifestDevice = $manifestDevices[0]
$expectedInstanceId = [string] $manifestDevice.InstanceId
$expectedParentInstanceId = [string] $manifestDevice.ParentInstanceId
if ($expectedInstanceId -notmatch '^USB\\VID_05AC&PID_12A8&MI_02\\' -or
    -not $expectedParentInstanceId) {
    throw 'The saved recovery manifest has an unsafe device identity.'
}

$activeRecords = @(& $activateScript -ActivationExe $activationExe)
if ($activeRecords.Count -ne 1 -or
    $activeRecords[0].InstanceId -ine $expectedInstanceId -or
    $activeRecords[0].ParentInstanceId -ine $expectedParentInstanceId) {
    throw 'The connected iPhone/USB instance does not match the saved recovery manifest. Automatic restore was stopped.'
}
& $restoreScript -ManifestPath $manifestPath

Write-Host ''
Write-Host '[NEXT] Unplug the iPhone and reconnect it to the SAME physical USB port.'
