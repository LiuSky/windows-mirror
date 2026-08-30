#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace valeria {

using Bytes = std::vector<std::uint8_t>;

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void requireRange(std::size_t size, std::size_t offset, std::size_t count,
                         const char* what) {
    if (offset > size || count > size - offset) {
        throw ParseError(std::string("truncated ") + what);
    }
}

inline std::uint16_t readBe16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) |
                                      static_cast<std::uint16_t>(data[1]));
}

inline std::uint32_t readBeN(const std::uint8_t* data, std::uint8_t width) {
    if (width == 0 || width > 4) {
        throw ParseError("invalid big-endian integer width");
    }
    std::uint32_t value = 0;
    for (std::uint8_t i = 0; i < width; ++i) {
        value = (value << 8U) | data[i];
    }
    return value;
}

inline std::uint32_t readLe32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

inline std::uint64_t readLe64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[i]) << (i * 8U);
    }
    return value;
}

inline std::int64_t readLeI64(const std::uint8_t* data) {
    const std::uint64_t bits = readLe64(data);
    std::int64_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline double readLeDouble(const std::uint8_t* data) {
    const std::uint64_t bits = readLe64(data);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline void appendLe16(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

inline void appendLe32(Bytes& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
    }
}

inline void appendLe64(Bytes& out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
    }
}

inline void appendLeI64(Bytes& out, std::int64_t value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendLe64(out, bits);
}

inline void appendLeDouble(Bytes& out, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendLe64(out, bits);
}

inline void patchLe32(Bytes& out, std::size_t offset, std::uint32_t value) {
    requireRange(out.size(), offset, 4, "32-bit patch");
    for (unsigned i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
    }
}

inline std::string fourcc(std::uint32_t value) {
    std::string result(4, '?');
    result[0] = static_cast<char>(value >> 24U);
    result[1] = static_cast<char>(value >> 16U);
    result[2] = static_cast<char>(value >> 8U);
    result[3] = static_cast<char>(value);
    return result;
}

} // namespace valeria
