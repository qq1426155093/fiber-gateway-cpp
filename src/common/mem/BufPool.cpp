#include "BufPool.h"

#include <algorithm>
#include <cstdlib>

namespace fiber::mem {

BufPool::BufPool(size_t block_size) : block_size_(block_size) {
    if (block_size_ == 0) {
        block_size_ = 4096;
    }
}

BufPool::~BufPool() {
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

void BufPool::reset() {
    if (!head_) {
        return;
    }
    Block *block = head_->next;
    while (block) {
        Block *next = block->next;
        std::free(block->data);
        std::free(block);
        block = next;
    }
    head_->used = 0;
    head_->next = nullptr;
    current_ = head_;
}

BufPool::Block *BufPool::allocate_block(size_t size) {
    auto *block = static_cast<Block *>(std::malloc(sizeof(Block)));
    if (!block) {
        return nullptr;
    }
    block->data = static_cast<char *>(std::malloc(size));
    if (!block->data) {
        std::free(block);
        return nullptr;
    }
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
