//
// Created by dear on 2026/1/31.
//

#ifndef FIBER_HEADBUF_H
#define FIBER_HEADBUF_H

#include "../common/mem/BufPool.h"
namespace fiber::http {

// start     pos             last             end
//            |   readable     |   writable    |
struct BufChain : public common::NonMovable, common::NonCopyable {
    std::uint8_t *start; // start of memory
    std::uint8_t *end; // end of memory
    std::uint8_t *pos; // the reader pointer
    std::uint8_t *last; // the writer pointer
    BufChain *next;
    [[nodiscard]] size_t readable() const noexcept { return last - pos; }
    [[nodiscard]] size_t writable() const noexcept { return end - last; }
    [[nodiscard]] size_t capacity() const noexcept { return end - start; }
};

class HeaderBuffers {
public:
    static constexpr size_t kHeaderInitialSize = 8 * 1024;
    static constexpr size_t kHeaderLargeSize = 32 * 1024;
    static constexpr size_t kHeaderLargeMax = 4;

    struct Opt {
        size_t init_size = kHeaderInitialSize;
        size_t large_size = kHeaderLargeSize;
        size_t large_num = kHeaderLargeMax;
    };
    using Chain = BufChain;

    explicit HeaderBuffers() = default;
    explicit HeaderBuffers(const Opt &opt) : opt_(opt) {}

    Chain *alloc(fiber::mem::BufPool &pool) noexcept {
        Chain *c;
        if ((c = cursor_) != nullptr) {
            cursor_ = c->next;
            return c;
        }
        if (alloc_num_ > opt_.large_num) {
            return nullptr;
        }
        c = make_chain(pool, alloc_num_ ? opt_.large_size : opt_.init_size);
        if (c) {
            alloc_num_++;
            if (tail_ != nullptr) {
                tail_->next = c;
                tail_ = c;
            } else {
                head_ = c;
                tail_ = c;
            }
        }
        return c;
    }

    void reset() noexcept { cursor_ = head_; }
    [[nodiscard]] std::size_t alloc_num() const noexcept { return alloc_num_; }
    [[nodiscard]] const Opt &opt() const noexcept { return opt_; }
    [[nodiscard]] bool exhausted() const noexcept { return alloc_num_ > opt_.large_num; }

private:
    static Chain *make_chain(fiber::mem::BufPool &pool, std::size_t size) noexcept {
        auto *buf = static_cast<std::uint8_t *>(pool.alloc(size, alignof(char)));
        if (!buf) {
            return nullptr;
        }
        auto *chain = pool.alloc<Chain>();
        if (!chain) {
            return nullptr;
        }
        chain->start = buf;
        chain->end = buf + size;
        chain->pos = buf;
        chain->last = buf;
        chain->next = nullptr;
        return chain;
    }

    Opt opt_{};
    size_t alloc_num_{};
    Chain *head_{};
    Chain *tail_{};
    Chain *cursor_{};
};
} // namespace fiber::http

#endif // FIBER_HEADBUF_H
