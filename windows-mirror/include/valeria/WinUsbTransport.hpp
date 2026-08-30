#pragma once

#include "valeria/IUsbTransport.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winusb.h>
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace valeria {

class WinUsbTransport final : public IUsbTransport {
public:
    WinUsbTransport() = default;
    ~WinUsbTransport() override;

    WinUsbTransport(const WinUsbTransport&) = delete;
    WinUsbTransport& operator=(const WinUsbTransport&) = delete;

    void open(const std::string& deviceSelector) override;
    void close() noexcept override;
    bool isOpen() const noexcept override;
    const UsbInterfaceInfo& interfaceInfo() const override;
    std::size_t readSome(std::uint8_t* destination, std::size_t capacity,
                         std::chrono::milliseconds timeout) override;
    void writeAll(const std::uint8_t* data, std::size_t size,
                  std::chrono::milliseconds timeout) override;
    void controlOut(std::uint8_t requestType, std::uint8_t request,
                    std::uint16_t value, std::uint16_t index,
                    const std::uint8_t* data, std::uint16_t size,
                    std::chrono::milliseconds timeout) override;
    std::size_t controlIn(std::uint8_t requestType, std::uint8_t request,
                          std::uint16_t value, std::uint16_t index,
                          std::uint8_t* data, std::uint16_t size,
                          std::chrono::milliseconds timeout) override;

    static void activateValeria(const std::string& deviceSelector,
                                std::chrono::milliseconds timeout);

private:
#ifdef _WIN32
    void openPath(const std::wstring& path, const std::wstring& instanceId);
    void discoverValeriaPipes();
    static std::wstring findDevicePath(const GUID& interfaceGuid,
                                       const std::string& selector,
                                       std::wstring* instanceId);

    HANDLE deviceHandle_ = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE usbHandle_ = nullptr;
#endif
    UsbInterfaceInfo info_;
};

} // namespace valeria
