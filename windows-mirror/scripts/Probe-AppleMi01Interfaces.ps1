[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($env:OS -ne 'Windows_NT') {
    throw 'This diagnostic must run on Windows.'
}

# AppleUsb.inf currently publishes the first GUID for MI_01. The other GUIDs
# can also appear on the same live devnode through Apple's filter/UMDF stack.
# This probe deliberately prints neither device paths nor instance IDs.
$interfaceGuids = @(
    [Guid] '664BE590-54BD-4964-8A8C-6CD1314F6DC2',
    [Guid] 'DEE824EF-729B-4A0E-9C14-B7117D33A817',
    [Guid] 'F0B32BE3-6678-4879-9230-E43845D805EE'
)

$nativeSource = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class AppleInterfaceProbe
{
    private const uint DIGCF_PRESENT = 0x00000002;
    private const uint DIGCF_DEVICEINTERFACE = 0x00000010;
    private const uint GENERIC_READ = 0x80000000;
    private const uint GENERIC_WRITE = 0x40000000;
    private const uint FILE_SHARE_READ = 0x00000001;
    private const uint FILE_SHARE_WRITE = 0x00000002;
    private const uint OPEN_EXISTING = 3;
    private const uint FILE_ATTRIBUTE_NORMAL = 0x00000080;
    private const uint FILE_FLAG_OVERLAPPED = 0x40000000;
    private const int ERROR_NO_MORE_ITEMS = 259;
    private const uint APPLE_IOCTL_CONTROL_TRANSFER = 0x002200A0;
    private static readonly IntPtr InvalidHandleValue = new IntPtr(-1);

    [StructLayout(LayoutKind.Sequential)]
    private struct SP_DEVICE_INTERFACE_DATA
    {
        internal int cbSize;
        internal Guid InterfaceClassGuid;
        internal int Flags;
        internal UIntPtr Reserved;
    }

    public sealed class Result
    {
        public int Candidate { get; set; }
        public bool CreateFileOk { get; set; }
        public int CreateFileError { get; set; }
        public bool WinUsbInitializeOk { get; set; }
        public int WinUsbInitializeError { get; set; }
        public bool GetModeTested { get; set; }
        public bool GetModeOk { get; set; }
        public int GetModeError { get; set; }
        public int GetModeBytes { get; set; }
        public string GetModeData { get; set; }
    }

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern IntPtr SetupDiGetClassDevs(
        ref Guid classGuid, IntPtr enumerator, IntPtr parent, uint flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiEnumDeviceInterfaces(
        IntPtr deviceInfoSet, IntPtr deviceInfoData, ref Guid interfaceClassGuid,
        uint memberIndex, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SetupDiGetDeviceInterfaceDetail(
        IntPtr deviceInfoSet, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData,
        IntPtr deviceInterfaceDetailData, uint deviceInterfaceDetailDataSize,
        out uint requiredSize, IntPtr deviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateFile(
        string fileName, uint desiredAccess, uint shareMode, IntPtr securityAttributes,
        uint creationDisposition, uint flagsAndAttributes, IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(
        IntPtr deviceHandle, uint ioControlCode,
        byte[] inputBuffer, uint inputBufferSize,
        byte[] outputBuffer, uint outputBufferSize,
        out uint bytesReturned, IntPtr overlapped);

    [DllImport("winusb.dll", SetLastError = true)]
    private static extern bool WinUsb_Initialize(IntPtr deviceHandle, out IntPtr interfaceHandle);

    [DllImport("winusb.dll", SetLastError = true)]
    private static extern bool WinUsb_Free(IntPtr interfaceHandle);

    public static Result[] Run(Guid guid)
    {
        var output = new List<Result>();
        IntPtr set = SetupDiGetClassDevs(
            ref guid, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == InvalidHandleValue)
            return output.ToArray();

        try
        {
            for (uint index = 0; ; ++index)
            {
                var data = new SP_DEVICE_INTERFACE_DATA();
                data.cbSize = Marshal.SizeOf(typeof(SP_DEVICE_INTERFACE_DATA));
                if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref guid, index, ref data))
                {
                    if (Marshal.GetLastWin32Error() == ERROR_NO_MORE_ITEMS)
                        break;
                    continue;
                }

                uint required;
                SetupDiGetDeviceInterfaceDetail(
                    set, ref data, IntPtr.Zero, 0, out required, IntPtr.Zero);
                if (required == 0)
                    continue;

                IntPtr detail = Marshal.AllocHGlobal((int)required);
                try
                {
                    Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
                    if (!SetupDiGetDeviceInterfaceDetail(
                            set, ref data, detail, required, out required, IntPtr.Zero))
                        continue;

                    string path = Marshal.PtrToStringUni(IntPtr.Add(detail, 4));
                    var result = new Result { Candidate = checked((int)index + 1) };
                    IntPtr file = CreateFile(
                        path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, IntPtr.Zero);
                    if (file == InvalidHandleValue)
                    {
                        result.CreateFileError = Marshal.GetLastWin32Error();
                        output.Add(result);
                        continue;
                    }

                    result.CreateFileOk = true;
                    try
                    {
                        IntPtr usb;
                        result.WinUsbInitializeOk = WinUsb_Initialize(file, out usb);
                        if (!result.WinUsbInitializeOk)
                            result.WinUsbInitializeError = Marshal.GetLastWin32Error();
                        else
                            WinUsb_Free(usb);
                    }
                    finally
                    {
                        CloseHandle(file);
                    }

                    // The F0... MUX1 reference-string path is Apple's UMDF
                    // user-mode contract. Validate its read-only raw-control
                    // operation without printing the reference string.
                    if (guid == new Guid("F0B32BE3-6678-4879-9230-E43845D805EE"))
                    {
                        result.GetModeTested = true;
                        IntPtr controlFile = CreateFile(
                            path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);
                        if (controlFile == InvalidHandleValue)
                        {
                            result.GetModeError = Marshal.GetLastWin32Error();
                        }
                        else
                        {
                            try
                            {
                                // WINUSB_SETUP_PACKET C0/45/value0/index0/length4.
                                var setup = new byte[] { 0xC0, 0x45, 0, 0, 0, 0, 4, 0 };
                                var response = new byte[12]; // setup echo + four payload bytes
                                uint returned;
                                result.GetModeOk = DeviceIoControl(
                                    controlFile, APPLE_IOCTL_CONTROL_TRANSFER,
                                    setup, (uint)setup.Length,
                                    response, (uint)response.Length,
                                    out returned, IntPtr.Zero);
                                result.GetModeBytes = checked((int)returned);
                                if (!result.GetModeOk)
                                    result.GetModeError = Marshal.GetLastWin32Error();
                                else if (returned >= 12)
                                    result.GetModeData = BitConverter.ToString(response, 8, 4);
                            }
                            finally
                            {
                                CloseHandle(controlFile);
                            }
                        }
                    }
                    output.Add(result);
                }
                finally
                {
                    Marshal.FreeHGlobal(detail);
                }
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
        return output.ToArray();
    }
}
'@

Add-Type -TypeDefinition $nativeSource -Language CSharp

foreach ($guid in $interfaceGuids) {
    $results = @([AppleInterfaceProbe]::Run($guid))
    if ($results.Count -eq 0) {
        [pscustomobject]@{
            Guid                  = $guid.ToString('B').ToUpperInvariant()
            Candidate             = 0
            CreateFile            = 'NOT_ENUMERATED'
            WinUsbInitialize      = 'NOT_TESTED'
            GetMode               = 'NOT_TESTED'
        }
        continue
    }
    foreach ($result in $results) {
        [pscustomobject]@{
            Guid             = $guid.ToString('B').ToUpperInvariant()
            Candidate        = $result.Candidate
            CreateFile       = if ($result.CreateFileOk) { 'OK' } else { "ERROR_$($result.CreateFileError)" }
            WinUsbInitialize = if (-not $result.CreateFileOk) {
                'NOT_TESTED'
            }
            elseif ($result.WinUsbInitializeOk) {
                'OK'
            }
            else {
                "ERROR_$($result.WinUsbInitializeError)"
            }
            GetMode          = if (-not $result.GetModeTested) {
                'NOT_TESTED'
            }
            elseif (-not $result.GetModeOk) {
                "ERROR_$($result.GetModeError)"
            }
            else {
                "OK_BYTES_$($result.GetModeBytes)_DATA_$($result.GetModeData)"
            }
        }
    }
}
