#include "valeria/PacketFramer.hpp"

#include <algorithm>
#include <limits>

namespace valeria {

PacketFramer::PacketFramer(std::uint32_t maximumPacket)
    : maximumPacket_(maximumPacket) {
    if (maximumPacket_ < 8) {
        throw std::invalid_argument("maximum packet size must be at least 8 bytes");
    }
}

std::vector<Bytes> PacketFramer::push(const std::uint8_t* data, std::size_t size) {
    if (size != 0 && data == nullptr) {
        throw std::invalid_argument("non-empty PacketFramer input is null");
    }
    if (size > std::numeric_limits<std::size_t>::max() - buffered_.size()) {
        throw ParseError("USB stream accumulator overflow");
    }
    if (size != 0) {
        buffered_.insert(buffered_.end(), data, data + size);
    }

    std::vector<Bytes> packets;
    std::size_t consumed = 0;
    while (buffered_.size() - consumed >= 4) {
        const std::uint32_t packetSize = readLe32(buffered_.data() + consumed);
        if (packetSize < 8 || packetSize > maximumPacket_) {
            buffered_.clear();
            throw ParseError("invalid Valeria packet length " + std::to_string(packetSize));
        }
        if (buffered_.size() - consumed < packetSize) {
            break;
        }
        packets.emplace_back(buffered_.begin() + static_cast<std::ptrdiff_t>(consumed),
                             buffered_.begin() + static_cast<std::ptrdiff_t>(consumed + packetSize));
        consumed += packetSize;
    }

    if (consumed != 0) {
        buffered_.erase(buffered_.begin(),
                        buffered_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    return packets;
}

} // namespace valeria
