#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string_view>
#include <type_traits>

#include "common/mem/IoBuf.h"

namespace {

using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;

std::string_view readable_view(const IoBuf &buf) {
    return {reinterpret_cast<const char *>(buf.readable_data()), buf.readable()};
}

static_assert(std::is_copy_constructible_v<IoBuf>);
static_assert(std::is_copy_assignable_v<IoBuf>);
static_assert(std::is_move_constructible_v<IoBuf>);
static_assert(std::is_move_assignable_v<IoBuf>);
static_assert(!std::is_copy_constructible_v<IoBufChain>);
static_assert(std::is_move_constructible_v<IoBufChain>);

TEST(IoBufTest, AllocateCommitConsumeCopyAndMove) {
    IoBuf buf = IoBuf::allocate(32);
    ASSERT_TRUE(buf);
    EXPECT_EQ(buf.capacity(), 32u);
    EXPECT_EQ(buf.readable(), 0u);
    EXPECT_EQ(buf.writable(), 32u);
    EXPECT_TRUE(buf.unique());
    EXPECT_EQ(buf.use_count(), 1u);

    std::memcpy(buf.writable_data(), "hello", 5);
    buf.commit(5);
    EXPECT_EQ(readable_view(buf), "hello");
    EXPECT_EQ(buf.readable(), 5u);
    EXPECT_EQ(buf.writable(), 27u);

    IoBuf copy(buf);
    ASSERT_TRUE(copy);
    EXPECT_EQ(copy.use_count(), 2u);
    EXPECT_EQ(copy.data(), buf.data());
    EXPECT_EQ(copy.readable_data(), buf.readable_data());
    EXPECT_EQ(readable_view(copy), "hello");

    buf.consume(2);
    EXPECT_EQ(readable_view(buf), "llo");
    EXPECT_EQ(buf.headroom(), 2u);
    EXPECT_EQ(readable_view(copy), "hello");

    IoBuf assigned;
    assigned = buf;
    ASSERT_TRUE(assigned);
    EXPECT_EQ(buf.use_count(), 3u);
    EXPECT_EQ(assigned.data(), buf.data());
    EXPECT_EQ(assigned.readable_data(), buf.readable_data());
    EXPECT_EQ(readable_view(assigned), "llo");

    buf.clear();
    EXPECT_EQ(buf.readable(), 0u);
    EXPECT_EQ(buf.writable(), 32u);

    IoBuf moved = std::move(buf);
    EXPECT_FALSE(buf.valid());
    ASSERT_TRUE(moved);
    EXPECT_EQ(moved.capacity(), 32u);
    EXPECT_EQ(moved.use_count(), 3u);
}

TEST(IoBufTest, RetainSliceSharesStorageWithoutCopy) {
    IoBuf buf = IoBuf::allocate(32);
    ASSERT_TRUE(buf);

    std::memcpy(buf.writable_data(), "abcdef", 6);
    buf.commit(6);

    IoBuf slice = buf.retain_slice(1, 3);
    ASSERT_TRUE(slice);
    EXPECT_EQ(buf.use_count(), 2u);
    EXPECT_EQ(slice.use_count(), 2u);
    EXPECT_FALSE(buf.unique());
    EXPECT_EQ(readable_view(slice), "bcd");
    EXPECT_EQ(slice.writable(), 0u);

    slice.consume(1);
    EXPECT_EQ(readable_view(slice), "cd");
    EXPECT_EQ(readable_view(buf), "abcdef");
}

TEST(IoBufTest, UnsafeRetainSliceUsesSameStorage) {
    IoBuf buf = IoBuf::allocate(16);
    ASSERT_TRUE(buf);

    std::memcpy(buf.writable_data(), "payload", 7);
    buf.commit(7);

    IoBuf slice = buf.unsafe_retain_slice(2, 4);
    ASSERT_TRUE(slice);
    EXPECT_EQ(buf.use_count(), 2u);
    EXPECT_EQ(readable_view(slice), "yloa");
    EXPECT_EQ(slice.data(), buf.data());
}

TEST(IoBufTest, ChainExportsReadableAndWritableIovecs) {
    IoBuf a = IoBuf::allocate(8);
    IoBuf b = IoBuf::allocate(8);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);

    std::memcpy(a.writable_data(), "ab", 2);
    a.commit(2);
    std::memcpy(b.writable_data(), "cdef", 4);
    b.commit(4);

