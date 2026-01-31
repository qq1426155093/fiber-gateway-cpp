#ifndef FIBER_BUF_POOL_H
#define FIBER_BUF_POOL_H

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>

#include "../NonCopyable.h"
#include "../NonMovable.h"

namespace fiber::mem {

class BufPool : public common::NonCopyable, public common::NonMovable {
public:
    explicit BufPool(size_t block_size = 4096);
    ~BufPool();

    void *alloc(size_t size, size_t align = alignof(std::max_align_t));

    template <typename T>
    T *alloc(size_t n = 1) {
        size_t bytes = sizeof(T) * n;
        return static_cast<T *>(alloc(bytes, alignof(T)));
    }

private:
    struct Block {
        char *data = nullptr;
        size_t cap = 0;
        size_t used = 0;
        Block *next = nullptr;
    };

    struct LargeBlock {
        void *data = nullptr;
        LargeBlock *next = nullptr;
    };

    void *alloc_from_blocks(size_t size, size_t align);
    void *alloc_large(size_t size, size_t align);

    Block *allocate_block(size_t size);

    static size_t psz;

    Block *head_ = nullptr;
    Block *current_ = nullptr;
    LargeBlock *large_head_ = nullptr;
    size_t max_;
    size_t block_size_ = 0;
};

template <typename T>
class PoolAllocator {
public:
    using value_type = T;

    PoolAllocator() noexcept = default;
    explicit PoolAllocator(BufPool &pool) noexcept : pool_(&pool) {}

    template <typename U>
    PoolAllocator(const PoolAllocator<U> &other) noexcept : pool_(other.pool_) {}

    T *allocate(std::size_t n) {
        if (!pool_) {
            throw std::bad_alloc();
        }
        auto *ptr = pool_->alloc(sizeof(T) * n, alignof(T));
        if (!ptr) {
            throw std::bad_alloc();
        }
        return static_cast<T *>(ptr);
    }

    void deallocate(T *, std::size_t) noexcept {}

    template <typename U>
    bool operator==(const PoolAllocator<U> &other) const noexcept {
        return pool_ == other.pool_;
    }

    template <typename U>
    bool operator!=(const PoolAllocator<U> &other) const noexcept {
        return pool_ != other.pool_;
    }

private:
    template <typename U>
    friend class PoolAllocator;

    BufPool *pool_ = nullptr;
};

} // namespace fiber::mem

#endif // FIBER_BUF_POOL_H
