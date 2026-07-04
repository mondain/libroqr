#pragma once

#include <memory>
#include <string>

#include <picoquic.h>

namespace roqr::quic {

// RAII owner of a picoquic_quic_t. Configuration that must happen before
// any connection exists (verifier, ALPN) lives here.
class QuicContext {
public:
    static std::unique_ptr<QuicContext> create_client(
        const std::string& alpn, bool insecure_skip_verify);

    static std::unique_ptr<QuicContext> create_server(
        const std::string& alpn, const std::string& cert_file,
        const std::string& key_file, picoquic_stream_data_cb_fn cb,
        void* cb_ctx);

    ~QuicContext();
    QuicContext(const QuicContext&) = delete;
    QuicContext& operator=(const QuicContext&) = delete;

    picoquic_quic_t* get() { return quic_; }

private:
    explicit QuicContext(picoquic_quic_t* quic) : quic_(quic) {}
    picoquic_quic_t* quic_;
};

}  // namespace roqr::quic