    IoBufChain chain;
    ASSERT_TRUE(chain.append(std::move(a)));
    ASSERT_TRUE(chain.append(std::move(b)));
    EXPECT_EQ(chain.size(), 2u);
    EXPECT_EQ(chain.readable_bytes(), 6u);

    std::array<iovec, 4> iov{};
    int count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    ASSERT_EQ(count, 2);
    EXPECT_EQ(std::string_view(static_cast<const char *>(iov[0].iov_base), iov[0].iov_len), "ab");
    EXPECT_EQ(std::string_view(static_cast<const char *>(iov[1].iov_base), iov[1].iov_len), "cdef");

    chain.consume(3);
    EXPECT_EQ(chain.readable_bytes(), 3u);
    EXPECT_EQ(chain.size(), 2u);
    count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    ASSERT_EQ(count, 1);
    EXPECT_EQ(std::string_view(static_cast<const char *>(iov[0].iov_base), iov[0].iov_len), "def");
}

TEST(IoBufTest, DropEmptyFrontRemovesOnlyDrainedPrefix) {
    IoBuf a = IoBuf::allocate(4);
    IoBuf b = IoBuf::allocate(4);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);

    std::memcpy(a.writable_data(), "ab", 2);
    a.commit(2);
    std::memcpy(b.writable_data(), "cd", 2);
    b.commit(2);

    IoBufChain chain;
    ASSERT_TRUE(chain.append(std::move(a)));
    ASSERT_TRUE(chain.append(std::move(b)));

    chain.consume(2);
    ASSERT_NE(chain.front(), nullptr);
    EXPECT_EQ(chain.size(), 2u);
    EXPECT_EQ(chain.front()->readable(), 0u);

    chain.drop_empty_front();
    ASSERT_NE(chain.front(), nullptr);
    EXPECT_EQ(chain.size(), 1u);
    EXPECT_EQ(readable_view(*chain.front()), "cd");
    EXPECT_EQ(chain.writable_bytes(), 2u);
}

TEST(IoBufTest, ConsumeAndCompactDropsFullyConsumedFrontNodes) {
    IoBuf a = IoBuf::allocate(4);
    IoBuf b = IoBuf::allocate(4);
    IoBuf c = IoBuf::allocate(4);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    ASSERT_TRUE(c);

    std::memcpy(a.writable_data(), "ab", 2);
    a.commit(2);
    std::memcpy(b.writable_data(), "cd", 2);
    b.commit(2);
    std::memcpy(c.writable_data(), "ef", 2);
    c.commit(2);

    IoBufChain chain;
    ASSERT_TRUE(chain.append(std::move(a)));
    ASSERT_TRUE(chain.append(std::move(b)));
    ASSERT_TRUE(chain.append(std::move(c)));

    chain.consume_and_compact(3);
    ASSERT_NE(chain.front(), nullptr);
    EXPECT_EQ(chain.size(), 2u);
    EXPECT_EQ(readable_view(*chain.front()), "d");
    EXPECT_EQ(chain.readable_bytes(), 3u);
    EXPECT_EQ(chain.writable_bytes(), 4u);

    chain.consume_and_compact(3);
    EXPECT_TRUE(chain.empty());
    EXPECT_EQ(chain.size(), 0u);
    EXPECT_EQ(chain.front(), nullptr);
}

TEST(IoBufTest, ChainCommitBuildsWritableIovecs) {
    IoBuf a = IoBuf::allocate(4);
    IoBuf b = IoBuf::allocate(3);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);

    IoBufChain chain;
    ASSERT_TRUE(chain.append(std::move(a)));
    ASSERT_TRUE(chain.append(std::move(b)));
    EXPECT_EQ(chain.writable_bytes(), 7u);

    std::array<iovec, 4> iov{};
    int count = chain.fill_read_iov(iov.data(), static_cast<int>(iov.size()));
    ASSERT_EQ(count, 2);
    EXPECT_EQ(iov[0].iov_len, 4u);
    EXPECT_EQ(iov[1].iov_len, 3u);

    std::memcpy(iov[0].iov_base, "wxyz", 4);
    std::memcpy(iov[1].iov_base, "12", 2);
    chain.commit(6);

    EXPECT_EQ(chain.readable_bytes(), 6u);

    count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    ASSERT_EQ(count, 2);
    EXPECT_EQ(std::string_view(static_cast<const char *>(iov[0].iov_base), iov[0].iov_len), "wxyz");
    EXPECT_EQ(std::string_view(static_cast<const char *>(iov[1].iov_base), iov[1].iov_len), "12");
}

} // namespace
