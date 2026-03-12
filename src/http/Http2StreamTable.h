#ifndef FIBER_HTTP_HTTP2_STREAM_TABLE_H
#define FIBER_HTTP_HTTP2_STREAM_TABLE_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"

namespace fiber::http {

class Http2Stream;

class Http2StreamTable : public common::NonCopyable, public common::NonMovable {
public:
    Http2StreamTable() noexcept = default;
    ~Http2StreamTable() = default;

    [[nodiscard]] bool init(std::size_t max_active_streams) noexcept;
    void clear() noexcept;

    [[nodiscard]] Http2Stream *find(std::uint32_t stream_id) noexcept;
    [[nodiscard]] const Http2Stream *find(std::uint32_t stream_id) const noexcept;

    [[nodiscard]] bool insert(Http2Stream &stream) noexcept;
    [[nodiscard]] Http2Stream *erase(std::uint32_t stream_id) noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t bucket_count() const noexcept { return bucket_count_; }
    [[nodiscard]] std::size_t max_active_streams() const noexcept { return max_active_streams_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

private:
    struct Bucket {
        std::uint32_t stream_id = 0;
        Http2Stream *stream = nullptr;
    };

    [[nodiscard]] static std::size_t next_pow2(std::size_t value) noexcept;
    [[nodiscard]] static std::size_t hash_stream_id(std::uint32_t stream_id) noexcept;
    [[nodiscard]] std::size_t mask() const noexcept;
    [[nodiscard]] std::size_t probe_distance(std::size_t from, std::size_t to) const noexcept;
    [[nodiscard]] std::size_t find_slot(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] bool should_shift_bucket(std::size_t hole, std::size_t current, std::size_t home) const noexcept;
    void erase_at(std::size_t index) noexcept;

    std::unique_ptr<Bucket[]> buckets_;
    std::size_t bucket_count_ = 0;
    std::size_t size_ = 0;
    std::size_t max_active_streams_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_STREAM_TABLE_H
