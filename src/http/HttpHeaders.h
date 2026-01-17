#ifndef FIBER_HTTP_HEADERS_H
#define FIBER_HTTP_HEADERS_H

#include <cstddef>
#include <cstdint>
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
        const char *value = nullptr;
        uint32_t value_len = 0;
        uint64_t name_hash = 0;
        uint32_t next = 0;

        std::string_view name_view() const noexcept { return {name, name_len}; }
        std::string_view value_view() const noexcept { return {value, value_len}; }
    };

    explicit HttpHeaders(mem::BufPool &pool);

    bool add(std::string_view name, std::string_view value);
    bool set(std::string_view name, std::string_view value);
    std::string_view get(std::string_view name) const noexcept;
    bool contains(std::string_view name) const noexcept;
    size_t remove(std::string_view name) noexcept;

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
    const char *copy_to_pool(std::string_view data, bool &ok);

    mem::BufPool *pool_ = nullptr;
    FieldVector fields_;
    BucketVector bucket_head_;
    BucketVector bucket_tail_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HEADERS_H
