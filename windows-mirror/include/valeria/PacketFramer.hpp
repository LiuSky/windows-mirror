#pragma once

#include "valeria/ByteIO.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace valeria {

// USB bulk transfers have no message boundaries. This accumulator accepts both
// fragmented packets and multiple coalesced packets without discarding bytes.
class PacketFramer {
public:
    static constexpr std::uint32_t kDefaultMaximumPacket = 64U * 1024U * 1024U;

    explicit PacketFramer(std::uint32_t maximumPacket = kDefaultMaximumPacket);

    std::vector<Bytes> push(const std::uint8_t* data, std::size_t size);
    std::vector<Bytes> push(const Bytes& data) { return push(data.data(), data.size()); }
    void clear() noexcept { buffered_.clear(); }
    std::size_t bufferedSize() const noexcept { return buffered_.size(); }

private:
    std::uint32_t maximumPacket_;
    Bytes buffered_;
};

} // namespace valeria
