#include "HttpTransport.h"

#include <algorithm>
#include <array>

#include "../async/Timeout.h"

#include <openssl/bio.h>
#include <openssl/ssl.h>

namespace fiber::http {

TcpTransport::TcpTransport(std::unique_ptr<net::TcpStream> stream) : stream_(std::move(stream)) {
}

HttpTask<common::IoResult<void>> TcpTransport::handshake(std::chrono::seconds) {
    co_return common::IoResult<void>{};
}

HttpTask<common::IoResult<void>> TcpTransport::shutdown(std::chrono::seconds) {
    co_return common::IoResult<void>{};
}

HttpTask<common::IoResult<size_t>> TcpTransport::read(void *buf,
                                                      size_t len,
                                                      std::chrono::seconds timeout) {
    auto result = co_await fiber::async::timeout_for(
        [&]() { return stream_->read(buf, len); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return *result;
}

HttpTask<common::IoResult<size_t>> TcpTransport::write(const void *buf,
                                                       size_t len,
                                                       std::chrono::seconds timeout) {
    auto result = co_await fiber::async::timeout_for(
        [&]() { return stream_->write(buf, len); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return *result;
}

void TcpTransport::close() {
    if (stream_) {
        stream_->close();
    }
}

bool TcpTransport::valid() const noexcept {
    return stream_ && stream_->valid();
}

int TcpTransport::fd() const noexcept {
    return stream_ ? stream_->fd() : -1;
}

const net::SocketAddress &TcpTransport::remote_addr() const noexcept {
    return stream_->remote_addr();
}

common::IoResult<std::unique_ptr<TlsTransport>> TlsTransport::create(
    std::unique_ptr<net::TcpStream> stream,
    TlsContext &context) {
    auto transport = std::unique_ptr<TlsTransport>(new TlsTransport(std::move(stream), context));
    auto init_result = transport->init();
    if (!init_result) {
        return std::unexpected(init_result.error());
    }
    return transport;
}

TlsTransport::TlsTransport(std::unique_ptr<net::TcpStream> stream, TlsContext &context)
    : stream_(std::move(stream)), context_(&context), is_server_(context.is_server()) {
}

common::IoResult<void> TlsTransport::init() {
    if (!context_ || !context_->raw()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    ssl_ = SSL_new(context_->raw());
    if (!ssl_) {
        return std::unexpected(common::IoErr::NoMem);
    }

    BIO *rbio = BIO_new(BIO_s_mem());
    BIO *wbio = BIO_new(BIO_s_mem());
    if (!rbio || !wbio) {
        if (rbio) {
            BIO_free(rbio);
        }
        if (wbio) {
            BIO_free(wbio);
        }
        SSL_free(ssl_);
        ssl_ = nullptr;
        return std::unexpected(common::IoErr::NoMem);
    }

    BIO_set_mem_eof_return(rbio, -1);
    BIO_set_mem_eof_return(wbio, -1);
    SSL_set_bio(ssl_, rbio, wbio);
    rbio_ = SSL_get_rbio(ssl_);
    wbio_ = SSL_get_wbio(ssl_);
    if (is_server_) {
        SSL_set_accept_state(ssl_);
    } else {
        SSL_set_connect_state(ssl_);
    }
    return {};
}

HttpTask<common::IoResult<void>> TlsTransport::handshake(std::chrono::seconds timeout) {
    if (handshake_done_) {
        co_return common::IoResult<void>{};
    }
    for (;;) {
        int rc = is_server_ ? SSL_accept(ssl_) : SSL_connect(ssl_);
        if (rc == 1) {
            handshake_done_ = true;
            auto flush_result = co_await flush_wbio(timeout);
            if (!flush_result) {
                co_return std::unexpected(flush_result.error());
            }
            co_return common::IoResult<void>{};
        }
        int err = SSL_get_error(ssl_, rc);
        if (wbio_ && BIO_ctrl_pending(wbio_) > 0) {
            auto flush_result = co_await flush_wbio(timeout);
            if (!flush_result) {
                co_return std::unexpected(flush_result.error());
            }
        }
        if (err == SSL_ERROR_WANT_READ) {
            auto read_result = co_await read_raw(timeout);
            if (!read_result) {
                co_return std::unexpected(read_result.error());
            }
            if (*read_result == 0) {
                co_return std::unexpected(common::IoErr::ConnReset);
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            if (!wbio_ || BIO_ctrl_pending(wbio_) == 0) {
                auto read_result = co_await read_raw(timeout);
                if (!read_result) {
                    co_return std::unexpected(read_result.error());
                }
                if (*read_result == 0) {
                    co_return std::unexpected(common::IoErr::ConnReset);
                }
            }
            continue;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        co_return std::unexpected(common::IoErr::Invalid);
    }
}

HttpTask<common::IoResult<void>> TlsTransport::shutdown(std::chrono::seconds timeout) {
    if (!ssl_ || !handshake_done_) {
        co_return common::IoResult<void>{};
    }
    SSL_shutdown(ssl_);
    auto flush_result = co_await flush_wbio(timeout);
    if (!flush_result) {
        co_return std::unexpected(flush_result.error());
    }
    co_return common::IoResult<void>{};
}

HttpTask<common::IoResult<size_t>> TlsTransport::read(void *buf,
                                                      size_t len,
                                                      std::chrono::seconds timeout) {
    auto hs_result = co_await handshake(context_->options().handshake_timeout);
    if (!hs_result) {
        co_return std::unexpected(hs_result.error());
    }
    for (;;) {
        int rc = SSL_read(ssl_, buf, static_cast<int>(len));
        if (rc > 0) {
            auto flush_result = co_await flush_wbio(timeout);
            if (!flush_result) {
                co_return std::unexpected(flush_result.error());
            }
            co_return static_cast<size_t>(rc);
        }
        int err = SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_WANT_READ) {
            auto read_result = co_await read_raw(timeout);
            if (!read_result) {
                co_return std::unexpected(read_result.error());
            }
            if (*read_result == 0) {
                co_return std::unexpected(common::IoErr::ConnReset);
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            auto flush_result = co_await flush_wbio(timeout);
            if (!flush_result) {
                co_return std::unexpected(flush_result.error());
            }
            continue;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            co_return static_cast<size_t>(0);
        }
        co_return std::unexpected(common::IoErr::Invalid);
    }
}

HttpTask<common::IoResult<size_t>> TlsTransport::write(const void *buf,
                                                       size_t len,
                                                       std::chrono::seconds timeout) {
    auto hs_result = co_await handshake(context_->options().handshake_timeout);
    if (!hs_result) {
        co_return std::unexpected(hs_result.error());
    }
    for (;;) {
        int rc = SSL_write(ssl_, buf, static_cast<int>(len));
        if (rc > 0) {
            auto flush_result = co_await flush_wbio(timeout);
            if (!flush_result) {
                co_return std::unexpected(flush_result.error());
            }
            co_return static_cast<size_t>(rc);
        }
        int err = SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_WANT_READ) {
            auto read_result = co_await read_raw(timeout);
            if (!read_result) {
                co_return std::unexpected(read_result.error());
            }
            if (*read_result == 0) {
                co_return std::unexpected(common::IoErr::ConnReset);
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            auto flush_result = co_await flush_wbio(timeout);
            if (!flush_result) {
                co_return std::unexpected(flush_result.error());
            }
            continue;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            co_return std::unexpected(common::IoErr::BrokenPipe);
        }
        co_return std::unexpected(common::IoErr::Invalid);
    }
}

void TlsTransport::close() {
    if (!stream_) {
        return;
    }
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
        rbio_ = nullptr;
        wbio_ = nullptr;
    }
    stream_->close();
}

bool TlsTransport::valid() const noexcept {
    return stream_ && stream_->valid();
}

int TlsTransport::fd() const noexcept {
    return stream_ ? stream_->fd() : -1;
}

const net::SocketAddress &TlsTransport::remote_addr() const noexcept {
    return stream_->remote_addr();
}

HttpTask<common::IoResult<void>> TlsTransport::flush_wbio(std::chrono::seconds timeout) {
    if (!wbio_) {
        co_return common::IoResult<void>{};
    }
    std::array<char, 4096> buffer{};
    for (;;) {
        size_t pending = static_cast<size_t>(BIO_ctrl_pending(wbio_));
        if (pending == 0) {
            break;
        }
        size_t to_read = std::min(pending, buffer.size());
        int read_len = BIO_read(wbio_, buffer.data(), static_cast<int>(to_read));
        if (read_len <= 0) {
            break;
        }
        size_t offset = 0;
        size_t chunk_len = static_cast<size_t>(read_len);
        while (offset < chunk_len) {
            auto write_result = co_await fiber::async::timeout_for(
                [&]() { return stream_->write(buffer.data() + offset, chunk_len - offset); },
                timeout);
            if (!write_result) {
                co_return std::unexpected(write_result.error());
            }
            if (*write_result == 0) {
                co_return std::unexpected(common::IoErr::BrokenPipe);
            }
            offset += *write_result;
        }
    }
    co_return common::IoResult<void>{};
}

HttpTask<common::IoResult<size_t>> TlsTransport::read_raw(std::chrono::seconds timeout) {
    std::array<char, 8192> buffer{};
    auto read_result = co_await fiber::async::timeout_for(
        [&]() { return stream_->read(buffer.data(), buffer.size()); }, timeout);
    if (!read_result) {
        co_return std::unexpected(read_result.error());
    }
    if (*read_result > 0) {
        int written = BIO_write(rbio_, buffer.data(), static_cast<int>(*read_result));
        if (written <= 0) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }
    co_return *read_result;
}

} // namespace fiber::http
