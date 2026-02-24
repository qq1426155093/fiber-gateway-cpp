#include "UdpSocket.h"

namespace fiber::net {

UdpSocket::UdpSocket(fiber::event::EventLoop &loop) : socket_(loop) {
}

UdpSocket::~UdpSocket() {
}

fiber::common::IoResult<void> UdpSocket::bind(const SocketAddress &addr,
                                              const UdpBindOptions &options) {
    return socket_.bind(addr, options);
}

bool UdpSocket::valid() const noexcept {
    return socket_.valid();
}

int UdpSocket::fd() const noexcept {
    return socket_.fd();
}

void UdpSocket::close() {
    socket_.close();
}

UdpSocket::RecvFromAwaiter UdpSocket::recv_from(void *buf, size_t len) noexcept {
    return socket_.recv_from(buf, len);
}

UdpSocket::SendToAwaiter UdpSocket::send_to(const void *buf,
                                            size_t len,
                                            const SocketAddress &peer) noexcept {
    return socket_.send_to(buf, len, peer);
}

fiber::common::IoResult<UdpRecvResult> UdpSocket::try_recv_from(void *buf, size_t len) noexcept {
    return socket_.try_recv_from(buf, len);
}

fiber::common::IoResult<size_t> UdpSocket::try_send_to(const void *buf,
                                                       size_t len,
                                                       const SocketAddress &peer) noexcept {
    return socket_.try_send_to(buf, len, peer);
}

} // namespace fiber::net
