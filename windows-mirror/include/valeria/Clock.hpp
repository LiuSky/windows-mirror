#pragma once

#include <chrono>
#include <cstdint>

namespace valeria {

struct CMTime {
    std::int64_t value = 0;
    std::uint32_t timescale = 0;
    std::uint32_t flags = 0;
    std::int64_t epoch = 0;

    bool valid() const noexcept { return timescale != 0 && (flags & 1U) != 0; }
    double seconds() const noexcept;
};

class Clock {
public:
    Clock() = default;
    explicit Clock(std::uint64_t id) { reset(id); }

    void reset(std::uint64_t id);
    std::uint64_t id() const noexcept { return id_; }
    std::uint64_t elapsedMilliseconds() const noexcept;
    CMTime time() const noexcept;

private:
    std::uint64_t id_ = 0;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

} // namespace valeria
