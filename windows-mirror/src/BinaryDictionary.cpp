#include "valeria/BinaryDictionary.hpp"

#include <stdexcept>

namespace valeria {
namespace {

constexpr std::uint32_t kKeyValue = 0x6B657976U;
constexpr std::uint32_t kStringKey = 0x7374726BU;
constexpr std::uint32_t kBoolean = 0x62756C76U;
constexpr std::uint32_t kDictionary = 0x64696374U;
constexpr std::uint32_t kData = 0x64617476U;
constexpr std::uint32_t kString = 0x73747276U;
constexpr std::uint32_t kNumber = 0x6E6D6276U;

Bytes section(std::uint32_t magic, const Bytes& payload) {
    if (payload.size() > UINT32_MAX - 8U) {
        throw std::length_error("dictionary section is too large");
    }
    Bytes result;
    result.reserve(payload.size() + 8U);
    appendLe32(result, static_cast<std::uint32_t>(payload.size() + 8U));
    appendLe32(result, magic);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

Bytes stringSection(std::uint32_t magic, const std::string& value) {
    return section(magic, Bytes(value.begin(), value.end()));
}

Bytes serializeValue(const DictionaryValue& value) {
    Bytes payload;
    switch (value.kind) {
    case DictionaryValue::Kind::Boolean:
        payload.push_back(value.boolean ? 1U : 0U);
        return section(kBoolean, payload);
    case DictionaryValue::Kind::UInt32:
        payload.push_back(3U);
        appendLe32(payload, value.uint32);
        return section(kNumber, payload);
    case DictionaryValue::Kind::UInt64:
        payload.push_back(4U);
        appendLe64(payload, value.uint64);
        return section(kNumber, payload);
    case DictionaryValue::Kind::Float64:
        payload.push_back(6U);
        appendLeDouble(payload, value.float64);
        return section(kNumber, payload);
    case DictionaryValue::Kind::String:
        return stringSection(kString, value.string);
    case DictionaryValue::Kind::Data:
        return section(kData, value.data);
    case DictionaryValue::Kind::Dictionary:
        if (!value.dictionary) {
            throw std::invalid_argument("null nested dictionary");
        }
        return serializeDictionary(*value.dictionary);
    }
    throw std::invalid_argument("unknown dictionary value kind");
}

Bytes defaultAsbd() {
    Bytes result;
    result.reserve(56);
    appendLeDouble(result, 48000.0);
    appendLe32(result, 0x6C70636DU); // lpcm
    appendLe32(result, 12U);         // signed integer + packed
    appendLe32(result, 4U);
    appendLe32(result, 1U);
    appendLe32(result, 4U);
    appendLe32(result, 2U);
    appendLe32(result, 16U);
    appendLe32(result, 0U);
    appendLeDouble(result, 48000.0);
    appendLeDouble(result, 48000.0);
    return result;
}

} // namespace

DictionaryValue DictionaryValue::fromBoolean(bool value) {
    DictionaryValue result;
    result.kind = Kind::Boolean;
    result.boolean = value;
    return result;
}

DictionaryValue DictionaryValue::fromUInt32(std::uint32_t value) {
    DictionaryValue result;
    result.kind = Kind::UInt32;
    result.uint32 = value;
    return result;
}

DictionaryValue DictionaryValue::fromUInt64(std::uint64_t value) {
    DictionaryValue result;
    result.kind = Kind::UInt64;
    result.uint64 = value;
    return result;
}

DictionaryValue DictionaryValue::fromFloat64(double value) {
    DictionaryValue result;
    result.kind = Kind::Float64;
    result.float64 = value;
    return result;
}

DictionaryValue DictionaryValue::fromString(std::string value) {
    DictionaryValue result;
    result.kind = Kind::String;
    result.string = std::move(value);
    return result;
}

DictionaryValue DictionaryValue::fromData(Bytes value) {
    DictionaryValue result;
    result.kind = Kind::Data;
    result.data = std::move(value);
    return result;
}

DictionaryValue DictionaryValue::fromDictionary(Dictionary value) {
    DictionaryValue result;
    result.kind = Kind::Dictionary;
    result.dictionary = std::make_shared<Dictionary>(std::move(value));
    return result;
}

Bytes serializeDictionary(const Dictionary& dictionary) {
    Bytes payload;
    for (const auto& entry : dictionary) {
        Bytes pairPayload = stringSection(kStringKey, entry.first);
        Bytes encodedValue = serializeValue(entry.second);
        pairPayload.insert(pairPayload.end(), encodedValue.begin(), encodedValue.end());
        Bytes encodedPair = section(kKeyValue, pairPayload);
        payload.insert(payload.end(), encodedPair.begin(), encodedPair.end());
    }
    return section(kDictionary, payload);
}

Dictionary makeVideoRequestDictionary(std::uint32_t width, std::uint32_t height,
                                      bool advertiseHevc) {
    Dictionary displaySize{
        {"Width", DictionaryValue::fromFloat64(static_cast<double>(width))},
        {"Height", DictionaryValue::fromFloat64(static_cast<double>(height))},
    };

    // This order and the H264 capability key match recent Apple-host traces.
    Dictionary result{
        {"DisplaySize", DictionaryValue::fromDictionary(std::move(displaySize))},
    };
    // iOS 18 expects this key to exist even when the host wants AVC output.
    result.emplace_back("HEVCDecoderSupports444",
                        DictionaryValue::fromBoolean(advertiseHevc));
    result.emplace_back("H264DecoderSupports444", DictionaryValue::fromBoolean(true));
    result.emplace_back("Valeria", DictionaryValue::fromBoolean(true));
    return result;
}

Dictionary makeAudioRequestDictionary() {
    return {
        {"BufferAheadInterval", DictionaryValue::fromFloat64(0.07300000000000001)},
        {"deviceUID", DictionaryValue::fromString("Valeria")},
        {"ScreenLatency", DictionaryValue::fromFloat64(0.04)},
        {"formats", DictionaryValue::fromData(defaultAsbd())},
        {"EDIDAC3Support", DictionaryValue::fromUInt32(0)},
        {"deviceName", DictionaryValue::fromString("Valeria")},
    };
}

Dictionary makeSuccessDictionary() {
    return {{"Error", DictionaryValue::fromUInt32(0)}};
}

} // namespace valeria
