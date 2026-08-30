#pragma once

#include "valeria/ByteIO.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace valeria {

struct DictionaryValue;
using Dictionary = std::vector<std::pair<std::string, DictionaryValue>>;

struct DictionaryValue {
    enum class Kind { Boolean, UInt32, UInt64, Float64, String, Data, Dictionary };

    Kind kind = Kind::Boolean;
    bool boolean = false;
    std::uint32_t uint32 = 0;
    std::uint64_t uint64 = 0;
    double float64 = 0.0;
    std::string string;
    Bytes data;
    std::shared_ptr<Dictionary> dictionary;

    static DictionaryValue fromBoolean(bool value);
    static DictionaryValue fromUInt32(std::uint32_t value);
    static DictionaryValue fromUInt64(std::uint64_t value);
    static DictionaryValue fromFloat64(double value);
    static DictionaryValue fromString(std::string value);
    static DictionaryValue fromData(Bytes value);
    static DictionaryValue fromDictionary(Dictionary value);
};

Bytes serializeDictionary(const Dictionary& dictionary);
Dictionary makeVideoRequestDictionary(std::uint32_t width, std::uint32_t height,
                                      bool advertiseHevc);
Dictionary makeAudioRequestDictionary();
Dictionary makeSuccessDictionary();

} // namespace valeria
