#include "roqr/gateway/player_queue.hpp"

#include <algorithm>

namespace roqr::gateway {

bool PlayerQueue::push(roqr::rtmp::RtmpMessage msg, Kind kind) {
    std::lock_guard lock(mutex_);
    if (closed_) return false;
    bool dropped_one = false;
    if (queue_.size() >= max_) {
        auto victim = std::find_if(queue_.begin(), queue_.end(),
                                   [](const Entry& e) {
                                       return e.kind == Kind::Coded;
                                   });
        if (victim != queue_.end()) {
            queue_.erase(victim);
            ++dropped_;
            dropped_one = true;
        } else {
            // Only Init entries and still full: drop the incoming rather
            // than exceed the bound.
            ++dropped_;
            return false;
        }
    }
    queue_.push_back(Entry{std::move(msg), kind});
    cv_.notify_one();
    return !dropped_one;
}

std::optional<roqr::rtmp::RtmpMessage> PlayerQueue::pop() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return !queue_.empty() || closed_; });
    if (queue_.empty()) return std::nullopt;  // closed and drained
    roqr::rtmp::RtmpMessage m = std::move(queue_.front().msg);
    queue_.pop_front();
    return m;
}

void PlayerQueue::close() {
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    cv_.notify_all();
}

size_t PlayerQueue::size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

uint64_t PlayerQueue::dropped() const {
    std::lock_guard lock(mutex_);
    return dropped_;
}

void PlayerQueue::clear() {
    std::lock_guard lock(mutex_);
    queue_.clear();
}

}  // namespace roqr::gateway
