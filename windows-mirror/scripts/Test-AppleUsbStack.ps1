[CmdletBinding()]
param([string] $DeviceSelector)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ValeriaUsb.Common.ps1')

$records = @(Get-ValeriaAppleStackRecords -DeviceSelector $DeviceSelector)
if ($records.Count -eq 0) {
    throw 'No present USB\VID_05AC&PID_12A8 Apple parent device was found.'
}

$failed = $false
foreach ($record in $records) {
    Write-Host "Checking $($record.ParentInstanceId)"

    if ($record.ParentService -ine 'usbccgp') {
        Write-Warning "Parent service is '$($record.ParentService)'; expected usbccgp from AppleUsb.inf."
        $failed = $true
    }
    if (@($record.ParentLowerFilters | Where-Object { $_ -ieq 'AppleLowerFilter' }).Count -eq 0) {
        Write-Warning 'Parent LowerFilters does not contain AppleLowerFilter.'
        $failed = $true
    }
    if ($record.ParentDriverProvider -notmatch '^Apple(?:, Inc\.)?$') {
        Write-Warning "Parent provider is '$($record.ParentDriverProvider)'; expected Apple."
        $failed = $true
    }
    if ($null -ne $record.OriginalConfigurationValue -and
        [int] $record.OriginalConfigurationValue -ne 2) {
        Write-Warning "OriginalConfigurationValue is $($record.OriginalConfigurationValue), not the INF default 2. The healthy signed Apple parent/MI_01 stack remains authoritative; this tool will not rewrite it."
    }
    if ($null -ne $record.UsbccgpCapabilities -and
        [int] $record.UsbccgpCapabilities -ne 0x10) {
        Write-Warning ("UsbccgpCapabilities is 0x{0:X}; expected 0x10." -f [int] $record.UsbccgpCapabilities)
        $failed = $true
    }
    if (-not $record.AppleMuxPreserved) {
        Write-Warning 'MI_01 is not using Apple WinUSB with WUDFRd and AppleKmdfFilter.'
        $failed = $true
    }
    else {
        Write-Host "OK: Apple MI_01 remains on $($record.AppleMuxService) [$($record.AppleMuxDriverInf)]."
    }
}

if ($failed) {
    throw 'Unsupported or incomplete Apple USB driver stack. Repair Apple Devices before configuring MI_02.'
}

Write-Host 'OK: Apple USBCCGP parent, AppleLowerFilter, and MI_01 usbmux are preserved.'
