#include <catch2/catch_test_macros.hpp>

#include "roqr/rtmp/amf0.hpp"

using namespace roqr::rtmp;

namespace {
Amf0Value round_trip(const Amf0Value& v, size_t* consumed = nullptr) {
    std::vector<uint8_t> wire;
    amf0_encode(v, wire);
    Amf0Value out;
    auto n = amf0_decode(wire, out);
    REQUIRE(n.has_value());
    if (consumed != nullptr) *consumed = *n;
    REQUIRE(*n == wire.size());
    return out;
}
}  // namespace

TEST_CASE("amf0 round-trips every type") {
    CHECK(round_trip(Amf0Value::number(3.5)) == Amf0Value::number(3.5));
    CHECK(round_trip(Amf0Value::boolean(true)) == Amf0Value::boolean(true));
    CHECK(round_trip(Amf0Value::string("publish")) ==
          Amf0Value::string("publish"));
    CHECK(round_trip(Amf0Value::null()) == Amf0Value::null());
    CHECK(round_trip(Amf0Value::undefined()) == Amf0Value::undefined());
    CHECK(round_trip(Amf0Value::date(123456.0, -300)) ==
          Amf0Value::date(123456.0, -300));

    Amf0Value obj = Amf0Value::object();
    obj.set("app", Amf0Value::string("live"))
        .set("tcUrl", Amf0Value::string("rtmp://h/live"));
    Amf0Value nested = Amf0Value::object();
    nested.set("inner", obj);
    CHECK(round_trip(nested) == nested);

    Amf0Value arr = Amf0Value::strict_array();
    arr.push(Amf0Value::number(1)).push(Amf0Value::string("x"));
    CHECK(round_trip(arr) == arr);
}

TEST_CASE("ecma array decode trusts the end marker over the count") {
    // Count says 99 but only one property precedes the end marker.
    const std::vector<uint8_t> wire = {
        0x08, 0x00, 0x00, 0x00, 0x63,             // count 99 (a lie)
        0x00, 0x01, 'n',  0x00, 0x40, 0x00, 0, 0, 0, 0, 0, 0,
        0x00, 0x00, 0x09,
    };
    Amf0Value out;
    auto n = amf0_decode(wire, out);
    REQUIRE(n.has_value());
    CHECK(*n == wire.size());
    REQUIRE(out.type() == Amf0Value::Type::EcmaArray);
    REQUIRE(out.property_count() == 1);
    CHECK(out.value_at(0).as_number() == 2.0);
}

TEST_CASE("long string decodes to String") {
    std::string big(70000, 'x');
    Amf0Value v = Amf0Value::string(big);
    CHECK(round_trip(v).as_string() == big);
}

TEST_CASE("truncated input returns nullopt at every prefix") {
    Amf0Value obj = Amf0Value::object();
    obj.set("k", Amf0Value::number(1.0));
    std::vector<uint8_t> wire;
    amf0_encode(obj, wire);

    Amf0Value out;
    for (size_t len = 0; len < wire.size(); ++len) {
        CHECK_FALSE(
            amf0_decode(std::span<const uint8_t>(wire.data(), len), out)
                .has_value());
    }
}

TEST_CASE("unknown marker and depth bomb are rejected") {
    const uint8_t unknown[] = {0x0F, 0x00};
    Amf0Value out;
    CHECK_FALSE(amf0_decode(unknown, out).has_value());

    // 40 nested objects each holding property "a" -> exceeds depth 32.
    std::vector<uint8_t> bomb;
    for (int i = 0; i < 40; ++i) {
        bomb.push_back(0x03);
        bomb.push_back(0x00);
        bomb.push_back(0x01);
        bomb.push_back('a');
    }
    CHECK_FALSE(amf0_decode(bomb, out).has_value());
}

TEST_CASE("decode_all parses a command payload sequence") {
    std::vector<uint8_t> wire;
    amf0_encode(Amf0Value::string("connect"), wire);
    amf0_encode(Amf0Value::number(1.0), wire);
    Amf0Value obj = Amf0Value::object();
    obj.set("app", Amf0Value::string("live"));
    amf0_encode(obj, wire);

    auto values = amf0_decode_all(wire);
    REQUIRE(values.has_value());
    REQUIRE(values->size() == 3);
    CHECK((*values)[0].as_string() == "connect");
    CHECK((*values)[1].as_number() == 1.0);
    REQUIRE((*values)[2].find("app") != nullptr);
    CHECK((*values)[2].find("app")->as_string() == "live");

    wire.push_back(0x0F);  // trailing garbage marker
    CHECK_FALSE(amf0_decode_all(wire).has_value());
}
