#include "valeria/WinUsbTransport.hpp"

#ifdef _WIN32

#include <setupapi.h>
#include <usb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <limits>
#include <sstream>
#include <thread>
#include <vector>

namespace valeria {
namespace {

constexpr GUID kAppleMuxInterfaceGuid = {
    0xf0b32be3, 0x6678, 0x4879, {0x92, 0x30, 0xe4, 0x38, 0x45, 0xd8, 0x05, 0xee}};
constexpr GUID kValeriaMi02InterfaceGuid = {
    0x77e935b1, 0xb768, 0x4316, {0xa4, 0x66, 0x4e, 0x74, 0x5c, 0xfd, 0xdb, 0x24}};
constexpr DWORD kAppleIoctlControlTransfer = 0x002200a0;

std::string utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) {
        return "<invalid Windows string>";
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        throw UsbError("device selector is not valid UTF-8");
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring upper(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towupper(character));
    });
    return value;
}

std::string windowsError(const char* operation, DWORD error = GetLastError()) {
    wchar_t* raw = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstring detail = raw ? raw : L"";
    if (raw) {
        LocalFree(raw);
    }
    while (!detail.empty() && (detail.back() == L'\r' || detail.back() == L'\n' ||
                               detail.back() == L' ')) {
        detail.pop_back();
    }
    std::ostringstream result;
    result << operation << " failed with Windows error " << error;
    if (!detail.empty()) {
        result << " (" << utf8(detail) << ')';
    }
    return result.str();
}

std::wstring instanceId(HDEVINFO devices, SP_DEVINFO_DATA& data) {
    DWORD required = 0;
    SetupDiGetDeviceInstanceIdW(devices, &data, nullptr, 0, &required);
    if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
    }
    std::vector<wchar_t> buffer(required + 1, L'\0');
    if (!SetupDiGetDeviceInstanceIdW(devices, &data, buffer.data(), required, nullptr)) {
        return {};
    }
    return buffer.data();
}

std::string normalizedSelector(const std::string& input) {
    const std::size_t slash = input.find_last_of("\\/");
    if (slash != std::string::npos && slash + 1 < input.size()) {
        return input.substr(slash + 1);
    }
    return input;
}

struct FileHandle {
    HANDLE file = INVALID_HANDLE_VALUE;

    FileHandle() = default;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& other) noexcept : file(other.file) {
        other.file = INVALID_HANDLE_VALUE;
    }
    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            if (file != INVALID_HANDLE_VALUE) {
                CloseHandle(file);
            }
            file = other.file;
            other.file = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    ~FileHandle() {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
    }
};

FileHandle openAppleMux(const std::wstring& path) {
    FileHandle result;
    result.file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (result.file == INVALID_HANDLE_VALUE) {
        throw UsbError(windowsError("CreateFile(Apple MI_01 MUX1 interface)"));
    }
    return result;
}

DWORD boundedTimeout(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        return INFINITE;
    }
    return static_cast<DWORD>(std::min<std::int64_t>(timeout.count(), MAXDWORD - 1));
}

struct CompletionResult {
    bool success = false;
    bool timedOut = false;
    ULONG transferred = 0;
    DWORD error = ERROR_SUCCESS;
};

CompletionResult finishOverlapped(HANDLE file, OVERLAPPED& overlapped,
                                  std::chrono::milliseconds timeout) {
    CompletionResult result;
    const DWORD wait = WaitForSingleObject(overlapped.hEvent, boundedTimeout(timeout));
    if (wait == WAIT_TIMEOUT) {
        // Completion can race CancelIoEx. Always wait and query the final I/O
        // status: successful bytes must not be dropped or framing is lost.
        CancelIoEx(file, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        DWORD bytes = 0;
        if (GetOverlappedResult(file, &overlapped, &bytes, FALSE)) {
            result.success = true;
            result.transferred = bytes;
            return result;
        }
        result.error = GetLastError();
        result.timedOut = result.error == ERROR_OPERATION_ABORTED;
        return result;
    }
    if (wait != WAIT_OBJECT_0) {
        result.error = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
        return result;
    }
    DWORD bytes = 0;
    if (GetOverlappedResult(file, &overlapped, &bytes, FALSE)) {
        result.success = true;
        result.transferred = bytes;
    } else {
        result.error = GetLastError();
    }
    return result;
}

} // namespace

