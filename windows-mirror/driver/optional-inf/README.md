# Optional signed-INF fallback

Do not use this folder when Device Manager offers Microsoft's inbox **WinUSB
Device** entry. The default, certificate-free procedure is documented in
[`../README.md`](../README.md).

This fallback is for managed deployment or a Windows image that cannot expose
the manual inbox choice. `ValeriaWinUSB.inf` matches only
`USB\VID_05AC&PID_12A8&MI_02` and includes `winusb.inf`; it ships no `.sys` and
does not touch Apple's parent or MI_01. A signed catalog is still mandatory
because this is a new driver package.

Development-only example:

```powershell
..\..\scripts\Activate-ValeriaUsbMode.ps1
$password = Read-Host 'PFX password' -AsSecureString
.\New-ValeriaTestCertificate.ps1 -Password $password
.\Build-ValeriaWinUsbPackage.ps1 `
  -PfxPath .\test-signing\ValeriaMirror-Test.pfx `
  -PfxPassword $password `
  -NoTimestamp
.\Install-OptionalValeriaInf.ps1 `
  -TrustTestCertificate `
  -TestCertificatePath .\test-signing\ValeriaMirror-Test.cer
```

Test trust/root installation and Windows test mode are security-sensitive and
must not be used for production. Production deployment needs a
Microsoft-accepted catalog signature. Restore from the generated manifest with
`windows-mirror\scripts\Restore-AppleMi02.ps1`.
