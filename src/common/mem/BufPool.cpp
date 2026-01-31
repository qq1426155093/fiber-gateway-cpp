#include "BufPool.h"

#include <algorithm>
#include <cstdlib>

#if defined(_WIN32)
#include <malloc.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fiber::mem {

namespace {

size_t page_size() {
    static size_t size = 0;
    if (size != 0) {
        return size;
    }
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    size = static_cast<size_t>(info.dwPageSize);
#else
    long value = ::sysconf(_SC_PAGESIZE);
    if (value <= 0) {
        value = 4096;
    }
    size = static_cast<size_t>(value);
#endif
    if (size == 0) {
        size = 4096;
    }
    return size;
}

void *system_alloc(size_t size, size_t align) {
#if defined(_WIN32)
    return _aligned_malloc(size, align);
#else
    void *ptr = nullptr;
    if (posix_memalign(&ptr, align, size) != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

void system_free(void *ptr) {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

} // namespace

size_t BufPool::psz = page_size();

BufPool::BufPool(size_t block_size) : block_size_(block_size) {
    if (block_size_ <= sizeof(Block)) {
        block_size_ = 4096;
    }
    max_ = std::min(psz - 1, block_size_ - sizeof(Block));
}

BufPool::~BufPool() {
    LargeBlock *large = large_head_;
    while (large) {
        LargeBlock *next = large->next;
        system_free(large->data);
        large = next;
    }
    large_head_ = nullptr;

    Block *block = head_;
    while (block) {
        Block *next = block->next;
        std::free(block->data);
        std::free(block);
        block = next;
    }
    head_ = nullptr;
    current_ = nullptr;
}

void *BufPool::alloc(size_t size, size_t align) {
    if (size == 0) {
        return nullptr;
    }
    if (align < alignof(std::max_align_t)) {
        align = alignof(std::max_align_t);
    }
    if (size <= psz) {
        return alloc_from_blocks(size, align);
    }
    return alloc_large(size, align);
}

void *BufPool::alloc_from_blocks(size_t size, size_t align) {
    if (!current_) {
        if (!allocate_block(std::max(block_size_, size + align))) {
            return nullptr;
        }
    }

    for (;;) {
        void *ptr = current_->data + current_->used;
        size_t space = current_->cap - current_->used;
        void *aligned = std::align(align, size, ptr, space);
        if (aligned) {
            size_t used = static_cast<char *>(aligned) - current_->data + size;
            current_->used = used;
            return aligned;
        }
        if (!allocate_block(std::max(block_size_, size + align))) {
            return nullptr;
        }
    }
}

void *BufPool::alloc_large(size_t size, size_t align) {
    void *data = system_alloc(size, align);
    if (!data) {
        return nullptr;
    }
    auto *node = static_cast<LargeBlock *>(alloc_from_blocks(sizeof(LargeBlock), alignof(LargeBlock)));
    if (!node) {
        system_free(data);
        return nullptr;
    }
    node->data = data;
    node->next = large_head_;
    large_head_ = node;
    return data;
}

void BufPool::reset() {
    if (!head_) {
        return;
    }
    LargeBlock *large = large_head_;
    while (large) {
        LargeBlock *next = large->next;
        system_free(large->data);
        large = next;
    }
    large_head_ = nullptr;

    Block *block = head_->next;
    while (block) {
        Block *next = block->next;
        std::free(block);
        block = next;
    }
    head_->used = 0;
    head_->next = nullptr;
    current_ = head_;
}

BufPool::Block *BufPool::allocate_block(size_t size) {
    Block *block = static_cast<Block *>(system_alloc(block_size_, alignof(Block)));
    if (!block) {
        return nullptr;
    }
    block->data = reinterpret_cast<char *>(block) + sizeof(Block);
    block->cap = size;
    block->used = 0;
    block->next = nullptr;

    if (!head_) {
        head_ = block;
        current_ = block;
    } else {
        current_->next = block;
        current_ = block;
    }
    return block;
}

} // namespace fiber::mem
