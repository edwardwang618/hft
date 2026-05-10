#pragma once

#include "hft/md/binary_parser.hpp"
#include "hft/md/md_event.hpp"
#include "hft/md/parser.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hft::md {

template <class Sink> class FeedHandler {
public:
  explicit FeedHandler(Sink &sink) : sink_(sink) {}

  // transport 层喂字节进来. 内部会:
  //   1. append 到 buf_
  //   2. loop parse, 每得到一个 event 就 sink_.on_event(ev)
  //   3. parser 说 NeedMore 就 break, 剩余字节留在 buf_ 里等下次
  void on_bytes(const std::byte *data, std::size_t n) {
    buf_.insert(buf_.end(), data, data + n);

    std::size_t offset = 0;
    bool keep_going = true;
    while (keep_going) {
      std::span<const std::byte> view{buf_.data() + offset,
                                      buf_.size() - offset};
      ParseResult r = parser_.parse(view);

      switch (r.status) {
      case ParseResult::Ok:
        sink_.on_md(r.event.value());
        offset += r.consumed;
        msgs_++;
        break;
      case ParseResult::NeedMore:
        keep_going = false;
        break;
      case ParseResult::Error:
        bytes_ += offset;
        corrupt_++;
        buf_.clear();
        return;
      }
    }

    if (offset > 0) {
      buf_.erase(buf_.begin(), buf_.begin() + offset);
      bytes_ += offset;
    }
  }

  std::uint64_t msgs_parsed() const { return msgs_; }
  std::uint64_t bytes_consumed() const { return bytes_; }
  std::uint64_t corrupt_events() const { return corrupt_; }
  std::size_t buffered() const { return buf_.size(); } // debug 用

private:
  Sink &sink_;
  std::vector<std::byte> buf_;
  BinaryParser parser_; // 如果你的 parser 是无状态的, 这个可以去掉, 直接静态调
  std::uint64_t msgs_ = 0;
  std::uint64_t bytes_ = 0;
  std::uint64_t corrupt_ = 0;
};

} // namespace hft::md