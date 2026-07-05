#include "roqr/rtmp/chunk_reader.hpp"

#include <algorithm>

namespace roqr::rtmp {

namespace {
uint32_t be24(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 16 |
           static_cast<uint32_t>(p[1]) << 8 | p[2];
}
uint32_t be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 |
           static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | p[3];
}
uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[3]) << 24 |
           static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[1]) << 8 | p[0];
}
}  // namespace

void ChunkReader::feed(std::span<const uint8_t> data) {
    if (failed_) return;
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    parse();
}

std::optional<RtmpMessage> ChunkReader::next() {
    if (ready_.empty()) return std::nullopt;
    RtmpMessage m = std::move(ready_.front());
    ready_.pop_front();
    return m;
}

void ChunkReader::finalize(uint32_t csid, CsidState& st) {
    RtmpMessage m;
    m.timestamp = st.timestamp;
    m.type = st.type;
    m.message_stream_id = st.message_stream_id;
    m.chunk_stream_id = csid;
    assembling_bytes_ -= std::min<uint64_t>(assembling_bytes_, st.assembling.size());
    m.payload = std::move(st.assembling);
    st.assembling.clear();

    if (m.type == kTypeSetChunkSize && m.payload.size() >= 4) {
        const uint32_t size = be32(m.payload.data());
        if ((size & 0x80000000u) != 0 || size == 0) {
            failed_ = true;
            return;
        }
        chunk_size_ = size;
    } else if (m.type == kTypeAbort && m.payload.size() >= 4) {
        const uint32_t target = be32(m.payload.data());
        auto it = streams_.find(target);
        if (it != streams_.end()) {
            // The committed budget for this csid's incomplete assembly is
            // its declared length: bytes already buffered plus bytes still
            // remaining to arrive.
            const uint64_t committed =
                it->second.assembling.size() + it->second.remaining;
            assembling_bytes_ -= std::min<uint64_t>(assembling_bytes_, committed);
            it->second.assembling.clear();
            it->second.remaining = 0;
            it->second.have_header = false;
        }
    }
    ready_.push_back(std::move(m));
}

void ChunkReader::parse() {
    for (;;) {
        if (failed_ || buffer_.empty()) return;

        size_t pos = 0;
        const uint8_t b0 = buffer_[0];
        const uint8_t fmt = b0 >> 6;
        uint32_t csid = b0 & 0x3F;
        pos = 1;
        if (csid == 0) {
            if (buffer_.size() < 2) return;
            csid = 64 + buffer_[1];
            pos = 2;
        } else if (csid == 1) {
            if (buffer_.size() < 3) return;
            csid = 64 + buffer_[1] + static_cast<uint32_t>(buffer_[2]) * 256;
            pos = 3;
        }

        const size_t mh_len = fmt == 0 ? 11 : fmt == 1 ? 7 : fmt == 2 ? 3 : 0;
        if (buffer_.size() < pos + mh_len) return;

        CsidState& st = streams_[csid];
        if (fmt == 3 && !st.have_header) {
            failed_ = true;
            return;
        }

        const bool starting_new = st.remaining == 0;
        uint32_t ts_field = 0;
        bool extended = st.extended;  // fmt3 inherits
        if (fmt <= 2) {
            ts_field = be24(buffer_.data() + pos);
            extended = ts_field == 0xFFFFFF;
        }

        size_t ext_pos = pos + mh_len;
        if (extended && buffer_.size() < ext_pos + 4) return;
        uint32_t ext_value = 0;
        size_t header_total = ext_pos;
        if (extended) {
            ext_value = be32(buffer_.data() + ext_pos);
            header_total += 4;
        }

        // Parse the fmt0/fmt1 header fields into locals only; nothing on
        // `st` mutates until the full chunk body is confirmed buffered.
        uint32_t new_length = st.length;
        uint8_t new_type = st.type;
        uint32_t new_msid = st.message_stream_id;
        if (fmt == 0) {
            new_length = be24(buffer_.data() + pos + 3);
            new_type = buffer_[pos + 6];
            new_msid = le32(buffer_.data() + pos + 7);
        } else if (fmt == 1) {
            new_length = be24(buffer_.data() + pos + 3);
            new_type = buffer_[pos + 6];
        }

        // Compute the prospective remaining/take without mutating st.
        uint32_t prospective_remaining = st.remaining;
        if (starting_new) {
            prospective_remaining =
                (fmt == 0 || fmt == 1) ? new_length : st.length;
            if (prospective_remaining > kMaxMessageSize) {
                failed_ = true;
                return;
            }
            // Aggregate cap: refuse to start a new assembly if its declared
            // length, combined with everything already committed to other
            // in-progress csids, would exceed the outstanding budget. This
            // stops a flood of many chunk stream IDs each starting a large
            // message from driving unbounded memory use before any of them
            // is confirmed to actually carry that much data.
            if (assembling_bytes_ + static_cast<uint64_t>(prospective_remaining) >
                kMaxOutstanding) {
                failed_ = true;
                return;
            }
        }
        const uint32_t take = prospective_remaining < chunk_size_
                                   ? prospective_remaining
                                   : chunk_size_;

        if (buffer_.size() < header_total + take) return;

        // The full chunk (header + body) is buffered: commit the header to
        // the chunk-stream state.
        if (fmt == 0) {
            st.timestamp = extended ? ext_value : ts_field;
            st.delta = 0;
            st.length = new_length;
            st.type = new_type;
            st.message_stream_id = new_msid;
        } else if (fmt == 1) {
            st.delta = extended ? ext_value : ts_field;
            st.timestamp += st.delta;
            st.length = new_length;
            st.type = new_type;
        } else if (fmt == 2) {
            st.delta = extended ? ext_value : ts_field;
            st.timestamp += st.delta;
        } else if (starting_new) {
            // fmt3 starting a new message re-applies the stored delta.
            st.timestamp += st.delta;
        }
        st.extended = extended;
        st.have_header = true;

        if (starting_new) {
            st.remaining = prospective_remaining;
            st.assembling.clear();
            // Do not reserve st.remaining: it is attacker-controlled (up to
            // kMaxMessageSize) and only a handful of bytes may ever actually
            // arrive. Let the vector grow organically as bytes are appended.
            assembling_bytes_ += prospective_remaining;
        }

        st.assembling.insert(st.assembling.end(),
                             buffer_.begin() + static_cast<ptrdiff_t>(header_total),
                             buffer_.begin() + static_cast<ptrdiff_t>(header_total + take));
        st.remaining -= take;
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<ptrdiff_t>(header_total + take));

        if (st.remaining == 0) finalize(csid, st);
    }
}

}  // namespace roqr::rtmp
