#include "roqr/gateway/ingest.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "roqr/gateway/bridge.hpp"
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

    // Declared last: ~Client joins the network thread before the members
    // its handlers touch are destroyed.
    roqr::quic::Client client;

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

    void begin_publish(const std::string& name) {
        if (!client.connect(options.roqr_host, options.roqr_port,
                            [&] {
                                roqr::quic::ClientOptions o;
                                o.insecure_skip_verify =
                                    options.insecure_skip_verify;
                                return o;
                            }())) {
            return;
        }
        if (!client.wait_connected(std::chrono::seconds(5))) return;
        client.send(to_frame(build_connect(1, "live", "rtmp://roqr/live"), 0),
                    roqr::quic::DeliveryMode::Stream);
        client.send(to_frame(build_create_stream(2), 0),
                    roqr::quic::DeliveryMode::Stream);
        client.send(to_frame(build_publish(3, name), 0),
                    roqr::quic::DeliveryMode::Stream);
        {
            std::lock_guard lock(mutex);
            publishing = true;
        }
        cv.notify_all();
    }

    void forward(const roqr::rtmp::RtmpMessage& msg) {
        if (msg.payload.empty()) return;  // RoQR requires payload > 0
        client.send(to_frame(msg, 0), mode_for(msg));
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
    impl_->listener.stop();
    impl_->client.close();
    impl_->client.wait_closed(std::chrono::seconds(2));
}

bool IngestGateway::wait_publishing(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait_for(lock, timeout, [&] { return impl_->publishing; });
}

}  // namespace roqr::gateway
