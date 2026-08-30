Set-StrictMode -Version Latest

function Assert-ValeriaWindows {
    if ($env:OS -ne 'Windows_NT') {
        throw 'This script must run on Windows.'
    }
    if (-not (Get-Command Get-PnpDevice -ErrorAction SilentlyContinue)) {
        throw 'The Windows PnpDevice PowerShell module is required.'
    }
}

function Assert-ValeriaAdministrator {
    Assert-ValeriaWindows
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script from an elevated PowerShell session.'
    }
}

function Get-ValeriaDeviceProperty {
    param(
        [Parameter(Mandatory)] [string] $InstanceId,
        [Parameter(Mandatory)] [string] $KeyName
    )

    try {
        return (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName $KeyName -ErrorAction Stop).Data
    }
    catch {
        return $null
    }
}

function Get-ValeriaEnumRegistryValue {
    param(
        [Parameter(Mandatory)] [string] $InstanceId,
        [Parameter(Mandatory)] [string] $ValueName
    )

    $basePath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\$InstanceId"
    foreach ($path in @($basePath, (Join-Path $basePath 'Device Parameters'))) {
        try {
            $item = Get-ItemProperty -LiteralPath $path -Name $ValueName -ErrorAction Stop
            return $item.$ValueName
        }
        catch {
            # Depending on the Windows release, DDInstall.HW values can be
            # exposed at either hardware-key location. Keep probing.
        }
    }
    return $null
}

function Get-ValeriaDeviceParameterValue {
    param(
        [Parameter(Mandatory)] [string] $InstanceId,
        [Parameter(Mandatory)] [string] $ValueName
    )

    $path = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\$InstanceId\Device Parameters"
    try {
        return (Get-ItemProperty -LiteralPath $path -Name $ValueName -ErrorAction Stop).$ValueName
    }
    catch {
        return $null
    }
}

function Get-ValeriaAppleParentDevices {
    param([string] $DeviceSelector)

    Assert-ValeriaWindows
    $devices = @(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object {
        $_.InstanceId -match '^USB\\VID_05AC&PID_12A8\\'
    })
    if ($DeviceSelector) {
        $devices = @($devices | Where-Object {
            $_.InstanceId -imatch [regex]::Escape($DeviceSelector)
        })
    }
    return $devices
}

