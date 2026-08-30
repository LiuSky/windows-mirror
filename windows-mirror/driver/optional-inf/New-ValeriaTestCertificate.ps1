[CmdletBinding()]
param(
    [string] $OutputDirectory = (Join-Path $PSScriptRoot 'test-signing'),
    [Security.SecureString] $Password,
    [ValidateRange(1, 5)] [int] $ValidityYears = 2
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($env:OS -ne 'Windows_NT') {
    throw 'Certificate creation requires Windows.'
}
if (-not $Password) {
    $Password = Read-Host 'Password for the test-signing PFX' -AsSecureString
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$certificate = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject 'CN=Valeria Mirror Test Signing Only' `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -HashAlgorithm SHA256 `
    -KeyAlgorithm RSA `
    -KeyLength 3072 `
    -KeyExportPolicy Exportable `
    -NotAfter (Get-Date).AddYears($ValidityYears)

$pfxPath = Join-Path $OutputDirectory 'ValeriaMirror-Test.pfx'
$cerPath = Join-Path $OutputDirectory 'ValeriaMirror-Test.cer'
Export-PfxCertificate -Cert $certificate -FilePath $pfxPath -Password $Password | Out-Null
Export-Certificate -Cert $certificate -FilePath $cerPath -Type CERT | Out-Null

[pscustomobject] @{
    Thumbprint = $certificate.Thumbprint
    PfxPath    = $pfxPath
    CerPath    = $cerPath
    Warning    = 'Test only. A production package needs a Microsoft-signed catalog.'
}
