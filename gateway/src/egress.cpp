#include "roqr/gateway/egress.hpp"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "roqr/gateway/bridge.hpp"
#include "roqr/gateway/gap.hpp"
#include "roqr/gateway/rtmp_commands.hpp"
#include "roqr/quic/client.hpp"
#include "roqr/rtmp/classify.hpp"
#include "roqr/rtmp/server_session.hpp"

namespace roqr::gateway {

struct EgressGateway::Impl {
    EgressOptions options;
    roqr::rtmp::Listener listener;

    std::mutex mutex;
    std::condition_variable cv;
    bool playing = false;

    // Player session + init-frame cache, both guarded by player_mutex.
    // player_ready gates live delivery: the RTMP handshake and play must
    // complete (on_stream fires) before we write media to the fd.
    std::mutex player_mutex;
    roqr::rtmp::ServerSession* player = nullptr;
    bool player_ready = false;
    // Latest metadata + audio/video sequence headers, by message type, in
    // insertion order — replayed to a player when it starts playing so a
    // late-joining ffplay can decode even though play() was sent to the
    // relay before the player connected.
    std::vector<uint8_t> init_types;
    std::map<uint8_t, roqr::rtmp::RtmpMessage> init_cache;

    roqr::gateway::GapTracker gaps;  // draft s8, defined in gap.hpp

    // Declared last: ~Client joins the network thread before the members
    // its handlers touch are destroyed.
    roqr::quic::Client client;

    static bool is_init(const roqr::rtmp::RtmpMessage& msg) {
        if (msg.type == roqr::rtmp::kTypeDataAmf0 || msg.type == 15 /* AMF3 data */)
            return true;
        if (msg.type == roqr::rtmp::kTypeAudio ||
            msg.type == roqr::rtmp::kTypeVideo) {
            const auto info = msg.type == roqr::rtmp::kTypeVideo
                                  ? roqr::rtmp::classify_video(msg.payload)
                                  : roqr::rtmp::classify_audio(msg.payload);
            return info.cls == roqr::rtmp::MediaClass::SequenceHeader;
        }
        return false;
    }

    bool accept_video(const roqr::rtmp::RtmpMessage& msg) {
        return gaps.accept(msg.timestamp,
                          roqr::rtmp::classify_video(msg.payload).cls);
    }

    void on_frame(const roqr::Frame& f) {
        roqr::rtmp::RtmpMessage msg;
        if (!to_rtmp(f, msg)) return;
        if (msg.type == roqr::rtmp::kTypeCommandAmf0) return;  // relay replies
        if (msg.type == 9 && !accept_video(msg)) return;

        std::lock_guard lock(player_mutex);
        if (is_init(msg)) {
            if (init_cache.find(msg.type) == init_cache.end()) {
                init_types.push_back(msg.type);
            }
            init_cache[msg.type] = msg;
        }
        if (player != nullptr && player_ready) player->send(msg);
    }

    // Called on the session thread when the player issues play (after the
    // RTMP handshake). Primes it with the cached init frames, then opens
    // live delivery.
    void on_player_play() {
        std::lock_guard lock(player_mutex);
        if (player == nullptr) return;
        for (uint8_t type : init_types) player->send(init_cache.at(type));
        player_ready = true;
    }

    void begin_play() {
        roqr::quic::ClientOptions o;
        o.insecure_skip_verify = options.insecure_skip_verify;
        client.on_message([this](const roqr::Frame& f) { on_frame(f); });
        if (!client.connect(options.roqr_host, options.roqr_port, o)) return;
        if (!client.wait_connected(std::chrono::seconds(5))) return;
        client.send(to_frame(build_connect(1, "live", "rtmp://roqr/live"), 0),
                    roqr::quic::DeliveryMode::Stream);
        client.send(to_frame(build_create_stream(2), 0),
                    roqr::quic::DeliveryMode::Stream);
        client.send(to_frame(build_play(3, options.stream_name), 0),
                    roqr::quic::DeliveryMode::Stream);
        {
            std::lock_guard lock(mutex);
            playing = true;
        }
        cv.notify_all();
    }
};

EgressGateway::EgressGateway() : impl_(std::make_unique<Impl>()) {}
EgressGateway::~EgressGateway() { stop(); }

bool EgressGateway::start(const EgressOptions& options) {
    impl_->options = options;
    Impl* impl = impl_.get();
    impl->begin_play();  // connect + play before accepting the player
    return impl->listener.start(
        options.rtmp_port,
        [impl](roqr::rtmp::ServerSession& s) {
            {
                std::lock_guard lock(impl->player_mutex);
                impl->player = &s;
                impl->player_ready = false;
            }
            roqr::rtmp::SessionEvents e;
            e.on_stream = [impl](roqr::rtmp::ServerSession&,
                                 const std::string&, bool publishing) {
                if (!publishing) impl->on_player_play();  // a play request
            };
            e.on_close = [impl](roqr::rtmp::ServerSession& s) {
                std::lock_guard lock(impl->player_mutex);
                if (impl->player == &s) {
                    impl->player = nullptr;
                    impl->player_ready = false;
                }
            };
            return e;
        });
}

void EgressGateway::stop() {
    impl_->listener.stop();
    impl_->client.close();
    impl_->client.wait_closed(std::chrono::seconds(2));
}

bool EgressGateway::wait_playing(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait_for(lock, timeout, [&] { return impl_->playing; });
}

}  // namespace roqr::gateway
