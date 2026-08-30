#ifdef _WIN32

#include <windows.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// AppleUsbFilter (the official Apple UMDF component above MI_01) publishes a
// MUX1 reference-string interface with this GUID. Applications must use its
// IOCTL contract; the lower 664BE590... interface cannot be initialized as a
// WinUSB handle while Apple's UMDF/filter stack is active.
constexpr GUID kAppleMuxInterfaceGuid = {
    0xf0b32be3,
    0x6678,
    0x4879,
    {0x92, 0x30, 0xe4, 0x38, 0x45, 0xd8, 0x05, 0xee},
};

constexpr DWORD kAppleIoctlControlTransfer = 0x002200a0;
constexpr DWORD kControlTransferTimeoutMs = 5000;
constexpr UCHAR kRequestTypeVendorDeviceIn = 0xc0;
constexpr UCHAR kRequestGetMode = 0x45;
constexpr UCHAR kRequestSetMode = 0x52;
constexpr USHORT kValeriaMode = 2;
constexpr UCHAR kRequestTypeStandardDeviceIn = 0x80;
constexpr UCHAR kRequestGetConfiguration = 0x08;
constexpr UCHAR kRequestGetDescriptor = 0x06;
constexpr UCHAR kDescriptorDevice = 0x01;
constexpr UCHAR kDescriptorConfiguration = 0x02;
constexpr UCHAR kDescriptorInterface = 0x04;

enum ExitCode : int {
    kSuccess = 0,
    kAcceptedButUnverified = 2,
    kNoAppleDevice = 10,
    kClassicAmdsOnly = 11,
    kMi01InterfaceMissing = 12,
    kOpenFailed = 13,
    kAmbiguousDevice = 14,
    kControlTransferFailed = 20,
    kReenumerationTimeout = 21,
    kValeriaNotActive = 22,
    kBadArguments = 64,
};

struct Options {
    bool enable = false;
    bool help = false;
    std::wstring selector;
    DWORD wait_ms = 15000;
};

struct DeviceInfo {
    std::wstring path;
    std::wstring instance_id;
    std::wstring description;
    std::wstring service;
    std::wstring hardware_ids;
};

struct AppleUsbNode {
    std::wstring instance_id;
    std::wstring description;
    std::wstring service;
    std::wstring class_name;
    std::wstring hardware_ids;
};

struct TransferResult {
    bool ok = false;
    bool submitted = false;
    DWORD error = ERROR_SUCCESS;
    ULONG transferred = 0;
    std::vector<UCHAR> data;
};

struct InterfaceDescriptor {
    UCHAR number = 0;
    UCHAR alternate = 0;
    UCHAR endpoints = 0;
    UCHAR interface_class = 0;
    UCHAR subclass = 0;
    UCHAR protocol = 0;
};

struct ConfigurationDescriptor {
    UCHAR index = 0;
    UCHAR value = 0;
    UCHAR declared_interfaces = 0;
    USHORT total_length = 0;
    bool complete = false;
    bool has_valeria = false;
    bool has_usbmux = false;
    DWORD error = ERROR_SUCCESS;
    std::vector<InterfaceDescriptor> interfaces;
};

struct DescriptorSnapshot {
    bool device_ok = false;
    DWORD device_error = ERROR_SUCCESS;
    USHORT vendor_id = 0;
    USHORT product_id = 0;
    UCHAR configuration_count = 0;
    std::vector<ConfigurationDescriptor> configurations;
};

struct ProbeSnapshot {
    TransferResult get_mode;
    TransferResult get_configuration;
    DescriptorSnapshot descriptors;
};

struct WaitResult {
    bool saw_disappearance = false;
    bool saw_reappearance = false;
    bool state_changed = false;
    DWORD elapsed_ms = 0;
    DWORD last_open_error = ERROR_SUCCESS;
    std::optional<DeviceInfo> device;
    std::optional<ProbeSnapshot> snapshot;
};

std::string Utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return "<wide-string conversion failed>";
    }
    std::string output(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        value.data(),
                        static_cast<int>(value.size()),
                        output.data(),
                        count,
                        nullptr,
                        nullptr);
    return output;
}

std::wstring Upper(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towupper(ch));
    });
    return value;
}

bool ContainsInsensitive(const std::wstring& value, const std::wstring& needle) {
    return Upper(value).find(Upper(needle)) != std::wstring::npos;
}

std::string WindowsError(DWORD error) {
    if (error == ERROR_SUCCESS) {
        return "success";
    }

    wchar_t* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD count = FormatMessageW(flags,
                                       nullptr,
                                       error,
                                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                       reinterpret_cast<wchar_t*>(&message),
                                       0,
                                       nullptr);
    std::wstring text;
    if (count != 0 && message != nullptr) {
        text.assign(message, count);
        while (!text.empty() &&
               (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
            text.pop_back();
        }
    }
    if (message != nullptr) {
        LocalFree(message);
    }

    std::ostringstream output;
    output << error;
    if (!text.empty()) {
        output << " (" << Utf8(text) << ')';
    }
    return output.str();
}

std::wstring RegistryProperty(HDEVINFO set, SP_DEVINFO_DATA& device, DWORD property) {
    DWORD type = 0;
    DWORD bytes = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &device, property, &type, nullptr, 0, &bytes);
    if (bytes == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
    }

    std::vector<BYTE> buffer(bytes + (2 * sizeof(wchar_t)), 0);
    if (!SetupDiGetDeviceRegistryPropertyW(
            set, &device, property, &type, buffer.data(), bytes, nullptr)) {
        return {};
    }

    const wchar_t* text = reinterpret_cast<const wchar_t*>(buffer.data());
    if (type != REG_MULTI_SZ) {
        return text;
    }

    std::wstring joined;
    for (const wchar_t* item = text; *item != L'\0'; item += std::wcslen(item) + 1) {
        if (!joined.empty()) {
            joined += L" | ";
        }
        joined += item;
    }
    return joined;
}

