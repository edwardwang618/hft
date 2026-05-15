#pragma once
#include <cstdint>
#include <hft/md/md_event.hpp>
#include <string>

namespace hft::test {

// Encode a single MdEvent as one FIX-like line (terminating '\n' included).
std::string encode_fix(const hft::md::MdEvent &ev, std::uint64_t seq);

} // namespace hft::test