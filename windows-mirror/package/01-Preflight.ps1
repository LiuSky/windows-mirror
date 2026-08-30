[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$activationExe = Join-Path $PSScriptRoot 'bin\valeria-activate.exe'
$testStackScript = Join-Path $PSScriptRoot 'scripts\Test-AppleUsbStack.ps1'
$activateScript = Join-Path $PSScriptRoot 'scripts\Activate-ValeriaUsbMode.ps1'

foreach ($requiredPath in @($activationExe, $testStackScript, $activateScript)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "The package is incomplete. Missing: $requiredPath"
    }
}

Write-Host '=== ValeriaMirror preflight: Apple driver stack ==='
& $testStackScript

Write-Host ''
Write-Host '=== ValeriaMirror preflight: Apple mode 2 / MI_02 ==='
& $activateScript -ActivationExe $activationExe

Write-Host ''
Write-Host '[PASS] Apple parent/MI_01 are preserved and mode 2 activation completed.'
Write-Host '[NEXT] Run 02-Prepare-MI02.cmd as Administrator. Change only the printed MI_02 node.'
