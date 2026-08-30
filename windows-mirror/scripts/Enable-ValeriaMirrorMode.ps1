[CmdletBinding()]
param(
    [string] $ActivationExe,
    [string] $InstanceId,
    [string] $DeviceSelector,
    [ValidateRange(5, 60)] [int] $TimeoutSeconds = 45
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ValeriaUsb.Common.ps1')

Assert-ValeriaWindows
$beforeRecords = @(Get-ValeriaUsbRecords -InstanceId $InstanceId)
if ($beforeRecords.Count -gt 1) {
    throw 'More than one matching MI_02 is present. Pass an exact -InstanceId.'
}
if ($beforeRecords.Count -eq 1 -and $beforeRecords[0].ReadyForRealtimeMirror) {
    Write-Host 'Valeria configuration is already active and ready.'
    $beforeRecords[0]
    return
}

$stackSelector = $DeviceSelector
if (-not $stackSelector -and $beforeRecords.Count -eq 1) {
    $stackSelector = ($beforeRecords[0].ParentInstanceId -split '\\')[-1]
}
$stacks = @(Get-ValeriaAppleStackRecords -DeviceSelector $stackSelector)
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

Write-Host 'Requesting Apple mode 2. AppleLowerFilter should select effective configuration 5...'
Invoke-ValeriaMode2Activation `
    -ActivationExe $resolvedActivationExe `
    -DeviceSelector $selector `
    -TimeoutSeconds $TimeoutSeconds | Out-Null

$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$after = $null
do {
    $candidates = @(Get-ValeriaUsbRecords | Where-Object {
        $_.ParentInstanceId -ieq $stack.ParentInstanceId
    })
    if ($InstanceId) {
        $candidates = @($candidates | Where-Object { $_.InstanceId -ieq $InstanceId })
    }
    if ($candidates.Count -gt 1) {
        throw 'More than one MI_02 child matched the selected Apple parent.'
    }
    if ($candidates.Count -eq 1) {
        $after = $candidates[0]
        if ($after.ReadyForRealtimeMirror) {
            break
        }
    }
    Start-Sleep -Milliseconds 400
} while ([DateTime]::UtcNow -lt $deadline)

if (-not $after) {
    throw 'Mode 2 was verified, but Windows did not expose the Valeria MI_02 child.'
}
if (-not $after.IsWinUsb) {
    throw 'Valeria MI_02 is now visible but is not bound to WinUSB. Run Prepare-InboxWinUsbMi02.ps1 and complete the Device Manager step.'
}
if (-not $after.DeviceInterfaceGuidConfigured) {
    throw 'MI_02 uses WinUSB but lacks the app GUID. Run Install-ValeriaWinUsb.ps1.'
}
if (-not $after.WinUsbInterfaceRegistered) {
    throw 'The GUID is configured but not registered. Unplug/reconnect to the same USB port, then run this script again.'
}
if (-not $after.IsValeriaInterface) {
    throw 'The selected MI_02 does not advertise FF/2A/FF after verified activation.'
}
if (-not $after.ReadyForRealtimeMirror) {
    throw ("Valeria is not ready: " + ($after.BlockingReasons -join ' '))
}

Write-Host 'Ready: MI_02 advertises FF/2A/FF, uses winusb.sys, and Apple MI_01 remains intact.'
Write-Host 'This is the continuous AV bulk-stream path; no screenshot API is used.'
$after