function Get-ValeriaAppleStackRecord {
    param([Parameter(Mandatory)] $ParentDevice)

    $parentId = [string] $ParentDevice.InstanceId
    $parentService = [string] (Get-ValeriaDeviceProperty -InstanceId $parentId -KeyName 'DEVPKEY_Device_Service')
    $parentInf = [string] (Get-ValeriaDeviceProperty -InstanceId $parentId -KeyName 'DEVPKEY_Device_DriverInfPath')
    $parentProvider = [string] (Get-ValeriaDeviceProperty -InstanceId $parentId -KeyName 'DEVPKEY_Device_DriverProvider')
    $parentLowerFilters = @(
        Get-ValeriaDeviceProperty -InstanceId $parentId -KeyName 'DEVPKEY_Device_LowerFilters'
    ) | Where-Object { $_ }
    $originalConfigurationValue = Get-ValeriaEnumRegistryValue `
        -InstanceId $parentId -ValueName 'OriginalConfigurationValue'
    $usbccgpCapabilities = Get-ValeriaEnumRegistryValue `
        -InstanceId $parentId -ValueName 'UsbccgpCapabilities'

    $mi01 = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object {
        $_.InstanceId -match '^USB\\VID_05AC&PID_12A8&MI_01\\' -and
        ([string] (Get-ValeriaDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_Parent') -ieq $parentId)
    } | Select-Object -First 1)
    $mi01Service = $null
    $mi01Provider = $null
    $mi01Inf = $null
    $mi01UpperFilters = @()
    if ($mi01.Count -eq 1) {
        $mi01Service = Get-ValeriaDeviceProperty -InstanceId $mi01[0].InstanceId -KeyName 'DEVPKEY_Device_Service'
        $mi01Provider = Get-ValeriaDeviceProperty -InstanceId $mi01[0].InstanceId -KeyName 'DEVPKEY_Device_DriverProvider'
        $mi01Inf = Get-ValeriaDeviceProperty -InstanceId $mi01[0].InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath'
        $mi01UpperFilters = @(
            Get-ValeriaDeviceProperty -InstanceId $mi01[0].InstanceId -KeyName 'DEVPKEY_Device_UpperFilters'
        ) | Where-Object { $_ }
    }

    $parentHealthy = ($parentService -ieq 'usbccgp') -and
        (@($parentLowerFilters | Where-Object { $_ -ieq 'AppleLowerFilter' }).Count -gt 0) -and
        ($parentProvider -imatch '^Apple(?:, Inc\.)?$')
    $muxHealthy = ($mi01.Count -eq 1) -and
        ([string] $mi01Service -ieq 'WinUSB') -and
        ([string] $mi01Provider -imatch '^Apple(?:, Inc\.)?$') -and
        (@($mi01UpperFilters | Where-Object { $_ -ieq 'WUDFRd' }).Count -gt 0) -and
        (@($mi01UpperFilters | Where-Object { $_ -ieq 'AppleKmdfFilter' }).Count -gt 0)

    [pscustomobject] @{
        ParentInstanceId          = $parentId
        ParentPnpStatus           = [string] $ParentDevice.Status
        ParentFriendlyName        = [string] $ParentDevice.FriendlyName
        ParentService             = $parentService
        ParentDriverInf           = $parentInf
        ParentDriverProvider      = $parentProvider
        ParentLowerFilters        = $parentLowerFilters
        OriginalConfigurationValue = $originalConfigurationValue
        UsbccgpCapabilities       = $usbccgpCapabilities
        AppleParentPreserved      = $parentHealthy
        AppleMuxInstanceId        = if ($mi01.Count -eq 1) { [string] $mi01[0].InstanceId } else { $null }
        AppleMuxService           = [string] $mi01Service
        AppleMuxDriverInf         = [string] $mi01Inf
        AppleMuxDriverProvider    = [string] $mi01Provider
        AppleMuxUpperFilters      = $mi01UpperFilters
        AppleMuxPreserved         = $muxHealthy
        AppleStackHealthy         = $parentHealthy -and $muxHealthy -and ([string] $ParentDevice.Status -ieq 'OK')
    }
}

function Get-ValeriaAppleStackRecords {
    param([string] $DeviceSelector)

    return @(Get-ValeriaAppleParentDevices -DeviceSelector $DeviceSelector | ForEach-Object {
        Get-ValeriaAppleStackRecord -ParentDevice $_
    })
}

function Test-ValeriaDeviceInterfaceRegistered {
    param(
        [Parameter(Mandatory)] [string] $InstanceId,
        [string] $InterfaceGuid = '{77E935B1-B768-4316-A466-4E745CFDDB24}'
    )

    $classPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\DeviceClasses\$InterfaceGuid"
    $encodedInstanceId = $InstanceId.Replace('\', '#')
    try {
        return @(Get-ChildItem -LiteralPath $classPath -ErrorAction Stop | Where-Object {
            $_.PSChildName -imatch [regex]::Escape($encodedInstanceId)
        }).Count -gt 0
    }
    catch {
        return $false
    }
}

function Get-ValeriaMi02Devices {
    param([string] $InstanceId)

    Assert-ValeriaWindows
    $devices = @(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object {
        $_.InstanceId -match '^USB\\VID_05AC&PID_12A8&MI_02\\'
    })

    if ($InstanceId) {
        $devices = @($devices | Where-Object { $_.InstanceId -ieq $InstanceId })
    }
    return $devices
}

function Get-ValeriaUsbRecord {
    param([Parameter(Mandatory)] $Device)

    $instanceId = [string] $Device.InstanceId
    $compatibleIds = @(
        Get-ValeriaDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_CompatibleIds'
    ) | Where-Object { $_ }
    $hardwareIds = @(
        Get-ValeriaDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_HardwareIds'
    ) | Where-Object { $_ }

    $parentId = [string] (Get-ValeriaDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_Parent')
    $parentService = $null
    $parentInf = $null
    $parentProvider = $null
    $parentLowerFilters = @()
    $originalConfigurationValue = $null
    $usbccgpCapabilities = $null
    if ($parentId) {
        $parentService = Get-ValeriaDeviceProperty -InstanceId $parentId -KeyName 'DEVPKEY_Device_Service'
        $parentInf = Get-ValeriaDeviceProperty -InstanceId $parentId -KeyName 'DEVPKEY_Device_DriverInfPath'
        $parentProvider = Get-ValeriaDeviceProperty -InstanceId $parentId -KeyName 'DEVPKEY_Device_DriverProvider'
        $parentLowerFilters = @(
            Get-ValeriaDeviceProperty -InstanceId $parentId -KeyName 'DEVPKEY_Device_LowerFilters'
        ) | Where-Object { $_ }
        $originalConfigurationValue = Get-ValeriaEnumRegistryValue `
            -InstanceId $parentId -ValueName 'OriginalConfigurationValue'
        $usbccgpCapabilities = Get-ValeriaEnumRegistryValue `
            -InstanceId $parentId -ValueName 'UsbccgpCapabilities'
    }

    $mi01 = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object {
        $_.InstanceId -match '^USB\\VID_05AC&PID_12A8&MI_01\\' -and
        ([string] (Get-ValeriaDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_Parent') -ieq $parentId)
    } | Select-Object -First 1)
    $mi01Service = $null
    $mi01Provider = $null
    $mi01Inf = $null
    $mi01UpperFilters = @()
    if ($mi01.Count -eq 1) {
        $mi01Service = Get-ValeriaDeviceProperty -InstanceId $mi01[0].InstanceId -KeyName 'DEVPKEY_Device_Service'
        $mi01Provider = Get-ValeriaDeviceProperty -InstanceId $mi01[0].InstanceId -KeyName 'DEVPKEY_Device_DriverProvider'
        $mi01Inf = Get-ValeriaDeviceProperty -InstanceId $mi01[0].InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath'
        $mi01UpperFilters = @(
            Get-ValeriaDeviceProperty -InstanceId $mi01[0].InstanceId -KeyName 'DEVPKEY_Device_UpperFilters'
        ) | Where-Object { $_ }
    }

    $service = [string] (Get-ValeriaDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_Service')
    $driverInf = [string] (Get-ValeriaDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_DriverInfPath')
    $driverProvider = [string] (Get-ValeriaDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_DriverProvider')
    $isValeria = @($compatibleIds | Where-Object {
        $_ -ieq 'USB\Class_FF&SubClass_2A&Prot_FF'
    }).Count -gt 0
    $isNcm = @($compatibleIds | Where-Object {
        $_ -imatch '^USB\\Class_02&SubClass_0D'
    }).Count -gt 0
    $isWinUsb = $service -ieq 'WinUSB'
    $isMicrosoftInboxWinUsb = $isWinUsb -and ($driverInf -ieq 'winusb.inf') -and
        ($driverProvider -imatch '^Microsoft')
    $configuredInterfaceGuids = @(
        Get-ValeriaDeviceParameterValue -InstanceId $instanceId -ValueName 'DeviceInterfaceGUIDs'
    ) | Where-Object { $_ }
    $interfaceGuidConfigured = @($configuredInterfaceGuids | Where-Object {
        $_ -ieq '{77E935B1-B768-4316-A466-4E745CFDDB24}'
    }).Count -gt 0
    $interfaceRegistered = Test-ValeriaDeviceInterfaceRegistered -InstanceId $instanceId
    $hasAppleLowerFilter = @($parentLowerFilters | Where-Object {
        $_ -ieq 'AppleLowerFilter'
    }).Count -gt 0
    $appleParent = ([string] $parentService -ieq 'usbccgp') -and
        $hasAppleLowerFilter -and
        ([string] $parentProvider -imatch '^Apple(?:, Inc\.)?$')
    $hasAppleKmdfFilter = @($mi01UpperFilters | Where-Object {
        $_ -ieq 'AppleKmdfFilter'
    }).Count -gt 0
    $hasWudfRd = @($mi01UpperFilters | Where-Object {
        $_ -ieq 'WUDFRd'
    }).Count -gt 0
    $appleMux = ($mi01.Count -eq 1) -and
        ([string] $mi01Service -ieq 'WinUSB') -and
        ([string] $mi01Provider -imatch '^Apple(?:, Inc\.)?$') -and
        $hasAppleKmdfFilter -and $hasWudfRd

    $profile = if ($isValeria) {
        'ValeriaMirror'
    }
    elseif ($isNcm) {
        'NcmIdle'
    }
    else {
        'Unknown'
    }

    $blockingReasons = [Collections.Generic.List[string]]::new()
    if (-not $appleParent) {
        $blockingReasons.Add('The parent is not the supported Apple USBCCGP + AppleLowerFilter stack.')
    }
    if (-not $appleMux) {
        $blockingReasons.Add('Apple MI_01 usbmux WinUSB function is not present and healthy.')
    }
    if (-not $isWinUsb) {
        $blockingReasons.Add("MI_02 service is '$service', not WinUSB.")
    }
    if (-not $interfaceGuidConfigured) {
        $blockingReasons.Add('MI_02 Device Parameters does not contain the Valeria device-interface GUID.')
    }
    if (-not $interfaceRegistered) {
        $blockingReasons.Add('The Valeria WinUSB device interface is not registered; reconnect to the same USB port.')
    }
    if (-not $isValeria) {
        $blockingReasons.Add('MI_02 does not advertise USB class FF/subclass 2A/protocol FF.')
    }
    if ([string] $Device.Status -ine 'OK') {
        $blockingReasons.Add("MI_02 PnP status is '$($Device.Status)'.")
    }

    [pscustomobject] @{
        InstanceId             = $instanceId
        PnpStatus              = [string] $Device.Status
        FriendlyName           = [string] $Device.FriendlyName
        HardwareIds            = $hardwareIds
        CompatibleIds          = $compatibleIds
        UsbProfile             = $profile
        Service                = $service
        DriverInf              = $driverInf
        DriverProvider         = $driverProvider
        DriverVersion          = [string] (Get-ValeriaDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_DriverVersion')
        ParentInstanceId       = $parentId
        ParentService          = [string] $parentService
        ParentDriverInf        = [string] $parentInf
        ParentDriverProvider   = [string] $parentProvider
        ParentLowerFilters     = $parentLowerFilters
        OriginalConfigurationValue = $originalConfigurationValue
        UsbccgpCapabilities    = $usbccgpCapabilities
        AppleParentPreserved   = $appleParent
        AppleMuxInstanceId     = if ($mi01.Count -eq 1) { [string] $mi01[0].InstanceId } else { $null }
        AppleMuxService        = [string] $mi01Service
        AppleMuxDriverInf      = [string] $mi01Inf
        AppleMuxDriverProvider = [string] $mi01Provider
        AppleMuxUpperFilters   = $mi01UpperFilters
        AppleMuxPreserved      = $appleMux
        IsWinUsb               = $isWinUsb
        IsMicrosoftInboxWinUsb = $isMicrosoftInboxWinUsb
        ConfiguredInterfaceGuids = $configuredInterfaceGuids
        DeviceInterfaceGuidConfigured = $interfaceGuidConfigured
        WinUsbInterfaceRegistered = $interfaceRegistered
        IsValeriaInterface     = $isValeria
        ReadyForRealtimeMirror = $appleParent -and $appleMux -and $isWinUsb -and
            $interfaceGuidConfigured -and $interfaceRegistered -and $isValeria -and
            ([string] $Device.Status -ieq 'OK')
        BlockingReasons        = $blockingReasons.ToArray()
    }
}

function Get-ValeriaUsbRecords {
    param([string] $InstanceId)

    return @(Get-ValeriaMi02Devices -InstanceId $InstanceId | ForEach-Object {
        Get-ValeriaUsbRecord -Device $_
    })
}

function Wait-ValeriaUsbRecord {
    param(
        [string] $InstanceId,
        [ValidateSet('Present', 'WinUSB', 'Ready')] [string] $Until = 'Ready',
        [ValidateRange(1, 300)] [int] $TimeoutSeconds = 30
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $records = @(Get-ValeriaUsbRecords -InstanceId $InstanceId)
        foreach ($record in $records) {
            $matched = switch ($Until) {
                'Present' { $true }
                'WinUSB'  { $record.IsWinUsb }
                'Ready'   { $record.ReadyForRealtimeMirror }
            }
            if ($matched) {
                return $record
            }
        }
        Start-Sleep -Milliseconds 400
    } while ([DateTime]::UtcNow -lt $deadline)

    return $null
}

function Invoke-ValeriaNativeCommand {
    param(
        [Parameter(Mandatory)] [string] $FilePath,
        [Parameter(Mandatory)] [string[]] $ArgumentList
    )

    $lines = @(& $FilePath @ArgumentList 2>&1 | ForEach-Object { [string] $_ })
    $exitCode = $LASTEXITCODE
    if ($lines.Count -gt 0) {
        $lines | ForEach-Object { Write-Host $_ }
    }
    if ($exitCode -ne 0) {
        throw "$FilePath failed with exit code $exitCode."
    }
    return $lines
}

function Get-ValeriaRelativePath {
    param(
        [Parameter(Mandatory)] [string] $BasePath,
        [Parameter(Mandatory)] [string] $TargetPath
    )

    $baseFullPath = [IO.Path]::GetFullPath($BasePath).TrimEnd('\') + '\'
    $targetFullPath = [IO.Path]::GetFullPath($TargetPath)
    $baseUri = [Uri]::new($baseFullPath)
    $targetUri = [Uri]::new($targetFullPath)
    return [Uri]::UnescapeDataString($baseUri.MakeRelativeUri($targetUri).ToString()).Replace('/', '\')
}

function Resolve-ValeriaActivationExecutable {
    param([string] $ActivationExe)

    if ($ActivationExe) {
        return (Resolve-Path -LiteralPath $ActivationExe -ErrorAction Stop).Path
    }

    $mirrorRoot = Join-Path $PSScriptRoot '..'
    $activationRoot = Join-Path $mirrorRoot 'activation'
    $candidates = @(
        (Join-Path $mirrorRoot 'build\activation\Release\valeria-activate.exe'),
        (Join-Path $mirrorRoot 'build\activation\RelWithDebInfo\valeria-activate.exe'),
        (Join-Path $mirrorRoot 'build\activation\valeria-activate.exe'),
        (Join-Path $activationRoot 'build\Release\valeria-activate.exe'),
        (Join-Path $activationRoot 'build\RelWithDebInfo\valeria-activate.exe'),
        (Join-Path $activationRoot 'build\valeria-activate.exe')
    )
    $found = @($candidates | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    } | Select-Object -First 1)
    if ($found.Count -ne 1) {
        throw 'valeria-activate.exe was not found. Build windows-mirror or windows-mirror\activation, or pass -ActivationExe.'
    }
    return (Resolve-Path -LiteralPath $found[0]).Path
}

function Invoke-ValeriaMode2Activation {
    param(
        [Parameter(Mandatory)] [string] $ActivationExe,
        [string] $DeviceSelector,
        [ValidateRange(5, 60)] [int] $TimeoutSeconds = 45
    )

    $arguments = [Collections.Generic.List[string]]::new()
    $arguments.Add('--enable')
    if ($DeviceSelector) {
        $arguments.Add('--device')
        $arguments.Add($DeviceSelector)
    }
    $arguments.Add('--wait-ms')
    $arguments.Add([string] ($TimeoutSeconds * 1000))

    $output = @(Invoke-ValeriaNativeCommand `
        -FilePath $ActivationExe `
        -ArgumentList $arguments.ToArray())
    if (@($output | Where-Object {
            $_ -match 'RESULT state=VALERIA_(?:ALREADY_)?ACTIVE exit=0'
        }).Count -eq 0) {
        throw 'Activation returned exit 0 without a verified VALERIA_ACTIVE result.'
    }
    return $output
}

function Install-ValeriaDriverForHardwareId {
    param(
        [Parameter(Mandatory)] [string] $InfPath,
        [string] $HardwareId = 'USB\VID_05AC&PID_12A8&MI_02'
    )

    $nativeType = 'ValeriaMirror.NativeMethods' -as [type]
    if (-not $nativeType) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace ValeriaMirror {
    public static class NativeMethods {
        [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool UpdateDriverForPlugAndPlayDevices(
            IntPtr hwndParent,
            string hardwareId,
            string fullInfPath,
            uint installFlags,
            out bool rebootRequired);

        public static bool ForceDriver(string hardwareId, string fullInfPath) {
            const uint INSTALLFLAG_FORCE = 0x00000001;
            const uint INSTALLFLAG_NONINTERACTIVE = 0x00000004;
            bool rebootRequired;
            if (!UpdateDriverForPlugAndPlayDevices(
                    IntPtr.Zero,
                    hardwareId,
                    fullInfPath,
                    INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE,
                    out rebootRequired)) {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            return rebootRequired;
        }
    }
}
'@
    }

    $resolvedInf = (Resolve-Path -LiteralPath $InfPath -ErrorAction Stop).Path
    return [ValeriaMirror.NativeMethods]::ForceDriver($HardwareId, $resolvedInf)
}
