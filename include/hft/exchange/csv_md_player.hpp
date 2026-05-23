// include/hft/exchange/csv_md_player.hpp
// Loads a timed sequence of MD events from a CSV file.
// 从 CSV 文件加载带时间戳的 MD 事件序列。
//
// CSV format (whitespace around commas ignored; lines beginning with '#' are
// comments):
//   delay_ms, type,   sym, id, side, price,  qty
//   0,        Add,    1,   1,  Sell, 101.00, 1000
//   2000,     Cancel, 1,   1,  ,     ,
//
// Columns / 字段:
//   delay_ms : ms from scenario start (after OE connection is accepted)
//              从场景启动（OE 连接建立后）经过的毫秒数
//   type     : Add | Cancel | Reduce | Clear
//   sym      : symbol id (uint32)
//   id       : order id (uint64)  — Add, Cancel, Reduce
//   side     : Buy | Sell         — Add only
//   price    : decimal price, e.g. 101.0000 (converted via kPriceScale)
//              十进制价格，内部乘以 kPriceScale 转换为整数
//   qty      : quantity (int64)   — Add, Reduce
//
// Events with the same delay_ms are dispatched in file order.
// 相同 delay_ms 的事件按文件顺序依次发出。
#pragma once

#include <hft/core/types.hpp>
#include <hft/md/md_event.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hft::exchange {

struct TimedEvent {
  uint32_t    delay_ms;
  md::MdEvent ev;
};

class CsvMdPlayer {
public:
  // Returns empty vector on error (diagnostics printed to stderr).
  // 出错返回空 vector（错误信息输出到 stderr）。
  static std::vector<TimedEvent> load(const std::string &path) noexcept {
    std::ifstream f(path);
    if (!f) {
      fprintf(stderr, "ERROR: cannot open scenario CSV: %s\n", path.c_str());
      return {};
    }

    std::vector<TimedEvent> out;
    std::string             line;
    int                     line_no = 0;

    while (std::getline(f, line)) {
      ++line_no;
      // Strip inline comments and trim.
      const auto hash = line.find('#');
      if (hash != std::string::npos) line.resize(hash);
      trim(line);
      if (line.empty()) continue;

      const auto fields = split(line);
      if (fields.size() < 3) {
        fprintf(stderr, "WARN:  CSV:%d too few fields — skipping\n", line_no);
        continue;
      }

      uint32_t delay_ms = 0;
      if (!parse_uint(fields[0], delay_ms)) {
        fprintf(stderr, "WARN:  CSV:%d bad delay_ms — skipping\n", line_no);
        continue;
      }

      const std::string_view type = fields[1];
      uint32_t               sym  = 0;
      if (!parse_uint(fields[2], sym)) {
        fprintf(stderr, "WARN:  CSV:%d bad sym — skipping\n", line_no);
        continue;
      }

      std::optional<md::MdEvent> ev;

      if (type == "Add") {
        if (fields.size() < 7) {
          fprintf(stderr, "WARN:  CSV:%d Add needs 7 columns — skipping\n", line_no);
          continue;
        }
        uint64_t id; Side side; double price_d; int64_t qty;
        if (!parse_uint(fields[3], id))        { warn(line_no, "bad id");    continue; }
        if (!parse_side(fields[4], side))       { warn(line_no, "bad side");  continue; }
        if (!parse_price(fields[5], price_d))   { warn(line_no, "bad price"); continue; }
        if (!parse_int64(fields[6], qty))        { warn(line_no, "bad qty");   continue; }
        const Price px = static_cast<Price>(price_d * kPriceScale + 0.5);
        ev = md::Add{0, sym, id, side, px, qty};

      } else if (type == "Cancel") {
        if (fields.size() < 4) {
          fprintf(stderr, "WARN:  CSV:%d Cancel needs 4 columns — skipping\n", line_no);
          continue;
        }
        uint64_t id;
        if (!parse_uint(fields[3], id)) { warn(line_no, "bad id"); continue; }
        ev = md::Cancel{0, sym, id};

      } else if (type == "Reduce") {
        if (fields.size() < 5) {
          fprintf(stderr, "WARN:  CSV:%d Reduce needs 5 columns — skipping\n", line_no);
          continue;
        }
        uint64_t id; int64_t new_qty;
        if (!parse_uint(fields[3], id))        { warn(line_no, "bad id");      continue; }
        if (!parse_int64(fields[4], new_qty))   { warn(line_no, "bad new_qty"); continue; }
        ev = md::Reduce{0, sym, id, new_qty};

      } else if (type == "Clear") {
        ev = md::Clear{0, sym};

      } else {
        fprintf(stderr, "WARN:  CSV:%d unknown type '%.*s' — skipping\n",
                line_no, static_cast<int>(type.size()), type.data());
        continue;
      }

      out.push_back({delay_ms, *ev});
    }

    // Stable sort preserves file order within the same delay_ms.
    // stable_sort 保证相同 delay_ms 内按文件顺序排列。
    std::stable_sort(out.begin(), out.end(),
                     [](const TimedEvent &a, const TimedEvent &b) {
                       return a.delay_ms < b.delay_ms;
                     });

    fprintf(stderr, "INFO:  loaded %zu MD events from %s\n",
            out.size(), path.c_str());
    return out;
  }

private:
  static void warn(int ln, const char *msg) noexcept {
    fprintf(stderr, "WARN:  CSV:%d %s — skipping\n", ln, msg);
  }

