#include <hft/oe/oe_decoder.hpp>
#include <hft/oe/oe_encoder.hpp>
#include <hft/oe/wire_oe.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace hft;
using namespace hft::oe;

// ── mock listener ─────────────────────────────────────────────────────────────

struct MockListener : strategy::IOrderListener {
    struct FillEvent  { OrderId exch_id; Qty qty; Price px; };
    struct RejectEvent{ OrderId clord_id; std::string reason; };

    void on_ack(OrderId c, OrderId e)        override { acks.push_back({c, e}); }
    void on_fill(OrderId e, Qty q, Price p)  override { fills.push_back({e, q, p}); }
    void on_cancel_ack(OrderId e)            override { cancel_acks.push_back(e); }
    void on_reject(OrderId c, std::string_view r) override {
        rejects.push_back({c, std::string(r)});
    }

    std::vector<std::pair<OrderId,OrderId>> acks;
    std::vector<FillEvent>                  fills;
    std::vector<OrderId>                    cancel_acks;
    std::vector<RejectEvent>                rejects;
};

// ── encoder tests ─────────────────────────────────────────────────────────────

TEST(OeEncoder, NewOrderRoundtrip) {
    std::array<std::byte, 64> buf{};
    std::size_t n = OeEncoder::encode_new(buf, 42, 7, Side::Buy, 10000, 100);
    ASSERT_EQ(n, sizeof(wire::NewOrder));

    wire::NewOrder msg{};
    std::memcpy(&msg, buf.data(), sizeof(msg));
    EXPECT_EQ(msg.hdr.msg_type, wire::kNewOrder);
    EXPECT_EQ(msg.hdr.msg_len,  static_cast<uint16_t>(sizeof(wire::NewOrder)));
    EXPECT_EQ(msg.hdr.clord_id, 42u);
    EXPECT_EQ(msg.hdr.sym,      7u);
    EXPECT_EQ(msg.side,         static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(msg.px,           10000);
    EXPECT_EQ(msg.qty,          100);
}

TEST(OeEncoder, CancelRoundtrip) {
    std::array<std::byte, 64> buf{};
    std::size_t n = OeEncoder::encode_cancel(buf, 5, 3, 99);
    ASSERT_EQ(n, sizeof(wire::Cancel));

    wire::Cancel msg{};
    std::memcpy(&msg, buf.data(), sizeof(msg));
    EXPECT_EQ(msg.hdr.msg_type, wire::kCancel);
    EXPECT_EQ(msg.hdr.clord_id, 5u);
    EXPECT_EQ(msg.hdr.sym,      3u);
    EXPECT_EQ(msg.exch_id,      99u);
}

TEST(OeEncoder, ReplaceRoundtrip) {
    std::array<std::byte, 64> buf{};
    std::size_t n = OeEncoder::encode_replace(buf, 1, 2, 55, 20000, 50);
    ASSERT_EQ(n, sizeof(wire::Replace));

    wire::Replace msg{};
    std::memcpy(&msg, buf.data(), sizeof(msg));
    EXPECT_EQ(msg.hdr.msg_type, wire::kReplace);
    EXPECT_EQ(msg.exch_id,      55u);
    EXPECT_EQ(msg.px,           20000);
    EXPECT_EQ(msg.qty,          50);
}

TEST(OeEncoder, BufferTooSmallReturnsZero) {
    std::array<std::byte, 4> tiny{};
    EXPECT_EQ(OeEncoder::encode_new(tiny, 1, 1, Side::Buy, 1, 1), 0u);
}

// ── decoder tests ─────────────────────────────────────────────────────────────

// Helper: build a raw Ack frame and return it as bytes.
static std::vector<std::byte> make_ack(OrderId clord_id, OrderId exch_id) {
    wire::Ack msg{};
    msg.hdr.msg_type = wire::kAck;
    msg.hdr.msg_len  = sizeof(wire::Ack);
    msg.hdr.clord_id = clord_id;
    msg.exch_id      = exch_id;
    std::vector<std::byte> v(sizeof(msg));
    std::memcpy(v.data(), &msg, sizeof(msg));
    return v;
}

static std::vector<std::byte> make_fill(OrderId exch_id, Qty qty, Price px) {
    wire::Fill msg{};
    msg.hdr.msg_type  = wire::kFill;
    msg.hdr.msg_len   = sizeof(wire::Fill);
    msg.exch_id       = exch_id;
    msg.filled_qty    = qty;
    msg.px            = px;
    std::vector<std::byte> v(sizeof(msg));
    std::memcpy(v.data(), &msg, sizeof(msg));
    return v;
}

static std::vector<std::byte> make_cancel_ack(OrderId exch_id) {
    wire::CancelAck msg{};
    msg.hdr.msg_type = wire::kCancelAck;
    msg.hdr.msg_len  = sizeof(wire::CancelAck);
    msg.exch_id      = exch_id;
    std::vector<std::byte> v(sizeof(msg));
    std::memcpy(v.data(), &msg, sizeof(msg));
    return v;
}

static std::vector<std::byte> make_reject(OrderId clord_id, const char* reason) {
    wire::Reject msg{};
    msg.hdr.msg_type = wire::kReject;
    msg.hdr.msg_len  = sizeof(wire::Reject);
    msg.hdr.clord_id = clord_id;
    std::strncpy(msg.reason, reason, wire::kRejectReasonLen - 1);
    std::vector<std::byte> v(sizeof(msg));
    std::memcpy(v.data(), &msg, sizeof(msg));
    return v;
}

TEST(OeDecoder, AckFiresCallback) {
    MockListener ml;
    OeDecoder dec(ml);
    auto frame = make_ack(10, 999);
    dec.on_bytes(frame.data(), frame.size());
    ASSERT_EQ(ml.acks.size(), 1u);
    EXPECT_EQ(ml.acks[0].first,  10u);
    EXPECT_EQ(ml.acks[0].second, 999u);
}

TEST(OeDecoder, FillFiresCallback) {
    MockListener ml;
    OeDecoder dec(ml);
    auto frame = make_fill(77, 50, 12345);
    dec.on_bytes(frame.data(), frame.size());
    ASSERT_EQ(ml.fills.size(), 1u);
    EXPECT_EQ(ml.fills[0].exch_id, 77u);
    EXPECT_EQ(ml.fills[0].qty,     50);
    EXPECT_EQ(ml.fills[0].px,      12345);
}

TEST(OeDecoder, CancelAckFiresCallback) {
    MockListener ml;
    OeDecoder dec(ml);
    auto frame = make_cancel_ack(42);
    dec.on_bytes(frame.data(), frame.size());
    ASSERT_EQ(ml.cancel_acks.size(), 1u);
    EXPECT_EQ(ml.cancel_acks[0], 42u);
}

TEST(OeDecoder, RejectFiresCallback) {
    MockListener ml;
    OeDecoder dec(ml);
    auto frame = make_reject(3, "price_out_of_range");
    dec.on_bytes(frame.data(), frame.size());
    ASSERT_EQ(ml.rejects.size(), 1u);
    EXPECT_EQ(ml.rejects[0].clord_id, 3u);
    EXPECT_EQ(ml.rejects[0].reason,   "price_out_of_range");
}

TEST(OeDecoder, MultipleFramesInOneBatch) {
    MockListener ml;
    OeDecoder dec(ml);

    auto a1 = make_ack(1, 100);
    auto a2 = make_ack(2, 200);
    auto f1 = make_fill(100, 10, 5000);

    std::vector<std::byte> batch;
    batch.insert(batch.end(), a1.begin(), a1.end());
    batch.insert(batch.end(), a2.begin(), a2.end());
    batch.insert(batch.end(), f1.begin(), f1.end());

    dec.on_bytes(batch.data(), batch.size());

    EXPECT_EQ(ml.acks.size(),  2u);
    EXPECT_EQ(ml.fills.size(), 1u);
}

TEST(OeDecoder, FragmentedDelivery) {
    MockListener ml;
    OeDecoder dec(ml);

    auto frame = make_ack(7, 77);
    // Feed one byte at a time.
    for (std::size_t i = 0; i < frame.size(); ++i)
        dec.on_bytes(frame.data() + i, 1);

    ASSERT_EQ(ml.acks.size(), 1u);
    EXPECT_EQ(ml.acks[0].second, 77u);
}
