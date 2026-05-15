#include "fix_fixtures.hpp"

#include <hft/md/wire_fix.hpp>
#include <sstream>
#include <type_traits>
#include <variant>

namespace hft::test {
using namespace hft::md;

static int side_to_int(Side s) {
  return s == Side::Buy ? wire_fix::kSideBuy : wire_fix::kSideSell;
}

std::string encode_fix(const MdEvent &ev, std::uint64_t seq) {
  std::ostringstream o;
  std::visit(
      [&](const auto &e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, Add>) {
          o << "35=A|34=" << seq << "|52=" << e.ts << "|55=" << e.sym
            << "|37=" << e.id << "|54=" << side_to_int(e.side) << "|44=" << e.px
            << "|38=" << e.qty;
        } else if constexpr (std::is_same_v<T, Cancel>) {
          o << "35=X|34=" << seq << "|52=" << e.ts << "|55=" << e.sym
            << "|37=" << e.id;
        } else if constexpr (std::is_same_v<T, Reduce>) {
          o << "35=R|34=" << seq << "|52=" << e.ts << "|55=" << e.sym
            << "|37=" << e.id << "|39=" << e.new_qty;
        } else if constexpr (std::is_same_v<T, Exec>) {
          o << "35=E|34=" << seq << "|52=" << e.ts << "|55=" << e.sym
            << "|37=" << e.id << "|32=" << e.exec_qty << "|44=" << e.px
            << "|17=" << e.trade_id;
        } else if constexpr (std::is_same_v<T, Replace>) {
          o << "35=G|34=" << seq << "|52=" << e.ts << "|55=" << e.sym
            << "|41=" << e.old_id << "|37=" << e.new_id
            << "|54=" << side_to_int(e.side) << "|44=" << e.px
            << "|38=" << e.qty;
        } else if constexpr (std::is_same_v<T, Trade>) {
          o << "35=T|34=" << seq << "|52=" << e.ts << "|55=" << e.sym
            << "|44=" << e.px << "|38=" << e.qty << "|17=" << e.trade_id;
        } else if constexpr (std::is_same_v<T, Clear>) {
          o << "35=C|34=" << seq << "|52=" << e.ts << "|55=" << e.sym;
        }
      },
      ev);
  o << '\n';
  return o.str();
}

} // namespace hft::test