  static void trim(std::string &s) noexcept {
    const auto f = s.find_first_not_of(" \t\r");
    if (f == std::string::npos) { s.clear(); return; }
    s = s.substr(f, s.find_last_not_of(" \t\r") - f + 1);
  }

  static std::string_view trim_sv(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
      sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t'))
      sv.remove_suffix(1);
    return sv;
  }

  static std::vector<std::string_view> split(std::string_view line) noexcept {
    std::vector<std::string_view> result;
    std::size_t i = 0;
    while (true) {
      const auto j = line.find(',', i);
      result.push_back(trim_sv(line.substr(i, j == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : j - i)));
      if (j == std::string_view::npos) break;
      i = j + 1;
    }
    return result;
  }

  template <class T>
  static bool parse_uint(std::string_view s, T &out) noexcept {
    s = trim_sv(s);
    if (s.empty()) return false;
    T tmp{};
    for (char c : s) {
      if (c < '0' || c > '9') return false;
      tmp = tmp * 10 + static_cast<T>(c - '0');
    }
    out = tmp;
    return true;
  }

  static bool parse_int64(std::string_view s, int64_t &out) noexcept {
    s = trim_sv(s);
    if (s.empty()) return false;
    bool neg = false;
    if (s.front() == '-') { neg = true; s.remove_prefix(1); }
    if (s.empty()) return false;
    int64_t tmp = 0;
    for (char c : s) {
      if (c < '0' || c > '9') return false;
      tmp = tmp * 10 + (c - '0');
    }
    out = neg ? -tmp : tmp;
    return true;
  }

  static bool parse_price(std::string_view s, double &out) noexcept {
    s = trim_sv(s);
    if (s.empty()) return false;
    // Manual parse: integer part + optional fractional part.
    // 手动解析：整数部分 + 可选小数部分。
    double result = 0.0;
    bool   seen_dot = false;
    double frac     = 0.1;
    for (char c : s) {
      if (c == '.') {
        if (seen_dot) return false;
        seen_dot = true;
      } else if (c >= '0' && c <= '9') {
        if (seen_dot) { result += (c - '0') * frac; frac *= 0.1; }
        else          { result = result * 10.0 + (c - '0'); }
      } else {
        return false;
      }
    }
    out = result;
    return true;
  }

  static bool parse_side(std::string_view s, Side &out) noexcept {
    s = trim_sv(s);
    if (s == "Buy" || s == "buy" || s == "BUY")   { out = Side::Buy;  return true; }
    if (s == "Sell" || s == "sell" || s == "SELL") { out = Side::Sell; return true; }
    return false;
  }
};

} // namespace hft::exchange
