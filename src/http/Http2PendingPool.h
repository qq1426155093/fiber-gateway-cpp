#ifndef FIBER_HTTP_HTTP2_PENDING_POOL_H
#define FIBER_HTTP_HTTP2_PENDING_POOL_H

#include <cstddef>
#include <cstdint>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http2Pending.h"

namespace fiber::http {

class Http2PendingPool : public common::NonCopyable, public common::NonMovable {
public:
    explicit Http2PendingPool(std::size_t max_free_entries = 64) noexcept : max_free_entries_(max_free_entries) {}
    ~Http2PendingPool();

    [[nodiscard]] Http2PendingEntry *create(Http2Stream &stream, std::uint32_t stream_id, Http2PendingKind kind,
                                            Http2SendPayload &&payload, std::uint8_t first_frame_flags = 0,
                                            std::uint8_t last_frame_flags = 0,
                                            Http2PendingEntry::ChangeFn on_change = nullptr,
                                            void *user_ctx = nullptr) noexcept;
    void destroy(Http2PendingEntry *entry) noexcept;

private:
    Http2PendingEntry *free_entries_ = nullptr;
    std::size_t free_entry_count_ = 0;
    std::size_t max_free_entries_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_PENDING_POOL_H
