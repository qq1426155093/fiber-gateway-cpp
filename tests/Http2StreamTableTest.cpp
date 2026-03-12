#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "http/Http2Stream.h"
#include "http/Http2StreamTable.h"

namespace {

std::size_t hash_stream_id(std::uint32_t stream_id) {
    std::uint32_t value = stream_id * 2654435761u;
    value ^= value >> 16;
    return value;
}

std::array<std::uint32_t, 3> find_colliding_stream_ids(std::size_t bucket_count) {
    std::array<std::uint32_t, 3> out{};
    std::size_t target_bucket = hash_stream_id(1) & (bucket_count - 1);
    std::size_t found = 0;

    for (std::uint32_t stream_id = 1; stream_id < 100000 && found < out.size(); ++stream_id) {
        if ((hash_stream_id(stream_id) & (bucket_count - 1)) == target_bucket) {
            out[found++] = stream_id;
        }
    }

    EXPECT_EQ(found, out.size());
    return out;
}

} // namespace

TEST(Http2StreamTableTest, InitializesFixedCapacityFromMaxActiveStreams) {
    fiber::http::Http2StreamTable table;

    ASSERT_TRUE(table.init(3));
    EXPECT_EQ(table.max_active_streams(), 3u);
    EXPECT_EQ(table.bucket_count(), 8u);
    EXPECT_TRUE(table.empty());
}

TEST(Http2StreamTableTest, InsertsFindsAndRejectsDuplicateStreamIds) {
    fiber::http::Http2StreamTable table;
    ASSERT_TRUE(table.init(4));

    fiber::http::Http2Stream stream1(1);
    fiber::http::Http2Stream stream3(3);
    fiber::http::Http2Stream duplicate1(1);

    EXPECT_TRUE(table.insert(stream1));
    EXPECT_TRUE(table.insert(stream3));
    EXPECT_FALSE(table.insert(duplicate1));
    EXPECT_EQ(table.size(), 2u);
    EXPECT_EQ(table.find(1), &stream1);
    EXPECT_EQ(table.find(3), &stream3);
    EXPECT_EQ(table.find(5), nullptr);
}

TEST(Http2StreamTableTest, RejectsInsertPastConfiguredMaxActiveStreams) {
    fiber::http::Http2StreamTable table;
    ASSERT_TRUE(table.init(2));

    fiber::http::Http2Stream stream1(1);
    fiber::http::Http2Stream stream3(3);
    fiber::http::Http2Stream stream5(5);

    EXPECT_TRUE(table.insert(stream1));
    EXPECT_TRUE(table.insert(stream3));
    EXPECT_FALSE(table.insert(stream5));
    EXPECT_EQ(table.size(), 2u);
}

TEST(Http2StreamTableTest, EraseKeepsLaterCollisionsReachable) {
    fiber::http::Http2StreamTable table;
    ASSERT_TRUE(table.init(4));

    auto ids = find_colliding_stream_ids(table.bucket_count());
    fiber::http::Http2Stream stream_a(ids[0]);
    fiber::http::Http2Stream stream_b(ids[1]);
    fiber::http::Http2Stream stream_c(ids[2]);

    ASSERT_TRUE(table.insert(stream_a));
    ASSERT_TRUE(table.insert(stream_b));
    ASSERT_TRUE(table.insert(stream_c));

    EXPECT_EQ(table.erase(stream_a.stream_id()), &stream_a);
    EXPECT_EQ(table.find(stream_b.stream_id()), &stream_b);
    EXPECT_EQ(table.find(stream_c.stream_id()), &stream_c);

    EXPECT_EQ(table.erase(stream_b.stream_id()), &stream_b);
    EXPECT_EQ(table.find(stream_c.stream_id()), &stream_c);
    EXPECT_EQ(table.size(), 1u);
}
