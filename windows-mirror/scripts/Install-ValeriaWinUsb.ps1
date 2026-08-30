[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'Medium')]
param(
    [string] $ManifestPath,
    [string] $InstanceId,
    [string] $BackupDirectory = (Join-Path $env:ProgramData 'ValeriaMirror\DriverBackup')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ValeriaUsb.Common.ps1')

Assert-ValeriaAdministrator
if (-not $ManifestPath) {
    $candidateManifests = @(Get-ChildItem -LiteralPath $BackupDirectory -Filter 'manifest.json' -Recurse -File -ErrorAction Stop |
        Sort-Object LastWriteTimeUtc -Descending)
    foreach ($candidate in $candidateManifests) {
        try {
            $candidateData = Get-Content -LiteralPath $candidate.FullName -Raw | ConvertFrom-Json
            if ([int] $candidateData.SchemaVersion -eq 2) {
                $ManifestPath = $candidate.FullName
                break
            }
        }
        catch {
            # Ignore unrelated or incomplete manifests and keep looking.
        }
    }
    if (-not $ManifestPath) {
        throw 'No inbox-WinUSB preparation manifest was found. Run Prepare-InboxWinUsbMi02.ps1 first.'
    }
}

$resolvedManifest = (Resolve-Path -LiteralPath $ManifestPath -ErrorAction Stop).Path
$manifest = Get-Content -LiteralPath $resolvedManifest -Raw | ConvertFrom-Json
if ([int] $manifest.SchemaVersion -ne 2 -or
    $manifest.BindingMethod -ine 'MicrosoftInboxWinUsbManualSelection' -or
    $manifest.HardwareId -ine 'USB\VID_05AC&PID_12A8&MI_02') {
    throw 'The selected manifest is not an inbox-WinUSB preparation manifest.'
}
$expectedInstanceId = [string] $manifest.Devices[0].InstanceId
if ($expectedInstanceId -notmatch '^USB\\VID_05AC&PID_12A8&MI_02\\') {
    throw "Unsafe MI_02 instance ID in manifest: $expectedInstanceId"
}
if ($InstanceId -and $InstanceId -ine $expectedInstanceId) {
    throw 'The requested instance ID does not match the preparation manifest.'
}

$records = @(Get-ValeriaUsbRecords -InstanceId $expectedInstanceId)
if ($records.Count -ne 1) {
    throw 'The prepared MI_02 device is not present. Reconnect it to the same physical USB port.'
}
$record = $records[0]
if (-not $record.AppleParentPreserved -or -not $record.AppleMuxPreserved) {
    throw 'The Apple parent or MI_01 binding changed. Do not continue; restore the Apple stack.'
}
if ($record.Service -ine 'WinUSB' -or $record.DriverInf -ine 'winusb.inf' -or
    $record.DriverProvider -notmatch '^Microsoft') {
    throw "MI_02 is not on Microsoft's inbox WinUSB Device driver (service=$($record.Service), INF=$($record.DriverInf), provider=$($record.DriverProvider))."
}

$deviceParametersPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\$($record.InstanceId)\Device Parameters"
$interfaceGuid = '{77E935B1-B768-4316-A466-4E745CFDDB24}'
$existingGuids = @(
    Get-ValeriaDeviceParameterValue -InstanceId $record.InstanceId -ValueName 'DeviceInterfaceGUIDs'
) | Where-Object { $_ }
$newGuids = @($existingGuids + $interfaceGuid | Sort-Object -Unique)

if (-not $PSCmdlet.ShouldProcess(
        "$deviceParametersPath\DeviceInterfaceGUIDs",
        "Add $interfaceGuid without changing any driver package")) {
    return
}

New-Item -Path $deviceParametersPath -Force | Out-Null
New-ItemProperty `
    -LiteralPath $deviceParametersPath `
    -Name 'DeviceInterfaceGUIDs' `
    -PropertyType MultiString `
    -Value ([string[]] $newGuids) `
    -Force | Out-Null

$manifest.ConfiguredUtc = [DateTime]::UtcNow.ToString('o')
$manifest.InboxDriverInf = $record.DriverInf
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resolvedManifest -Encoding UTF8

Write-Host 'Configured the WinUSB interface GUID. No INF, certificate, libusb filter, or third-party .sys was installed.'
Write-Warning 'Now unplug the iPhone and reconnect it to the SAME physical USB port so winusb.sys registers the interface.'
Write-Host 'After reconnecting, run Enable-ValeriaMirrorMode.ps1, then Get-ValeriaUsbState.ps1 -RequireReady.'