std::wstring DeviceInstanceId(HDEVINFO set, SP_DEVINFO_DATA& device) {
    DWORD characters = 0;
    SetupDiGetDeviceInstanceIdW(set, &device, nullptr, 0, &characters);
    if (characters == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
    }

    std::vector<wchar_t> buffer(characters + 1, L'\0');
    if (!SetupDiGetDeviceInstanceIdW(set, &device, buffer.data(), characters, nullptr)) {
        return {};
    }
    return buffer.data();
}

std::vector<DeviceInfo> EnumerateMi01Interfaces(DWORD* enumeration_error = nullptr) {
    std::vector<DeviceInfo> devices;
    if (enumeration_error != nullptr) {
        *enumeration_error = ERROR_SUCCESS;
    }

    HDEVINFO set = SetupDiGetClassDevsW(&kAppleMuxInterfaceGuid,
                                        nullptr,
                                        nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        if (enumeration_error != nullptr) {
            *enumeration_error = GetLastError();
        }
        return devices;
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(
                set, nullptr, &kAppleMuxInterfaceGuid, index, &interface_data)) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_ITEMS && enumeration_error != nullptr) {
                *enumeration_error = error;
            }
            break;
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(
            set, &interface_data, nullptr, 0, &required, nullptr);
        if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }

        std::vector<BYTE> buffer(required, 0);
        auto* detail =
            reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA device_data{};
        device_data.cbSize = sizeof(device_data);
        if (!SetupDiGetDeviceInterfaceDetailW(set,
                                              &interface_data,
                                              detail,
                                              required,
                                              nullptr,
                                              &device_data)) {
            continue;
        }

        DeviceInfo result;
        result.path = detail->DevicePath;
        result.instance_id = DeviceInstanceId(set, device_data);
        result.description = RegistryProperty(set, device_data, SPDRP_DEVICEDESC);
        result.service = RegistryProperty(set, device_data, SPDRP_SERVICE);
        result.hardware_ids = RegistryProperty(set, device_data, SPDRP_HARDWAREID);
        const bool is_supported_mi01 =
            ContainsInsensitive(result.instance_id, L"VID_05AC&PID_12A8&MI_01") ||
            ContainsInsensitive(result.hardware_ids, L"VID_05AC&PID_12A8&MI_01");
        if (!is_supported_mi01) {
            continue;
        }
        devices.push_back(std::move(result));
    }

    SetupDiDestroyDeviceInfoList(set);
    return devices;
}

std::vector<AppleUsbNode> EnumerateAppleUsbNodes() {
    std::vector<AppleUsbNode> devices;
    HDEVINFO set = SetupDiGetClassDevsW(
        nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) {
        return devices;
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device_data{};
        device_data.cbSize = sizeof(device_data);
        if (!SetupDiEnumDeviceInfo(set, index, &device_data)) {
            break;
        }

        AppleUsbNode node;
        node.instance_id = DeviceInstanceId(set, device_data);
        node.hardware_ids = RegistryProperty(set, device_data, SPDRP_HARDWAREID);
        if (!ContainsInsensitive(node.instance_id, L"VID_05AC") &&
            !ContainsInsensitive(node.hardware_ids, L"VID_05AC")) {
            continue;
        }
        node.description = RegistryProperty(set, device_data, SPDRP_DEVICEDESC);
        node.service = RegistryProperty(set, device_data, SPDRP_SERVICE);
        node.class_name = RegistryProperty(set, device_data, SPDRP_CLASS);
        devices.push_back(std::move(node));
    }

    SetupDiDestroyDeviceInfoList(set);
    return devices;
}

bool IsClassicAmds(const AppleUsbNode& node) {
    return ContainsInsensitive(node.service, L"USBAAPL") ||
           ContainsInsensitive(node.description, L"Apple Mobile Device USB Driver");
}

class AppleMuxSession {
  public:
    AppleMuxSession() = default;
    AppleMuxSession(const AppleMuxSession&) = delete;
    AppleMuxSession& operator=(const AppleMuxSession&) = delete;

    ~AppleMuxSession() {
        Close();
    }

    bool Open(const std::wstring& path, DWORD& error) {
        Close();
        file_ = CreateFileW(path.c_str(),
                            GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                            nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            return false;
        }
        error = ERROR_SUCCESS;
        return true;
    }

