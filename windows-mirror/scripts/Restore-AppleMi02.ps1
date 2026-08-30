[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High')]
param(
    [string] $ManifestPath,
    [string] $BackupDirectory = (Join-Path $env:ProgramData 'ValeriaMirror\DriverBackup'),
    [switch] $AllMatchingDevices,
    [switch] $KeepValeriaPackage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ValeriaUsb.Common.ps1')

Assert-ValeriaAdministrator
if (-not $ManifestPath) {
    $latest = Get-ChildItem -LiteralPath $BackupDirectory -Filter 'manifest.json' -Recurse -File -ErrorAction Stop |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if (-not $latest) {
        throw "No recovery manifest exists below $BackupDirectory."
    }
    $ManifestPath = $latest.FullName
}

$resolvedManifest = (Resolve-Path -LiteralPath $ManifestPath -ErrorAction Stop).Path
$manifestDirectory = Split-Path -Parent $resolvedManifest
$manifest = Get-Content -LiteralPath $resolvedManifest -Raw | ConvertFrom-Json
$schemaVersion = [int] $manifest.SchemaVersion
if ($schemaVersion -notin @(1, 2) -or $manifest.HardwareId -ine 'USB\VID_05AC&PID_12A8&MI_02') {
    throw 'The recovery manifest is not a supported Valeria MI_02 manifest.'
}
foreach ($device in @($manifest.Devices)) {
    if ([string] $device.InstanceId -notmatch '^USB\\VID_05AC&PID_12A8&MI_02\\') {
        throw "Unsafe device target in recovery manifest: $($device.InstanceId)"
    }
}
$hasPreselectionBinding = $true
if ($schemaVersion -eq 2 -and
    $manifest.PSObject.Properties.Name -contains 'OriginalBindingCapturedBeforeManualSelection') {
    $hasPreselectionBinding = [bool] $manifest.OriginalBindingCapturedBeforeManualSelection
    if (-not $hasPreselectionBinding) {
        Write-Warning 'This manifest was created after inbox WinUSB was already selected. It can restore GUID state but cannot reconstruct the earlier NCM/network binding.'
    }
}

$present = @(Get-ValeriaUsbRecords)
if ($present.Count -eq 0) {
    throw 'MI_02 is not present. Run Activate-ValeriaUsbMode.ps1 first, then restore while Valeria MI_02 is visible.'
}
if ($present.Count -gt 1 -and -not $AllMatchingDevices) {
    throw 'More than one matching iPhone is present. Disconnect all but one, or explicitly pass -AllMatchingDevices.'
}
if (-not $PSCmdlet.ShouldProcess(
        $manifest.HardwareId,
        "Restore the previous MI_02 driver recorded in $resolvedManifest")) {
    return
}

$rebootRequired = $false
$devicesWithoutRecordedDriver = @($manifest.Devices | Where-Object {
    -not $_.OriginalDriverInf
})
$sources = @($manifest.Devices | Where-Object { $_.OriginalDriverInf } | ForEach-Object {
    $source = $null
    if ($_.ExportedInfRelativePath) {
        $source = Join-Path $manifestDirectory ([string] $_.ExportedInfRelativePath)
    }
    elseif ($_.OriginalDriverInf -and $_.OriginalDriverInf -notmatch '^oem\d+\.inf$') {
        $source = Join-Path $env:windir (Join-Path 'INF' ([string] $_.OriginalDriverInf))
    }

    [pscustomobject] @{
        OriginalDriverInf = [string] $_.OriginalDriverInf
        SourceInf         = $source
    }
} | Sort-Object OriginalDriverInf -Unique)

if ($schemaVersion -eq 2) {
    foreach ($device in @($manifest.Devices)) {
        $deviceParametersPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\$($device.InstanceId)\Device Parameters"
        $previousGuids = @($device.PreviousDeviceInterfaceGuids) | Where-Object { $_ }
        if ($previousGuids.Count -gt 0) {
            New-Item -Path $deviceParametersPath -Force | Out-Null
            New-ItemProperty `
                -LiteralPath $deviceParametersPath `
                -Name 'DeviceInterfaceGUIDs' `
                -PropertyType MultiString `
                -Value ([string[]] $previousGuids) `
                -Force | Out-Null
        }
        elseif (Test-Path -LiteralPath $deviceParametersPath) {
            Remove-ItemProperty `
                -LiteralPath $deviceParametersPath `
                -Name 'DeviceInterfaceGUIDs' `
                -ErrorAction SilentlyContinue
        }
    }
}

foreach ($source in $sources) {
    if (-not $source.SourceInf -or -not (Test-Path -LiteralPath $source.SourceInf -PathType Leaf)) {
        throw "Cannot locate recovery source for $($source.OriginalDriverInf)."
    }
    Invoke-ValeriaNativeCommand -FilePath 'pnputil.exe' -ArgumentList @(
        '/add-driver', $source.SourceInf
    ) | Out-Null
    if (Install-ValeriaDriverForHardwareId -InfPath $source.SourceInf -HardwareId $manifest.HardwareId) {
        $rebootRequired = $true
    }
}

$removedDriverlessDevice = $false
foreach ($device in $devicesWithoutRecordedDriver) {
    Write-Warning "No preexisting MI_02 driver was recorded for $($device.InstanceId); removing that child devnode so Windows can enumerate it cleanly on reconnect."
    Invoke-ValeriaNativeCommand -FilePath 'pnputil.exe' -ArgumentList @(
        '/remove-device', [string] $device.InstanceId
    ) | Out-Null
    $removedDriverlessDevice = $true
}

$customPublishedInf = $null
if ($manifest.PSObject.Properties.Name -contains 'CustomPublishedInf') {
    $customPublishedInf = [string] $manifest.CustomPublishedInf
}
if (-not $KeepValeriaPackage -and $customPublishedInf -match '^oem\d+\.inf$') {
    $deleteOutput = @(& pnputil.exe /delete-driver $customPublishedInf /force 2>&1 | ForEach-Object { [string] $_ })
    $deleteOutput | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "The previous driver was restored, but $customPublishedInf could not be removed from Driver Store."
    }
}

if (-not $removedDriverlessDevice) {
    Invoke-ValeriaNativeCommand -FilePath 'pnputil.exe' -ArgumentList @('/scan-devices') | Out-Null
}
Write-Host 'The previous MI_02 driver binding and interface-GUID settings were restored.'
if ($hasPreselectionBinding) {
    Write-Warning 'Unplug and reconnect the iPhone to leave the transient Valeria USB configuration and restore normal USB networking.'
}
else {
    Write-Warning 'To restore networking, use Device Manager to select the original USB NCM/network driver, then unplug/reconnect.'
}
if ($rebootRequired) {
    Write-Warning 'Windows reported that a reboot is required.'
}
