#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "roqr/rtmp/message.hpp"

namespace roqr::gateway {

// Thread-safe bounded queue between the RoQR receive thread (producer) and
// the egress writer thread (consumer). When full, the oldest Coded entry is
// evicted so decoder config (metadata + sequence headers, Kind::Init) is
// never dropped while stale coded media is (draft s11). close() unblocks a
// waiting consumer so the writer thread can exit.
class PlayerQueue {
public:
    enum class Kind { Init, Coded };  // Init = metadata / sequence header

    explicit PlayerQueue(size_t max_messages) : max_(max_messages) {}

    // Enqueue. On overflow, drop the oldest Coded entry to make room; if the
    // queue holds only Init entries (degenerate) drop the incoming Coded
    // message instead of growing. Returns false if the message was dropped
    // (either the incoming one or the evicted victim's slot is the caller's
    // signal that a drop happened). Never blocks.
    bool push(roqr::rtmp::RtmpMessage msg, Kind kind);

    // Block until a message is available or the queue is closed. Returns
    // nullopt only when the queue is closed AND drained.
    std::optional<roqr::rtmp::RtmpMessage> pop();

    void close();
    size_t size() const;
    uint64_t dropped() const;

    // Drop all queued entries without counting them toward dropped() — used
    // when installing a new player so stale prior-era frames don't linger
    // ahead of the new player's init frames; this is a deliberate flush, not
    // the loss dropped() tracks.
    void clear();

private:
    struct Entry {
        roqr::rtmp::RtmpMessage msg;
        Kind kind;
    };
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Entry> queue_;
    size_t max_;
    uint64_t dropped_ = 0;
    bool closed_ = false;
};

}  // namespace roqr::gateway
