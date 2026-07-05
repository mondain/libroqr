#include <catch2/catch_test_macros.hpp>

#include "roqr/relayd/media_router.hpp"

using namespace roqr::relayd;

namespace {
void* handle(uintptr_t v) { return reinterpret_cast<void*>(v); }
}  // namespace

TEST_CASE("subscribers are routed by stream name, publisher excluded") {
    MediaRouter r;
    r.register_publisher(handle(1), "cam");
    r.register_subscriber(handle(2), "cam");
    r.register_subscriber(handle(3), "cam");
    r.register_subscriber(handle(4), "other");

    auto subs = r.subscribers_for_publisher(handle(1));
    REQUIRE(subs.size() == 2);
    CHECK((subs[0] == handle(2) || subs[0] == handle(3)));
    CHECK(subs[0] != handle(1));

    CHECK(r.subscribers_for_publisher(handle(99)).empty());  // unknown
}

TEST_CASE("init frames replay in insertion order") {
    MediaRouter r;
    r.register_publisher(handle(1), "cam");
    r.cache_init("cam", 18, {0x01});  // metadata
    r.cache_init("cam", 9, {0x02});   // video seq header
    r.cache_init("cam", 8, {0x03});   // audio seq header
    // Re-caching the same message type replaces it.
    r.cache_init("cam", 9, {0x22});

    auto frames = r.init_frames("cam");
    REQUIRE(frames.size() == 3);
    CHECK(frames[0] == std::vector<uint8_t>{0x01});
    CHECK(frames[1] == std::vector<uint8_t>{0x22});
    CHECK(frames[2] == std::vector<uint8_t>{0x03});

    CHECK(r.init_frames("missing").empty());
}

TEST_CASE("remove drops publisher and subscriber state") {
    MediaRouter r;
    r.register_publisher(handle(1), "cam");
    r.register_subscriber(handle(2), "cam");
    CHECK(r.stream_of(handle(2)) == "cam");

    r.remove(handle(2));
    CHECK(r.subscribers_for_publisher(handle(1)).empty());
    CHECK(r.stream_of(handle(2)).empty());

    r.remove(handle(1));
    CHECK(r.stream_of(handle(1)).empty());
    r.register_subscriber(handle(3), "cam");
    CHECK(r.subscribers_for_publisher(handle(1)).empty());  // no publisher
}
