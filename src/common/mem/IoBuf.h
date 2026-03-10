#ifndef FIBER_COMMON_MEM_IOBUF_H
#define FIBER_COMMON_MEM_IOBUF_H

#include <cstddef>
#include <cstdint>
#include <sys/uio.h>

namespace fiber::mem {

class IoBuf {
public:
    IoBuf() noexcept = default;
    ~IoBuf();

    IoBuf(const IoBuf &other) noexcept;
    IoBuf &operator=(const IoBuf &other) noexcept;

    IoBuf(IoBuf &&other) noexcept;
    IoBuf &operator=(IoBuf &&other) noexcept;

    [[nodiscard]] static IoBuf allocate(std::size_t capacity) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t view_size() const noexcept;
    [[nodiscard]] std::size_t readable() const noexcept;
    [[nodiscard]] std::size_t writable() const noexcept;
    [[nodiscard]] std::size_t headroom() const noexcept;
    [[nodiscard]] std::size_t tailroom() const noexcept;
    [[nodiscard]] bool unique() const noexcept;
    [[nodiscard]] std::uint32_t use_count() const noexcept;

    [[nodiscard]] std::uint8_t *data() noexcept;
    [[nodiscard]] const std::uint8_t *data() const noexcept;
    [[nodiscard]] std::uint8_t *view_begin() noexcept;
    [[nodiscard]] const std::uint8_t *view_begin() const noexcept;
    [[nodiscard]] std::uint8_t *view_end() noexcept;
    [[nodiscard]] const std::uint8_t *view_end() const noexcept;
    [[nodiscard]] std::uint8_t *readable_data() noexcept;
    [[nodiscard]] const std::uint8_t *readable_data() const noexcept;
    [[nodiscard]] std::uint8_t *writable_data() noexcept;
    [[nodiscard]] const std::uint8_t *writable_data() const noexcept;

    void clear() noexcept;
    void reset() noexcept;
    void consume(std::size_t bytes) noexcept;
    void commit(std::size_t bytes) noexcept;
    void swap(IoBuf &other) noexcept;

    [[nodiscard]] IoBuf retain_slice(std::size_t offset, std::size_t len) const noexcept;
    [[nodiscard]] IoBuf unsafe_retain_slice(std::size_t offset, std::size_t len) const noexcept;

private:
    struct ControlBlock;

    explicit IoBuf(ControlBlock *control, std::uint8_t *view_begin, std::uint8_t *view_end, std::uint8_t *pos,
                   std::uint8_t *last) noexcept;

    [[nodiscard]] IoBuf retain_slice_impl(std::size_t offset, std::size_t len, bool safe) const noexcept;

    static std::uint8_t *storage_begin(ControlBlock *control) noexcept;
    static const std::uint8_t *storage_begin(const ControlBlock *control) noexcept;
    static void retain(ControlBlock *control) noexcept;
    static void unsafe_retain(ControlBlock *control) noexcept;
    static void release(ControlBlock *control) noexcept;

    void reset_handle() noexcept;

    ControlBlock *control_ = nullptr;
    std::uint8_t *view_begin_ = nullptr;
    std::uint8_t *view_end_ = nullptr;
    std::uint8_t *pos_ = nullptr;
    std::uint8_t *last_ = nullptr;
};

class IoBufChain {
public:
    IoBufChain() noexcept = default;
    ~IoBufChain();

    IoBufChain(const IoBufChain &) = delete;
    IoBufChain &operator=(const IoBufChain &) = delete;

    IoBufChain(IoBufChain &&other) noexcept;
    IoBufChain &operator=(IoBufChain &&other) noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t readable_bytes() const noexcept;
    [[nodiscard]] std::size_t writable_bytes() const noexcept;

    bool append(IoBuf &&buf) noexcept;
    bool prepend(IoBuf &&buf) noexcept;
    void clear() noexcept;
    void consume(std::size_t bytes) noexcept;
    void drop_empty_front() noexcept;
    void consume_and_compact(std::size_t bytes) noexcept;
    void commit(std::size_t bytes) noexcept;

    [[nodiscard]] int fill_write_iov(struct iovec *iov, int max_iov) const noexcept;
    [[nodiscard]] int fill_read_iov(struct iovec *iov, int max_iov) const noexcept;

    [[nodiscard]] IoBuf *front() noexcept;
    [[nodiscard]] const IoBuf *front() const noexcept;

private:
    struct Node {
        IoBuf buf;
        Node *next = nullptr;
    };

    static void delete_nodes(Node *node) noexcept;

    Node *head_ = nullptr;
    Node *tail_ = nullptr;
    std::size_t size_ = 0;
    std::size_t readable_bytes_ = 0;
    std::size_t writable_bytes_ = 0;
};

} // namespace fiber::mem

#endif // FIBER_COMMON_MEM_IOBUF_H
