#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace roqr::rtmp {

// AMF0 value covering the types RTMP command traffic uses. Objects and
// ECMA arrays are ordered name/value sequences (parallel keys_/values_
// storage keeps the recursive type fully standard); strict arrays are
// value sequences stored in values_.
class Amf0Value {
public:
    enum class Type {
        Number,
        Boolean,
        String,
        Object,
        EcmaArray,
        StrictArray,
        Null,
        Undefined,
        Date,
    };

    Amf0Value() = default;  // Null

    static Amf0Value number(double v);
    static Amf0Value boolean(bool v);
    static Amf0Value string(std::string v);
    static Amf0Value object();
    static Amf0Value ecma_array();
    static Amf0Value strict_array();
    static Amf0Value null();
    static Amf0Value undefined();
    static Amf0Value date(double ms, int16_t tz = 0);

    Type type() const { return type_; }
    double as_number() const { return number_; }
    bool as_boolean() const { return boolean_; }
    const std::string& as_string() const { return string_; }
    int16_t date_tz() const { return tz_; }

    // Object / EcmaArray properties (ordered).
    size_t property_count() const { return keys_.size(); }
    const std::string& key_at(size_t i) const { return keys_[i]; }
    const Amf0Value& value_at(size_t i) const { return values_[i]; }
    const Amf0Value* find(const std::string& key) const;
    // Valid only on Object/EcmaArray values (asserted in debug builds).
    Amf0Value& set(std::string key, Amf0Value value);

    // StrictArray elements.
    size_t element_count() const { return values_.size(); }
    const Amf0Value& element_at(size_t i) const { return values_[i]; }
    // Valid only on StrictArray values (asserted in debug builds).
    Amf0Value& push(Amf0Value value);

    bool operator==(const Amf0Value&) const = default;

private:
    Type type_ = Type::Null;
    double number_ = 0;
    bool boolean_ = false;
    int16_t tz_ = 0;
    std::string string_;
    std::vector<std::string> keys_;
    std::vector<Amf0Value> values_;
};

// Appends the AMF0 encoding of value to out. Strings longer than 65535
// bytes encode as Long String (marker 0x0C).
void amf0_encode(const Amf0Value& value, std::vector<uint8_t>& out);

// Decodes one AMF0 value from the front of data. Returns the consumed
// byte count, or nullopt on truncated/malformed input (unknown markers,
// nesting deeper than 32 levels).
std::optional<size_t> amf0_decode(std::span<const uint8_t> data,
                                  Amf0Value& out);

// Decodes consecutive AMF0 values until data is exhausted (RTMP command
// payload form). Returns nullopt if any value fails to decode.
std::optional<std::vector<Amf0Value>> amf0_decode_all(
    std::span<const uint8_t> data);

}  // namespace roqr::rtmp
