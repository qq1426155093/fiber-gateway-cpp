#ifndef FIBER_HTTP_HTTP_TRANSPORT_H
#define FIBER_HTTP_HTTP_TRANSPORT_H

#include <chrono>
#include <cstddef>
#include <memory>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../net/TcpStream.h"
#include "HttpTask.h"
#include "TlsContext.h"

struct bio_st;
typedef struct bio_st BIO;

namespace fiber::http {

class HttpTransport : public common::NonCopyable, public common::NonMovable {
public:
    virtual ~HttpTransport() = default;

    virtual HttpTask<common::IoResult<void>> handshake(std::chrono::seconds timeout) = 0;
    virtual HttpTask<common::IoResult<void>> shutdown(std::chrono::seconds timeout) = 0;
    virtual HttpTask<common::IoResult<size_t>> read(void *buf,
                                                    size_t len,
                                                    std::chrono::seconds timeout) = 0;
    virtual HttpTask<common::IoResult<size_t>> write(const void *buf,
                                                     size_t len,
                                                     std::chrono::seconds timeout) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual int fd() const noexcept = 0;
    [[nodiscard]] virtual const net::SocketAddress &remote_addr() const noexcept = 0;
};

class TcpTransport final : public HttpTransport {
public:
    explicit TcpTransport(std::unique_ptr<net::TcpStream> stream);

    HttpTask<common::IoResult<void>> handshake(std::chrono::seconds timeout) override;
    HttpTask<common::IoResult<void>> shutdown(std::chrono::seconds timeout) override;
    HttpTask<common::IoResult<size_t>> read(void *buf,
                                            size_t len,
                                            std::chrono::seconds timeout) override;
    HttpTask<common::IoResult<size_t>> write(const void *buf,
                                             size_t len,
                                             std::chrono::seconds timeout) override;
    void close() override;
    [[nodiscard]] bool valid() const noexcept override;
    [[nodiscard]] int fd() const noexcept override;
    [[nodiscard]] const net::SocketAddress &remote_addr() const noexcept override;

private:
    std::unique_ptr<net::TcpStream> stream_;
};

class TlsTransport final : public HttpTransport {
public:
    static common::IoResult<std::unique_ptr<TlsTransport>> create(
        std::unique_ptr<net::TcpStream> stream,
        TlsContext &context);

    HttpTask<common::IoResult<void>> handshake(std::chrono::seconds timeout) override;
    HttpTask<common::IoResult<void>> shutdown(std::chrono::seconds timeout) override;
    HttpTask<common::IoResult<size_t>> read(void *buf,
                                            size_t len,
                                            std::chrono::seconds timeout) override;
    HttpTask<common::IoResult<size_t>> write(const void *buf,
                                             size_t len,
                                             std::chrono::seconds timeout) override;
    void close() override;
    [[nodiscard]] bool valid() const noexcept override;
    [[nodiscard]] int fd() const noexcept override;
    [[nodiscard]] const net::SocketAddress &remote_addr() const noexcept override;

private:
    TlsTransport(std::unique_ptr<net::TcpStream> stream, TlsContext &context);
    common::IoResult<void> init();

    HttpTask<common::IoResult<void>> flush_wbio(std::chrono::seconds timeout);
    HttpTask<common::IoResult<size_t>> read_raw(std::chrono::seconds timeout);

    std::unique_ptr<net::TcpStream> stream_;
    TlsContext *context_ = nullptr;
    SSL *ssl_ = nullptr;
    BIO *rbio_ = nullptr;
    BIO *wbio_ = nullptr;
    bool handshake_done_ = false;
    bool is_server_ = true;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_TRANSPORT_H
