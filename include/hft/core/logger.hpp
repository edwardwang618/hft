#pragma once

#include <cstdarg>
#include <cstdio>
#include <mutex>

// ============================================================================
// Logger (simplified for demo)
// ----------------------------------------------------------------------------
// Production-grade HFT loggers defer formatting to a background thread, use
// lock-free MPSC queues, store (fmt_ptr, packed_args) binary records on the
// hot path, and reconstruct text offline. See NanoLog / quill for reference.
//
// For this project we only need:
//   - LOG_INFO / LOG_WARN / LOG_ERROR for cold paths (startup, shutdown)
//   - LOG_HOT for hot paths; compiled out in release unless HFT_HOT_LOG=1
//
// A global mutex guards the FILE* so concurrent threads don't interleave
// bytes. This mutex is ONLY touched by cold-path logs in this project.
// ============================================================================

namespace hft {

namespace detail {

inline std::mutex &log_mutex() {
  static std::mutex m;
  return m;
}

inline void log_line(const char *level, const char *fmt, va_list ap) noexcept {
  std::lock_guard<std::mutex> lk(log_mutex());
  std::fprintf(stderr, "[%s] ", level);
  std::vfprintf(stderr, fmt, ap);
  std::fputc('\n', stderr);
}

} // namespace detail

inline void log_info(const char *fmt, ...) noexcept {
  va_list ap;
  va_start(ap, fmt);
  detail::log_line("INFO ", fmt, ap);
  va_end(ap);
}

inline void log_warn(const char *fmt, ...) noexcept {
  va_list ap;
  va_start(ap, fmt);
  detail::log_line("WARN ", fmt, ap);
  va_end(ap);
}

inline void log_error(const char *fmt, ...) noexcept {
  va_list ap;
  va_start(ap, fmt);
  detail::log_line("ERROR", fmt, ap);
  va_end(ap);
}

} // namespace hft

// ---------------------------------------------------------------------------
// Macros. The cold-path ones are trivial wrappers; the hot-path one is
// compiled out unless HFT_HOT_LOG=1, so you pay zero cost in release.
// ---------------------------------------------------------------------------
#define LOG_INFO(...) ::hft::log_info(__VA_ARGS__)
#define LOG_WARN(...) ::hft::log_warn(__VA_ARGS__)
#define LOG_ERROR(...) ::hft::log_error(__VA_ARGS__)

#if defined(HFT_HOT_LOG) && HFT_HOT_LOG
#define LOG_HOT(...) ::hft::log_info(__VA_ARGS__)
#else
#define LOG_HOT(...) ((void)0)
#endif