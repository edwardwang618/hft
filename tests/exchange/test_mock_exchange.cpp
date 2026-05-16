#include <gtest/gtest.h>
#include <hft/exchange/binary_serializer.hpp>
#include <hft/exchange/byte_sink.hpp>
#include <hft/exchange/mock_exchange.hpp>
#include <hft/md/binary_parser.hpp>
#include <hft/pipeline/feed_handler.hpp>

using namespace hft;

namespace {
struct RecordingSink {
  std::vector<md::MdEvent> events;
  std::vector<std::uint64_t> seqs;
  void on_md(std::uint64_t seq, const md::MdEvent &ev) noexcept {
    seqs.push_back(seq);
    events.push_back(ev);
  }
};
} // namespace

TEST(MockExchange, ScriptedRoundTripBinary) {
  RecordingSink rec;
  pipeline::FeedHandler<RecordingSink, md::BinaryParser> fh{rec};
  exchange::DirectSink<pipeline::FeedHandler<RecordingSink, md::BinaryParser>>
      sink{fh};
  exchange::BinarySerializer ser;
  exchange::MockExchange mx{sink, ser, /*start_seq=*/100};

  mx.push(md::Add{1, 7, 42, Side::Buy, 10000, 5});
  mx.push(md::Add{2, 7, 43, Side::Sell, 10010, 3});
  mx.push(md::Cancel{3, 7, 42});
  mx.run();

  ASSERT_EQ(rec.events.size(), 3u);
  EXPECT_EQ(rec.seqs[0], 100u);
  EXPECT_EQ(rec.seqs[1], 101u);
  EXPECT_EQ(rec.seqs[2], 102u);
  EXPECT_TRUE(std::holds_alternative<md::Add>(rec.events[0]));
  EXPECT_TRUE(std::holds_alternative<md::Cancel>(rec.events[2]));
}