[CmdletBinding()]
param(
    [string] $InstanceId,
    [switch] $AsJson,
    [switch] $RequireReady
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ValeriaUsb.Common.ps1')

$records = @(Get-ValeriaUsbRecords -InstanceId $InstanceId)
if ($records.Count -eq 0) {
    throw 'No present PID_12A8 MI_02 was found. Initial mode 1 may omit it; run Activate-ValeriaUsbMode.ps1 first.'
}

if ($AsJson) {
    $records | ConvertTo-Json -Depth 8
}
else {
    foreach ($record in $records) {
        Write-Host "MI_02:       $($record.InstanceId)"
        Write-Host "Profile:     $($record.UsbProfile)"
        Write-Host "MI_02 driver: $($record.Service) [$($record.DriverInf), $($record.DriverProvider)]"
        Write-Host "Inbox WinUSB: $($record.IsMicrosoftInboxWinUsb)"
        Write-Host "Parent:       $($record.ParentService) [$($record.ParentDriverInf)]"
        Write-Host "LowerFilters: $($record.ParentLowerFilters -join ', ')"
        Write-Host "Apple MI_01:  $($record.AppleMuxService) [$($record.AppleMuxDriverInf)]"
        Write-Host "App GUID:     configured=$($record.DeviceInterfaceGuidConfigured), registered=$($record.WinUsbInterfaceRegistered)"
        Write-Host "Ready:        $($record.ReadyForRealtimeMirror)"

        if ($null -ne $record.OriginalConfigurationValue -and
            [int] $record.OriginalConfigurationValue -ne 2) {
            Write-Warning "Apple OriginalConfigurationValue is $($record.OriginalConfigurationValue), not the INF default 2. The live healthy Apple stack is used and this tool will not rewrite it."
        }
        if ($null -ne $record.UsbccgpCapabilities -and
            [int] $record.UsbccgpCapabilities -ne 0x10) {
            Write-Warning ("Apple UsbccgpCapabilities is 0x{0:X}, expected 0x10. This tool will not rewrite it." -f [int] $record.UsbccgpCapabilities)
        }
        foreach ($reason in $record.BlockingReasons) {
            Write-Warning $reason
        }
    }
}

if ($RequireReady -and @($records | Where-Object { -not $_.ReadyForRealtimeMirror }).Count -gt 0) {
    throw 'The Valeria realtime-mirror USB path is not ready.'
}