WinUsbTransport::~WinUsbTransport() {
    close();
}

std::wstring WinUsbTransport::findDevicePath(const GUID& interfaceGuid,
                                              const std::string& selector,
                                              std::wstring* selectedInstanceId) {
    HDEVINFO devices = SetupDiGetClassDevsW(&interfaceGuid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        throw UsbError(windowsError("SetupDiGetClassDevs"));
    }

    const std::wstring wanted = upper(wide(normalizedSelector(selector)));
    std::vector<std::pair<std::wstring, std::wstring>> matches;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &interfaceGuid, index,
                                         &interfaceData)) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_ITEMS) {
                SetupDiDestroyDeviceInfoList(devices);
                throw UsbError(windowsError("SetupDiEnumDeviceInterfaces", error));
            }
            break;
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &required,
                                         nullptr);
        if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        std::vector<std::uint8_t> storage(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA deviceData{};
        deviceData.cbSize = sizeof(deviceData);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, required,
                                              nullptr, &deviceData)) {
            continue;
        }
        const std::wstring id = instanceId(devices, deviceData);
        const std::wstring path = detail->DevicePath;
        if (wanted.empty() || upper(id).find(wanted) != std::wstring::npos ||
            upper(path).find(wanted) != std::wstring::npos) {
            matches.emplace_back(path, id);
        }
    }
    SetupDiDestroyDeviceInfoList(devices);

    if (matches.empty()) {
        return {};
    }
    if (matches.size() != 1) {
        throw UsbError("more than one matching iPhone USB function is present; pass --device");
    }
    if (selectedInstanceId) {
        *selectedInstanceId = matches.front().second;
    }
    return matches.front().first;
}

void WinUsbTransport::activateValeria(const std::string& deviceSelector,
                                      std::chrono::milliseconds timeout) {
    std::wstring id;
    const std::wstring path = findDevicePath(kAppleMuxInterfaceGuid, deviceSelector, &id);
    if (path.empty()) {
        throw UsbError(
            "Apple MI_01 MUX1 interface was not found; install/repair Apple Devices");
    }
    FileHandle handle = openAppleMux(path);

    // AppleUsbFilter's MUX1 contract: packed WINUSB_SETUP_PACKET followed by
    // the response payload. MI_01 stays entirely on Apple's official stack;
    // only the re-enumerated MI_02 is opened through the public WinUSB API.
    std::array<std::uint8_t, 8> setup{
        0xC0, 0x52, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00};
    std::array<std::uint8_t, 9> response{};

    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        throw UsbError(windowsError("CreateEvent(control transfer)"));
    }
    ULONG transferred = 0;
    BOOL completed = DeviceIoControl(handle.file, kAppleIoctlControlTransfer,
                                     setup.data(),
                                     static_cast<DWORD>(setup.size()), response.data(),
                                     static_cast<DWORD>(response.size()), &transferred,
                                     &overlapped);
    DWORD error = completed ? ERROR_SUCCESS : GetLastError();
    bool submittedPending = false;
    if (!completed && error == ERROR_IO_PENDING) {
        submittedPending = true;
        const CompletionResult completion = finishOverlapped(handle.file, overlapped, timeout);
        if (completion.success) {
            completed = TRUE;
            transferred = completion.transferred;
            error = ERROR_SUCCESS;
        } else {
            error = completion.error;
        }
    }
    CloseHandle(overlapped.hEvent);

    // Once an asynchronous SET_MODE was accepted by AppleUsbFilter, any
    // completion error can be the re-enumeration racing the old handle. The
    // authoritative result remains open()'s FF/2A/FF + bulk-pipe gate.
    if (!completed && !submittedPending && error != ERROR_DEVICE_NOT_CONNECTED &&
        error != ERROR_OPERATION_ABORTED) {
        throw UsbError(windowsError("SET_MODE C0/52", error));
    }
    if (completed &&
        (transferred != static_cast<ULONG>(response.size()) || response[8] != 0)) {
        throw UsbError("SET_MODE C0/52 returned an unexpected response");
    }
}

