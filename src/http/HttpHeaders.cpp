#include "HttpHeaders.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace fiber::http {

namespace {

uint64_t hash_name(std::string_view name) {
    uint64_t hash = 14695981039346656037ull;
    for (char ch : name) {
        unsigned char lower = static_cast<unsigned char>(ch);
        if (lower >= 'A' && lower <= 'Z') {
            lower = static_cast<unsigned char>(lower - 'A' + 'a');
        }
        hash ^= lower;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool equals_ascii_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char left = static_cast<unsigned char>(a[i]);
        unsigned char right = static_cast<unsigned char>(b[i]);
        if (left >= 'A' && left <= 'Z') {
            left = static_cast<unsigned char>(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = static_cast<unsigned char>(right - 'A' + 'a');
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

size_t next_pow2(size_t value) {
    if (value <= 1) {
        return 1;
    }
    value--;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    if constexpr (sizeof(size_t) >= 8) {
        value |= value >> 32;
    }
    return value + 1;
}

} // namespace

HttpHeaders::HttpHeaders(mem::BufPool &pool)
    : pool_(&pool),
      fields_(mem::PoolAllocator<HeaderField>(pool)),
      bucket_head_(mem::PoolAllocator<uint32_t>(pool)),
      bucket_tail_(mem::PoolAllocator<uint32_t>(pool)) {
}

bool HttpHeaders::add(std::string_view name, std::string_view value) {
    if (!ensure_buckets() || !pool_) {
        return false;
    }
    if (name.size() > std::numeric_limits<uint32_t>::max() ||
        value.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    bool ok = true;
    const char *name_ptr = copy_to_pool(name, ok);
    if (!ok) {
        return false;
    }
    const char *value_ptr = copy_to_pool(value, ok);
    if (!ok) {
        return false;
    }

    HeaderField field{};
    field.name = name_ptr;
    field.name_len = static_cast<uint32_t>(name.size());
    field.value = value_ptr;
    field.value_len = static_cast<uint32_t>(value.size());
    field.name_hash = hash_name(name);
    field.next = kInvalidIndex;

    uint32_t index = static_cast<uint32_t>(fields_.size());
    fields_.push_back(field);

    uint32_t bucket = static_cast<uint32_t>(field.name_hash & (bucket_head_.size() - 1));
    if (bucket_head_[bucket] == kInvalidIndex) {
        bucket_head_[bucket] = index;
        bucket_tail_[bucket] = index;
    } else {
        uint32_t tail = bucket_tail_[bucket];
        fields_[tail].next = index;
        bucket_tail_[bucket] = index;
    }
    return true;
}

bool HttpHeaders::set(std::string_view name, std::string_view value) {
    remove(name);
    return add(name, value);
}

std::string_view HttpHeaders::get(std::string_view name) const noexcept {
    uint32_t index = find_first_index(name);
    if (index == kInvalidIndex) {
        return {};
    }
    return fields_[index].value_view();
}

bool HttpHeaders::contains(std::string_view name) const noexcept {
    return find_first_index(name) != kInvalidIndex;
}

size_t HttpHeaders::remove(std::string_view name) noexcept {
    if (fields_.empty()) {
        return 0;
    }
    uint64_t name_hash = hash_name(name);
    size_t removed = 0;
    size_t out = 0;
    for (size_t i = 0; i < fields_.size(); ++i) {
        const auto &field = fields_[i];
        if (field.name_hash == name_hash &&
            equals_ascii_ci(field.name_view(), name)) {
            ++removed;
            continue;
        }
        if (out != i) {
            fields_[out] = field;
        }
        ++out;
    }
    if (removed == 0) {
        return 0;
    }
    fields_.resize(out);
    rebuild_buckets();
    return removed;
}

void HttpHeaders::reserve_bytes(size_t bytes) {
    size_t estimated_fields = bytes / 32 + 8;
    fields_.reserve(estimated_fields);
    if (bucket_head_.empty()) {
        size_t buckets = next_pow2(std::max(kDefaultBuckets, estimated_fields * 2));
        init_buckets(buckets);
    }
}

void HttpHeaders::clear() noexcept {
    fields_.clear();
    if (!bucket_head_.empty()) {
        std::fill(bucket_head_.begin(), bucket_head_.end(), kInvalidIndex);
        std::fill(bucket_tail_.begin(), bucket_tail_.end(), kInvalidIndex);
    }
}

void HttpHeaders::release() noexcept {
    FieldVector empty_fields(fields_.get_allocator());
    fields_.swap(empty_fields);

    BucketVector empty_head(bucket_head_.get_allocator());
    bucket_head_.swap(empty_head);

    BucketVector empty_tail(bucket_tail_.get_allocator());
    bucket_tail_.swap(empty_tail);
}

size_t HttpHeaders::size() const noexcept {
    return fields_.size();
}

HttpHeaders::FieldVector::const_iterator HttpHeaders::begin() const noexcept {
    return fields_.begin();
}

HttpHeaders::FieldVector::const_iterator HttpHeaders::end() const noexcept {
    return fields_.end();
}

bool HttpHeaders::ensure_buckets() {
    if (bucket_head_.empty()) {
        init_buckets(kDefaultBuckets);
    }
    return !bucket_head_.empty();
}

void HttpHeaders::init_buckets(size_t count) {
    if (count == 0) {
        count = kDefaultBuckets;
    }
    count = next_pow2(count);
    if (count < kDefaultBuckets) {
        count = kDefaultBuckets;
    }
    bucket_head_.assign(count, kInvalidIndex);
    bucket_tail_.assign(count, kInvalidIndex);
}

void HttpHeaders::rebuild_buckets() noexcept {
    if (bucket_head_.empty()) {
        return;
    }
    std::fill(bucket_head_.begin(), bucket_head_.end(), kInvalidIndex);
    std::fill(bucket_tail_.begin(), bucket_tail_.end(), kInvalidIndex);
    for (uint32_t index = 0; index < fields_.size(); ++index) {
        auto &field = fields_[index];
        field.next = kInvalidIndex;
        uint32_t bucket = static_cast<uint32_t>(field.name_hash & (bucket_head_.size() - 1));
        if (bucket_head_[bucket] == kInvalidIndex) {
            bucket_head_[bucket] = index;
            bucket_tail_[bucket] = index;
        } else {
            uint32_t tail = bucket_tail_[bucket];
            fields_[tail].next = index;
            bucket_tail_[bucket] = index;
        }
    }
}

uint32_t HttpHeaders::find_first_index(std::string_view name) const noexcept {
    if (bucket_head_.empty()) {
        return kInvalidIndex;
    }
    uint64_t name_hash = hash_name(name);
    uint32_t bucket = static_cast<uint32_t>(name_hash & (bucket_head_.size() - 1));
    uint32_t index = bucket_head_[bucket];
    while (index != kInvalidIndex) {
        const auto &field = fields_[index];
        if (field.name_hash == name_hash &&
            equals_ascii_ci(field.name_view(), name)) {
            return index;
        }
        index = field.next;
    }
    return kInvalidIndex;
}

const char *HttpHeaders::copy_to_pool(std::string_view data, bool &ok) {
    if (data.empty()) {
        return "";
    }
    if (!pool_) {
        ok = false;
        return nullptr;
    }
    char *ptr = static_cast<char *>(pool_->alloc(data.size(), alignof(char)));
    if (!ptr) {
        ok = false;
        return nullptr;
    }
    std::memcpy(ptr, data.data(), data.size());
    return ptr;
}

} // namespace fiber::http
