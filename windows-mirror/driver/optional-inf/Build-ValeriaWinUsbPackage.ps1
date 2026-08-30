[CmdletBinding(DefaultParameterSetName = 'Thumbprint')]
param(
    [Parameter(ParameterSetName = 'Thumbprint', Mandatory)]
    [ValidatePattern('^[0-9A-Fa-f ]{40}$')]
    [string] $CertificateThumbprint,

    [Parameter(ParameterSetName = 'Pfx', Mandatory)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $PfxPath,

    [Parameter(ParameterSetName = 'Pfx', Mandatory)]
    [Security.SecureString] $PfxPassword,

    [string] $Inf2CatPath,
    [string] $SignToolPath,
    [string] $OperatingSystems = '10_X64,10_ARM64',
    [string] $TimestampUrl = 'http://timestamp.digicert.com',
    [switch] $NoTimestamp
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($env:OS -ne 'Windows_NT') {
    throw 'Driver packaging requires Windows and the Windows Driver Kit.'
}

$driverDirectory = $PSScriptRoot
$infPath = Join-Path $driverDirectory 'ValeriaWinUSB.inf'
$catalogPath = Join-Path $driverDirectory 'ValeriaWinUSB.cat'
if (-not (Test-Path -LiteralPath $infPath -PathType Leaf)) {
    throw "Missing $infPath"
}

function Find-WdkTool {
    param(
        [Parameter(Mandatory)] [string] $Name,
        [string] $ExplicitPath
    )

    if ($ExplicitPath) {
        return (Resolve-Path -LiteralPath $ExplicitPath -ErrorAction Stop).Path
    }

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $kitsRoot = ${env:ProgramFiles(x86)}
    if ($kitsRoot) {
        $binRoot = Join-Path $kitsRoot 'Windows Kits\10\bin'
        $candidate = Get-ChildItem -LiteralPath $binRoot -Filter $Name -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.DirectoryName -match '\\x64$' } |
            Sort-Object { [version] ($_.Directory.Parent.Name -replace '[^0-9.]', '') } -Descending |
            Select-Object -First 1
        if ($candidate) {
            return $candidate.FullName
        }
    }

    throw "$Name was not found. Install the Windows Driver Kit or pass its full path."
}

$inf2cat = Find-WdkTool -Name 'Inf2Cat.exe' -ExplicitPath $Inf2CatPath
$signtool = Find-WdkTool -Name 'SignTool.exe' -ExplicitPath $SignToolPath

Write-Host 'Validating INF and generating catalog...'
& $inf2cat "/driver:$driverDirectory" "/os:$OperatingSystems" /verbose
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $catalogPath -PathType Leaf)) {
    throw "Inf2Cat failed with exit code $LASTEXITCODE."
}

$signArguments = [Collections.Generic.List[string]]::new()
$signArguments.Add('sign')
$signArguments.Add('/fd')
$signArguments.Add('SHA256')

if ($PSCmdlet.ParameterSetName -eq 'Thumbprint') {
    $signArguments.Add('/sha1')
    $signArguments.Add(($CertificateThumbprint -replace ' ', ''))
}
else {
    $credential = [pscredential]::new('pfx', $PfxPassword)
    $plainPassword = $credential.GetNetworkCredential().Password
    $signArguments.Add('/f')
    $signArguments.Add((Resolve-Path -LiteralPath $PfxPath).Path)
    $signArguments.Add('/p')
    $signArguments.Add($plainPassword)
}

if (-not $NoTimestamp) {
    $signArguments.Add('/tr')
    $signArguments.Add($TimestampUrl)
    $signArguments.Add('/td')
    $signArguments.Add('SHA256')
}
$signArguments.Add($catalogPath)

Write-Host 'Signing catalog...'
& $signtool @signArguments
if ($LASTEXITCODE -ne 0) {
    throw "SignTool failed with exit code $LASTEXITCODE."
}

Write-Host 'Verifying catalog signature...'
& $signtool verify /pa /v $catalogPath
if ($LASTEXITCODE -ne 0) {
    throw "Catalog verification failed with exit code $LASTEXITCODE."
}

Write-Host "Package ready: $driverDirectory"
Write-Host 'It contains no custom .sys; the INF loads the Microsoft inbox winusb.sys.'
