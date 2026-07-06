#include "roqr/gateway/egress.hpp"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "roqr/gateway/bridge.hpp"
#include "roqr/gateway/gap.hpp"
#include "roqr/gateway/player_queue.hpp"
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

    // Bounded queue + dedicated writer thread (draft s11): on_frame (QUIC
    // network thread) and on_player_play (session thread) only enqueue;
    // the writer thread does the blocking player->send, so a stalled RTMP
    // player never wedges the QUIC loop. Must be declared before `client`
    // (see below) so they're destroyed after it re: construction order,
    // but they're joined/closed explicitly in stop() well before that.
    static constexpr size_t kMaxQueuedMessages = 512;
    PlayerQueue queue{kMaxQueuedMessages};
    std::thread writer;
    bool writer_started = false;

    // Declared last: ~Client joins the network thread before the members
    // its handlers touch are destroyed.
    roqr::quic::Client client;

    void writer_loop() {
        for (;;) {
            auto msg = queue.pop();
            if (!msg) return;  // closed and drained
            roqr::rtmp::ServerSession* p = nullptr;
            {
                std::lock_guard lock(player_mutex);
                if (player_ready) p = player;
            }
            if (p != nullptr) p->send(*msg);
        }
    }

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

        const bool init = is_init(msg);
        {
            std::lock_guard lock(player_mutex);
            if (init) {
                if (init_cache.find(msg.type) == init_cache.end()) {
                    init_types.push_back(msg.type);
                }
                init_cache[msg.type] = msg;
            }
            if (!player_ready) return;  // pre-play: cache only, don't enqueue
        }
        queue.push(std::move(msg),
                   init ? PlayerQueue::Kind::Init : PlayerQueue::Kind::Coded);
    }

    // Called on the session thread when the player issues play (after the
    // RTMP handshake). Enqueues the cached init frames (so the writer
    // thread delivers them, not the session thread) and sets player_ready
    // under the lock before releasing, so on_frame cannot interleave a live
    // coded frame ahead of the init frames.
    void on_player_play() {
        std::lock_guard lock(player_mutex);
        if (player == nullptr) return;
        for (uint8_t type : init_types) {
            queue.push(init_cache.at(type), PlayerQueue::Kind::Init);
        }
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
    if (!impl->writer_started) {
        impl->writer = std::thread([impl] { impl->writer_loop(); });
        impl->writer_started = true;
    }
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
    // 1. Stop the QUIC network thread so on_frame stops enqueuing.
    impl_->client.close();
    impl_->client.wait_closed(std::chrono::seconds(2));
    // 2. Unblock a writer that may be stuck in a blocking send to a stalled
    //    player: shut down the player's fd so the send returns.
    {
        std::lock_guard lock(impl_->player_mutex);
        if (impl_->player != nullptr) impl_->player->close();
    }
    // 3. Close the queue and join the writer (it drains, sends fail fast on
    //    the shut-down fd, then exits).
    impl_->queue.close();
    if (impl_->writer.joinable()) impl_->writer.join();
    impl_->writer_started = false;
    // 4. Now no thread touches the player: safe to destroy the sessions.
    impl_->listener.stop();
}

uint64_t EgressGateway::frames_dropped() const {
    return impl_->queue.dropped();
}

bool EgressGateway::wait_playing(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait_for(lock, timeout, [&] { return impl_->playing; });
}

}  // namespace roqr::gateway