void WinUsbTransport::open(const std::string& deviceSelector) {
    close();
    std::string lastValeriaError = "Valeria MI_02 device interface is not present";
    const auto tryOpenValeria = [&]() -> bool {
        std::wstring id;
        const std::wstring path = findDevicePath(kValeriaMi02InterfaceGuid,
                                                  deviceSelector, &id);
        if (path.empty()) {
            lastValeriaError = "Valeria MI_02 device interface is not present";
            return false;
        }
        try {
            openPath(path, id);
            discoverValeriaPipes();
            return true;
        } catch (const std::exception& error) {
            lastValeriaError = error.what();
            close();
            return false;
        } catch (...) {
            lastValeriaError = "unknown error while opening Valeria MI_02";
            close();
            return false;
        }
    };

    if (tryOpenValeria()) {
        return;
    }
    activateValeria(deviceSelector, std::chrono::seconds(10));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (tryOpenValeria()) {
            return;
        }
    }
    throw UsbError("MI_02 did not return as Valeria FF/2A/FF with bulk IN/OUT within 30s; "
                   "last open error: " + lastValeriaError);
}

void WinUsbTransport::openPath(const std::wstring& path, const std::wstring& id) {
    deviceHandle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (deviceHandle_ == INVALID_HANDLE_VALUE) {
        throw UsbError(windowsError("CreateFile(Valeria MI_02)"));
    }
    if (!WinUsb_Initialize(deviceHandle_, &usbHandle_)) {
        const DWORD error = GetLastError();
        close();
        throw UsbError(windowsError("WinUsb_Initialize(Valeria MI_02)", error));
    }
    info_.devicePath = utf8(path);
    info_.instanceId = utf8(id);
}

void WinUsbTransport::discoverValeriaPipes() {
    USB_INTERFACE_DESCRIPTOR matched{};
    UCHAR matchedAlternate = 0;
    bool foundInterface = false;
    for (unsigned alternate = 0; alternate <= UCHAR_MAX; ++alternate) {
        USB_INTERFACE_DESCRIPTOR descriptor{};
        if (!WinUsb_QueryInterfaceSettings(usbHandle_, static_cast<UCHAR>(alternate),
                                           &descriptor)) {
            if (alternate == 0) {
                throw UsbError(windowsError("WinUsb_QueryInterfaceSettings"));
            }
            break;
        }
        if (descriptor.bInterfaceClass == 0xFF && descriptor.bInterfaceSubClass == 0x2A &&
            descriptor.bInterfaceProtocol == 0xFF) {
            matched = descriptor;
            matchedAlternate = static_cast<UCHAR>(alternate);
            foundInterface = true;
            break;
        }
    }
    if (!foundInterface) {
        throw UsbError("MI_02 is not the Valeria FF/2A/FF interface");
    }
    if (!WinUsb_SetCurrentAlternateSetting(usbHandle_, matchedAlternate)) {
        throw UsbError(windowsError("WinUsb_SetCurrentAlternateSetting"));
    }

    WINUSB_PIPE_INFORMATION bestIn{};
    WINUSB_PIPE_INFORMATION bestOut{};
    bool foundIn = false;
    bool foundOut = false;
    for (UCHAR index = 0; index < matched.bNumEndpoints; ++index) {
        WINUSB_PIPE_INFORMATION pipe{};
        if (!WinUsb_QueryPipe(usbHandle_, matchedAlternate, index, &pipe)) {
            throw UsbError(windowsError("WinUsb_QueryPipe"));
        }
        if (pipe.PipeType != UsbdPipeTypeBulk) {
            continue;
        }
        if (USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) {
            if (!foundIn || pipe.MaximumPacketSize > bestIn.MaximumPacketSize) {
                bestIn = pipe;
                foundIn = true;
            }
        } else if (!foundOut || pipe.MaximumPacketSize > bestOut.MaximumPacketSize) {
            bestOut = pipe;
            foundOut = true;
        }
    }
    if (!foundIn || !foundOut) {
        throw UsbError("Valeria interface does not expose bulk IN and bulk OUT pipes");
    }

    info_.interfaceNumber = matched.bInterfaceNumber;
    info_.alternateSetting = matchedAlternate;
    info_.bulkIn = bestIn.PipeId;
    info_.bulkOut = bestOut.PipeId;
    info_.bulkInMaxPacket = bestIn.MaximumPacketSize;
    info_.bulkOutMaxPacket = bestOut.MaximumPacketSize;

    UCHAR enabled = TRUE;
    WinUsb_SetPipePolicy(usbHandle_, info_.bulkIn, AUTO_CLEAR_STALL,
                         sizeof(enabled), &enabled);
    WinUsb_SetPipePolicy(usbHandle_, info_.bulkOut, AUTO_CLEAR_STALL,
                         sizeof(enabled), &enabled);
    WinUsb_SetPipePolicy(usbHandle_, info_.bulkIn, ALLOW_PARTIAL_READS,
                         sizeof(enabled), &enabled);
    WinUsb_ResetPipe(usbHandle_, info_.bulkIn);
    WinUsb_ResetPipe(usbHandle_, info_.bulkOut);
}

