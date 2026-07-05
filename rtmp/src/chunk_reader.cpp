#include "roqr/rtmp/chunk_reader.hpp"

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
            it->second.assembling.clear();
            it->second.remaining = 0;
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

        // Apply the header to the chunk-stream state.
        if (fmt == 0) {
            st.timestamp = extended ? ext_value : ts_field;
            st.delta = 0;
            st.length = be24(buffer_.data() + pos + 3);
            st.type = buffer_[pos + 6];
            st.message_stream_id = le32(buffer_.data() + pos + 7);
        } else if (fmt == 1) {
            st.delta = extended ? ext_value : ts_field;
            st.timestamp += st.delta;
            st.length = be24(buffer_.data() + pos + 3);
            st.type = buffer_[pos + 6];
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
            if (st.length > kMaxMessageSize) {
                failed_ = true;
                return;
            }
            st.remaining = st.length;
            st.assembling.clear();
            st.assembling.reserve(st.length);
        }

        const uint32_t take =
            st.remaining < chunk_size_ ? st.remaining : chunk_size_;
        if (buffer_.size() < header_total + take) return;

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
