#include "Http2PendingPool.h"

#include <new>

namespace fiber::http {

Http2PendingPool::~Http2PendingPool() {
    while (free_entries_) {
        Http2PendingEntry *entry = free_entries_;
        free_entries_ = entry->next;
        delete entry;
    }
    free_entry_count_ = 0;
}

Http2PendingEntry *Http2PendingPool::create(Http2Stream &stream, std::uint32_t stream_id, Http2PendingKind kind,
                                            Http2SendPayload &&payload, std::uint8_t first_frame_flags,
                                            std::uint8_t last_frame_flags, Http2PendingEntry::ChangeFn on_change,
                                            void *user_ctx) noexcept {
    Http2PendingEntry *entry = free_entries_;
    if (entry) {
        free_entries_ = entry->next;
        entry->next = nullptr;
        --free_entry_count_;
    } else {
        entry = new (std::nothrow) Http2PendingEntry{};
        if (!entry) {
            return nullptr;
        }
    }

    entry->init(stream, stream_id, kind, std::move(payload), first_frame_flags, last_frame_flags, on_change, user_ctx);
    return entry;
}

void Http2PendingPool::destroy(Http2PendingEntry *entry) noexcept {
    if (!entry) {
        return;
    }

    entry->reset();
    if (free_entry_count_ < max_free_entries_) {
        entry->next = free_entries_;
        free_entries_ = entry;
        ++free_entry_count_;
        return;
    }

    delete entry;
}

} // namespace fiber::http
