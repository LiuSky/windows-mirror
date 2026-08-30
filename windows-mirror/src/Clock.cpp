#include "valeria/Clock.hpp"

namespace valeria {

double CMTime::seconds() const noexcept {
    return timescale == 0 ? 0.0 : static_cast<double>(value) / timescale;
}

void Clock::reset(std::uint64_t id) {
    id_ = id;
    start_ = std::chrono::steady_clock::now();
}

std::uint64_t Clock::elapsedMilliseconds() const noexcept {
    const auto elapsed = std::chrono::steady_clock::now() - start_;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

CMTime Clock::time() const noexcept {
    CMTime result;
    result.value = static_cast<std::int64_t>(elapsedMilliseconds());
    result.timescale = 1000;
    result.flags = 1;
    result.epoch = 0;
    return result;
}

} // namespace valeria
