#include "roqr/gateway/connection_supervisor.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace roqr::gateway {

struct ConnectionSupervisor::Impl {
    std::string host;
    uint16_t port = 0;
    roqr::quic::ClientOptions client_opts;
    ReconnectPolicy policy;
    ReadyHandler on_ready;
    MessageHandler on_message;

    std::mutex mutex;
    std::condition_variable cv;
    bool stopping = false;
    bool started = false;
    bool drop_signaled = false;
    std::unique_ptr<roqr::quic::Client> client;

    bool connected = false;
    bool failed = false;
    uint64_t reconnect_count = 0;

    std::thread thread;

    // initial_backoff * 2^(attempts-1), clamped so the doubling can never
    // overflow: the loop stops as soon as the running delay reaches
    // max_backoff, and the exponent itself is capped defensively.
    std::chrono::milliseconds compute_backoff(unsigned attempts) const {
        auto delay = std::min(policy.initial_backoff, policy.max_backoff);
        if (attempts <= 1) return delay;
        const unsigned exponent = std::min(attempts - 1, 62u);
        for (unsigned i = 0; i < exponent; ++i) {
            if (delay >= policy.max_backoff) return policy.max_backoff;
            delay *= 2;
        }
        return std::min(delay, policy.max_backoff);
    }

    // Handles one failed connect/serve attempt: bumps the attempt counter,
    // gives up if the policy says so, otherwise waits out an interruptible
    // backoff. Returns false if the run loop should break (stop requested or
    // gave up), true if it should retry.
    bool handle_failed_attempt(unsigned& attempts) {
        std::unique_lock<std::mutex> lock(mutex);
        if (stopping) return false;
        ++attempts;
        if (policy.max_attempts != 0 && attempts >= policy.max_attempts) {
            failed = true;
            cv.notify_all();
            return false;
        }
        const auto delay = compute_backoff(attempts);
        cv.wait_for(lock, delay, [&] { return stopping; });
        return true;
    }

    void run() {
        bool first_connect = true;
        unsigned attempts = 0;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping) break;
            }

            auto c = std::make_unique<roqr::quic::Client>();
            c->on_message(on_message);
            c->on_closed([this](uint64_t /*app_error_code*/) {
                // Runs on the (possibly dying) network thread: only signal,
                // never touch the Client here.
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    drop_signaled = true;
                }
                cv.notify_all();
            });

            // Reset drop_signaled exactly once per iteration, for THIS fresh
            // client, before connect() can start its network thread (so
            // before on_closed can possibly fire for it). The previous
            // client, if any, was already destroyed at the end of the prior
            // iteration (dead.reset() joined its network thread), so no
            // stale on_closed from it can still be in flight here. From this
            // point on, every drop_signaled=true belongs to the current
            // client and must be preserved until observed -- in particular,
            // the success path below must NOT clear it again, or a
            // connection that dies right after coming up (or during
            // wait_connected itself) would have its drop signal erased and
            // the subsequent cv.wait() would block forever.
            {
                std::lock_guard<std::mutex> lock(mutex);
                drop_signaled = false;
            }

            if (!c->connect(host, port, client_opts)) {
                c.reset();
                if (!handle_failed_attempt(attempts)) break;
                continue;
            }

            // Publish before wait_connected so stop() can close() it to
            // unblock a pending handshake promptly.
            roqr::quic::Client* cur = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex);
                client = std::move(c);
                cur = client.get();
                // stop() may have run between the top-of-loop stopping check
                // and this publish (e.g. while connect() was in flight), and
                // found client == nullptr, so it couldn't close() anything.
                // Re-check under the same lock stop() uses: whichever of
                // stop()/publish runs second sees the other's state, so
                // close() is always invoked on the published client if
                // stopping was ever set. close() is non-blocking (just sets
                // a flag and wakes the network thread), so it's safe here.
                if (stopping) cur->close();
            }

            const bool up = cur->wait_connected(policy.connect_timeout);

            {
                bool stop_now = false;
                std::unique_ptr<roqr::quic::Client> dead;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopping) {
                        dead = std::move(client);
                        stop_now = true;
                    }
                }
                // Destroy outside the lock: ~Client joins the network
                // thread, which may synchronously invoke the on_closed
                // lambda above (it takes this same mutex).
                dead.reset();
                if (stop_now) break;
            }

            if (!up) {
                std::unique_ptr<roqr::quic::Client> dead;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    dead = std::move(client);
                }
                dead.reset();
                if (!handle_failed_attempt(attempts)) break;
                continue;
            }

            // SUCCESS. Do NOT reset drop_signaled here: it was already reset
            // once for this client before connect() (see above), and an
            // on_closed for this same client may have already fired (e.g.
            // during wait_connected) and set it -- that signal must survive
            // so the cv.wait() below observes it immediately instead of
            // blocking forever on a connection that is already dead.
            {
                std::lock_guard<std::mutex> lock(mutex);
                connected = true;
                if (!first_connect) ++reconnect_count;
            }
            first_connect = false;
            attempts = 0;
            cv.notify_all();

            on_ready(*cur);  // sends the handshake on the supervisor thread

            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return drop_signaled || stopping; });
            }

            bool should_stop = false;
            {
                std::unique_ptr<roqr::quic::Client> dead;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    connected = false;
                    dead = std::move(client);
                    should_stop = stopping;
                }
                dead.reset();  // joins the network thread: safe here, this
                               // is the supervisor thread, not the network
                               // thread.
            }
            if (should_stop) break;
            // Drop => immediate retry, no backoff.
        }

        std::unique_ptr<roqr::quic::Client> dead;
        {
            std::lock_guard<std::mutex> lock(mutex);
            connected = false;
            if (client) dead = std::move(client);
        }
        dead.reset();
    }
};

ConnectionSupervisor::ConnectionSupervisor(std::string host, uint16_t port,
                                           roqr::quic::ClientOptions opts,
                                           ReconnectPolicy policy,
                                           ReadyHandler on_ready,
                                           MessageHandler on_message)
    : impl_(std::make_unique<Impl>()) {
    impl_->host = std::move(host);
    impl_->port = port;
    impl_->client_opts = std::move(opts);
    impl_->policy = policy;
    impl_->on_ready = std::move(on_ready);
    impl_->on_message = std::move(on_message);
}

ConnectionSupervisor::~ConnectionSupervisor() { stop(); }

void ConnectionSupervisor::start() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->started) return;
    impl_->started = true;
    Impl* impl = impl_.get();
    impl_->thread = std::thread([impl] { impl->run(); });
}

void ConnectionSupervisor::stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->started) return;
        impl_->stopping = true;
        if (impl_->client) impl_->client->close();
        impl_->cv.notify_all();
    }
    if (impl_->thread.joinable()) impl_->thread.join();
}

bool ConnectionSupervisor::send(roqr::Frame frame, roqr::quic::DeliveryMode mode) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->client || !impl_->connected) return false;
    return impl_->client->send(std::move(frame), mode);
}

bool ConnectionSupervisor::connected() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->connected;
}

bool ConnectionSupervisor::failed() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->failed;
}

uint64_t ConnectionSupervisor::reconnect_count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->reconnect_count;
}

}  // namespace roqr::gateway
