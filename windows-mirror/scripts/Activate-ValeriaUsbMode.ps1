[CmdletBinding()]
param(
    [string] $ActivationExe,
    [string] $DeviceSelector,
    [ValidateRange(5, 60)] [int] $TimeoutSeconds = 45
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ValeriaUsb.Common.ps1')

Assert-ValeriaWindows
$stacks = @(Get-ValeriaAppleStackRecords -DeviceSelector $DeviceSelector)
if ($stacks.Count -eq 0) {
    throw 'No supported PID_12A8 Apple USB parent/MI_01 stack was found.'
}
if ($stacks.Count -gt 1) {
    throw 'More than one iPhone is connected. Pass -DeviceSelector with a unique serial/instance-ID substring.'
}
$stack = $stacks[0]
if (-not $stack.AppleStackHealthy) {
    throw 'The Apple USBCCGP + AppleLowerFilter + MI_01 stack is not healthy.'
}

$resolvedActivationExe = Resolve-ValeriaActivationExecutable -ActivationExe $ActivationExe
$selector = if ($DeviceSelector) {
    $DeviceSelector
}
else {
    ($stack.ParentInstanceId -split '\\')[-1]
}

Write-Host 'Requesting Apple mode 2 through official MI_01; MI_02 is not required yet...'
Invoke-ValeriaMode2Activation `
    -ActivationExe $resolvedActivationExe `
    -DeviceSelector $selector `
    -TimeoutSeconds $TimeoutSeconds | Out-Null

$deadline = [DateTime]::UtcNow.AddSeconds(15)
$mi02 = $null
do {
    $mi02 = @(Get-ValeriaUsbRecords | Where-Object {
        $_.ParentInstanceId -ieq $stack.ParentInstanceId -and $_.IsValeriaInterface
    } | Select-Object -First 1)
    if ($mi02.Count -eq 1) {
        $mi02 = $mi02[0]
        break
    }
    $mi02 = $null
    Start-Sleep -Milliseconds 400
} while ([DateTime]::UtcNow -lt $deadline)

if (-not $mi02) {
    throw 'Mode 2 was verified, but Windows did not expose the Valeria MI_02 PnP node.'
}

Write-Host "Valeria MI_02 is visible: $($mi02.InstanceId)"
if (-not $mi02.IsMicrosoftInboxWinUsb) {
    Write-Host 'Next: run Prepare-InboxWinUsbMi02.ps1, then manually select Microsoft WinUSB Device for this MI_02 node.'
}
elseif (-not $mi02.DeviceInterfaceGuidConfigured -or -not $mi02.WinUsbInterfaceRegistered) {
    Write-Host 'MI_02 already uses inbox WinUSB. Run Prepare-InboxWinUsbMi02.ps1 and Install-ValeriaWinUsb.ps1 to register the app GUID.'
}
else {
    Write-Host 'MI_02 already has the Microsoft WinUSB binding and project interface GUID.'
}

$mi02
