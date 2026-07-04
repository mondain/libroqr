#include "roqr/quic/context.hpp"

namespace roqr::quic {

std::unique_ptr<QuicContext> QuicContext::create_client(
    const std::string& alpn, bool insecure_skip_verify) {
    const uint64_t now = picoquic_current_time();
    picoquic_quic_t* quic = picoquic_create(
        1, nullptr, nullptr, nullptr, alpn.c_str(), nullptr, nullptr,
        nullptr, nullptr, nullptr, now, nullptr, nullptr, nullptr, 0);
    if (quic == nullptr) return nullptr;
    if (insecure_skip_verify) {
        picoquic_set_null_verifier(quic);
    }
    return std::unique_ptr<QuicContext>(new QuicContext(quic));
}

std::unique_ptr<QuicContext> QuicContext::create_server(
    const std::string& alpn, const std::string& cert_file,
    const std::string& key_file, picoquic_stream_data_cb_fn cb,
    void* cb_ctx) {
    const uint64_t now = picoquic_current_time();
    picoquic_quic_t* quic = picoquic_create(
        8, cert_file.c_str(), key_file.c_str(), nullptr, alpn.c_str(), cb,
        cb_ctx, nullptr, nullptr, nullptr, now, nullptr, nullptr, nullptr,
        0);
    if (quic == nullptr) return nullptr;
    return std::unique_ptr<QuicContext>(new QuicContext(quic));
}

QuicContext::~QuicContext() {
    if (quic_ != nullptr) picoquic_free(quic_);
}

}  // namespace roqr::quic
