#include <hft/pipeline/gap_detector.hpp>
#include <gtest/gtest.h>
#include <vector>

using namespace hft;
using namespace hft::pipeline;

namespace {

// ─── Minimal sink: records (seq, event) pairs ─────────────────────────

struct Sink {
  struct Entry { uint64_t seq; md::MdEvent ev; };
  std::vector<Entry> received;
  void on_md(uint64_t seq, const md::MdEvent &e) {
    received.push_back({seq, e});
  }
};

// ─── Helpers ──────────────────────────────────────────────────────────

md::MdEvent make_add(md::SymbolId sym = 1, OrderId id = 1) {
  return md::Add{0, sym, id, Side::Buy, 100, 10};
}

[[maybe_unused]] md::MdEvent make_cancel(OrderId id = 1) {
  return md::Cancel{0, 1, id};
}

// Build a GapDetector wired to a Sink, with recovery/overflow tracking.
struct Fixture {
  Sink sink;

  struct GapRequest { uint64_t from, to; };
  std::vector<GapRequest> gap_requests;
  int overflow_count = 0;

  GapDetector<Sink> det{
    sink,
    [this](uint64_t f, uint64_t t) { gap_requests.push_back({f, t}); },
    [this]()                        { ++overflow_count; },
    /*max_buffer=*/4
  };
};

} // namespace

// ═══════════════════════════════════════════════════════════════════════

TEST(GapDetector, InOrderDelivery) {
  Fixture fx;
  fx.det.on_md(1, make_add(1, 1));
  fx.det.on_md(2, make_add(1, 2));
  fx.det.on_md(3, make_add(1, 3));

  EXPECT_EQ(fx.sink.received.size(), 3u);
  EXPECT_EQ(fx.sink.received[0].seq, 1u);
  EXPECT_EQ(fx.sink.received[2].seq, 3u);
  EXPECT_FALSE(fx.det.in_gap());
  EXPECT_TRUE(fx.gap_requests.empty());
}

TEST(GapDetector, DuplicateIsDropped) {
  Fixture fx;
  fx.det.on_md(1, make_add());
  fx.det.on_md(1, make_add()); // duplicate
  fx.det.on_md(2, make_add());

  EXPECT_EQ(fx.sink.received.size(), 2u);
  EXPECT_EQ(fx.det.dropped(), 1u);
}

TEST(GapDetector, SimpleGapTriggerRecovery) {
  Fixture fx;
  fx.det.on_md(1, make_add(1, 1));
  // seq 2 is missing
  fx.det.on_md(3, make_add(1, 3)); // arrives out of order

  // seq 3 buffered, not yet delivered
  EXPECT_EQ(fx.sink.received.size(), 1u);
  EXPECT_TRUE(fx.det.in_gap());
  EXPECT_EQ(fx.det.buffered(), 1u);

  // Recovery requested for the missing seq 2
  ASSERT_EQ(fx.gap_requests.size(), 1u);
  EXPECT_EQ(fx.gap_requests[0].from, 2u);
  EXPECT_EQ(fx.gap_requests[0].to,   2u);
}

TEST(GapDetector, GapFillDeliversPendingBuffer) {
  Fixture fx;
  fx.det.on_md(1, make_add(1, 1));
  fx.det.on_md(3, make_add(1, 3)); // buffered
  fx.det.on_md(4, make_add(1, 4)); // also buffered

  EXPECT_EQ(fx.sink.received.size(), 1u);

  // Recovery delivers the missing seq 2
  fx.det.on_md(2, make_add(1, 2));

  // Now 2, 3, 4 should all be delivered in order
  EXPECT_EQ(fx.sink.received.size(), 4u);
  EXPECT_EQ(fx.sink.received[1].seq, 2u);
  EXPECT_EQ(fx.sink.received[2].seq, 3u);
  EXPECT_EQ(fx.sink.received[3].seq, 4u);
  EXPECT_FALSE(fx.det.in_gap());
}