    TransferResult ControlIn(UCHAR request_type,
                             UCHAR request,
                             USHORT value,
                             USHORT index,
                             USHORT length,
                             DWORD timeout_ms = kControlTransferTimeoutMs) {
        TransferResult result;
        if (file_ == INVALID_HANDLE_VALUE) {
            result.error = ERROR_INVALID_HANDLE;
            return result;
        }

        // AppleUsbFilter's 0x2200A0 ABI is an eight-byte packed
        // WINUSB_SETUP_PACKET followed by the IN payload in the output buffer.
        // This is the same contract used by Apple's own MobileDevice process.
        std::vector<UCHAR> input(8, 0);
        input[0] = request_type;
        input[1] = request;
        input[2] = static_cast<UCHAR>(value & 0xff);
        input[3] = static_cast<UCHAR>((value >> 8) & 0xff);
        input[4] = static_cast<UCHAR>(index & 0xff);
        input[5] = static_cast<UCHAR>((index >> 8) & 0xff);
        input[6] = static_cast<UCHAR>(length & 0xff);
        input[7] = static_cast<UCHAR>((length >> 8) & 0xff);
        std::vector<UCHAR> output(8 + static_cast<size_t>(length), 0);

        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (overlapped.hEvent == nullptr) {
            result.error = GetLastError();
            return result;
        }

        DWORD returned = 0;
        const BOOL immediate = DeviceIoControl(file_,
                                               kAppleIoctlControlTransfer,
                                               input.data(),
                                               static_cast<DWORD>(input.size()),
                                               output.data(),
                                               static_cast<DWORD>(output.size()),
                                               &returned,
                                               &overlapped);
        result.submitted = immediate != FALSE;
        if (!immediate) {
            const DWORD submit_error = GetLastError();
            if (submit_error != ERROR_IO_PENDING) {
                result.error = submit_error;
                CloseHandle(overlapped.hEvent);
                return result;
            }
            result.submitted = true;

            const DWORD wait = WaitForSingleObject(overlapped.hEvent, timeout_ms);
            if (wait == WAIT_TIMEOUT) {
                CancelIoEx(file_, &overlapped);
                WaitForSingleObject(overlapped.hEvent, INFINITE);
                DWORD ignored = 0;
                GetOverlappedResult(file_, &overlapped, &ignored, FALSE);
                result.error = ERROR_TIMEOUT;
                CloseHandle(overlapped.hEvent);
                return result;
            }
            if (wait != WAIT_OBJECT_0) {
                result.error = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
                CancelIoEx(file_, &overlapped);
                WaitForSingleObject(overlapped.hEvent, INFINITE);
                DWORD ignored = 0;
                GetOverlappedResult(file_, &overlapped, &ignored, FALSE);
                CloseHandle(overlapped.hEvent);
                return result;
            }
            if (!GetOverlappedResult(file_, &overlapped, &returned, FALSE)) {
                result.error = GetLastError();
                CloseHandle(overlapped.hEvent);
                return result;
            }
        }
        CloseHandle(overlapped.hEvent);

        if (returned < 8 || returned > static_cast<DWORD>(output.size())) {
            result.error = ERROR_INVALID_DATA;
            return result;
        }
        result.transferred = returned - 8;
        result.data.assign(output.begin() + 8, output.begin() + returned);
        result.ok = true;
        result.error = ERROR_SUCCESS;
        return result;
    }

    void Close() {
        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
    }

  private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
};

TransferResult ControlIn(AppleMuxSession& session,
                         UCHAR request_type,
                         UCHAR request,
                         USHORT value,
                         USHORT index,
                         USHORT length) {
    return session.ControlIn(request_type, request, value, index, length);
}

bool GetDescriptor(AppleMuxSession& session,
                   UCHAR type,
                   UCHAR index,
                   std::vector<UCHAR>& buffer,
                   ULONG& transferred,
                   DWORD& error) {
    transferred = 0;
    const auto result = ControlIn(session,
                                  kRequestTypeStandardDeviceIn,
                                  kRequestGetDescriptor,
                                  static_cast<USHORT>((type << 8) | index),
                                  0,
                                  static_cast<USHORT>(buffer.size()));
    if (result.ok) {
        transferred = result.transferred;
        buffer = result.data;
        error = ERROR_SUCCESS;
        return true;
    }
    error = result.error;
    buffer.clear();
    return false;
}

USHORT ReadLe16(const std::vector<UCHAR>& bytes, size_t offset) {
    return static_cast<USHORT>(bytes[offset]) |
           static_cast<USHORT>(static_cast<USHORT>(bytes[offset + 1]) << 8);
}

