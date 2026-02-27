#ifndef FIBER_HTTP_HTTP_TRANSPORT_H
#define FIBER_HTTP_HTTP_TRANSPORT_H

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../net/TcpStream.h"
#include "../net/TlsTcpStream.h"
#include "HeadBuf.h"
#include "TlsContext.h"

namespace fiber::http {

class HttpTransport : public common::NonCopyable, public common::NonMovable {
public:
    virtual ~HttpTransport() = default;

    virtual fiber::async::Task<common::IoResult<void>> handshake(std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<void>> shutdown(std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> read(void *buf, size_t len,
                                                              std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> read_into(BufChain *buf,
                                                                   std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> readv_into(BufChain *bufs,
                                                                    std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> write(const void *buf, size_t len,
                                                               std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> write(BufChain *buf, std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> writev(BufChain *buf, std::chrono::milliseconds timeout) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual int fd() const noexcept = 0;
    [[nodiscard]] virtual std::string negotiated_alpn() const noexcept = 0;
    [[nodiscard]] virtual const net::SocketAddress &remote_addr() const noexcept = 0;
};

class TcpTransport final : public HttpTransport {
public:
    explicit TcpTransport(std::unique_ptr<net::TcpStream> stream);

    fiber::async::Task<common::IoResult<void>> handshake(std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<void>> shutdown(std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> read(void *buf, size_t len,
                                                      std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> read_into(BufChain *buf, std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> readv_into(BufChain *bufs, std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> write(const void *buf, size_t len,
                                                       std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> write(BufChain *buf, std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> writev(BufChain *buf, std::chrono::milliseconds timeout) override;
    void close() override;
    [[nodiscard]] bool valid() const noexcept override;
    [[nodiscard]] int fd() const noexcept override;
    [[nodiscard]] std::string negotiated_alpn() const noexcept override;
    [[nodiscard]] const net::SocketAddress &remote_addr() const noexcept override;

private:
    std::unique_ptr<net::TcpStream> stream_;
};

class TlsTransport final : public HttpTransport {
public:
    ~TlsTransport() override;
    static common::IoResult<std::unique_ptr<TlsTransport>> create(std::unique_ptr<net::TlsTcpStream> stream,
                                                                  TlsContext &context);

    fiber::async::Task<common::IoResult<void>> handshake(std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<void>> shutdown(std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> read(void *buf, size_t len,
                                                      std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> read_into(BufChain *buf, std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> readv_into(BufChain *bufs, std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> write(const void *buf, size_t len,
                                                       std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> write(BufChain *buf, std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> writev(BufChain *buf, std::chrono::milliseconds timeout) override;
    void close() override;
    [[nodiscard]] bool valid() const noexcept override;
    [[nodiscard]] int fd() const noexcept override;
    [[nodiscard]] std::string negotiated_alpn() const noexcept override;
    [[nodiscard]] const net::SocketAddress &remote_addr() const noexcept override;

private:
    TlsTransport(std::unique_ptr<net::TlsTcpStream> stream, TlsContext &context);
    common::IoResult<void> init();

    std::unique_ptr<net::TlsTcpStream> stream_;
    TlsContext *context_ = nullptr;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_TRANSPORT_H
