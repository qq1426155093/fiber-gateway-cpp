#include "Http2StreamTable.h"

#include <limits>
#include <new>

#include "Http2Stream.h"

namespace fiber::http {

bool Http2StreamTable::init(std::size_t max_active_streams) noexcept {
    clear();
    buckets_.reset();
    bucket_count_ = 0;
    max_active_streams_ = max_active_streams;

    if (max_active_streams == 0) {
        return true;
    }
    if (max_active_streams > (std::numeric_limits<std::size_t>::max() / 2)) {
        return false;
    }

    bucket_count_ = next_pow2(max_active_streams * 2);
    buckets_.reset(new (std::nothrow) Bucket[bucket_count_]{});
    if (!buckets_) {
        bucket_count_ = 0;
        max_active_streams_ = 0;
        return false;
    }
    return true;
}

void Http2StreamTable::clear() noexcept {
    buckets_.reset();
    bucket_count_ = 0;
    size_ = 0;
    max_active_streams_ = 0;
}

Http2Stream *Http2StreamTable::find(std::uint32_t stream_id) noexcept {
    std::size_t slot = find_slot(stream_id);
    if (slot == bucket_count_) {
        return nullptr;
    }
    return buckets_[slot].stream;
}

const Http2Stream *Http2StreamTable::find(std::uint32_t stream_id) const noexcept {
    std::size_t slot = find_slot(stream_id);
    if (slot == bucket_count_) {
        return nullptr;
    }
    return buckets_[slot].stream;
}

bool Http2StreamTable::insert(Http2Stream &stream) noexcept {
    if (!buckets_ || bucket_count_ == 0 || size_ >= max_active_streams_) {
        return false;
    }

    std::uint32_t stream_id = stream.stream_id();
    std::size_t idx = hash_stream_id(stream_id) & mask();
    for (std::size_t probed = 0; probed < bucket_count_; ++probed) {
        Bucket &bucket = buckets_[idx];
        if (!bucket.stream) {
            bucket.stream_id = stream_id;
            bucket.stream = &stream;
            ++size_;
            return true;
        }
        if (bucket.stream_id == stream_id) {
            return false;
        }
        idx = (idx + 1) & mask();
    }

    return false;
}

Http2Stream *Http2StreamTable::erase(std::uint32_t stream_id) noexcept {
    std::size_t slot = find_slot(stream_id);
    if (slot == bucket_count_) {
        return nullptr;
    }

    Http2Stream *stream = buckets_[slot].stream;
    erase_at(slot);
    return stream;
}

std::size_t Http2StreamTable::next_pow2(std::size_t value) noexcept {
    if (value <= 1) {
        return 1;
    }

    std::size_t out = 1;
    while (out < value) {
        out <<= 1;
    }
    return out;
}

std::size_t Http2StreamTable::hash_stream_id(std::uint32_t stream_id) noexcept {
    std::uint32_t value = stream_id * 2654435761u;
    value ^= value >> 16;
    return value;
}

std::size_t Http2StreamTable::mask() const noexcept { return bucket_count_ - 1; }

std::size_t Http2StreamTable::probe_distance(std::size_t from, std::size_t to) const noexcept {
    if (to >= from) {
        return to - from;
    }
    return bucket_count_ - from + to;
}

std::size_t Http2StreamTable::find_slot(std::uint32_t stream_id) const noexcept {
    if (!buckets_ || bucket_count_ == 0) {
        return bucket_count_;
    }

    std::size_t idx = hash_stream_id(stream_id) & mask();
    for (std::size_t probed = 0; probed < bucket_count_; ++probed) {
        const Bucket &bucket = buckets_[idx];
        if (!bucket.stream) {
            return bucket_count_;
        }
        if (bucket.stream_id == stream_id) {
            return idx;
        }
        idx = (idx + 1) & mask();
    }
    return bucket_count_;
}

bool Http2StreamTable::should_shift_bucket(std::size_t hole, std::size_t current, std::size_t home) const noexcept {
    return probe_distance(home, hole) < probe_distance(home, current);
}

void Http2StreamTable::erase_at(std::size_t index) noexcept {
    std::size_t hole = index;
    std::size_t current = (hole + 1) & mask();

    while (buckets_[current].stream) {
        std::size_t home = hash_stream_id(buckets_[current].stream_id) & mask();
        if (should_shift_bucket(hole, current, home)) {
            buckets_[hole] = buckets_[current];
            hole = current;
        }
        current = (current + 1) & mask();
    }

    buckets_[hole] = Bucket{};
    --size_;
}

} // namespace fiber::http