DescriptorSnapshot ReadDescriptors(AppleMuxSession& session) {
    DescriptorSnapshot snapshot;
    std::vector<UCHAR> device(18, 0);
    ULONG transferred = 0;
    if (!GetDescriptor(
            session, kDescriptorDevice, 0, device, transferred, snapshot.device_error) ||
        device.size() < 18) {
        if (snapshot.device_error == ERROR_SUCCESS) {
            snapshot.device_error = ERROR_INVALID_DATA;
        }
        return snapshot;
    }

    snapshot.device_ok = true;
    snapshot.vendor_id = ReadLe16(device, 8);
    snapshot.product_id = ReadLe16(device, 10);
    snapshot.configuration_count = device[17];

    for (UCHAR index = 0; index < snapshot.configuration_count; ++index) {
        ConfigurationDescriptor configuration;
        configuration.index = index;

        std::vector<UCHAR> header(9, 0);
        if (!GetDescriptor(session,
                           kDescriptorConfiguration,
                           index,
                           header,
                           transferred,
                           configuration.error) ||
            header.size() < 9) {
            if (configuration.error == ERROR_SUCCESS) {
                configuration.error = ERROR_INVALID_DATA;
            }
            snapshot.configurations.push_back(std::move(configuration));
            continue;
        }

        configuration.total_length = ReadLe16(header, 2);
        configuration.declared_interfaces = header[4];
        configuration.value = header[5];
        if (configuration.total_length < 9 || configuration.total_length > 16384) {
            configuration.error = ERROR_INVALID_DATA;
            snapshot.configurations.push_back(std::move(configuration));
            continue;
        }

        std::vector<UCHAR> bytes(configuration.total_length, 0);
        if (!GetDescriptor(session,
                           kDescriptorConfiguration,
                           index,
                           bytes,
                           transferred,
                           configuration.error) ||
            bytes.size() < configuration.total_length) {
            if (configuration.error == ERROR_SUCCESS) {
                configuration.error = ERROR_INVALID_DATA;
            }
            snapshot.configurations.push_back(std::move(configuration));
            continue;
        }

        configuration.complete = true;
        for (size_t offset = 0; offset + 2 <= bytes.size();) {
            const UCHAR length = bytes[offset];
            const UCHAR type = bytes[offset + 1];
            if (length < 2 || offset + length > bytes.size()) {
                configuration.complete = false;
                configuration.error = ERROR_INVALID_DATA;
                break;
            }
            if (type == kDescriptorInterface && length >= 9) {
                InterfaceDescriptor interface_descriptor;
                interface_descriptor.number = bytes[offset + 2];
                interface_descriptor.alternate = bytes[offset + 3];
                interface_descriptor.endpoints = bytes[offset + 4];
                interface_descriptor.interface_class = bytes[offset + 5];
                interface_descriptor.subclass = bytes[offset + 6];
                interface_descriptor.protocol = bytes[offset + 7];
                if (interface_descriptor.interface_class == 0xff &&
                    interface_descriptor.subclass == 0x2a &&
                    interface_descriptor.protocol == 0xff) {
                    configuration.has_valeria = true;
                }
                if (interface_descriptor.interface_class == 0xff &&
                    interface_descriptor.subclass == 0xfe &&
                    interface_descriptor.protocol == 0x02) {
                    configuration.has_usbmux = true;
                }
                configuration.interfaces.push_back(interface_descriptor);
            }
            offset += length;
        }
        snapshot.configurations.push_back(std::move(configuration));
    }
    return snapshot;
}

ProbeSnapshot CollectSnapshot(AppleMuxSession& session) {
    ProbeSnapshot snapshot;

    // Source of truth: libimobiledevice/usbmuxd master uses an IN vendor/device
    // request for both operations. GET_MODE is C0/45/value0/index0/length4.
    snapshot.get_mode =
        ControlIn(session, kRequestTypeVendorDeviceIn, kRequestGetMode, 0, 0, 4);
    snapshot.get_configuration = ControlIn(
        session, kRequestTypeStandardDeviceIn, kRequestGetConfiguration, 0, 0, 1);
    snapshot.descriptors = ReadDescriptors(session);
    return snapshot;
}

