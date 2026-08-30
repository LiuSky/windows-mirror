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

& $activateScript -ActivationExe $activationExe
& $restoreScript

Write-Host ''
Write-Host '[NEXT] Unplug the iPhone and reconnect it to the SAME physical USB port.'
