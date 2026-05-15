#pragma once
#include "md_event.hpp"
#include <cstddef>
#include <cstdint>
#include <hft/md/md_event.hpp>
#include <optional>
#include <span>

namespace hft::md {

struct ParseResult {
  enum Status { Ok, NeedMore, Error };
  Status status = NeedMore;
  size_t consumed = 0;
  std::optional<MdEvent> event;
  std::uint64_t seq = 0;

  static ParseResult need_more() { return {NeedMore, 0, std::nullopt}; }
  static ParseResult error(size_t c = 0) { return {Error, c, std::nullopt}; }
  static ParseResult ok(size_t c, MdEvent e, std::uint64_t seq) {
    return {Ok, c, std::move(e), seq};
  }
};

class IParser {
public:
  virtual ~IParser() = default;
  virtual ParseResult parse(std::span<const std::byte> buf) = 0;
};

} // namespace hft::md