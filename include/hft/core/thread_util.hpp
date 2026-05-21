// include/hft/core/thread_util.hpp
// CPU affinity and scheduler helpers for thread isolation.
// CPU 亲和性与调度器辅助函数，用于线程隔离。
#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <sched.h>

namespace hft::core {

// Pin the calling thread to a single CPU core.
// 将当前线程绑定到指定 CPU 核心，防止 OS 迁移线程导致 cache 失效。
inline bool pin_to_core(int core) noexcept {
  cpu_set_t cs;
  CPU_ZERO(&cs);
  CPU_SET(core, &cs);
  return pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs) == 0;
}

// Set calling thread to SCHED_FIFO real-time scheduling.
// Requires CAP_SYS_NICE or root. Returns false if permission denied
// (non-fatal). 设置为 SCHED_FIFO 实时调度。需要 CAP_SYS_NICE
// 权限，失败时非致命。
inline bool set_realtime_priority(int priority = 80) noexcept {
  struct sched_param sp{};
  sp.sched_priority = priority;
  return sched_setscheduler(0, SCHED_FIFO, &sp) == 0;
}

// Demote calling thread to SCHED_IDLE — for background/logging threads so they
// never preempt the hot-path cores.
// 将后台线程降级为 SCHED_IDLE，避免抢占热路径核心。
inline bool set_idle_priority() noexcept {
  struct sched_param sp{};
  sp.sched_priority = 0;
  return sched_setscheduler(0, SCHED_IDLE, &sp) == 0;
}

} // namespace hft::core
