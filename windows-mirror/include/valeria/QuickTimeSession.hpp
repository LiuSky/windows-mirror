#pragma once

#include "valeria/BinaryDictionary.hpp"
#include "valeria/Clock.hpp"
#include "valeria/CoreMedia.hpp"
#include "valeria/IUsbTransport.hpp"
#include "valeria/PacketFramer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace valeria {

namespace wire {
constexpr std::uint32_t kPing = 0x70696E67U;
constexpr std::uint32_t kSync = 0x73796E63U;
constexpr std::uint32_t kReply = 0x72706C79U;
constexpr std::uint32_t kAsync = 0x6173796EU;
constexpr std::uint32_t kTime = 0x74696D65U;
constexpr std::uint32_t kCwpa = 0x63777061U;
constexpr std::uint32_t kAfmt = 0x61666D74U;
constexpr std::uint32_t kCvrp = 0x63767270U;
constexpr std::uint32_t kClok = 0x636C6F6BU;
constexpr std::uint32_t kOg = 0x676F2120U;
constexpr std::uint32_t kSkew = 0x736B6577U;
constexpr std::uint32_t kStop = 0x73746F70U;
constexpr std::uint32_t kFeed = 0x66656564U;
constexpr std::uint32_t kEat = 0x65617421U;
constexpr std::uint32_t kHpd1 = 0x68706431U;
constexpr std::uint32_t kHpa1 = 0x68706131U;
constexpr std::uint32_t kNeed = 0x6E656564U;
constexpr std::uint32_t kHpd0 = 0x68706430U;
constexpr std::uint32_t kHpa0 = 0x68706130U;
constexpr std::uint64_t kEmptyCfType = 1U;
} // namespace wire

struct SessionOptions {
    std::uint32_t requestedWidth = 1920;
    std::uint32_t requestedHeight = 1200;
    bool advertiseHevc = true;
    std::chrono::milliseconds ioTimeout{1000};
};

struct SessionCallbacks {
    std::function<void(const VideoFormat&)> onVideoFormat;
    std::function<void(const VideoSample&)> onVideoSample;
    std::function<void(const AudioFormat&)> onAudioFormat;
    std::function<void(const AudioSample&)> onAudioSample;
    std::function<void(const std::string&)> onLog;
};

class QuickTimeSession {
public:
    QuickTimeSession(IUsbTransport& transport, SessionOptions options,
                     SessionCallbacks callbacks = {});

    void run(std::atomic_bool& stopRequested);
    void closeSession() noexcept;
    void handlePacket(const Bytes& packet);

private:
    void handleSync(const Bytes& packet);
    void handleAsync(const Bytes& packet);
    void handleVideoSample(const std::uint8_t* data, std::size_t size);
    void handleAudioSample(const std::uint8_t* data, std::size_t size);
    void send(const Bytes& packet);
    void log(const std::string& message) const;

    Bytes makeHpd1() const;
    Bytes makeHpa1(std::uint64_t clockRef) const;
    static Bytes makePing();
    static Bytes makeAsyncHeader(std::uint32_t subtype, std::uint64_t clockRef);
    static Bytes makeClockReply(std::uint64_t correlationId, std::uint64_t clockRef);
    static Bytes makeSimpleReply(std::uint64_t correlationId);

    double calculateSkew() const noexcept;
    void trackAudioTime(const CMTime& deviceTime);

    IUsbTransport& transport_;
    SessionOptions options_;
    SessionCallbacks callbacks_;
    PacketFramer framer_;
    Clock hostClock_;
    Clock localAudioClock_;
    std::uint64_t deviceAudioClockRef_ = 0;
    Bytes needMessage_;
    std::optional<VideoFormat> currentVideoFormat_;
    std::optional<AudioFormat> currentAudioFormat_;
    bool firstAudioTimeTaken_ = false;
    CMTime firstDeviceAudioTime_;
    std::uint64_t firstLocalAudioMs_ = 0;
    CMTime lastDeviceAudioTime_;
    std::uint64_t lastLocalAudioMs_ = 0;
    bool closing_ = false;
};

} // namespace valeria
