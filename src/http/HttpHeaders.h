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

inline void to_lowercase(std::string_view name, char *dst) {
    for (size_t i = 0; i < name.size(); ++i) {
        unsigned char lower = static_cast<unsigned char>(name[i]);
        if (lower >= 'A' && lower <= 'Z') {
            lower = static_cast<unsigned char>(lower - 'A' + 'a');
        }
        dst[i] = static_cast<char>(lower);
    }
}

class HttpHeaders : public common::NonCopyable, public common::NonMovable {
public:
    struct HeaderField {
        const char *name = nullptr;
        uint32_t name_len = 0;
        const char *lowcase_name = nullptr;
        const char *value = nullptr;
        uint32_t value_len = 0;
        uint64_t name_hash = 0;
        HeaderField *next_bucket = nullptr;
        HeaderField *next_all = nullptr;
        HeaderField *prev_all = nullptr;

        std::string_view name_view() const noexcept { return {name, name_len}; }
        std::string_view lowcase_view() const noexcept { return {lowcase_name, name_len}; }
        std::string_view value_view() const noexcept { return {value, value_len}; }
    };

    explicit HttpHeaders(mem::BufPool &pool);

    HeaderField *add(std::string_view name, std::string_view value);
    HeaderField *add(std::string_view name, std::string_view value, char *lowcase_name, uint64_t hash);
    HeaderField *set(std::string_view name, std::string_view value);
    HeaderField *set(std::string_view name, std::string_view value, char *lowcase_name, uint64_t hash);
    // add_view/set_view keep external pointers; caller guarantees lifetime.
    // For correct lowcase-key lookups, prefer add_view with lowcase_name or pass lowercase name.
    HeaderField *add_view(std::string_view name, std::string_view value);
    HeaderField *add_view(std::string_view name, std::string_view value, char *lowcase_name, uint64_t hash);
    HeaderField *set_view(std::string_view name, std::string_view value);
    HeaderField *set_view(std::string_view name, std::string_view value, char *lowcase_name, uint64_t hash);
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
        MatchIterator(const HttpHeaders *headers, std::string_view key, uint64_t hash, const HeaderField *node) :
            headers_(headers), key_(key), hash_(hash), node_(node) {}

        reference operator*() const { return *node_; }
        pointer operator->() const { return node_; }
        MatchIterator &operator++() {
            if (!headers_ || !node_) {
                return *this;
            }
            node_ = headers_->next_match_node(node_, key_, hash_);
            return *this;
        }
        MatchIterator operator++(int) {
            MatchIterator copy = *this;
            ++(*this);
            return copy;
        }
        bool operator==(const MatchIterator &other) const { return node_ == other.node_; }
        bool operator!=(const MatchIterator &other) const { return !(*this == other); }

    private:
        const HttpHeaders *headers_ = nullptr;
        std::string_view key_;
        uint64_t hash_ = 0;
        const HeaderField *node_ = nullptr;
    };

    class MatchRange {
    public:
        MatchRange() = default;
        MatchRange(const HttpHeaders *headers, std::string_view key, uint64_t hash) :
            headers_(headers), key_(key), hash_(hash) {}

        MatchIterator begin() const noexcept {
            if (!headers_) {
                return end();
            }
            return MatchIterator(headers_, key_, hash_, headers_->find_first_node_lowcase(key_, hash_));
        }
        MatchIterator end() const noexcept { return MatchIterator(headers_, key_, hash_, nullptr); }

    private:
        friend class HttpHeaders;

        const HttpHeaders *headers_ = nullptr;
        std::string owned_key_;
        std::string_view key_;
        uint64_t hash_ = 0;
    };

    MatchRange get_all(std::string_view lowcase_key, uint64_t hash) const noexcept;
    MatchRange get_all(std::string_view name) const;

    class ConstIterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = HeaderField;
        using difference_type = std::ptrdiff_t;
        using pointer = const HeaderField *;
        using reference = const HeaderField &;

        ConstIterator() = default;
        explicit ConstIterator(const HeaderField *node) : node_(node) {}

        reference operator*() const { return *node_; }
        pointer operator->() const { return node_; }
        ConstIterator &operator++() {
            if (node_) {
                node_ = node_->next_all;
            }
            return *this;
        }
        ConstIterator operator++(int) {
            ConstIterator copy = *this;
            ++(*this);
            return copy;
        }
        bool operator==(const ConstIterator &other) const { return node_ == other.node_; }
        bool operator!=(const ConstIterator &other) const { return !(*this == other); }

    private:
        const HeaderField *node_ = nullptr;
    };

    void reserve_bytes(size_t bytes);
    void clear() noexcept;
    void release() noexcept;
    size_t size() const noexcept;

    ConstIterator begin() const noexcept;
    ConstIterator end() const noexcept;

private:
    using BucketVector = std::vector<HeaderField *, mem::PoolAllocator<HeaderField *>>;

    static constexpr size_t kDefaultBuckets = 32;

    bool ensure_buckets();
    void init_buckets(size_t count);
    void rebuild_buckets() noexcept;
    HeaderField *find_first_node(std::string_view name) const noexcept;
    const HeaderField *find_first_node_lowcase(std::string_view lowcase_key, uint64_t hash) const noexcept;
    const HeaderField *next_match_node(const HeaderField *start, std::string_view lowcase_key,
                                       uint64_t hash) const noexcept;
    size_t remove_lowcase(std::string_view lowcase_key, uint64_t hash) noexcept;
    const char *copy_to_pool(std::string_view data);
    const char *copy_lowercase_to_pool(std::string_view data, uint64_t &hash);
    HeaderField *alloc_field() noexcept {
        return static_cast<HeaderField *>(pool_->alloc(sizeof(HeaderField), alignof(HeaderField)));
    }
    HeaderField *link_field(HeaderField *field) noexcept;

    mem::BufPool *pool_ = nullptr;
    BucketVector bucket_head_;
    HeaderField *all_head_ = nullptr;
    HeaderField *all_tail_ = nullptr;
    size_t size_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HEADERS_H
