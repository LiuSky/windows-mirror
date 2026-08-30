[CmdletBinding()]
param(
    [string] $InstanceId,
    [string] $BackupDirectory = (Join-Path $env:ProgramData 'ValeriaMirror\DriverBackup'),
    [switch] $OpenDeviceManager
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ValeriaUsb.Common.ps1')

Assert-ValeriaAdministrator
$records = @(Get-ValeriaUsbRecords -InstanceId $InstanceId)
if ($records.Count -eq 0) {
    throw 'No present PID_12A8 MI_02 device was found.'
}
if ($records.Count -gt 1) {
    throw 'Connect exactly one matching iPhone before manually selecting a driver.'
}
$record = $records[0]
if (-not $record.AppleParentPreserved -or -not $record.AppleMuxPreserved) {
    throw 'Repair/install the current Apple Devices USB package before continuing.'
}
$alreadyInboxWinUsb = ($record.Service -ieq 'WinUSB') -and
    ($record.DriverInf -ieq 'winusb.inf') -and
    ($record.DriverProvider -imatch '^Microsoft')
if ($alreadyInboxWinUsb) {
    Write-Warning 'MI_02 already uses inbox WinUSB. A GUID manifest will still be created, but it cannot reconstruct the earlier network binding unless an older preparation backup exists.'
}

$runDirectory = Join-Path $BackupDirectory ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmssfffZ'))
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null
$exportedInfRelativePath = $null
if ($record.DriverInf -match '^oem\d+\.inf$') {
    $exportDirectory = Join-Path $runDirectory ("original-" + [IO.Path]::GetFileNameWithoutExtension($record.DriverInf))
    New-Item -ItemType Directory -Path $exportDirectory -Force | Out-Null
    Invoke-ValeriaNativeCommand -FilePath 'pnputil.exe' -ArgumentList @(
        '/export-driver', $record.DriverInf, $exportDirectory
    ) | Out-Null
    $exportedInf = Get-ChildItem -LiteralPath $exportDirectory -Filter '*.inf' -Recurse -File |
        Select-Object -First 1
    if (-not $exportedInf) {
        throw "No INF was found after exporting $($record.DriverInf)."
    }
    $exportedInfRelativePath = Get-ValeriaRelativePath `
        -BasePath $runDirectory `
        -TargetPath $exportedInf.FullName
}

$previousGuids = @(
    Get-ValeriaDeviceParameterValue -InstanceId $record.InstanceId -ValueName 'DeviceInterfaceGUIDs'
) | Where-Object { $_ }
$manifestPath = Join-Path $runDirectory 'manifest.json'
$manifest = [ordered] @{
    SchemaVersion      = 2
    BindingMethod      = 'MicrosoftInboxWinUsbManualSelection'
    CreatedUtc         = [DateTime]::UtcNow.ToString('o')
    HardwareId         = 'USB\VID_05AC&PID_12A8&MI_02'
    InterfaceGuid      = '{77E935B1-B768-4316-A466-4E745CFDDB24}'
    OriginalBindingCapturedBeforeManualSelection = -not $alreadyInboxWinUsb
    ConfiguredUtc      = $null
    InboxDriverInf     = $null
    Devices            = @([ordered] @{
        InstanceId                    = $record.InstanceId
        OriginalDriverInf             = $record.DriverInf
        OriginalDriverProvider        = $record.DriverProvider
        OriginalDriverVersion         = $record.DriverVersion
        OriginalService               = $record.Service
        ExportedInfRelativePath       = $exportedInfRelativePath
        PreviousDeviceInterfaceGuids  = $previousGuids
        ParentInstanceId              = $record.ParentInstanceId
        ParentDriverInf               = $record.ParentDriverInf
        AppleMuxDriverInf             = $record.AppleMuxDriverInf
    })
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host "Backup complete: $manifestPath"
Write-Host ''
if ($alreadyInboxWinUsb) {
    Write-Host 'The Device Manager selection is already complete.'
    Write-Host "Next: .\scripts\Install-ValeriaWinUsb.ps1 -ManifestPath '$manifestPath'"
}
else {
    Write-Host 'In Device Manager, change ONLY this device node:'
    Write-Host "  $($record.InstanceId)"
    Write-Host '  1. Update driver -> Browse my computer for drivers.'
    Write-Host '  2. Let me pick from a list of available drivers on my computer.'
    Write-Host '  3. Select Universal Serial Bus devices -> WinUSB Device (Microsoft).'
    Write-Host '  4. Do not change the PID_12A8 parent or MI_01.'
    Write-Host "  5. Then run: .\scripts\Install-ValeriaWinUsb.ps1 -ManifestPath '$manifestPath'"
}

if ($OpenDeviceManager -and -not $alreadyInboxWinUsb) {
    Start-Process 'devmgmt.msc'
}

$nextAction = if ($alreadyInboxWinUsb) {
    'Run Install-ValeriaWinUsb.ps1 with this manifest.'
}
else {
    'Manually select Microsoft WinUSB Device for MI_02 only.'
}
[pscustomobject] @{
    InstanceId   = $record.InstanceId
    ManifestPath = $manifestPath
    NextAction   = $nextAction
}
