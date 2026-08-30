#pragma once

#include "valeria/ByteIO.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace valeria {

class UsbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct UsbInterfaceInfo {
    std::string devicePath;
    std::string instanceId;
    std::uint8_t interfaceNumber = 0;
    std::uint8_t alternateSetting = 0;
    std::uint8_t bulkIn = 0;
    std::uint8_t bulkOut = 0;
    std::uint16_t bulkInMaxPacket = 0;
    std::uint16_t bulkOutMaxPacket = 0;
};

class IUsbTransport {
public:
    virtual ~IUsbTransport() = default;

    virtual void open(const std::string& deviceSelector) = 0;
    virtual void close() noexcept = 0;
    virtual bool isOpen() const noexcept = 0;
    virtual const UsbInterfaceInfo& interfaceInfo() const = 0;

    // Returns zero only for a timeout. Disconnects and hard failures throw UsbError.
    virtual std::size_t readSome(std::uint8_t* destination, std::size_t capacity,
                                 std::chrono::milliseconds timeout) = 0;
    virtual void writeAll(const std::uint8_t* data, std::size_t size,
                          std::chrono::milliseconds timeout) = 0;

    virtual void controlOut(std::uint8_t requestType, std::uint8_t request,
                            std::uint16_t value, std::uint16_t index,
                            const std::uint8_t* data, std::uint16_t size,
                            std::chrono::milliseconds timeout) = 0;
    virtual std::size_t controlIn(std::uint8_t requestType, std::uint8_t request,
                                  std::uint16_t value, std::uint16_t index,
                                  std::uint8_t* data, std::uint16_t size,
                                  std::chrono::milliseconds timeout) = 0;

    void writeAll(const Bytes& bytes, std::chrono::milliseconds timeout) {
        writeAll(bytes.data(), bytes.size(), timeout);
    }
};

} // namespace valeria
