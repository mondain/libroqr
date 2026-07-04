#include "roqr/rtmp/amf0.hpp"

#include <bit>

namespace roqr::rtmp {

namespace {

constexpr uint8_t kNumber = 0x00;
constexpr uint8_t kBoolean = 0x01;
constexpr uint8_t kString = 0x02;
constexpr uint8_t kObject = 0x03;
constexpr uint8_t kNull = 0x05;
constexpr uint8_t kUndefined = 0x06;
constexpr uint8_t kEcmaArray = 0x08;
constexpr uint8_t kObjectEnd = 0x09;
constexpr uint8_t kStrictArray = 0x0A;
constexpr uint8_t kDate = 0x0B;
constexpr uint8_t kLongString = 0x0C;

void put_u16(uint16_t v, std::vector<uint8_t>& out) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_u32(uint32_t v, std::vector<uint8_t>& out) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_f64(double d, std::vector<uint8_t>& out) {
    const uint64_t b = std::bit_cast<uint64_t>(d);
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>(b >> shift));
    }
}

void put_property_name(const std::string& name, std::vector<uint8_t>& out) {
    put_u16(static_cast<uint16_t>(name.size()), out);
    out.insert(out.end(), name.begin(), name.end());
}

void put_properties(const Amf0Value& v, std::vector<uint8_t>& out) {
    for (size_t i = 0; i < v.property_count(); ++i) {
        put_property_name(v.key_at(i), out);
        amf0_encode(v.value_at(i), out);
    }
    put_u16(0, out);
    out.push_back(kObjectEnd);
}

}  // namespace

Amf0Value Amf0Value::number(double v) {
    Amf0Value r;
    r.type_ = Type::Number;
    r.number_ = v;
    return r;
}

Amf0Value Amf0Value::boolean(bool v) {
    Amf0Value r;
    r.type_ = Type::Boolean;
    r.boolean_ = v;
    return r;
}

Amf0Value Amf0Value::string(std::string v) {
    Amf0Value r;
    r.type_ = Type::String;
    r.string_ = std::move(v);
    return r;
}

Amf0Value Amf0Value::object() {
    Amf0Value r;
    r.type_ = Type::Object;
    return r;
}

Amf0Value Amf0Value::ecma_array() {
    Amf0Value r;
    r.type_ = Type::EcmaArray;
    return r;
}

Amf0Value Amf0Value::strict_array() {
    Amf0Value r;
    r.type_ = Type::StrictArray;
    return r;
}

Amf0Value Amf0Value::null() { return Amf0Value{}; }

Amf0Value Amf0Value::undefined() {
    Amf0Value r;
    r.type_ = Type::Undefined;
    return r;
}

Amf0Value Amf0Value::date(double ms, int16_t tz) {
    Amf0Value r;
    r.type_ = Type::Date;
    r.number_ = ms;
    r.tz_ = tz;
    return r;
}

const Amf0Value* Amf0Value::find(const std::string& key) const {
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) return &values_[i];
    }
    return nullptr;
}

Amf0Value& Amf0Value::set(std::string key, Amf0Value value) {
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            values_[i] = std::move(value);
            return *this;
        }
    }
    keys_.push_back(std::move(key));
    values_.push_back(std::move(value));
    return *this;
}

Amf0Value& Amf0Value::push(Amf0Value value) {
    values_.push_back(std::move(value));
    return *this;
}

void amf0_encode(const Amf0Value& value, std::vector<uint8_t>& out) {
    switch (value.type()) {
        case Amf0Value::Type::Number:
            out.push_back(kNumber);
            put_f64(value.as_number(), out);
            break;
        case Amf0Value::Type::Boolean:
            out.push_back(kBoolean);
            out.push_back(value.as_boolean() ? 1 : 0);
            break;
        case Amf0Value::Type::String:
            if (value.as_string().size() > 0xFFFF) {
                out.push_back(kLongString);
                put_u32(static_cast<uint32_t>(value.as_string().size()), out);
            } else {
                out.push_back(kString);
                put_u16(static_cast<uint16_t>(value.as_string().size()), out);
            }
            out.insert(out.end(), value.as_string().begin(),
                       value.as_string().end());
            break;
        case Amf0Value::Type::Object:
            out.push_back(kObject);
            put_properties(value, out);
            break;
        case Amf0Value::Type::EcmaArray:
            out.push_back(kEcmaArray);
            put_u32(static_cast<uint32_t>(value.property_count()), out);
            put_properties(value, out);
            break;
        case Amf0Value::Type::StrictArray:
            out.push_back(kStrictArray);
            put_u32(static_cast<uint32_t>(value.element_count()), out);
            for (size_t i = 0; i < value.element_count(); ++i) {
                amf0_encode(value.element_at(i), out);
            }
            break;
        case Amf0Value::Type::Null:
            out.push_back(kNull);
            break;
        case Amf0Value::Type::Undefined:
            out.push_back(kUndefined);
            break;
        case Amf0Value::Type::Date:
            out.push_back(kDate);
            put_f64(value.as_number(), out);
            put_u16(static_cast<uint16_t>(value.date_tz()), out);
            break;
    }
}

std::optional<size_t> amf0_decode(std::span<const uint8_t> data,
                                  Amf0Value& out) {
    // Implemented in the next task.
    (void)data;
    (void)out;
    return std::nullopt;
}

std::optional<std::vector<Amf0Value>> amf0_decode_all(
    std::span<const uint8_t> data) {
    (void)data;
    return std::nullopt;
}

}  // namespace roqr::rtmp
