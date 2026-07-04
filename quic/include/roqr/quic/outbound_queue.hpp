#pragma once

#include <deque>
#include <mutex>
#include <optional>

#include "roqr/frame.hpp"
#include "roqr/quic/delivery.hpp"

namespace roqr::quic {

struct Outbound {
    roqr::Frame frame;
    DeliveryMode mode;
};

// Thread-safe FIFO between app threads (push) and the network thread (pop).
class OutboundQueue {
public:
    void push(Outbound item) {
        std::lock_guard lock(mutex_);
        items_.push_back(std::move(item));
    }

    std::optional<Outbound> pop() {
        std::lock_guard lock(mutex_);
        if (items_.empty()) return std::nullopt;
        Outbound item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<Outbound> items_;
};

}  // namespace roqr::quic
