// banjo-nx: the frame statistics Renut's renderer reports into
// (renut_engine/perf/frame_stats.h, copied from Renut by
// tools/renut_gpu_import.py). Renut's own frame_stats.cpp also hooks the game's
// main loop (appMainTickPreDraw / appMainDraw), which banjo-nx's FPS code hooks
// already, so this keeps the numbers without those hooks: a game frame is one
// D3DDevice_Swap, its length the time between two.
#include "renut_engine/perf/frame_stats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>

#include <fmt/format.h> // blur-nx (2026-11-13): WaitReport

// Counter 0 of the console profiler: presented frames, which the Status Monitor
// overlay's FPS (SaltyNX) and the profiler's "game N fps" are made from
// (sdk/src/ui/switch_perf.cpp; a stub on the PC, me_pc_stubs.cpp).
extern "C" void RexSwitchPerfCount(unsigned id);
extern "C" void RexSwitchPerfHitch(uint64_t start, uint64_t end);

#include "profiler/profiler.h" // the on-screen profiler's GPU, present and render thread rows
#include "renut_engine/gpu/banjo_renut_tuning.h" // the [renut] report and its in-run A/B

namespace renut::frame_stats {

namespace {

using Clock = std::chrono::steady_clock;

struct State {
  std::mutex mutex;
  Snapshot snap;
  Clock::time_point last_frame{};
  Clock::time_point window_start{};
  int window_frames = 0;
  double window_worst = 0.0;
  double gpu_sum = 0.0, render_sum = 0.0, wait_sum = 0.0;
  int gpu_n = 0, render_n = 0;
  // Wait sources: 0 = fence, 1 = swap_ahead, 2 = display, 3 = limiter.
  double source_ms[4] = {};
  uint64_t source_n[4] = {};
  double frame_wait[4] = {};
  double last_gpu_ms = 0.0;
  double last_render_ms = 0.0;
  bool has_gpu = false;
  bool has_render = false;
  PacingSample ring[PacingSnapshot::kRingSize]{};
  int ring_head = 0;
  int ring_count = 0;
  uint64_t total_hitches = 0;
  uint64_t last_frame_tick = 0;
  Clock::time_point report_start = Clock::now();
};

State &S() {
  static State s;
  return s;
}

std::string FormatWaitReportLocked(const State &s, Clock::time_point now) {
  const double seconds = std::max(
      1e-3, std::chrono::duration<double>(now - s.report_start).count());
  return fmt::format("game thread's waits for the renderer per second (#37's "
                     "census): fences {:.0f} ({:.1f} ms), Swap's frames-ahead "
                     "limit {:.0f} ({:.1f} ms), the display's refresh {:.0f} "
                     "({:.1f} ms), limiter {:.0f} ({:.1f} ms)",
                     double(s.source_n[0]) / seconds, s.source_ms[0] / seconds,
                     double(s.source_n[1]) / seconds, s.source_ms[1] / seconds,
                     double(s.source_n[2]) / seconds, s.source_ms[2] / seconds,
                     double(s.source_n[3]) / seconds, s.source_ms[3] / seconds);
}

inline uint64_t ReadCntpctTick() {
#if defined(__SWITCH__) && defined(__aarch64__)
  uint64_t tick = 0;
  asm volatile("mrs %0, cntpct_el0" : "=r"(tick));
  return tick;
#else
  return 0;
#endif
}

} // namespace

void OnGameFrame() {
  banjo::renut_gpu::StatsGameFrame();
  const uint64_t now_tick = ReadCntpctTick();
  uint64_t prev_tick = 0;
  bool report_hitch = false;
  {
    State &s = S();
    std::lock_guard lock(s.mutex);
    const auto now = Clock::now();
    if (s.last_frame != Clock::time_point{}) {
      const double ms =
          std::chrono::duration<double, std::milli>(now - s.last_frame).count();
      Snapshot &snap = s.snap;
      if (snap.history_count < Snapshot::kHistory) {
        snap.history[snap.history_count++] = float(ms);
      } else {
        std::copy(snap.history + 1, snap.history + Snapshot::kHistory,
                  snap.history);
        snap.history[Snapshot::kHistory - 1] = float(ms);
      }

      const double total_wait =
          s.frame_wait[0] + s.frame_wait[1] + s.frame_wait[2] + s.frame_wait[3];
      PacingSample sample;
      sample.frame_ms = float(ms);
      sample.cpu_active_ms = float(std::max(0.0, ms - total_wait));
      sample.wait_fence_ms = float(s.frame_wait[0]);
      sample.wait_swap_ahead_ms = float(s.frame_wait[1]);
      sample.wait_display_ms = float(s.frame_wait[2]);
      sample.wait_limiter_ms = float(s.frame_wait[3]);
      sample.render_thread_ms = float(s.last_render_ms);
      sample.gpu_ms = float(s.last_gpu_ms);
      s.ring[s.ring_head] = sample;
      s.ring_head = (s.ring_head + 1) % PacingSnapshot::kRingSize;
      if (s.ring_count < PacingSnapshot::kRingSize) {
        ++s.ring_count;
      }
      if (ms > 45.0 && ms < 5000.0) {
        ++s.total_hitches;
        report_hitch = true;
      }

      s.window_worst = std::max(s.window_worst, ms);
      ++s.window_frames;
    } else {
      s.window_start = now;
    }
    for (double &w : s.frame_wait) {
      w = 0.0;
    }
    s.last_frame = now;
    prev_tick = s.last_frame_tick;
    s.last_frame_tick = now_tick;
    const double window =
        std::chrono::duration<double>(now - s.window_start).count();
    if (window >= 1.0 && s.window_frames) {
      Snapshot &snap = s.snap;
      snap.fps = s.window_frames / window;
      snap.frame_ms = window * 1000.0 / s.window_frames;
      snap.worst_ms = s.window_worst;
      snap.cpu_ms = std::max(0.0, snap.frame_ms - s.wait_sum / s.window_frames);
      if (s.gpu_n) {
        snap.gpu_ms = s.gpu_sum / s.gpu_n;
        snap.gpu_known = true;
      }
      if (s.render_n) {
        snap.render_thread_ms = s.render_sum / s.render_n;
        snap.render_thread_known = true;
      }
      s.window_start = now;
      s.window_frames = 0;
      s.window_worst = 0.0;
      s.gpu_sum = s.render_sum = s.wait_sum = 0.0;
      s.gpu_n = s.render_n = 0;
    }
  }
  if (report_hitch && prev_tick && now_tick > prev_tick) {
    RexSwitchPerfHitch(prev_tick, now_tick);
  }
}

void OnGpuFrame(double ms) {
  banjo::renut_gpu::StatsGpuFrame(ms);
  banjo::profiler::SetGpuFrameMs(ms); // the profiler window's "GPU Frame"
                                      // (package 27: nobody fed it before)
  State &s = S();
  std::lock_guard lock(s.mutex);
  s.gpu_sum += ms;
  ++s.gpu_n;
  s.last_gpu_ms = ms;
  s.has_gpu = true;
}

void OnRenderThreadFrame(double busy_ms) {
  banjo::renut_gpu::StatsRenderThreadFrame(busy_ms);
  // The profiler window (package 27): the render thread's frame ends with its
  // present, so this is also the time between two host presents ("Present");
  // one thread, as RecordHostPresent needs.
  banjo::profiler::SetRenderThreadMs(busy_ms);
  banjo::profiler::RecordHostPresent();
  // Package 27c: the presented frame for the console's FPS. Only banjo-nx's own
  // renderer (and the SDK's) counted them, so with Renut's renderer the Status
  // Monitor kept an old value (19.7) and the profile said "game 0.0 fps".
  RexSwitchPerfCount(0);
  State &s = S();
  std::lock_guard lock(s.mutex);
  s.render_sum += busy_ms;
  ++s.render_n;
  s.last_render_ms = busy_ms;
  s.has_render = true;
}

void OnGameThreadWait(double ms, int source) {
  State &s = S();
  std::lock_guard lock(s.mutex);
  s.wait_sum += ms;
  if (source >= 0 && source < 4) {
    s.frame_wait[source] += ms;
    s.source_ms[source] += ms;
    ++s.source_n[source];
  }
}

std::string WaitReport() {
  State &s = S();
  std::lock_guard lock(s.mutex);
  const auto now = Clock::now();
  std::string out = FormatWaitReportLocked(s, now);
  for (int i = 0; i < 4; ++i) {
    s.source_ms[i] = 0.0;
    s.source_n[i] = 0;
  }
  s.report_start = now;
  return out;
}

std::string WaitReportPeek() {
  State &s = S();
  std::lock_guard lock(s.mutex);
  return FormatWaitReportLocked(s, Clock::now());
}

Snapshot Get() {
  State &s = S();
  std::lock_guard lock(s.mutex);
  return s.snap;
}

PacingSnapshot GetPacingSnapshot() {
  std::array<PacingSample, PacingSnapshot::kRingSize> samples{};
  int count = 0;
  uint64_t total_hitches = 0;
  bool gpu_known = false;
  bool render_known = false;
  {
    State &s = S();
    std::lock_guard lock(s.mutex);
    count = s.ring_count;
    total_hitches = s.total_hitches;
    gpu_known = s.has_gpu;
    render_known = s.has_render;
    const int start = (s.ring_head - count + PacingSnapshot::kRingSize) %
                      PacingSnapshot::kRingSize;
    for (int i = 0; i < count; ++i) {
      samples[i] = s.ring[(start + i) % PacingSnapshot::kRingSize];
    }
  }

  PacingSnapshot out;
  out.count = count;
  out.total_hitches = total_hitches;
  out.gpu_known = gpu_known;
  out.render_thread_known = render_known;
  if (count <= 0) {
    return out;
  }

  std::array<float, PacingSnapshot::kRingSize> sorted{};
  double sum_frame = 0.0, sum_cpu = 0.0, sum_fence = 0.0, sum_swap = 0.0;
  double sum_disp = 0.0, sum_lim = 0.0, sum_rt = 0.0, sum_gpu = 0.0;
  constexpr double kVblankMs = 16.6667;

  for (int i = 0; i < count; ++i) {
    const PacingSample &p = samples[i];
    sorted[i] = p.frame_ms;
    sum_frame += p.frame_ms;
    sum_cpu += p.cpu_active_ms;
    sum_fence += p.wait_fence_ms;
    sum_swap += p.wait_swap_ahead_ms;
    sum_disp += p.wait_display_ms;
    sum_lim += p.wait_limiter_ms;
    sum_rt += p.render_thread_ms;
    sum_gpu += p.gpu_ms;
    if (p.frame_ms > 45.0f) {
      ++out.hitches;
    }
    const long vb =
        std::clamp(std::lround(double(p.frame_ms) / kVblankMs), 1L, 5L);
    ++out.vblanks[vb - 1];
  }

  std::sort(sorted.begin(), sorted.begin() + count);
  out.best_ms = sorted[0];
  out.worst_ms = sorted[count - 1];
  out.median_ms = (count & 1)
                      ? sorted[count / 2]
                      : 0.5 * (sorted[count / 2 - 1] + sorted[count / 2]);
  out.avg_ms = sum_frame / double(count);
  out.avg_fps = out.avg_ms > 0.0 ? 1000.0 / out.avg_ms : 0.0;

  const int low_n = std::max(1, count / 100);
  double low_sum = 0.0;
  for (int i = count - low_n; i < count; ++i) {
    low_sum += sorted[i];
  }
  const double low_mean_ms = low_sum / double(low_n);
  out.one_percent_low_fps = low_mean_ms > 0.0 ? 1000.0 / low_mean_ms : 0.0;

  const double stutter_thresh = out.median_ms * 2.0;
  for (int i = 0; i < count; ++i) {
    if (double(sorted[i]) > stutter_thresh) {
      ++out.stutters;
    }
  }

  const PacingSample &cur = samples[count - 1];
  const double inv = 1.0 / double(count);
  out.cur_cpu_active_ms = cur.cpu_active_ms;
  out.avg_cpu_active_ms = sum_cpu * inv;
  out.cur_wait_fence_ms = cur.wait_fence_ms;
  out.avg_wait_fence_ms = sum_fence * inv;
  out.cur_wait_swap_ahead_ms = cur.wait_swap_ahead_ms;
  out.avg_wait_swap_ahead_ms = sum_swap * inv;
  out.cur_wait_display_ms = cur.wait_display_ms;
  out.avg_wait_display_ms = sum_disp * inv;
  out.cur_wait_limiter_ms = cur.wait_limiter_ms;
  out.avg_wait_limiter_ms = sum_lim * inv;
  out.cur_render_thread_ms = cur.render_thread_ms;
  out.avg_render_thread_ms = sum_rt * inv;
  out.cur_gpu_ms = cur.gpu_ms;
  out.avg_gpu_ms = sum_gpu * inv;
  return out;
}

} // namespace renut::frame_stats
