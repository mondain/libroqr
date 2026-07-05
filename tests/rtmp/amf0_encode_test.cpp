#include <catch2/catch_test_macros.hpp>

#include "roqr/rtmp/amf0.hpp"

using namespace roqr::rtmp;

TEST_CASE("amf0 encodes scalars to known bytes") {
    std::vector<uint8_t> out;

    amf0_encode(Amf0Value::number(1.0), out);
    CHECK(out == std::vector<uint8_t>{0x00, 0x3F, 0xF0, 0, 0, 0, 0, 0, 0});

    out.clear();
    amf0_encode(Amf0Value::boolean(true), out);
    CHECK(out == std::vector<uint8_t>{0x01, 0x01});

    out.clear();
    amf0_encode(Amf0Value::string("connect"), out);
    CHECK(out == std::vector<uint8_t>{0x02, 0x00, 0x07, 'c', 'o', 'n', 'n',
                                      'e', 'c', 't'});

    out.clear();
    amf0_encode(Amf0Value::null(), out);
    CHECK(out == std::vector<uint8_t>{0x05});

    out.clear();
    amf0_encode(Amf0Value::undefined(), out);
    CHECK(out == std::vector<uint8_t>{0x06});
}

TEST_CASE("amf0 encodes an object with end marker") {
    Amf0Value obj = Amf0Value::object();
    obj.set("app", Amf0Value::string("live"));

    std::vector<uint8_t> out;
    amf0_encode(obj, out);
    const std::vector<uint8_t> expected = {
        0x03,                                     // object marker
        0x00, 0x03, 'a',  'p',  'p',              // property name
        0x02, 0x00, 0x04, 'l',  'i',  'v', 'e',   // string value
        0x00, 0x00, 0x09,                         // object end
    };
    CHECK(out == expected);
}

TEST_CASE("amf0 encodes ecma array with count and end marker") {
    Amf0Value arr = Amf0Value::ecma_array();
    arr.set("n", Amf0Value::number(2.0));

    std::vector<uint8_t> out;
    amf0_encode(arr, out);
    const std::vector<uint8_t> expected = {
        0x08, 0x00, 0x00, 0x00, 0x01,             // marker + count 1
        0x00, 0x01, 'n',                          // name
        0x00, 0x40, 0x00, 0, 0, 0, 0, 0, 0,       // number 2.0
        0x00, 0x00, 0x09,                         // end
    };
    CHECK(out == expected);
}

TEST_CASE("amf0 encodes strict array and date") {
    Amf0Value arr = Amf0Value::strict_array();
    arr.push(Amf0Value::number(1.0));
    arr.push(Amf0Value::boolean(false));

    std::vector<uint8_t> out;
    amf0_encode(arr, out);
    const std::vector<uint8_t> expected_arr = {
        0x0A, 0x00, 0x00, 0x00, 0x02,             // marker + count 2
        0x00, 0x3F, 0xF0, 0, 0, 0, 0, 0, 0,       // number 1.0
        0x01, 0x00,                               // boolean false
    };
    CHECK(out == expected_arr);

    out.clear();
    amf0_encode(Amf0Value::date(1.0, 0), out);
    const std::vector<uint8_t> expected_date = {
        0x0B, 0x3F, 0xF0, 0, 0, 0, 0, 0, 0, 0x00, 0x00,  // ms + tz 0
    };
    CHECK(out == expected_date);
}

TEST_CASE("set replaces an existing property") {
    Amf0Value obj = Amf0Value::object();
    obj.set("k", Amf0Value::number(1.0));
    obj.set("k", Amf0Value::number(2.0));
    REQUIRE(obj.property_count() == 1);
    CHECK(obj.value_at(0).as_number() == 2.0);
    REQUIRE(obj.find("k") != nullptr);
    CHECK(obj.find("missing") == nullptr);
}