TEST(GapDetector, MultipleGapsRequestedIncrementally) {
  Fixture fx;
  fx.det.on_md(1, make_add(1, 1));
  fx.det.on_md(3, make_add(1, 3)); // gap: request [2,2]
  fx.det.on_md(6, make_add(1, 6)); // request [3,5]: continues from recovery_req_through_+1

  // Detector tracks what was requested, not what's in the buffer.
  // Seq 3 is buffered but still requested — recovery duplicate is a no-op.
  ASSERT_EQ(fx.gap_requests.size(), 2u);
  EXPECT_EQ(fx.gap_requests[0].from, 2u);
  EXPECT_EQ(fx.gap_requests[0].to,   2u);
  EXPECT_EQ(fx.gap_requests[1].from, 3u); // continues after last requested seq
  EXPECT_EQ(fx.gap_requests[1].to,   5u);
}

TEST(GapDetector, GapFillRevealsDeeperGap) {
  Fixture fx;
  fx.det.on_md(1, make_add(1, 1));
  fx.det.on_md(3, make_add(1, 3)); // gap at 2, request [2,2]
  fx.det.on_md(6, make_add(1, 6)); // request [4,5]

  // Fill first gap: delivers 3, then finds gap at 4
  fx.det.on_md(2, make_add(1, 2));

  EXPECT_EQ(fx.sink.received.size(), 3u); // 1, 2, 3 delivered
  EXPECT_TRUE(fx.det.in_gap());           // still waiting for 4, 5
  EXPECT_EQ(fx.det.buffered(), 1u);       // 6 still buffered
}

TEST(GapDetector, FullDrainClearsGapFlag) {
  Fixture fx;
  fx.det.on_md(1, make_add(1, 1));
  fx.det.on_md(3, make_add(1, 3));
  fx.det.on_md(4, make_add(1, 4));

  fx.det.on_md(2, make_add(1, 2)); // fills gap

  EXPECT_FALSE(fx.det.in_gap());
  EXPECT_EQ(fx.det.buffered(), 0u);
  EXPECT_EQ(fx.sink.received.size(), 4u);
}

TEST(GapDetector, OverflowFiresWhenBufferFull) {
  Fixture fx; // max_buffer = 4
  fx.det.on_md(1, make_add(1, 1));
  // Skip 2..5 and push 5 buffered entries
  fx.det.on_md(3, make_add(1, 3));
  fx.det.on_md(4, make_add(1, 4));
  fx.det.on_md(5, make_add(1, 5));
  fx.det.on_md(6, make_add(1, 6));
  EXPECT_EQ(fx.overflow_count, 0); // 4 entries = exactly at limit

  fx.det.on_md(7, make_add(1, 7)); // 5 entries: exceeds max
  EXPECT_EQ(fx.overflow_count, 1);
}

TEST(GapDetector, ResetReanchorsExpected) {
  Fixture fx;
  fx.det.on_md(1, make_add(1, 1));
  fx.det.on_md(3, make_add(1, 3)); // gap

  // Snapshot received; live feed restarts at seq 100
  fx.det.reset(100);

  EXPECT_FALSE(fx.det.in_gap());
  EXPECT_EQ(fx.det.buffered(), 0u);
  EXPECT_EQ(fx.det.expected_seq(), 100u);

  fx.det.on_md(100, make_add(1, 10));
  fx.det.on_md(101, make_add(1, 11));
  EXPECT_EQ(fx.sink.received.size(), 3u); // 1 before reset + 2 after
  EXPECT_FALSE(fx.det.in_gap());
}

TEST(GapDetector, StaleRetransmitInBufferIsSkipped) {
  Fixture fx;
  fx.det.on_md(1, make_add(1, 1));
  fx.det.on_md(3, make_add(1, 3)); // buffered
  fx.det.on_md(4, make_add(1, 4)); // buffered

  // Recovery delivers 2 AND re-delivers 3 (double-send)
  fx.det.on_md(2, make_add(1, 2));
  fx.det.on_md(3, make_add(1, 3)); // stale retransmit → dropped

  EXPECT_EQ(fx.sink.received.size(), 4u); // 1, 2, 3, 4 — no duplicates
  EXPECT_EQ(fx.det.dropped(), 1u);
}

TEST(GapDetector, FirstSeqSetsBaseline) {
  // Exchange might start at any seq, not necessarily 1.
  Fixture fx;
  fx.det.on_md(500, make_add(1, 1));
  fx.det.on_md(501, make_add(1, 2));

  EXPECT_EQ(fx.sink.received.size(), 2u);
  EXPECT_FALSE(fx.det.in_gap());
  EXPECT_TRUE(fx.gap_requests.empty()); // no gap
}