std::string HexBytes(const std::vector<UCHAR>& bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (size_t index = 0; index < bytes.size(); ++index) {
        if (index != 0) {
            output << ' ';
        }
        output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return output.str();
}

std::string HexByte(UCHAR value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<unsigned int>(value);
    return output.str();
}

std::optional<UCHAR> ActiveConfiguration(const ProbeSnapshot& snapshot) {
    if (!snapshot.get_configuration.ok || snapshot.get_configuration.transferred != 1 ||
        snapshot.get_configuration.data.size() != 1) {
        return std::nullopt;
    }
    return snapshot.get_configuration.data.front();
}

const ConfigurationDescriptor* FindConfiguration(const ProbeSnapshot& snapshot, UCHAR value) {
    for (const auto& configuration : snapshot.descriptors.configurations) {
        if (configuration.value == value) {
            return &configuration;
        }
    }
    return nullptr;
}

bool HasVisibleValeria(const ProbeSnapshot& snapshot) {
    for (const auto& configuration : snapshot.descriptors.configurations) {
        if (configuration.has_valeria) {
            return true;
        }
    }
    return false;
}

bool HasActiveValeria(const ProbeSnapshot& snapshot) {
    const auto active = ActiveConfiguration(snapshot);
    if (!active.has_value()) {
        return false;
    }
    const auto* configuration = FindConfiguration(snapshot, *active);
    return configuration != nullptr && configuration->has_valeria;
}

std::string SnapshotFingerprint(const ProbeSnapshot& snapshot) {
    std::ostringstream output;
    if (snapshot.get_mode.ok) {
        output << "m=" << HexBytes(snapshot.get_mode.data);
    }
    if (const auto active = ActiveConfiguration(snapshot); active.has_value()) {
        output << ";a=" << static_cast<unsigned int>(*active);
    }
    if (snapshot.descriptors.device_ok) {
        output << ";n=" << static_cast<unsigned int>(snapshot.descriptors.configuration_count);
        for (const auto& configuration : snapshot.descriptors.configurations) {
            output << ";c=" << static_cast<unsigned int>(configuration.value) << ':'
                   << configuration.has_valeria << ':' << configuration.has_usbmux;
        }
    }
    return output.str();
}

void PrintDevice(const DeviceInfo& device, const std::string& prefix = "[INFO]") {
    std::cout << prefix << " MI01 MUX1 interface: present (device identifier redacted)\n";
    std::cout << prefix << " description: " << Utf8(device.description) << '\n';
    std::cout << prefix << " service: "
              << (device.service.empty() ? "<not reported>" : Utf8(device.service)) << '\n';
    std::cout << prefix << " hardware IDs: " << Utf8(device.hardware_ids) << '\n';
}

void PrintTransfer(const char* name,
                   const char* packet,
                   const TransferResult& transfer,
                   ULONG expected) {
    if (!transfer.ok) {
        std::cout << "[WARN] " << name << " " << packet
                  << " failed: " << WindowsError(transfer.error) << '\n';
        return;
    }
    std::cout << "[INFO] " << name << " " << packet << " -> "
              << HexBytes(transfer.data) << " (" << transfer.transferred << " byte(s))";
    if (transfer.transferred != expected) {
        std::cout << " [unexpected length; wanted " << expected << ']';
    }
    std::cout << '\n';
}

void PrintSnapshot(const ProbeSnapshot& snapshot, const char* phase) {
    std::cout << "[INFO] ---- " << phase << " USB state ----\n";
    PrintTransfer("GET_MODE", "C0/45 value=0 index=0 length=4", snapshot.get_mode, 4);
    PrintTransfer("GET_CONFIGURATION",
                  "80/08 value=0 index=0 length=1",
                  snapshot.get_configuration,
                  1);

    if (!snapshot.descriptors.device_ok) {
        std::cout << "[WARN] DEVICE_DESCRIPTOR failed: "
                  << WindowsError(snapshot.descriptors.device_error) << '\n';
        return;
    }

    std::cout << "[INFO] device descriptor: VID=" << std::hex << std::setfill('0')
              << std::setw(4) << snapshot.descriptors.vendor_id << " PID=" << std::setw(4)
              << snapshot.descriptors.product_id << std::dec << ", bNumConfigurations="
              << static_cast<unsigned int>(snapshot.descriptors.configuration_count) << '\n';

    for (const auto& configuration : snapshot.descriptors.configurations) {
        std::cout << "[INFO] configuration index="
                  << static_cast<unsigned int>(configuration.index) << " value="
                  << static_cast<unsigned int>(configuration.value) << " interfaces="
                  << static_cast<unsigned int>(configuration.declared_interfaces)
                  << " totalLength=" << configuration.total_length;
        if (!configuration.complete) {
            std::cout << " descriptor-read=FAILED(" << WindowsError(configuration.error) << ')';
        }
        if (configuration.has_valeria) {
            std::cout << " VALERIA=FF/2A/FF";
        }
        if (configuration.has_usbmux) {
            std::cout << " USBMUX=FF/FE/02";
        }
        std::cout << '\n';

        for (const auto& interface_descriptor : configuration.interfaces) {
            std::cout << "[INFO]   interface="
                      << static_cast<unsigned int>(interface_descriptor.number) << " alt="
                      << static_cast<unsigned int>(interface_descriptor.alternate)
                      << " endpoints="
                      << static_cast<unsigned int>(interface_descriptor.endpoints) << " class="
                      << HexByte(interface_descriptor.interface_class) << '/'
                      << HexByte(interface_descriptor.subclass) << '/'
                      << HexByte(interface_descriptor.protocol) << '\n';
        }
    }

    if (const auto active = ActiveConfiguration(snapshot); active.has_value()) {
        const auto* descriptor = FindConfiguration(snapshot, *active);
        if (descriptor != nullptr && descriptor->has_valeria) {
            std::cout << "[OK] active configuration " << static_cast<unsigned int>(*active)
                      << " contains Valeria FF/2A/FF.\n";
        } else if (HasVisibleValeria(snapshot)) {
            std::cout << "[WARN] Valeria is visible in a descriptor, but active configuration "
                      << static_cast<unsigned int>(*active) << " does not contain it.\n";
        } else {
            std::cout << "[INFO] active configuration " << static_cast<unsigned int>(*active)
                      << " does not expose Valeria.\n";
        }
    } else if (HasVisibleValeria(snapshot)) {
        std::cout << "[WARN] Valeria descriptor is visible, but active configuration could not "
                     "be read.\n";
    }
}

bool DeviceMatches(const DeviceInfo& candidate, const DeviceInfo& original) {
    if (!original.instance_id.empty() &&
        Upper(candidate.instance_id) == Upper(original.instance_id)) {
        return true;
    }
    return !original.path.empty() && Upper(candidate.path) == Upper(original.path);
}

std::optional<DeviceInfo> FindSameDevice(const std::vector<DeviceInfo>& devices,
                                         const DeviceInfo& original) {
    for (const auto& candidate : devices) {
        if (DeviceMatches(candidate, original)) {
            return candidate;
        }
    }
    if (devices.size() == 1) {
        return devices.front();
    }
    return std::nullopt;
}

WaitResult WaitForReenumeration(const DeviceInfo& original,
                                const ProbeSnapshot& before,
                                DWORD timeout_ms) {
    WaitResult result;
    const auto start = std::chrono::steady_clock::now();
    const std::string before_fingerprint = SnapshotFingerprint(before);
    auto next_probe = start;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        result.elapsed_ms = static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
        if (result.elapsed_ms >= timeout_ms) {
            break;
        }

        const auto devices = EnumerateMi01Interfaces();
        auto current = FindSameDevice(devices, original);
        if (!current.has_value()) {
            result.saw_disappearance = true;
            result.device.reset();
        } else {
            result.device = current;
            if (result.saw_disappearance) {
                result.saw_reappearance = true;
                break;
            }

            // A fast disconnect/reconnect can happen between two SetupAPI polls.
            // A changed mode/configuration fingerprint is independent evidence of
            // re-enumeration, so sample it at a low rate while the path stays present.
            if (now >= next_probe && result.elapsed_ms >= 300) {
                next_probe = now + std::chrono::milliseconds(500);
                AppleMuxSession session;
                DWORD open_error = ERROR_SUCCESS;
                if (session.Open(current->path, open_error)) {
                    ProbeSnapshot snapshot = CollectSnapshot(session);
                    // A path can remain visible briefly while the old devnode is
                    // tearing down.  Failed control reads produce a different
                    // fingerprint too, but are not evidence that the new USB
                    // configuration is ready.  Only accept a readable device
                    // descriptor as a state-change sample.
                    if (snapshot.descriptors.device_ok &&
                        (SnapshotFingerprint(snapshot) != before_fingerprint ||
                         HasActiveValeria(snapshot))) {
                        result.state_changed = true;
                        result.saw_reappearance = true;
                        result.snapshot = std::move(snapshot);
                        break;
                    }
                    if (!snapshot.descriptors.device_ok) {
                        result.last_open_error = snapshot.descriptors.device_error;
                    }
                } else {
                    result.last_open_error = open_error;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!result.device.has_value()) {
        const auto devices = EnumerateMi01Interfaces();
        result.device = FindSameDevice(devices, original);
    }
    return result;
}

std::optional<ProbeSnapshot> OpenAndProbe(const DeviceInfo& device,
                                         DWORD& open_error,
                                         DWORD retry_ms = 0) {
    const auto start = std::chrono::steady_clock::now();
    std::optional<ProbeSnapshot> last_snapshot;
    while (true) {
        AppleMuxSession session;
        if (session.Open(device.path, open_error)) {
            ProbeSnapshot snapshot = CollectSnapshot(session);
            if (snapshot.descriptors.device_ok || retry_ms == 0) {
                return snapshot;
            }
            open_error = snapshot.descriptors.device_error;
            last_snapshot = std::move(snapshot);
        }
        const auto elapsed = static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count());
        if (elapsed >= retry_ms) {
            return last_snapshot;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void PrintAppleNodes(const std::vector<AppleUsbNode>& nodes) {
    if (nodes.empty()) {
        std::cout << "[INFO] SetupAPI found no present USB node with Apple VID 05AC.\n";
        return;
    }
    std::cout << "[INFO] Present Apple USB nodes:\n";
    for (const auto& node : nodes) {
        std::cout << "[INFO]   description=" << Utf8(node.description)
                  << " service="
                  << (node.service.empty() ? "<none>" : Utf8(node.service))
                  << " class=" << Utf8(node.class_name) << '\n';
        std::cout << "[INFO]   hardware IDs=" << Utf8(node.hardware_ids) << '\n';
    }
}

void PrintOpenAdvice(DWORD error) {
    std::cout << "[ERROR] Apple MI01 MUX1 open failed: " << WindowsError(error) << '\n';
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED) {
        std::cout << "[ERROR] Apple Devices/iTunes or AppleMobileDeviceService may own MUX1. "
                     "Close Apple device applications and retry from an elevated terminal. "
                     "This tool intentionally does not stop services.\n";
    }
}

int DiagnoseMissingMi01(DWORD enumeration_error) {
    if (enumeration_error != ERROR_SUCCESS) {
        std::cout << "[ERROR] SetupAPI MI01 enumeration failed: "
                  << WindowsError(enumeration_error) << '\n';
    }
    const auto nodes = EnumerateAppleUsbNodes();
    PrintAppleNodes(nodes);
    const bool classic = std::any_of(nodes.begin(), nodes.end(), IsClassicAmds);
    if (classic) {
        std::cout << "[ERROR] RESULT state=UNSUPPORTED_CLASSIC_AMDS exit="
                  << kClassicAmdsOnly << '\n';
        std::cout << "[ERROR] The legacy usbaapl/usbaapl64 parent driver does not expose the "
                     "modern AppleUsbFilter MUX1 contract used here. Install Apple Devices; "
                     "AMDeviceRequestAbbreviatedSendSync is a restore/signing request, not a "
                     "raw USB control-transfer API.\n";
        return kClassicAmdsOnly;
    }
    if (nodes.empty()) {
        std::cout << "[ERROR] RESULT state=NO_APPLE_USB_DEVICE exit=" << kNoAppleDevice
                  << '\n';
        return kNoAppleDevice;
    }
    std::cout << "[ERROR] RESULT state=MI01_APPLE_MUX_INTERFACE_MISSING exit="
              << kMi01InterfaceMissing << '\n';
    std::cout << "[ERROR] An Apple USB device is present, but AppleUsbFilter's MI01 MUX1 "
                 "application interface is not exposed by the active driver stack.\n";
    return kMi01InterfaceMissing;
}

bool ParseUnsigned(const std::wstring& value, DWORD& result) {
    if (value.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != L'\0' || parsed > 60000UL) {
        return false;
    }
    result = static_cast<DWORD>(parsed);
    return true;
}

std::optional<Options> ParseOptions(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--enable") {
            options.enable = true;
        } else if (argument == L"--probe") {
            options.enable = false;
        } else if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
            options.help = true;
        } else if (argument == L"--device") {
            if (++index >= argc) {
                return std::nullopt;
            }
            options.selector = argv[index];
        } else if (argument == L"--wait-ms") {
            if (++index >= argc || !ParseUnsigned(argv[index], options.wait_ms) ||
                options.wait_ms < 1000) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    }
    return options;
}

void PrintHelp() {
    std::cout
        << "valeria-activate - Apple-driver-preserving Valeria USB mode probe/activator\n\n"
        << "Usage:\n"
        << "  valeria-activate.exe [--probe] [--device TEXT]\n"
        << "  valeria-activate.exe --enable [--device TEXT] [--wait-ms 15000]\n\n"
        << "Options:\n"
        << "  --probe          Read-only probe (default).\n"
        << "  --enable         Send SET_MODE Valeria and verify re-enumerated descriptors.\n"
        << "  --device TEXT    Select by case-insensitive instance-ID/path substring.\n"
        << "  --wait-ms N      Re-enumeration timeout, 1000..60000 ms.\n"
        << "  --help           Show this help.\n";
}

std::vector<DeviceInfo> SelectDevices(const std::vector<DeviceInfo>& devices,
                                      const std::wstring& selector) {
    if (selector.empty()) {
        return devices;
    }
    std::vector<DeviceInfo> selected;
    for (const auto& device : devices) {
        if (ContainsInsensitive(device.instance_id, selector) ||
            ContainsInsensitive(device.path, selector)) {
            selected.push_back(device);
        }
    }
    return selected;
}

int RunProbe(const std::vector<DeviceInfo>& devices) {
    bool all_opened = true;
    for (size_t index = 0; index < devices.size(); ++index) {
        std::cout << "[INFO] ===== MI01 candidate " << (index + 1) << '/' << devices.size()
                  << " =====\n";
        PrintDevice(devices[index]);
        DWORD open_error = ERROR_SUCCESS;
        const auto snapshot = OpenAndProbe(devices[index], open_error);
        if (!snapshot.has_value()) {
            all_opened = false;
            PrintOpenAdvice(open_error);
            continue;
        }
        PrintSnapshot(*snapshot, "current");
    }

    if (!all_opened) {
        std::cout << "[ERROR] RESULT state=MI01_MUX_PRESENT_BUT_NOT_OPENABLE exit=" << kOpenFailed
                  << '\n';
        return kOpenFailed;
    }
    std::cout << "[OK] RESULT state=PROBE_COMPLETE exit=0\n";
    return kSuccess;
}

int RunEnable(const DeviceInfo& device, DWORD wait_ms) {
    PrintDevice(device);

    AppleMuxSession session;
    DWORD open_error = ERROR_SUCCESS;
    if (!session.Open(device.path, open_error)) {
        PrintOpenAdvice(open_error);
        std::cout << "[ERROR] RESULT state=MI01_MUX_PRESENT_BUT_NOT_OPENABLE exit=" << kOpenFailed
                  << '\n';
        return kOpenFailed;
    }

    const ProbeSnapshot before = CollectSnapshot(session);
    PrintSnapshot(before, "before");
    if (HasActiveValeria(before)) {
        std::cout << "[OK] RESULT state=VALERIA_ALREADY_ACTIVE exit=0\n";
        return kSuccess;
    }

    std::cout << "[INFO] Sending SET_MODE Valeria: C0/52 value=0 index=2 length=1.\n";
    // Important: this is device-to-host (0xC0), not the old 0x40/length-0
    // request in quicktime_video_hack_windows. usbmuxd expects one response
    // byte and treats value 0 as acceptance.
    const TransferResult set_mode = ControlIn(
        session, kRequestTypeVendorDeviceIn, kRequestSetMode, 0, kValeriaMode, 1);
    PrintTransfer("SET_MODE", "C0/52 value=0 index=2 length=1", set_mode, 1);
    const bool request_accepted = set_mode.ok && set_mode.transferred == 1 &&
                                  set_mode.data.size() == 1 && set_mode.data.front() == 0;
    if (set_mode.ok && !request_accepted) {
        std::cout << "[ERROR] Device returned an unexpected SET_MODE response; expected one "
                     "zero byte.\n";
    } else if (!set_mode.ok && set_mode.error == ERROR_DEVICE_NOT_CONNECTED) {
        std::cout << "[WARN] The control call observed disconnect. Verification continues "
                     "because mode switching intentionally re-enumerates USB.\n";
    }

    session.Close();
    const WaitResult wait = WaitForReenumeration(device, before, wait_ms);
    std::cout << "[INFO] re-enumeration: disappeared="
              << (wait.saw_disappearance ? "yes" : "no")
              << " reappeared-or-state-changed="
              << (wait.saw_reappearance ? "yes" : "no")
              << " stateChanged=" << (wait.state_changed ? "yes" : "no")
              << " elapsedMs=" << wait.elapsed_ms << '\n';

    if (!wait.device.has_value()) {
        PrintAppleNodes(EnumerateAppleUsbNodes());
        std::cout << "[ERROR] RESULT state=MI01_DID_NOT_REAPPEAR exit="
                  << kReenumerationTimeout << '\n';
        return kReenumerationTimeout;
    }

    ProbeSnapshot after;
    if (wait.snapshot.has_value()) {
        after = *wait.snapshot;
    } else {
        DWORD after_open_error = ERROR_SUCCESS;
        const auto after_snapshot = OpenAndProbe(*wait.device, after_open_error, 3000);
        if (!after_snapshot.has_value()) {
            PrintOpenAdvice(after_open_error);
            std::cout << "[ERROR] The device reappeared, but post-switch USB state could not be "
                         "verified.\n";
            std::cout << "[ERROR] RESULT state=REENUMERATED_BUT_UNVERIFIED exit="
                      << kAcceptedButUnverified << '\n';
            return kAcceptedButUnverified;
        }
        after = *after_snapshot;
    }
    PrintSnapshot(after, "after");

    if (HasActiveValeria(after)) {
        std::cout << "[OK] RESULT state=VALERIA_ACTIVE exit=0\n";
        std::cout << "[OK] Activation gate passed: the active USB configuration exposes "
                     "FF/2A/FF. Bulk endpoint binding/streaming is the next independent gate.\n";
        return kSuccess;
    }

    if (HasVisibleValeria(after)) {
        const auto active = ActiveConfiguration(after);
        std::cout << "[ERROR] RESULT state=VALERIA_VISIBLE_BUT_NOT_ACTIVE exit="
                  << kValeriaNotActive << '\n';
        std::cout << "[ERROR] The driver selected configuration "
                  << (active.has_value() ? std::to_string(*active) : std::string("<unknown>"))
                  << ", while Valeria exists in another configuration. The program will not "
                     "issue SET_CONFIGURATION through a composite child or write registry "
                     "configuration overrides.\n";
        return kValeriaNotActive;
    }

    if (!request_accepted) {
        std::cout << "[ERROR] RESULT state=SET_MODE_FAILED exit=" << kControlTransferFailed
                  << '\n';
        return kControlTransferFailed;
    }

    if (!wait.saw_reappearance && !wait.state_changed) {
        std::cout << "[ERROR] RESULT state=REENUMERATION_TIMEOUT exit="
                  << kReenumerationTimeout << '\n';
        return kReenumerationTimeout;
    }

    std::cout << "[ERROR] RESULT state=SET_MODE_ACCEPTED_BUT_VALERIA_UNVERIFIED exit="
              << kAcceptedButUnverified << '\n';
    return kAcceptedButUnverified;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const auto options = ParseOptions(argc, argv);
    if (!options.has_value()) {
        PrintHelp();
        std::cout << "\n[ERROR] Invalid arguments.\n";
        return kBadArguments;
    }
    if (options->help) {
        PrintHelp();
        return kSuccess;
    }

    std::cout << "[INFO] Transport policy: Apple-signed driver + Microsoft winusb.sys only; "
                 "no libusb0/filter driver.\n";
    std::cout << "[INFO] Protocol policy: continuous Valeria media only; this executable is "
                 "the activation diagnostic, not a completed mirror.\n";

    DWORD enumeration_error = ERROR_SUCCESS;
    const auto all_devices = EnumerateMi01Interfaces(&enumeration_error);
    if (all_devices.empty()) {
        return DiagnoseMissingMi01(enumeration_error);
    }
    const auto selected = SelectDevices(all_devices, options->selector);
    if (selected.empty()) {
        std::cout << "[ERROR] No MI01 MUX1 interface matches --device (selector redacted).\n";
        for (const auto& device : all_devices) {
            PrintDevice(device);
        }
        return kNoAppleDevice;
    }

    if (!options->enable) {
        return RunProbe(selected);
    }
    if (selected.size() != 1) {
        std::cout << "[ERROR] --enable matched " << selected.size()
                  << " devices. Use --device with an instance-ID/serial substring.\n";
        for (const auto& device : selected) {
            PrintDevice(device);
        }
        std::cout << "[ERROR] RESULT state=AMBIGUOUS_DEVICE exit=" << kAmbiguousDevice << '\n';
        return kAmbiguousDevice;
    }
    return RunEnable(selected.front(), options->wait_ms);
}

#else

#include <iostream>

int main() {
    std::cerr << "valeria-activate is a Windows-only Apple USB diagnostic.\n";
    return 64;
}

#endif
