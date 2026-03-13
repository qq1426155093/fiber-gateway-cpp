#ifndef FIBER_COMMON_INTRUSIVE_LIST_H
#define FIBER_COMMON_INTRUSIVE_LIST_H

#include <cstddef>
#include <cstdint>

namespace fiber::common {

struct IntrusiveListHook {
    IntrusiveListHook *prev = nullptr;
    IntrusiveListHook *next = nullptr;
    bool in_list = false;

    [[nodiscard]] bool linked() const noexcept { return in_list; }
};

template <typename T, std::size_t Offset>
class IntrusiveList {
public:
    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

    [[nodiscard]] T *front() noexcept { return owner_from_hook(head_); }
    [[nodiscard]] const T *front() const noexcept { return owner_from_hook(head_); }

    [[nodiscard]] T *next_of(T &owner) noexcept { return owner_from_hook(hook_of(owner).next); }
    [[nodiscard]] const T *next_of(const T &owner) const noexcept { return owner_from_hook(hook_of(owner).next); }

    void push_back(T &owner) noexcept {
        IntrusiveListHook &hook = hook_of(owner);
        if (hook.in_list) {
            return;
        }

        hook.prev = tail_;
        hook.next = nullptr;
        if (tail_) {
            tail_->next = &hook;
        } else {
            head_ = &hook;
        }
        tail_ = &hook;
        hook.in_list = true;
    }

    void erase(T &owner) noexcept {
        IntrusiveListHook &hook = hook_of(owner);
        if (!hook.in_list) {
            return;
        }

        if (hook.prev) {
            hook.prev->next = hook.next;
        } else {
            head_ = hook.next;
        }
        if (hook.next) {
            hook.next->prev = hook.prev;
        } else {
            tail_ = hook.prev;
        }
        hook.prev = nullptr;
        hook.next = nullptr;
        hook.in_list = false;
    }

private:
    [[nodiscard]] static IntrusiveListHook &hook_of(T &owner) noexcept {
        return *reinterpret_cast<IntrusiveListHook *>(reinterpret_cast<std::uint8_t *>(&owner) + Offset);
    }

    [[nodiscard]] static const IntrusiveListHook &hook_of(const T &owner) noexcept {
        return *reinterpret_cast<const IntrusiveListHook *>(reinterpret_cast<const std::uint8_t *>(&owner) + Offset);
    }

    [[nodiscard]] static T *owner_from_hook(IntrusiveListHook *hook) noexcept {
        if (!hook) {
            return nullptr;
        }
        return reinterpret_cast<T *>(reinterpret_cast<std::uint8_t *>(hook) - Offset);
    }

    [[nodiscard]] static const T *owner_from_hook(const IntrusiveListHook *hook) noexcept {
        if (!hook) {
            return nullptr;
        }
        return reinterpret_cast<const T *>(reinterpret_cast<const std::uint8_t *>(hook) - Offset);
    }

    IntrusiveListHook *head_ = nullptr;
    IntrusiveListHook *tail_ = nullptr;
};

} // namespace fiber::common

#endif // FIBER_COMMON_INTRUSIVE_LIST_H