void WinUsbTransport::close() noexcept {
    if (usbHandle_) {
        WinUsb_AbortPipe(usbHandle_, info_.bulkIn);
        WinUsb_AbortPipe(usbHandle_, info_.bulkOut);
        WinUsb_Free(usbHandle_);
        usbHandle_ = nullptr;
    }
    if (deviceHandle_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(deviceHandle_, nullptr);
        CloseHandle(deviceHandle_);
        deviceHandle_ = INVALID_HANDLE_VALUE;
    }
    info_ = {};
}

bool WinUsbTransport::isOpen() const noexcept {
    return usbHandle_ != nullptr && deviceHandle_ != INVALID_HANDLE_VALUE;
}

const UsbInterfaceInfo& WinUsbTransport::interfaceInfo() const {
    if (!isOpen()) {
        throw UsbError("WinUSB transport is not open");
    }
    return info_;
}

std::size_t WinUsbTransport::readSome(std::uint8_t* destination, std::size_t capacity,
                                     std::chrono::milliseconds timeout) {
    if (!isOpen() || destination == nullptr || capacity == 0) {
        throw UsbError("invalid WinUSB read");
    }
    const ULONG request = static_cast<ULONG>(
        std::min<std::size_t>(capacity, std::numeric_limits<ULONG>::max()));
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        throw UsbError(windowsError("CreateEvent(read)"));
    }
    ULONG transferred = 0;
    BOOL completed = WinUsb_ReadPipe(usbHandle_, info_.bulkIn, destination, request,
                                     &transferred, &overlapped);
    const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
    try {
        if (!completed) {
            if (error != ERROR_IO_PENDING) {
                throw UsbError(windowsError("WinUsb_ReadPipe", error));
            }
            const CompletionResult completion = finishOverlapped(deviceHandle_, overlapped,
                                                                  timeout);
            if (completion.timedOut) {
                CloseHandle(overlapped.hEvent);
                return 0;
            }
            if (!completion.success) {
                throw UsbError(windowsError("WinUsb_ReadPipe completion", completion.error));
            }
            transferred = completion.transferred;
        }
    } catch (...) {
        CloseHandle(overlapped.hEvent);
        throw;
    }
    CloseHandle(overlapped.hEvent);
    return transferred;
}

void WinUsbTransport::writeAll(const std::uint8_t* data, std::size_t size,
                               std::chrono::milliseconds timeout) {
    if (!isOpen() || (size != 0 && data == nullptr)) {
        throw UsbError("invalid WinUSB write");
    }
    std::size_t position = 0;
    while (position < size) {
        const ULONG request = static_cast<ULONG>(std::min<std::size_t>(
            size - position, std::numeric_limits<ULONG>::max()));
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) {
            throw UsbError(windowsError("CreateEvent(write)"));
        }
        ULONG transferred = 0;
        BOOL completed = WinUsb_WritePipe(usbHandle_, info_.bulkOut,
                                          const_cast<PUCHAR>(data + position), request,
                                          &transferred, &overlapped);
        const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
        try {
            if (!completed) {
                if (error != ERROR_IO_PENDING) {
                    throw UsbError(windowsError("WinUsb_WritePipe", error));
                }
                const CompletionResult completion = finishOverlapped(deviceHandle_, overlapped,
                                                                      timeout);
                if (!completion.success) {
                    throw UsbError(windowsError("WinUsb_WritePipe completion",
                                                completion.error));
                }
                transferred = completion.transferred;
            }
        } catch (...) {
            CloseHandle(overlapped.hEvent);
            throw;
        }
        CloseHandle(overlapped.hEvent);
        if (transferred == 0) {
            throw UsbError("WinUsb_WritePipe completed without writing bytes");
        }
        position += transferred;
    }
}

