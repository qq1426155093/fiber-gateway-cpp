#ifndef FIBER_HTTP_HEADERS_H
#define FIBER_HTTP_HEADERS_H

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"

namespace fiber::http {

class HttpHeaders : public common::NonCopyable, public common::NonMovable {
public:
    struct HeaderField {
        const char *name = nullptr;
        uint32_t name_len = 0;
        const char *lowcase_name = nullptr;
        uint32_t lowcase_len = 0;
        const char *value = nullptr;
        uint32_t value_len = 0;
        uint64_t name_hash = 0;
        uint32_t next = 0;

        std::string_view name_view() const noexcept { return {name, name_len}; }
        std::string_view lowcase_view() const noexcept { return {lowcase_name, lowcase_len}; }
        std::string_view value_view() const noexcept { return {value, value_len}; }
    };

    explicit HttpHeaders(mem::BufPool &pool);

    bool add(std::string_view name, std::string_view value);
    bool add(std::string_view name, std::string_view value, std::string_view lowcase_name, uint64_t hash);
    bool set(std::string_view name, std::string_view value);
    bool set(std::string_view name, std::string_view value, std::string_view lowcase_name, uint64_t hash);
    std::string_view get(std::string_view name) const noexcept;
    bool contains(std::string_view name) const noexcept;
    size_t remove(std::string_view name) noexcept;

    class MatchIterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = HeaderField;
        using difference_type = std::ptrdiff_t;
        using pointer = const HeaderField *;
        using reference = const HeaderField &;

        MatchIterator() = default;
        MatchIterator(const HttpHeaders *headers, std::string_view key, uint64_t hash, uint32_t index)
            : headers_(headers), key_(key), hash_(hash), index_(index) {}

        reference operator*() const { return headers_->fields_[index_]; }
        pointer operator->() const { return &headers_->fields_[index_]; }
        MatchIterator &operator++() {
            if (!headers_ || index_ == kInvalidIndex) {
                return *this;
            }
            index_ = headers_->next_match_index(index_, key_, hash_);
            return *this;
        }
        MatchIterator operator++(int) {
            MatchIterator copy = *this;
            ++(*this);
            return copy;
        }
        bool operator==(const MatchIterator &other) const {
            return headers_ == other.headers_ && index_ == other.index_;
        }
        bool operator!=(const MatchIterator &other) const { return !(*this == other); }

    private:
        const HttpHeaders *headers_ = nullptr;
        std::string_view key_;
        uint64_t hash_ = 0;
        uint32_t index_ = kInvalidIndex;
    };

    class MatchRange {
    public:
        MatchRange() = default;
        MatchRange(const HttpHeaders *headers, std::string_view key, uint64_t hash)
            : headers_(headers), key_(key), hash_(hash) {}

        MatchIterator begin() const noexcept {
            if (!headers_) {
                return end();
            }
            return MatchIterator(headers_, key_, hash_, headers_->find_first_index_lowcase(key_, hash_));
        }
        MatchIterator end() const noexcept { return MatchIterator(headers_, key_, hash_, kInvalidIndex); }

    private:
        friend class HttpHeaders;

        const HttpHeaders *headers_ = nullptr;
        std::string owned_key_;
        std::string_view key_;
        uint64_t hash_ = 0;
    };

    MatchRange get_all(std::string_view lowcase_key, uint64_t hash) const noexcept;
    MatchRange get_all(std::string_view name) const;

    void reserve_bytes(size_t bytes);
    void clear() noexcept;
    void release() noexcept;
    size_t size() const noexcept;

    std::vector<HeaderField, mem::PoolAllocator<HeaderField>>::const_iterator begin() const noexcept;
    std::vector<HeaderField, mem::PoolAllocator<HeaderField>>::const_iterator end() const noexcept;

private:
    using FieldVector = std::vector<HeaderField, mem::PoolAllocator<HeaderField>>;
    using BucketVector = std::vector<uint32_t, mem::PoolAllocator<uint32_t>>;

    static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
    static constexpr size_t kDefaultBuckets = 32;

    bool ensure_buckets();
    void init_buckets(size_t count);
    void rebuild_buckets() noexcept;
    uint32_t find_first_index(std::string_view name) const noexcept;
    uint32_t find_first_index_lowcase(std::string_view lowcase_key, uint64_t hash) const noexcept;
    uint32_t next_match_index(uint32_t start, std::string_view lowcase_key, uint64_t hash) const noexcept;
    size_t remove_lowcase(std::string_view lowcase_key, uint64_t hash) noexcept;
    const char *copy_to_pool(std::string_view data, bool &ok);
    const char *copy_lowercase_to_pool(std::string_view data, uint64_t &hash, bool &ok);

    mem::BufPool *pool_ = nullptr;
    FieldVector fields_;
    BucketVector bucket_head_;
    BucketVector bucket_tail_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HEADERS_H
