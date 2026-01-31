#include "HttpHeaders.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace fiber::http {

namespace {

uint64_t hash_name(std::string_view name) {
    std::uint32_t hash = 0;
    for (char ch : name) {
        unsigned char lower = static_cast<unsigned char>(ch);
        if (lower >= 'A' && lower <= 'Z') {
            lower = static_cast<unsigned char>(lower - 'A' + 'a');
        }
        hash = hash * 31 + lower;
    }
    return static_cast<uint64_t>(hash);
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

void to_lowcase_and_hash(std::string_view name, std::string &out, uint64_t &hash_out) {
    out.resize(name.size());
    std::uint32_t hash = 0;
    for (size_t i = 0; i < name.size(); ++i) {
        unsigned char lower = static_cast<unsigned char>(name[i]);
        if (lower >= 'A' && lower <= 'Z') {
            lower = static_cast<unsigned char>(lower - 'A' + 'a');
        }
        out[i] = static_cast<char>(lower);
        hash = hash * 31 + lower;
    }
    hash_out = static_cast<uint64_t>(hash);
}

void init_field(HttpHeaders::HeaderField *field,
                const char *name_ptr,
                uint32_t name_len,
                char *lowcase_ptr,
                const char *value_ptr,
                uint32_t value_len,
                uint64_t hash) {
    field->name = name_ptr;
    field->name_len = name_len;
    field->lowcase_name = lowcase_ptr;
    field->value = value_ptr;
    field->value_len = value_len;
    field->name_hash = hash;
    field->next_bucket = nullptr;
    field->next_all = nullptr;
    field->prev_all = nullptr;
}

} // namespace

HttpHeaders::HttpHeaders(mem::BufPool &pool)
    : pool_(&pool),
      bucket_head_(mem::PoolAllocator<HeaderField *>(pool)) {
}

HttpHeaders::HeaderField *HttpHeaders::add(std::string_view name, std::string_view value) {
    if (!ensure_buckets() || !pool_) {
        return nullptr;
    }
    if (name.size() > std::numeric_limits<uint32_t>::max() ||
        value.size() > std::numeric_limits<uint32_t>::max()) {
        return nullptr;
    }

    bool ok = true;
    const char *name_ptr = copy_to_pool(name, ok);
    if (!ok) {
        return nullptr;
    }
    uint64_t hash = 0;
    const char *lowcase_ptr = copy_lowercase_to_pool(name, hash, ok);
    if (!ok) {
        return nullptr;
    }
    const char *value_ptr = copy_to_pool(value, ok);
    if (!ok) {
        return nullptr;
    }
    HeaderField *field = alloc_field(ok);
    if (!ok) {
        return nullptr;
    }

    init_field(field,
               name_ptr,
               static_cast<uint32_t>(name.size()),
               lowcase_ptr,
               static_cast<uint32_t>(name.size()),
               value_ptr,
               static_cast<uint32_t>(value.size()),
               hash);
    return link_field(field);
}

HttpHeaders::HeaderField *HttpHeaders::add(std::string_view name, std::string_view value,
                                           char *lowcase_name, uint64_t hash) {
    if (!ensure_buckets() || !pool_) {
        return nullptr;
    }
    if (name.size() > std::numeric_limits<uint32_t>::max() ||
        value.size() > std::numeric_limits<uint32_t>::max()) {
        return nullptr;
    }

    bool ok = true;
    const char *name_ptr = copy_to_pool(name, ok);
    if (!ok) {
        return nullptr;
    }
    const char *lowcase_ptr = copy_to_pool(std::string_view(lowcase_name, name.size()), ok);
    if (!ok) {
        return nullptr;
    }
    const char *value_ptr = copy_to_pool(value, ok);
    if (!ok) {
        return nullptr;
    }
    HeaderField *field = alloc_field(ok);
    if (!ok) {
        return nullptr;
    }

    init_field(field,
               name_ptr,
               static_cast<uint32_t>(name.size()),
               const_cast<char *>(lowcase_ptr),
               value_ptr,
               static_cast<uint32_t>(value.size()),
               hash);
    return link_field(field);
}

HttpHeaders::HeaderField *HttpHeaders::set(std::string_view name, std::string_view value) {
    remove(name);
    return add(name, value);
}

HttpHeaders::HeaderField *HttpHeaders::set(std::string_view name, std::string_view value,
                                           char *lowcase_name, uint64_t hash) {
    remove_lowcase(std::string_view(lowcase_name, name.size()), hash);
    return add(name, value, lowcase_name, hash);
}

HttpHeaders::HeaderField *HttpHeaders::add_view(std::string_view name, std::string_view value) {
    if (!ensure_buckets()) {
        return nullptr;
    }
    if (name.size() > std::numeric_limits<uint32_t>::max() ||
        value.size() > std::numeric_limits<uint32_t>::max()) {
        return nullptr;
    }
    uint64_t hash = hash_name(name);
    HeaderField *field = nullptr;
    bool ok = true;
    field = alloc_field(ok);
    if (!ok) {
        return nullptr;
    }

    init_field(field,
               name.data(),
               static_cast<uint32_t>(name.size()),
               name.data(),
               static_cast<uint32_t>(name.size()),
               value.data(),
               static_cast<uint32_t>(value.size()),
               hash);
    return link_field(field);
}

HttpHeaders::HeaderField *HttpHeaders::add_view(std::string_view name, std::string_view value,
                                                char *lowcase_name, uint64_t hash) {
    if (!ensure_buckets()) {
        return nullptr;
    }
    if (name.size() > std::numeric_limits<uint32_t>::max() ||
        value.size() > std::numeric_limits<uint32_t>::max()) {
        return nullptr;
    }

    bool ok = true;
    HeaderField *field = alloc_field(ok);
    if (!ok) {
        return nullptr;
    }

    init_field(field,
               name.data(),
               static_cast<uint32_t>(name.size()),
               lowcase_name,
               value.data(),
               static_cast<uint32_t>(value.size()),
               hash);
    return link_field(field);
}

HttpHeaders::HeaderField *HttpHeaders::set_view(std::string_view name, std::string_view value) {
    remove(name);
    return add_view(name, value);
}

HttpHeaders::HeaderField *HttpHeaders::set_view(std::string_view name, std::string_view value,
                                                char *lowcase_name, uint64_t hash) {
    remove_lowcase(std::string_view(lowcase_name, name.size()), hash);
    return add_view(name, value, lowcase_name, hash);
}

std::string_view HttpHeaders::get(std::string_view name) const noexcept {
    HeaderField *node = find_first_node(name);
    if (!node) {
        return {};
    }
    return node->value_view();
}

bool HttpHeaders::contains(std::string_view name) const noexcept {
    return find_first_node(name) != nullptr;
}

HttpHeaders::MatchRange HttpHeaders::get_all(std::string_view lowcase_key, uint64_t hash) const noexcept {
    return MatchRange(this, lowcase_key, hash);
}

HttpHeaders::MatchRange HttpHeaders::get_all(std::string_view name) const {
    MatchRange range;
    range.headers_ = this;
    to_lowcase_and_hash(name, range.owned_key_, range.hash_);
    range.key_ = range.owned_key_;
    return range;
}

size_t HttpHeaders::remove(std::string_view name) noexcept {
    if (!all_head_ || bucket_head_.empty()) {
        return 0;
    }
    uint64_t name_hash = hash_name(name);
    std::uint32_t bucket = static_cast<std::uint32_t>(name_hash & (bucket_head_.size() - 1));

    size_t removed = 0;
    HeaderField *prev_bucket = nullptr;
    HeaderField *node = bucket_head_[bucket];
    while (node) {
        HeaderField *next = node->next_bucket;
        if (node->name_hash == name_hash && equals_ascii_ci(node->name_view(), name)) {
            if (prev_bucket) {
                prev_bucket->next_bucket = next;
            } else {
                bucket_head_[bucket] = next;
            }

            if (node->prev_all) {
                node->prev_all->next_all = node->next_all;
            } else {
                all_head_ = node->next_all;
            }
            if (node->next_all) {
                node->next_all->prev_all = node->prev_all;
            } else {
                all_tail_ = node->prev_all;
            }

            ++removed;
            --size_;
        } else {
            prev_bucket = node;
        }
        node = next;
    }
    return removed;
}

size_t HttpHeaders::remove_lowcase(std::string_view lowcase_key, uint64_t hash) noexcept {
    if (!all_head_ || bucket_head_.empty()) {
        return 0;
    }
    std::uint32_t bucket = static_cast<std::uint32_t>(hash & (bucket_head_.size() - 1));

    size_t removed = 0;
    HeaderField *prev_bucket = nullptr;
    HeaderField *node = bucket_head_[bucket];
    while (node) {
        HeaderField *next = node->next_bucket;
        if (node->name_hash == hash && equals_ascii_ci(node->lowcase_view(), lowcase_key)) {
            if (prev_bucket) {
                prev_bucket->next_bucket = next;
            } else {
                bucket_head_[bucket] = next;
            }

            if (node->prev_all) {
                node->prev_all->next_all = node->next_all;
            } else {
                all_head_ = node->next_all;
            }
            if (node->next_all) {
                node->next_all->prev_all = node->prev_all;
            } else {
                all_tail_ = node->prev_all;
            }

            ++removed;
            --size_;
        } else {
            prev_bucket = node;
        }
        node = next;
    }
    return removed;
}

void HttpHeaders::reserve_bytes(size_t bytes) {
    size_t estimated_fields = bytes / 32 + 8;
    size_t buckets = next_pow2(std::max(kDefaultBuckets, estimated_fields * 2));
    if (bucket_head_.size() < buckets) {
        init_buckets(buckets);
        rebuild_buckets();
    }
}

void HttpHeaders::clear() noexcept {
    all_head_ = nullptr;
    all_tail_ = nullptr;
    size_ = 0;
    if (!bucket_head_.empty()) {
        std::fill(bucket_head_.begin(), bucket_head_.end(), nullptr);
    }
}

void HttpHeaders::release() noexcept {
    all_head_ = nullptr;
    all_tail_ = nullptr;
    size_ = 0;
    BucketVector empty;
    bucket_head_.swap(empty);
}

size_t HttpHeaders::size() const noexcept {
    return size_;
}

HttpHeaders::ConstIterator HttpHeaders::begin() const noexcept {
    return ConstIterator(all_head_);
}

HttpHeaders::ConstIterator HttpHeaders::end() const noexcept {
    return ConstIterator(nullptr);
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
    bucket_head_.assign(count, nullptr);
}

void HttpHeaders::rebuild_buckets() noexcept {
    if (bucket_head_.empty()) {
        return;
    }
    std::fill(bucket_head_.begin(), bucket_head_.end(), nullptr);
    for (HeaderField *node = all_head_; node; node = node->next_all) {
        std::uint32_t bucket = static_cast<std::uint32_t>(node->name_hash & (bucket_head_.size() - 1));
        node->next_bucket = bucket_head_[bucket];
        bucket_head_[bucket] = node;
    }
}

HttpHeaders::HeaderField *HttpHeaders::find_first_node(std::string_view name) const noexcept {
    if (bucket_head_.empty()) {
        return nullptr;
    }
    uint64_t name_hash = hash_name(name);
    std::uint32_t bucket = static_cast<std::uint32_t>(name_hash & (bucket_head_.size() - 1));
    HeaderField *node = bucket_head_[bucket];
    while (node) {
        if (node->name_hash == name_hash && equals_ascii_ci(node->name_view(), name)) {
            return node;
        }
        node = node->next_bucket;
    }
    return nullptr;
}

const HttpHeaders::HeaderField *HttpHeaders::find_first_node_lowcase(std::string_view lowcase_key,
                                                                     uint64_t hash) const noexcept {
    if (bucket_head_.empty()) {
        return nullptr;
    }
    std::uint32_t bucket = static_cast<std::uint32_t>(hash & (bucket_head_.size() - 1));
    HeaderField *node = bucket_head_[bucket];
    while (node) {
        if (node->name_hash == hash && equals_ascii_ci(node->lowcase_view(), lowcase_key)) {
            return node;
        }
        node = node->next_bucket;
    }
    return nullptr;
}

const HttpHeaders::HeaderField *HttpHeaders::next_match_node(const HeaderField *start,
                                                             std::string_view lowcase_key,
                                                             uint64_t hash) const noexcept {
    if (!start) {
        return nullptr;
    }
    const HeaderField *node = start->next_bucket;
    while (node) {
        if (node->name_hash == hash && equals_ascii_ci(node->lowcase_view(), lowcase_key)) {
            return node;
        }
        node = node->next_bucket;
    }
    return nullptr;
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

const char *HttpHeaders::copy_lowercase_to_pool(std::string_view data, uint64_t &hash, bool &ok) {
    std::uint32_t local_hash = 0;
    if (data.empty()) {
        hash = 0;
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
    for (size_t i = 0; i < data.size(); ++i) {
        unsigned char lower = static_cast<unsigned char>(data[i]);
        if (lower >= 'A' && lower <= 'Z') {
            lower = static_cast<unsigned char>(lower - 'A' + 'a');
        }
        ptr[i] = static_cast<char>(lower);
        local_hash = local_hash * 31 + lower;
    }
    hash = static_cast<uint64_t>(local_hash);
    return ptr;
}

HttpHeaders::HeaderField *HttpHeaders::alloc_field(bool &ok) {
    if (!pool_) {
        ok = false;
        return nullptr;
    }
    void *ptr = pool_->alloc(sizeof(HeaderField), alignof(HeaderField));
    if (!ptr) {
        ok = false;
        return nullptr;
    }
    return static_cast<HeaderField *>(ptr);
}

HttpHeaders::HeaderField *HttpHeaders::link_field(HeaderField *field) noexcept {
    std::uint32_t bucket = static_cast<std::uint32_t>(field->name_hash & (bucket_head_.size() - 1));
    field->next_bucket = bucket_head_[bucket];
    bucket_head_[bucket] = field;

    if (!all_head_) {
        all_head_ = field;
        all_tail_ = field;
    } else {
        field->prev_all = all_tail_;
        all_tail_->next_all = field;
        all_tail_ = field;
    }
    ++size_;
    return field;
}

} // namespace fiber::http
