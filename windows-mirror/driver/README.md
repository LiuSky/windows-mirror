# MI_02: Microsoft inbox WinUSB, no custom driver package

The default path installs no custom INF, certificate, `.sys`, libusb0, or
libusb filter. It keeps Apple's current USB stack and manually assigns only the
Valeria/NCM `MI_02` child to Microsoft's in-box **WinUSB Device** driver.

This path is documented by Microsoft in
[WinUSB installation for developers](https://learn.microsoft.com/windows-hardware/drivers/usbcon/winusb-installation):
Device Manager can load the system-provided WinUSB Device driver, after which
an application-specific `DeviceInterfaceGUIDs` value is added under the
device's `Device Parameters` key and the device is reconnected to the same
physical port.

## Why the Apple and Microsoft drivers can coexist

Apple's current `AppleUsb.inf` creates separate PnP nodes:

| Node | Binding after setup |
|---|---|
| `USB\VID_05AC&PID_12A8` | Apple package: `usbccgp` + `AppleLowerFilter` |
| `...&MI_01` | Apple package: WinUSB + `WUDFRd` + `AppleKmdfFilter`/UMDF |
| `...&MI_02` | Microsoft package: in-box `winusb.inf` / `winusb.sys` |

The Apple parent install section includes `usb.inf` / `Composite.Dev.NT`, has
an INF default `OriginalConfigurationValue=2`, sets
`UsbccgpCapabilities=0x10`, and adds `AppleLowerFilter`. Apple's MI_01 section
uses the lower GUID `{664be590-54bd-4964-8a8c-6cd1314f6dc2}` while its official
UMDF component dynamically publishes the application-facing MUX1 GUID
`{f0b32be3-6678-4879-9230-e43845d805ee}`. The activator opens MUX1 and uses
AppleUsbFilter's control IOCTL; it does not call `WinUsb_Initialize` on the
lower MI_01 path. The scripts verify the signed Apple bindings and never
replace or edit them. A healthy live stack remains authoritative if its
runtime `OriginalConfigurationValue` differs from the INF default. In
particular, AppleLowerFilter can write `4` when it selects the fifth descriptor
(`configuration 5`); this is the expected zero-based descriptor index and must
not be rewritten to `5`. The legacy
`usbaapl64.sys` parent is not supported by this route; install/repair the
current Apple Devices package.

Apple's INF has no MI_02 Valeria bulk-access binding. An Apple-only setup can
activate the hidden configuration through MI_01, but it cannot open MI_02's
bulk pipes. The minimal working composition is therefore Apple parent + Apple
MI_01 + Microsoft inbox WinUSB on MI_02.

MI_02 uses the same hardware ID in two configurations:

- ordinary mode: USB NCM/network interface;
- mirror mode: Valeria `USB\Class_FF&SubClass_2A&Prot_FF`.

Selecting WinUSB for that child makes it persist across re-enumeration, but USB
tethering on MI_02 is unavailable until the original network driver is
restored. MI_01/usbmux remains intact.

## Default setup

Run elevated PowerShell from `windows-mirror` with exactly one iPhone attached:

```powershell
.\scripts\Test-AppleUsbStack.ps1
.\scripts\Activate-ValeriaUsbMode.ps1
.\scripts\Prepare-InboxWinUsbMi02.ps1 -OpenDeviceManager
```

Initial Apple mode 1 can expose only four configurations and no MI_02 PnP
child. `Activate-ValeriaUsbMode.ps1` therefore runs first through official
MI_01; it requests mode 2/configuration 5 and waits until Valeria MI_02 exists.
It does not require or modify an MI_02 driver.

The preparation script then exports an OEM MI_02 package when present and records
the existing binding plus registry state under
`%ProgramData%\ValeriaMirror\DriverBackup`. It prints the exact MI_02 instance
ID. In Device Manager, change only that node:

1. **Update driver** → **Browse my computer for drivers**.
2. **Let me pick from a list of available drivers on my computer**.
3. Select **Universal Serial Bus devices** → **WinUSB Device** (Microsoft).
4. Do not change the PID_12A8 composite parent or MI_01.

If the “Universal Serial Bus devices” class is absent, this Windows image
cannot use Microsoft's manual inbox path; only then consider the optional
signed-INF fallback below.

After Device Manager finishes, run the command printed by the preparation
script, for example:

```powershell
.\scripts\Install-ValeriaWinUsb.ps1 `
  -ManifestPath 'C:\ProgramData\ValeriaMirror\DriverBackup\...\manifest.json'
```

This second script refuses Apple, libusb, Zadig, or project INF bindings. It
continues only when `Service=WinUSB`, `DriverInf=winusb.inf`, and the provider
is Microsoft. Its only mutation is adding
`{77E935B1-B768-4316-A466-4E745CFDDB24}` to the MI_02
`Device Parameters\DeviceInterfaceGUIDs` multi-string.

Unplug the iPhone and reconnect it to the **same physical USB port**, as the
Microsoft instructions require. Changing ports creates another device instance
and requires repeating the manual selection/GUID registration for that port.

Then validate and enter mirror mode:

```powershell
.\scripts\Enable-ValeriaMirrorMode.ps1
.\scripts\Get-ValeriaUsbState.ps1 -RequireReady
```

After the physical reconnect, MI_02 can disappear again with initial mode 1.
`Enable-ValeriaMirrorMode.ps1` intentionally activates through MI_01 before it
looks for MI_02, then waits for the saved inbox WinUSB child to return. Thus the
first-time sequence is `activate → bind/register → reconnect → activate →
ready`; later sessions only need the final enable/ready step.

Ready means all of the following are true:

- parent service is `usbccgp`, with `AppleLowerFilter`;
- Apple MI_01 remains its official filtered WinUSB function;
- MI_02 uses Microsoft's `winusb.inf` and publishes the project GUID;
- mode switching changed MI_02 to `FF/2A/FF`;
- the GUID-backed WinUSB device interface is registered.

## Mode-selection constraint

`valeria-activate.exe --enable` requests Apple **mode 2** over official MI_01.
`AppleLowerFilter` reads mode state using vendor request `0x45`, takes the first
response byte as the preferred configuration, caps it at 5, and selects that
configuration. Mode 2 produces effective configuration 5, where MI_02 is
Valeria. Mode 4/configuration 6 is not used because the official filter clamps
the selection to 5.

No script writes `OriginalConfigurationValue`. Success is accepted only when
the activator returns verified `VALERIA_ACTIVE` and the re-enumerated MI_02
reports `FF/2A/FF` through the Microsoft WinUSB binding.

## Parallels re-enumeration limitation

Mode switching disconnects and re-enumerates the iPhone. On macOS hosts,
Parallels must retain exclusive ownership across that boundary. If macOS
`usbmuxd` or Apple USB-NCM drivers reclaim an interface first, Windows can show
the PID_12A8 parent as Code 10 and no usable MI_02 will exist. A `00` response
from SET_MODE is not success in this state.

Do not bind an NCM `02/0D/00` child to WinUSB and do not change
`OriginalConfigurationValue` to work around it. Reassign or physically
reconnect the whole iPhone to the VM and rerun the descriptor gate. If the VM
still cannot retain the device and expose `FF/2A/FF`, use a physical Windows
host for the hardware test; CI and protocol fixtures cannot validate this USB
ownership boundary.

## Restore normal MI_02 networking

Use the preparation manifest, or omit it to choose the newest backup:

```powershell
.\scripts\Restore-AppleMi02.ps1
```

If MI_02 is absent in initial mode 1, run
`Activate-ValeriaUsbMode.ps1` immediately before restore.
The script restores the recorded driver and previous interface-GUID value.
Then unplug/reconnect the phone. It never changes the Apple parent or MI_01.

## Optional signed-INF deployment

`optional-inf/` is not the default route. It exists for managed deployment or
for Windows installations whose Device Manager does not offer **WinUSB
Device**. Its INF still loads only Microsoft's `winusb.sys`; it does not ship a
custom `.sys`. Windows nevertheless requires the INF's catalog to be signed.

For development, the folder includes catalog/test-certificate helpers. Test
signing can require Windows test mode, a restart, and Secure Boot changes.
Production requires a Microsoft-accepted production signature. None of that is
needed for the manual inbox path above.

## Continuous mirror transport requirement

The Windows application enumerates
`{77E935B1-B768-4316-A466-4E745CFDDB24}`, calls `WinUsb_Initialize`, and rejects
the device unless interface 0 is `FF/2A/FF` with bulk IN and bulk OUT pipes.
It uses overlapped `WinUsb_ReadPipe`/`WinUsb_WritePipe`, retains fragmented
frames across reads, splits coalesced frames, and processes continuous
`FEED`/`EAT!` media packets for decode/render. A one-shot screenshot API is not
part of this design or an accepted fallback.