std::size_t WinUsbTransport::controlIn(std::uint8_t requestType, std::uint8_t request,
                                      std::uint16_t value, std::uint16_t index,
                                      std::uint8_t* data, std::uint16_t size,
                                      std::chrono::milliseconds timeout) {
    if (!isOpen() || (size != 0 && data == nullptr)) {
        throw UsbError("invalid WinUSB control IN request");
    }
    WINUSB_SETUP_PACKET setup{requestType, request, value, index, size};
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        throw UsbError(windowsError("CreateEvent(control IN)"));
    }
    ULONG transferred = 0;
    BOOL completed = WinUsb_ControlTransfer(usbHandle_, setup, data, size,
                                            &transferred, &overlapped);
    const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
    try {
        if (!completed) {
            if (error != ERROR_IO_PENDING) {
                throw UsbError(windowsError("WinUsb_ControlTransfer(IN)", error));
            }
            const CompletionResult completion = finishOverlapped(deviceHandle_, overlapped,
                                                                  timeout);
            if (!completion.success) {
                throw UsbError(windowsError("WinUsb_ControlTransfer completion",
                                            completion.error));
            }
            transferred = completion.transferred;
        }
    } catch (...) {
        CloseHandle(overlapped.hEvent);
        throw;
    }
    CloseHandle(overlapped.hEvent);
    return transferred;
}

void WinUsbTransport::controlOut(std::uint8_t requestType, std::uint8_t request,
                                 std::uint16_t value, std::uint16_t index,
                                 const std::uint8_t* data, std::uint16_t size,
                                 std::chrono::milliseconds timeout) {
    // WinUSB's API takes a mutable buffer even for host-to-device transfers.
    const std::size_t transferred = controlIn(requestType, request, value, index,
                                              const_cast<std::uint8_t*>(data), size, timeout);
    if (transferred != size) {
        throw UsbError("short WinUSB control OUT transfer");
    }
}

} // namespace valeria

#else

namespace valeria {

WinUsbTransport::~WinUsbTransport() = default;
void WinUsbTransport::open(const std::string&) { throw UsbError("WinUSB requires Windows"); }
void WinUsbTransport::close() noexcept {}
bool WinUsbTransport::isOpen() const noexcept { return false; }
const UsbInterfaceInfo& WinUsbTransport::interfaceInfo() const {
    throw UsbError("WinUSB requires Windows");
}
std::size_t WinUsbTransport::readSome(std::uint8_t*, std::size_t,
                                     std::chrono::milliseconds) {
    throw UsbError("WinUSB requires Windows");
}
void WinUsbTransport::writeAll(const std::uint8_t*, std::size_t,
                               std::chrono::milliseconds) {
    throw UsbError("WinUSB requires Windows");
}
void WinUsbTransport::controlOut(std::uint8_t, std::uint8_t, std::uint16_t,
                                 std::uint16_t, const std::uint8_t*, std::uint16_t,
                                 std::chrono::milliseconds) {
    throw UsbError("WinUSB requires Windows");
}
std::size_t WinUsbTransport::controlIn(std::uint8_t, std::uint8_t, std::uint16_t,
                                      std::uint16_t, std::uint8_t*, std::uint16_t,
                                      std::chrono::milliseconds) {
    throw UsbError("WinUSB requires Windows");
}
void WinUsbTransport::activateValeria(const std::string&, std::chrono::milliseconds) {
    throw UsbError("WinUSB requires Windows");
}

} // namespace valeria

#endif
