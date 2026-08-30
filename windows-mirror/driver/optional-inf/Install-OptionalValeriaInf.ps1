[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High')]
param(
    [string] $InfPath = (Join-Path $PSScriptRoot 'ValeriaWinUSB.inf'),
    [string] $BackupDirectory = (Join-Path $env:ProgramData 'ValeriaMirror\DriverBackup'),
    [string] $InstanceId,
    [switch] $AllMatchingDevices,
    [string] $TestCertificatePath,
    [switch] $TrustTestCertificate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\..\scripts\ValeriaUsb.Common.ps1')

Assert-ValeriaAdministrator
$resolvedInf = (Resolve-Path -LiteralPath $InfPath -ErrorAction Stop).Path
$catalogPath = Join-Path (Split-Path -Parent $resolvedInf) 'ValeriaWinUSB.cat'
if (-not (Test-Path -LiteralPath $catalogPath -PathType Leaf)) {
    throw "Missing signed catalog: $catalogPath. Run Build-ValeriaWinUsbPackage.ps1 first."
}

if ($TrustTestCertificate) {
    if (-not $TestCertificatePath) {
        throw '-TrustTestCertificate also requires -TestCertificatePath.'
    }
    if (-not $PSCmdlet.ShouldProcess(
            'LocalMachine Root and TrustedPublisher certificate stores',
            "Trust the test certificate '$TestCertificatePath'")) {
        return
    }
    $resolvedCertificate = (Resolve-Path -LiteralPath $TestCertificatePath -ErrorAction Stop).Path
    Import-Certificate -FilePath $resolvedCertificate -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
    Import-Certificate -FilePath $resolvedCertificate -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null
    Write-Warning 'A test certificate was added to machine trust stores. Remove it after testing.'
}

$signature = Get-AuthenticodeSignature -FilePath $catalogPath
if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
    throw "Catalog signature is not trusted: $($signature.Status) - $($signature.StatusMessage)"
}

$allRecords = @(Get-ValeriaUsbRecords)
$records = if ($InstanceId) {
    @($allRecords | Where-Object { $_.InstanceId -ieq $InstanceId })
}
else {
    $allRecords
}
if ($records.Count -eq 0) {
    throw 'No present PID_12A8 MI_02 device was found.'
}
if ($allRecords.Count -gt 1 -and -not $AllMatchingDevices) {
    throw 'More than one matching iPhone is present. Disconnect all but one, or explicitly pass -AllMatchingDevices.'
}
if ($allRecords.Count -gt 1 -and $AllMatchingDevices) {
    # UpdateDriverForPlugAndPlayDevices applies to every present device with
    # this hardware ID, so every affected device must be included in backup.
    $records = $allRecords
}
foreach ($record in $records) {
    if (-not $record.AppleParentPreserved -or -not $record.AppleMuxPreserved) {
        throw "Unsupported Apple stack on $($record.InstanceId). Run Test-AppleUsbStack.ps1."
    }
}
if (@($records | Where-Object {
        $_.IsWinUsb -and $_.DriverProvider -ieq 'Valeria Mirror Project'
    }).Count -eq $records.Count) {
    Write-Host 'The Valeria MI_02 WinUSB package is already bound.'
    $records
    return
}
if (@($records | Where-Object {
        $_.IsWinUsb -and $_.DriverProvider -ieq 'Valeria Mirror Project'
    }).Count -gt 0) {
    throw 'Only some matching devices use the Valeria package. Connect and process one device at a time.'
}

$targetText = if ($records.Count -eq 1) { $records[0].InstanceId } else { "$($records.Count) matching MI_02 devices" }
if (-not $PSCmdlet.ShouldProcess(
        $targetText,
        'Back up the current MI_02 package and replace only MI_02 with Microsoft winusb.sys')) {
    return
}

$runDirectory = Join-Path $BackupDirectory ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmssfffZ'))
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null

$deviceBackups = [Collections.Generic.List[object]]::new()
$exportedPackages = @{}
foreach ($record in $records) {
    $originalInf = [string] $record.DriverInf
    $exportedInfRelativePath = $null
    if ($originalInf -match '^oem\d+\.inf$') {
        if (-not $exportedPackages.ContainsKey($originalInf)) {
            $exportDirectory = Join-Path $runDirectory ("original-" + [IO.Path]::GetFileNameWithoutExtension($originalInf))
            New-Item -ItemType Directory -Path $exportDirectory -Force | Out-Null
            Invoke-ValeriaNativeCommand -FilePath 'pnputil.exe' -ArgumentList @(
                '/export-driver', $originalInf, $exportDirectory
            ) | Out-Null

            $exportedInf = Get-ChildItem -LiteralPath $exportDirectory -Filter '*.inf' -Recurse -File |
                Select-Object -First 1
            if (-not $exportedInf) {
                throw "pnputil exported $originalInf but no INF was found in $exportDirectory."
            }
            $exportedPackages[$originalInf] = $exportedInf.FullName
        }
        $exportedInfRelativePath = Get-ValeriaRelativePath `
            -BasePath $runDirectory `
            -TargetPath ([string] $exportedPackages[$originalInf])
    }

    $deviceBackups.Add([pscustomobject] @{
        InstanceId               = $record.InstanceId
        HardwareId               = 'USB\VID_05AC&PID_12A8&MI_02'
        OriginalDriverInf        = $originalInf
        OriginalDriverProvider   = $record.DriverProvider
        OriginalDriverVersion    = $record.DriverVersion
        OriginalService          = $record.Service
        ExportedInfRelativePath  = $exportedInfRelativePath
        ParentInstanceId         = $record.ParentInstanceId
        ParentDriverInf          = $record.ParentDriverInf
        AppleMuxDriverInf        = $record.AppleMuxDriverInf
    })
}

$manifestPath = Join-Path $runDirectory 'manifest.json'
$manifest = [ordered] @{
    SchemaVersion       = 1
    CreatedUtc          = [DateTime]::UtcNow.ToString('o')
    HardwareId          = 'USB\VID_05AC&PID_12A8&MI_02'
    CustomInfSource     = $resolvedInf
    CustomPublishedInf  = $null
    Devices             = $deviceBackups.ToArray()
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "Recovery manifest: $manifestPath"

Invoke-ValeriaNativeCommand -FilePath 'pnputil.exe' -ArgumentList @(
    '/add-driver', $resolvedInf
) | Out-Null

$rebootRequired = Install-ValeriaDriverForHardwareId -InfPath $resolvedInf
Invoke-ValeriaNativeCommand -FilePath 'pnputil.exe' -ArgumentList @('/scan-devices') | Out-Null

$readyBindings = [Collections.Generic.List[object]]::new()
foreach ($record in $records) {
    $bound = Wait-ValeriaUsbRecord -InstanceId $record.InstanceId -Until WinUSB -TimeoutSeconds 20
    if (-not $bound) {
        throw "MI_02 did not bind to WinUSB: $($record.InstanceId). Restore with manifest $manifestPath"
    }
    if (-not $bound.AppleParentPreserved -or -not $bound.AppleMuxPreserved) {
        throw "The Apple parent or MI_01 changed unexpectedly. Restore with manifest $manifestPath"
    }
    $readyBindings.Add($bound)
}

$manifest.CustomPublishedInf = [string] $readyBindings[0].DriverInf
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host 'MI_02 now uses Microsoft winusb.sys. Apple USBCCGP/AppleLowerFilter and MI_01 remain installed.'
Write-Warning 'USB tethering on MI_02 is unavailable while this binding is installed.'
if ($rebootRequired) {
    Write-Warning 'Windows reported that a reboot is required.'
}

$readyBindings.ToArray()
