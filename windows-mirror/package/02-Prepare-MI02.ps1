[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$prepareScript = Join-Path $PSScriptRoot 'scripts\Prepare-InboxWinUsbMi02.ps1'
$installScript = Join-Path $PSScriptRoot 'scripts\Install-ValeriaWinUsb.ps1'
foreach ($requiredPath in @($prepareScript, $installScript)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "The package is incomplete. Missing: $requiredPath"
    }
}

Write-Host 'This step backs up the current MI_02 binding before opening Device Manager.'
$preparation = & $prepareScript -OpenDeviceManager
$manifest = Get-Content -LiteralPath $preparation.ManifestPath -Raw | ConvertFrom-Json
$recoveryPointer = Join-Path $env:ProgramData 'ValeriaMirror\PortableTesterRecoveryManifest.txt'
$capturedProperty = $manifest.PSObject.Properties['OriginalBindingCapturedBeforeManualSelection']
if ($capturedProperty -and
    $capturedProperty.Value -is [bool] -and
    $capturedProperty.Value -eq $true) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $recoveryPointer) -Force | Out-Null
    [IO.File]::WriteAllText(
        $recoveryPointer,
        $preparation.ManifestPath,
        [Text.Encoding]::UTF8
    )
    Write-Host "Saved the pre-WinUSB recovery manifest: $recoveryPointer"
}
else {
    Write-Warning 'MI_02 was already on WinUSB, so the earlier recovery pointer was not overwritten.'
}

Write-Host ''
Write-Warning 'In Device Manager, change ONLY the exact MI_02 instance printed above.'
Write-Warning 'Never change the PID_12A8 parent or MI_01.'
$confirmation = Read-Host "After Microsoft 'WinUSB Device' is selected, type MI02 to continue"
if ($confirmation -cne 'MI02') {
    throw 'Confirmation was not MI02. No GUID change was made.'
}

& $installScript -ManifestPath $preparation.ManifestPath -Confirm:$false

Write-Host ''
Write-Host '[NEXT] Unplug the iPhone and reconnect it to the SAME physical USB port.'
Write-Host '[NEXT] Close Apple Devices, then run 03-Start-Mirror.cmd.'
