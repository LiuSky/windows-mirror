#include "valeria/QuickTimeSession.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <thread>

namespace valeria {
namespace {

void appendTime(Bytes& output, const CMTime& time) {
    appendLeI64(output, time.value);
    appendLe32(output, time.timescale);
    appendLe32(output, time.flags);
    appendLeI64(output, time.epoch);
}

Bytes asyncDictionaryPacket(std::uint32_t subtype, std::uint64_t clockRef,
                            const Dictionary& dictionary) {
    Bytes payload = serializeDictionary(dictionary);
    Bytes result;
    result.reserve(20 + payload.size());
    appendLe32(result, static_cast<std::uint32_t>(20 + payload.size()));
    appendLe32(result, wire::kAsync);
    appendLe64(result, clockRef);
    appendLe32(result, subtype);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

bool sameVideoFormat(const VideoFormat& left, const VideoFormat& right) {
    return left.codec == right.codec && left.width == right.width &&
           left.height == right.height && left.nalLengthSize == right.nalLengthSize &&
           left.decoderConfigurationRecord == right.decoderConfigurationRecord;
}

} // namespace

QuickTimeSession::QuickTimeSession(IUsbTransport& transport, SessionOptions options,
                                   SessionCallbacks callbacks)
    : transport_(transport), options_(options), callbacks_(std::move(callbacks)) {
    if (options_.requestedWidth == 0 || options_.requestedHeight == 0) {
        throw std::invalid_argument("requested display size cannot be zero");
    }
}

void QuickTimeSession::run(std::atomic_bool& stopRequested) {
    std::array<std::uint8_t, 1024U * 1024U> readBuffer{};
    log("continuous Valeria receive loop started");
    while (!stopRequested.load(std::memory_order_relaxed)) {
        const std::size_t count = transport_.readSome(readBuffer.data(), readBuffer.size(),
                                                      options_.ioTimeout);
        if (count == 0) {
            continue;
        }
        for (const Bytes& packet : framer_.push(readBuffer.data(), count)) {
            try {
                handlePacket(packet);
            } catch (const ParseError& error) {
                log(std::string("discarded malformed protocol packet: ") + error.what());
            }
        }
    }
    log("continuous Valeria receive loop stopped");
}

void QuickTimeSession::handlePacket(const Bytes& packet) {
    requireRange(packet.size(), 0, 8, "Valeria packet");
    const std::uint32_t declaredLength = readLe32(packet.data());
    if (declaredLength != packet.size()) {
        throw ParseError("packet length does not match framed bytes");
    }

    const std::uint32_t magic = readLe32(packet.data() + 4);
    switch (magic) {
    case wire::kPing: {
        send(makePing());
        // Current Apple host behavior advertises display capabilities as soon
        // as PING completes; it repeats HPD1 because the first can race setup.
        const Bytes hpd1 = makeHpd1();
        send(hpd1);
        send(hpd1);
        log("PING -> PING + HPD1 x2");
        break;
    }
    case wire::kSync:
        handleSync(packet);
        break;
    case wire::kAsync:
        handleAsync(packet);
        break;
    default:
        log("ignored unknown top-level packet " + fourcc(magic));
        break;
    }
}

void QuickTimeSession::handleSync(const Bytes& packet) {
    requireRange(packet.size(), 0, 28, "SYNC packet");
    const std::uint64_t clockRef = readLe64(packet.data() + 8);
    const std::uint32_t subtype = readLe32(packet.data() + 16);
    const std::uint64_t correlationId = readLe64(packet.data() + 20);

    switch (subtype) {
    case wire::kCwpa: {
        requireRange(packet.size(), 28, 8, "CWPA device clock");
        const std::uint64_t deviceClock = readLe64(packet.data() + 28);
        deviceAudioClockRef_ = deviceClock;
        localAudioClock_.reset(deviceClock + 1000U);

        const Bytes hpd1 = makeHpd1();
        send(hpd1);
        send(hpd1);
        send(makeClockReply(correlationId, localAudioClock_.id()));
        send(makeHpa1(deviceClock));
        log("CWPA -> HPD1 x2, clock reply, HPA1");
        break;
    }
    case wire::kAfmt: {
        Bytes dictionary = serializeDictionary(makeSuccessDictionary());
        Bytes reply;
        reply.reserve(20 + dictionary.size());
        appendLe32(reply, static_cast<std::uint32_t>(20 + dictionary.size()));
        appendLe32(reply, wire::kReply);
        appendLe64(reply, correlationId);
        appendLe32(reply, 0);
        reply.insert(reply.end(), dictionary.begin(), dictionary.end());
        send(reply);
        log("AFMT -> success reply");
        break;
    }
    case wire::kCvrp: {
        requireRange(packet.size(), 28, 8, "CVRP device clock");
        const std::uint64_t deviceClock = readLe64(packet.data() + 28);
        needMessage_ = makeAsyncHeader(wire::kNeed, deviceClock);
        send(needMessage_);
        send(makeClockReply(correlationId, deviceClock + 0x1000AFU));
        log("CVRP -> NEED + video clock reply");
        break;
    }
    case wire::kClok:
        hostClock_.reset(clockRef + 0x10000U);
        send(makeClockReply(correlationId, hostClock_.id()));
        break;
    case wire::kTime: {
        Bytes reply;
        reply.reserve(44);
        appendLe32(reply, 44);
        appendLe32(reply, wire::kReply);
        appendLe64(reply, correlationId);
        appendLe32(reply, 0);
        appendTime(reply, hostClock_.time());
        send(reply);
        break;
    }
    case wire::kOg:
        send(makeSimpleReply(correlationId));
        break;
    case wire::kSkew: {
        Bytes reply;
        reply.reserve(28);
        appendLe32(reply, 28);
        appendLe32(reply, wire::kReply);
        appendLe64(reply, correlationId);
        appendLe32(reply, 0);
        appendLeDouble(reply, calculateSkew());
        send(reply);
        break;
    }
    case wire::kStop:
        firstAudioTimeTaken_ = false;
        send(makeSimpleReply(correlationId));
        break;
    default:
        log("ignored unknown SYNC subtype " + fourcc(subtype));
        break;
    }
}

void QuickTimeSession::handleAsync(const Bytes& packet) {
    requireRange(packet.size(), 0, 20, "ASYN packet");
    const std::uint32_t subtype = readLe32(packet.data() + 16);
    switch (subtype) {
    case wire::kFeed:
        try {
            handleVideoSample(packet.data() + 20, packet.size() - 20);
        } catch (...) {
            if (!needMessage_.empty()) {
                send(needMessage_);
            }
            throw;
        }
        if (!needMessage_.empty()) {
            send(needMessage_);
        }
        break;
    case wire::kEat:
        handleAudioSample(packet.data() + 20, packet.size() - 20);
        break;
    default:
        // SPRP/TJMP/SRAT/TBAS/RELS are notifications and need no reply.
        break;
    }
}

void QuickTimeSession::handleVideoSample(const std::uint8_t* data, std::size_t size) {
    SampleBuffer sample = parseSampleBuffer(data, size, cm::kVideo);
    if (sample.format && sample.format->video) {
        VideoFormat incoming = *sample.format->video;
        if (incoming.parameterSets.empty()) {
            if (currentVideoFormat_ && incoming.codec == currentVideoFormat_->codec &&
                !currentVideoFormat_->parameterSets.empty()) {
                incoming.nalLengthSize = currentVideoFormat_->nalLengthSize;
                incoming.decoderConfigurationRecord =
                    currentVideoFormat_->decoderConfigurationRecord;
                incoming.parameterSets = currentVideoFormat_->parameterSets;
                incoming.annexBParameterSets = currentVideoFormat_->annexBParameterSets;
            } else {
                throw ParseError("initial video format has no usable decoder configuration");
            }
        }
        const bool changed = !currentVideoFormat_ ||
                             !sameVideoFormat(*currentVideoFormat_, incoming);
        currentVideoFormat_ = std::move(incoming);
        if (changed && callbacks_.onVideoFormat) {
            callbacks_.onVideoFormat(*currentVideoFormat_);
        }
    }
    if (sample.sampleData.empty()) {
        return;
    }
    if (!currentVideoFormat_ || currentVideoFormat_->codec == VideoCodec::Unknown) {
        throw ParseError("video sample arrived before a supported format description");
    }

    VideoSample output;
    output.codec = currentVideoFormat_->codec;
    output.presentationTimestamp = sample.outputPresentationTimestamp;
    if (!sample.timing.empty()) {
        output.duration = sample.timing.front().duration;
        output.presentationTimestamp = sample.timing.front().presentationTimestamp;
        output.decodeTimestamp = sample.timing.front().decodeTimestamp;
    }
    output.annexB = lengthPrefixedToAnnexB(sample.sampleData.data(),
                                           sample.sampleData.size(),
                                           currentVideoFormat_->nalLengthSize);
    output.keyFrame = isKeyFrame(output.codec, output.annexB);
    if (callbacks_.onVideoSample) {
        callbacks_.onVideoSample(output);
    }
}

void QuickTimeSession::handleAudioSample(const std::uint8_t* data, std::size_t size) {
    SampleBuffer sample = parseSampleBuffer(data, size, cm::kSound);
    if (sample.format && sample.format->audio) {
        currentAudioFormat_ = *sample.format->audio;
        if (callbacks_.onAudioFormat) {
            callbacks_.onAudioFormat(*currentAudioFormat_);
        }
    }
    CMTime pts = sample.outputPresentationTimestamp;
    CMTime duration;
    if (!sample.timing.empty()) {
        duration = sample.timing.front().duration;
        pts = sample.timing.front().presentationTimestamp;
    }
    trackAudioTime(pts);
    if (!sample.sampleData.empty() && callbacks_.onAudioSample) {
        callbacks_.onAudioSample({pts, duration, std::move(sample.sampleData)});
    }
}

void QuickTimeSession::send(const Bytes& packet) {
    transport_.writeAll(packet, options_.ioTimeout);
}

void QuickTimeSession::log(const std::string& message) const {
    if (callbacks_.onLog) {
        callbacks_.onLog(message);
    }
}

Bytes QuickTimeSession::makeHpd1() const {
    return asyncDictionaryPacket(wire::kHpd1, wire::kEmptyCfType,
                                 makeVideoRequestDictionary(options_.requestedWidth,
                                                            options_.requestedHeight,
                                                            options_.advertiseHevc));
}

Bytes QuickTimeSession::makeHpa1(std::uint64_t clockRef) const {
    return asyncDictionaryPacket(wire::kHpa1, clockRef, makeAudioRequestDictionary());
}

Bytes QuickTimeSession::makePing() {
    Bytes result;
    result.reserve(16);
    appendLe32(result, 16);
    appendLe32(result, wire::kPing);
    appendLe64(result, 0x0000000100000000ULL);
    return result;
}

Bytes QuickTimeSession::makeAsyncHeader(std::uint32_t subtype, std::uint64_t clockRef) {
    Bytes result;
    result.reserve(20);
    appendLe32(result, 20);
    appendLe32(result, wire::kAsync);
    appendLe64(result, clockRef);
    appendLe32(result, subtype);
    return result;
}

Bytes QuickTimeSession::makeClockReply(std::uint64_t correlationId,
                                       std::uint64_t clockRef) {
    Bytes result;
    result.reserve(28);
    appendLe32(result, 28);
    appendLe32(result, wire::kReply);
    appendLe64(result, correlationId);
    appendLe32(result, 0);
    appendLe64(result, clockRef);
    return result;
}

Bytes QuickTimeSession::makeSimpleReply(std::uint64_t correlationId) {
    Bytes result;
    result.reserve(24);
    appendLe32(result, 24);
    appendLe32(result, wire::kReply);
    appendLe64(result, correlationId);
    appendLe32(result, 0);
    appendLe32(result, 0);
    return result;
}

void QuickTimeSession::trackAudioTime(const CMTime& deviceTime) {
    if (deviceTime.timescale == 0) {
        return;
    }
    if (!firstAudioTimeTaken_) {
        firstAudioTimeTaken_ = true;
        firstDeviceAudioTime_ = deviceTime;
        firstLocalAudioMs_ = localAudioClock_.elapsedMilliseconds();
    }
    lastDeviceAudioTime_ = deviceTime;
    lastLocalAudioMs_ = localAudioClock_.elapsedMilliseconds();
}

double QuickTimeSession::calculateSkew() const noexcept {
    if (!firstAudioTimeTaken_ || lastDeviceAudioTime_.timescale == 0 ||
        lastDeviceAudioTime_.value <= firstDeviceAudioTime_.value ||
        lastLocalAudioMs_ <= firstLocalAudioMs_) {
        return 48000.0;
    }
    const double localSeconds = static_cast<double>(lastLocalAudioMs_ - firstLocalAudioMs_) /
                                1000.0;
    const double deviceSeconds =
        static_cast<double>(lastDeviceAudioTime_.value - firstDeviceAudioTime_.value) /
        lastDeviceAudioTime_.timescale;
    if (deviceSeconds <= 0.0) {
        return 48000.0;
    }
    const double result = lastDeviceAudioTime_.timescale * localSeconds / deviceSeconds;
    return std::max(47000.0, std::min(49000.0, result));
}

void QuickTimeSession::closeSession() noexcept {
    if (closing_ || !transport_.isOpen()) {
        return;
    }
    closing_ = true;
    try {
        if (deviceAudioClockRef_ != 0) {
            send(makeAsyncHeader(wire::kHpa0, deviceAudioClockRef_));
        }
        send(makeAsyncHeader(wire::kHpd0, wire::kEmptyCfType));

        unsigned releases = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        std::array<std::uint8_t, 64U * 1024U> buffer{};
        while (releases < 2 && std::chrono::steady_clock::now() < deadline) {
            const std::size_t count = transport_.readSome(buffer.data(), buffer.size(),
                                                          std::chrono::milliseconds(200));
            for (const Bytes& packet : framer_.push(buffer.data(), count)) {
                if (packet.size() >= 20 && readLe32(packet.data() + 4) == wire::kAsync &&
                    readLe32(packet.data() + 16) == 0x72656C73U) { // RELS
                    ++releases;
                }
            }
        }
        send(makeAsyncHeader(wire::kHpd0, wire::kEmptyCfType));
    } catch (...) {
        // Best-effort shutdown; the transport still closes deterministically.
    }
}

} // namespace valeria
