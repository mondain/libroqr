#include "roqr/gateway/ingest.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "roqr/gateway/bridge.hpp"
#include "roqr/gateway/connection_supervisor.hpp"
#include "roqr/gateway/rtmp_commands.hpp"
#include "roqr/quic/client.hpp"
#include "roqr/rtmp/classify.hpp"
#include "roqr/rtmp/server_session.hpp"

namespace roqr::gateway {

struct IngestGateway::Impl {
    IngestOptions options;
    roqr::rtmp::Listener listener;
    std::mutex mutex;
    std::condition_variable cv;
    bool publishing = false;

    // Declared last: the supervisor (and the Client it owns) is torn down
    // before the members its handlers touch are destroyed.
    std::unique_ptr<ConnectionSupervisor> supervisor;

    roqr::quic::DeliveryMode mode_for(const roqr::rtmp::RtmpMessage& msg) {
        if (msg.type != 8 && msg.type != 9) {
            return roqr::quic::DeliveryMode::Stream;  // commands, metadata
        }
        const auto info = msg.type == 9
                              ? roqr::rtmp::classify_video(msg.payload)
                              : roqr::rtmp::classify_audio(msg.payload);
        if (info.force_stream ||
            info.cls == roqr::rtmp::MediaClass::SequenceHeader ||
            info.cls == roqr::rtmp::MediaClass::Metadata ||
            info.cls == roqr::rtmp::MediaClass::Control) {
            return roqr::quic::DeliveryMode::Stream;
        }
        return roqr::quic::DeliveryMode::Auto;
    }

    // Idempotent: only the first publish event stands up the supervisor
    // (one publisher at a time, gateway-grade). Subsequent publish events
    // are ignored.
    void begin_publish(const std::string& name) {
        if (supervisor) return;
        roqr::quic::ClientOptions client_opts;
        client_opts.insecure_skip_verify = options.insecure_skip_verify;
        client_opts.idle_timeout = options.idle_timeout;
        supervisor = std::make_unique<ConnectionSupervisor>(
            options.roqr_host, options.roqr_port, client_opts,
            options.reconnect,
            [this, name](roqr::quic::Client& c) {
                // Runs on the supervisor thread on EVERY (re)connection: a
                // reconnect re-sends connect/createStream/publish so the
                // relay resumes accepting this stream's media.
                c.send(to_frame(build_connect(1, "live", "rtmp://roqr/live"),
                                0),
                       roqr::quic::DeliveryMode::Stream);
                c.send(to_frame(build_create_stream(2), 0),
                       roqr::quic::DeliveryMode::Stream);
                c.send(to_frame(build_publish(3, name), 0),
                       roqr::quic::DeliveryMode::Stream);
                {
                    std::lock_guard lock(mutex);
                    publishing = true;
                }
                cv.notify_all();
            },
            [](const roqr::Frame&) {});
        supervisor->start();
    }

    void forward(const roqr::rtmp::RtmpMessage& msg) {
        if (msg.payload.empty()) return;  // RoQR requires payload > 0
        if (supervisor) supervisor->send(to_frame(msg, 0), mode_for(msg));
    }
};

IngestGateway::IngestGateway() : impl_(std::make_unique<Impl>()) {}
IngestGateway::~IngestGateway() { stop(); }

bool IngestGateway::start(const IngestOptions& options) {
    impl_->options = options;
    Impl* impl = impl_.get();
    return impl_->listener.start(
        options.rtmp_port,
        [impl](roqr::rtmp::ServerSession&) {
            roqr::rtmp::SessionEvents e;
            e.on_stream = [impl](roqr::rtmp::ServerSession&,
                                 const std::string& name, bool publishing) {
                if (publishing) impl->begin_publish(name);
            };
            e.on_message = [impl](roqr::rtmp::ServerSession&,
                                  const roqr::rtmp::RtmpMessage& msg) {
                impl->forward(msg);
            };
            return e;
        });
}

void IngestGateway::stop() {
    impl_->listener.stop();  // stop the RTMP source first: no more forwards
    if (impl_->supervisor) impl_->supervisor->stop();
}

bool IngestGateway::wait_publishing(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait_for(lock, timeout, [&] { return impl_->publishing; });
}

}  // namespace roqr::gateway
